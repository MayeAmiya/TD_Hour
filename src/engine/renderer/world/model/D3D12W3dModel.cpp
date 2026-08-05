#include "core/container/container_types.h"
#include "engine/renderer/world/model/D3D12W3dModel.h"

#include "engine/renderer/world/resource/WorldTextureCache.h"
#include "engine/renderer/world/model/Skeleton.h"
#include "engine/renderer/d3d12/runtime/D3D12Device.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace engine::render {
namespace {

static_assert(std::is_trivially_copyable_v<StaticMeshVertex>);
static_assert(sizeof(StaticMeshVertex) == 60);

void setError(container::String* error, container::String message) {
    if (error) *error = std::move(message);
}

StaticMeshBlendMode toBlendMode(const StaticMaterialDesc& material) noexcept {
    // W3D shader blend enums map directly to the RefCode ShaderClass values.
    if (material.sourceBlend == 1 && material.destinationBlend == 0) {
        return StaticMeshBlendMode::Opaque;
    }
    if (material.sourceBlend == 1 && material.destinationBlend == 1) {
        return StaticMeshBlendMode::Additive;
    }
    if (material.sourceBlend == 2 && material.destinationBlend == 5) {
        return StaticMeshBlendMode::Alpha;
    }
    if (material.sourceBlend == 0 && material.destinationBlend == 2) {
        return StaticMeshBlendMode::Multiply;
    }
    if (material.sourceBlend == 1 && material.destinationBlend == 3) {
        return StaticMeshBlendMode::Screen;
    }
    // Unsupported legacy combinations remain visible as conventional alpha
    // rather than silently writing opaque black texels.
    return StaticMeshBlendMode::Alpha;
}

[[nodiscard]] bool startsWithAsciiInsensitive(container::StringView value,
                                              container::StringView prefix) noexcept {
    if (value.size() < prefix.size()) return false;
    for (size_t index = 0; index < prefix.size(); ++index) {
        const char lhs = value[index];
        const char rhs = prefix[index];
        const char foldedLhs = lhs >= 'A' && lhs <= 'Z'
            ? static_cast<char>(lhs + ('a' - 'A')) : lhs;
        const char foldedRhs = rhs >= 'A' && rhs <= 'Z'
            ? static_cast<char>(rhs + ('a' - 'A')) : rhs;
        if (foldedLhs != foldedRhs) return false;
    }
    return true;
}

[[nodiscard]] bool isHouseColorMesh(container::StringView name) noexcept {
    // W3DAssetManager::Recolor_Mesh() checks the mesh part after its first
    // dot (for example `AVHUMMER.HOUSECOLOR`) and accepts an undotted mesh
    // name too. Keep that old convention at immutable-model upload time;
    // no shared vertex/material data is changed for an instance colour.
    const size_t dot = name.find('.');
    const container::StringView part = dot != container::StringView::npos && dot + 1u < name.size()
        ? name.substr(dot + 1u) : name;
    return startsWithAsciiInsensitive(part, "HOUSECOLOR");
}

[[nodiscard]] bool isHouseColorTexture(container::StringView name) noexcept {
    // RefCode only clones/recolours textures whose legacy source name begins
    // with ZHC. The shader receives this fact as a packet flag instead of
    // creating an instance texture or mutating WorldTextureCache.
    return startsWithAsciiInsensitive(name, "ZHC");
}

[[nodiscard]] bool isHouseColorInverseAlphaTexture(
    container::StringView name) noexcept {
    return name.size() >= 4u && startsWithAsciiInsensitive(name, "ZHCA");
}

} // namespace

struct D3D12W3dModel::Impl {
    struct Material {
        math::vec3 ambient{0.25f, 0.25f, 0.25f};
        math::vec4 diffuse{1.0f, 1.0f, 1.0f, 1.0f};
        math::vec3 specular{};
        math::vec3 emissive{};
        float shininess = 0.0f;
        bool lightingEnabled = false;
        bool primaryGradientDisabled = false;
        bool texturingEnabled = true;
        bool baseTextureIsLightmap = false;
        bool detailTextureIsLightmap = false;
        container::String materialName;
        container::String textureName;
        uint32_t textureSrvIndex = 0;
        bool ownsTextureReference = false;
        container::String detailTextureName;
        uint32_t detailTextureSrvIndex = 0;
        bool ownsDetailTextureReference = false;
        StaticTextureMapperStages textureMappers{};
        bool twoSided = false;
        StaticMeshAlphaTestMode alphaTestMode = StaticMeshAlphaTestMode::Disabled;
        uint8_t samplerMode = 0;
        uint8_t detailSamplerMode = 0;
        uint8_t detailColorFunc = 0;
        uint8_t detailAlphaFunc = 0;
        uint8_t fogFunc = 0;
        bool houseColorTexture = false;
        bool houseColorInverseAlphaMask = false;
        bool hasDetailTexture = false;
        bool depthWrite = true;
        StaticMeshDepthCompare depthCompare = StaticMeshDepthCompare::LessEqual;
        StaticMeshBlendMode blendMode = StaticMeshBlendMode::Opaque;
        uint32_t materialPass = 0;
    };

    struct Mesh {
        container::String name;
        container::Vector<container::String> visibilityControllerNames;
        d3d12::StaticBufferAllocation vertexBuffer;
        d3d12::StaticBufferAllocation indexBuffer;
        D3D12_VERTEX_BUFFER_VIEW vertexView{};
        D3D12_INDEX_BUFFER_VIEW indexView{};
        container::Vector<StaticMeshVertex> sortingVertices;
        container::Vector<uint32_t> sortingIndices;
        math::transform localTransform{};
        uint32_t sourceBoneIndex = UINT32_MAX;
        bool skinned = false;
        bool initiallyHidden = false;
        bool forceShadow = false;
        int32_t sortLevel = 0;
        bool houseColorVertexMaterial = false;
        container::Vector<StaticPrimitive> primitives;
    };

    static_assert(std::is_nothrow_move_constructible_v<Material>);
    static_assert(std::is_nothrow_move_constructible_v<Mesh>);

    d3d12::D3D12Device* device = nullptr;
    container::SharedPtr<WorldTextureCache> textures;
    container::Vector<Material> materials;
    container::Vector<W3dMaterialTextureBinding> materialTextureBindings;
    container::Vector<Mesh> meshes;
    size_t primitiveCount = 0;
    size_t skinnedMeshCount = 0;
    uint64_t residentBufferBytes = 0;
    uint64_t lastUsedFrame = 0;
    d3d12::GpuRetirementIdentity retirementIdentity;
    bool isRetired = false;
};

D3D12W3dModel::D3D12W3dModel(container::UniquePtr<Impl> impl) noexcept
    : m_impl(std::move(impl)) {}

D3D12W3dModel::~D3D12W3dModel() {
    retire();
}

container::SharedPtr<D3D12W3dModel> D3D12W3dModel::upload(
    d3d12::D3D12Device& device,
    container::SharedPtr<WorldTextureCache> textures,
    const CpuStaticModel& cpuModel,
    const d3d12::GpuRetirementIdentity& retirementIdentity,
    RenderAssetPriority priority,
    container::String* error,
    bool* deferred) {
    if (error) error->clear();
    if (deferred) *deferred = false;
    if (!textures) {
        setError(error, "W3D GPU upload requires a world texture cache");
        return nullptr;
    }
    if (cpuModel.empty()) {
        setError(error, "W3D GPU upload received an empty CPU model");
        return nullptr;
    }

    // Request every material payload before taking references or recording a
    // buffer/texture upload. A model with one late texture therefore waits as
    // a whole and cannot churn already-acquired SRVs on each retry.
    bool texturePayloadPending = false;
    for (const StaticMaterialDesc& material : cpuModel.materials) {
        if (!material.textureName.empty() &&
            !textures->prepare(material.textureName,
                               WorldTextureCache::Variant::ColorLegacyGamma,
                               priority)) {
            texturePayloadPending = true;
        }
        if (!material.detailTextureName.empty() &&
            !textures->prepare(material.detailTextureName,
                               WorldTextureCache::Variant::ColorLegacyGamma,
                               priority)) {
            texturePayloadPending = true;
        }
    }
    if (texturePayloadPending) {
        if (deferred) *deferred = true;
        setError(error, "W3D texture CPU preparation pending");
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    impl->device = &device;
    impl->textures = std::move(textures);
    impl->retirementIdentity = retirementIdentity;
    auto model = container::SharedPtr<D3D12W3dModel>(new D3D12W3dModel(std::move(impl)));

    model->m_impl->materials.reserve(cpuModel.materials.size());
    model->m_impl->materialTextureBindings.reserve(
        cpuModel.materials.size());
    for (const StaticMaterialDesc& source : cpuModel.materials) {
        Impl::Material material;
        material.materialName = source.name;
        material.diffuse = source.diffuse;
        material.lightingEnabled = source.lightingEnabled;
        if (material.lightingEnabled) {
            // Keep material response separate from the shared scene lights:
            // WorldRenderer evaluates material ambient/diffuse exactly once
            // against the sealed three-light environment.
            material.ambient = source.ambient;
            // ShaderClass::Apply() maps W3D's secondary-gradient bit directly
            // to D3DRS_SPECULARENABLE.  Several stock map models retain a
            // bright specular colour and very small shininess even with that
            // bit disabled; applying it merely because shininess is nonzero
            // produces the broad, pure-white local overexposure seen on
            // refinery/pump geometry.  Collapse disabled specular at the
            // backend boundary so the HLSL path cannot resurrect it.
            material.specular = source.secondaryGradientEnabled ? source.specular : math::vec3{};
            material.emissive = source.emissive;
            material.shininess = source.secondaryGradientEnabled ? source.shininess : 0.0f;
        } else {
            // RefCode's PRELIT_VERTEX mesh installation path disables
            // D3DRS_LIGHTING, so no material light or emissive term is added
            // to the texture/material/vertex-colour surface.
            material.ambient = {1.0f, 1.0f, 1.0f};
            material.specular = {};
            material.emissive = {};
            material.shininess = 0.0f;
        }
        material.textureName = source.textureName;
        material.primaryGradientDisabled = source.primaryGradientDisabled;
        material.texturingEnabled = source.texturingEnabled;
        material.baseTextureIsLightmap = source.baseTextureIsLightmap;
        material.detailTextureIsLightmap = source.detailTextureIsLightmap;
        material.houseColorTexture = isHouseColorTexture(material.textureName);
        material.houseColorInverseAlphaMask =
            isHouseColorInverseAlphaTexture(material.textureName);
        material.detailTextureName = source.detailTextureName;
        material.textureMappers = source.textureMappers;
        material.twoSided = source.twoSided;
        material.alphaTestMode = !source.alphaTest ? StaticMeshAlphaTestMode::Disabled
            : source.alphaTestInverted ? StaticMeshAlphaTestMode::LessEqual
                                       : StaticMeshAlphaTestMode::GreaterEqual;
        material.samplerMode = source.samplerMode;
        material.detailSamplerMode = source.detailSamplerMode;
        material.detailColorFunc = source.detailColorFunc;
        material.detailAlphaFunc = source.detailAlphaFunc;
        material.fogFunc = source.fogFunc;
        material.hasDetailTexture = source.hasDetailTexture;
        material.depthWrite = source.depthWrite;
        material.depthCompare = source.depthCompare < static_cast<uint8_t>(StaticMeshDepthCompare::Count)
            ? static_cast<StaticMeshDepthCompare>(source.depthCompare)
            : StaticMeshDepthCompare::LessEqual;
        material.blendMode = toBlendMode(source);
        material.materialPass = source.materialPass;
        if (!material.textureName.empty()) {
            const auto textureSrv = model->m_impl->textures->acquire(
                material.textureName,
                WorldTextureCache::Variant::ColorLegacyGamma, priority);
            if (!textureSrv) {
                if (deferred) *deferred = true;
                setError(error, "D3D12 texture upload failed for W3D material: " +
                    material.textureName);
                return nullptr;
            }
            material.textureSrvIndex = *textureSrv;
            material.ownsTextureReference = true;
        }
        if (!material.detailTextureName.empty()) {
            const auto textureSrv = model->m_impl->textures->acquire(
                material.detailTextureName,
                WorldTextureCache::Variant::ColorLegacyGamma, priority);
            if (!textureSrv) {
                if (deferred) *deferred = true;
                setError(error, "D3D12 texture upload failed for W3D detail material: " +
                    material.detailTextureName);
                return nullptr;
            }
            material.detailTextureSrvIndex = *textureSrv;
            material.ownsDetailTextureReference = true;
        }
        model->m_impl->materials.push_back(std::move(material));
        const Impl::Material& published =
            model->m_impl->materials.back();
        model->m_impl->materialTextureBindings.push_back({
            .materialIndex = static_cast<uint32_t>(
                model->m_impl->materialTextureBindings.size()),
            .materialName = published.materialName,
            .textureName = published.textureName,
            .textureSrvIndex = published.textureSrvIndex,
            .detailTextureName = published.detailTextureName,
            .detailTextureSrvIndex = published.detailTextureSrvIndex,
        });
    }

    model->m_impl->meshes.reserve(cpuModel.meshes.size());
    for (size_t sourceMeshIndex = 0;
         sourceMeshIndex < cpuModel.meshes.size(); ++sourceMeshIndex) {
        const CpuStaticMesh& source = cpuModel.meshes[sourceMeshIndex];
        if (source.vertices.empty() || source.indices.empty() || source.primitives.empty()) {
            continue;
        }
        if (source.vertices.size() >
                std::numeric_limits<UINT>::max() / sizeof(StaticMeshVertex) ||
            source.indices.size() >
                std::numeric_limits<UINT>::max() / sizeof(uint32_t)) {
            setError(error, "W3D mesh exceeds D3D12 view size limits: " + source.name);
            return nullptr;
        }

        for (const StaticPrimitive& primitive : source.primitives) {
            const uint64_t end = static_cast<uint64_t>(primitive.firstIndex) +
                                 primitive.indexCount;
            if (primitive.indexCount == 0 || end > source.indices.size()) {
                setError(error, "W3D primitive has an invalid index range: " + source.name);
                return nullptr;
            }
            if (primitive.materialIndex >= model->m_impl->materials.size()) {
                setError(error, "W3D primitive references a missing material: " + source.name);
                return nullptr;
            }
        }

        const uint64_t vertexBytes = source.vertices.size() * sizeof(StaticMeshVertex);
        const uint64_t indexBytes = source.indices.size() * sizeof(uint32_t);
        Impl::Mesh mesh;
        mesh.name = source.name;
        mesh.visibilityControllerNames.reserve(cpuModel.meshes.size());
        for (size_t controllerIndex = 0;
             controllerIndex < cpuModel.meshes.size(); ++controllerIndex) {
            if (subObjectVisibilityAffectsMesh(
                    cpuModel, controllerIndex, sourceMeshIndex)) {
                mesh.visibilityControllerNames.push_back(
                    cpuModel.meshes[controllerIndex].name);
            }
        }
        // Copy all potentially allocating CPU metadata before command-list
        // recording. After either CopyBufferRegion is recorded, cleanup must
        // be strictly non-throwing and fence-aware.
        mesh.localTransform = source.localTransform;
        mesh.sourceBoneIndex = source.sourceBoneIndex;
        mesh.skinned = source.skinned;
        mesh.initiallyHidden = source.initiallyHidden;
        mesh.forceShadow = source.forceShadow;
        mesh.sortLevel = source.sortLevel;
        mesh.houseColorVertexMaterial = isHouseColorMesh(source.name);
        if (mesh.skinned) ++model->m_impl->skinnedMeshCount;
        mesh.primitives = source.primitives;
        // Retain one immutable CPU copy for camera-dependent blended triangle
        // sorting. Ordinary opaque/cutout packets never touch these arrays.
        mesh.sortingVertices = source.vertices;
        mesh.sortingIndices = source.indices;
        mesh.vertexBuffer = device.recordStaticBufferUpload(
            source.vertices.data(), vertexBytes,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        mesh.indexBuffer = device.recordStaticBufferUpload(
            source.indices.data(), indexBytes, D3D12_RESOURCE_STATE_INDEX_BUFFER);
        if (!mesh.vertexBuffer || !mesh.indexBuffer) {
            // Either successful copy is already referenced by the open
            // command list, even though this model will not be published.
            device.retireStaticBufferAllocation(
                std::move(mesh.vertexBuffer), retirementIdentity);
            device.retireStaticBufferAllocation(
                std::move(mesh.indexBuffer), retirementIdentity);
            setError(error, "D3D12 static buffer upload failed for W3D mesh: " + source.name);
            return nullptr;
        }

        mesh.vertexView.BufferLocation = mesh.vertexBuffer.gpuAddress;
        mesh.vertexView.SizeInBytes = static_cast<UINT>(vertexBytes);
        mesh.vertexView.StrideInBytes = sizeof(StaticMeshVertex);
        mesh.indexView.BufferLocation = mesh.indexBuffer.gpuAddress;
        mesh.indexView.SizeInBytes = static_cast<UINT>(indexBytes);
        mesh.indexView.Format = DXGI_FORMAT_R32_UINT;
        model->m_impl->residentBufferBytes += vertexBytes + indexBytes;
        model->m_impl->primitiveCount += mesh.primitives.size();
        model->m_impl->meshes.push_back(std::move(mesh));
    }

    if (model->m_impl->meshes.empty()) {
        setError(error, "W3D GPU upload produced no renderable meshes");
        return nullptr;
    }
    return model;
}

void D3D12W3dModel::appendDrawPackets(
    const math::transform& entityWorld,
    container::Span<const math::transform> skinPalette,
    container::Span<const uint8_t> boneVisibility,
    container::Vector<StaticMeshDrawPacket>& output,
    float visualTimeSeconds,
    float directionalLightScale,
    container::Span<const W3dMaterialTextureOverride> materialTextureOverrides,
    math::vec3 scriptFlashTint,
    float heatVisionIntensity,
    bool heatVisionOnly,
    float objectOpacity,
    math::vec3 scriptIndicatorColor,
    bool hasScriptIndicatorColor,
    bool receivesDynamicLights,
    container::Span<const RenderSubObjectVisibility> subObjectVisibility,
    math::vec2 treePushAsideDirection,
    float treePushAsideAmount,
    float treePushAsideDistanceFactor,
    float treePushAsideDarkeningFactor,
    RenderVehicleTreadState vehicleTreads,
    const math::transform* previousEntityWorld,
    container::Span<const math::transform> previousSkinPalette,
    float interpolationAlpha) const {
    if (!m_impl || m_impl->isRetired || !m_impl->device) return;
    m_impl->lastUsedFrame = std::max(
        m_impl->lastUsedFrame, m_impl->device->frameOrdinal());

    const float sanitizedDirectionalLightScale =
        std::isfinite(directionalLightScale) && directionalLightScale > 0.0f
            ? directionalLightScale : 1.0f;
    const float sanitizedVisualTimeSeconds =
        std::isfinite(visualTimeSeconds) && visualTimeSeconds >= 0.0f
            ? visualTimeSeconds : 0.0f;
    const math::vec3 sanitizedScriptFlashTint{
        std::isfinite(scriptFlashTint.x()) ? scriptFlashTint.x() : 0.0f,
        std::isfinite(scriptFlashTint.y()) ? scriptFlashTint.y() : 0.0f,
        std::isfinite(scriptFlashTint.z()) ? scriptFlashTint.z() : 0.0f,
    };
    const float sanitizedHeatVisionIntensity =
        std::isfinite(heatVisionIntensity)
            ? std::clamp(heatVisionIntensity, 0.0f, 1.0f)
            : 0.0f;
    const float sanitizedObjectOpacity = std::isfinite(objectOpacity)
        ? std::clamp(objectOpacity, 0.0f, 1.0f)
        : 1.0f;
    const float sanitizedInterpolationAlpha =
        std::isfinite(interpolationAlpha)
        ? std::clamp(interpolationAlpha, 0.0f, 1.0f) : 1.0f;
    const bool validScriptIndicatorColor = hasScriptIndicatorColor &&
        std::isfinite(scriptIndicatorColor.x()) && std::isfinite(scriptIndicatorColor.y()) &&
        std::isfinite(scriptIndicatorColor.z());
    const math::vec3 sanitizedScriptIndicatorColor{
        validScriptIndicatorColor ? std::clamp(scriptIndicatorColor.x(), 0.0f, 1.0f) : 0.0f,
        validScriptIndicatorColor ? std::clamp(scriptIndicatorColor.y(), 0.0f, 1.0f) : 0.0f,
        validScriptIndicatorColor ? std::clamp(scriptIndicatorColor.z(), 0.0f, 1.0f) : 0.0f,
    };
    // W3DAssetManager treats RGB zero as no model recolour even when the
    // Object retains a non-zero alpha custom indicator value. Preserve that
    // distinction: the packet still exposes the durable black indicator for
    // later icon/UI consumers, but the house-colour shader path is inactive.
    const bool recoloursHouseGeometry = validScriptIndicatorColor &&
        (sanitizedScriptIndicatorColor.x() > 0.0f ||
         sanitizedScriptIndicatorColor.y() > 0.0f ||
         sanitizedScriptIndicatorColor.z() > 0.0f);
    const float pushLengthSquared =
        treePushAsideDirection.x() * treePushAsideDirection.x() +
        treePushAsideDirection.y() * treePushAsideDirection.y();
    const bool validTreePush = std::isfinite(pushLengthSquared) &&
        pushLengthSquared > std::numeric_limits<float>::epsilon() &&
        std::isfinite(treePushAsideAmount) && treePushAsideAmount > 0.0f &&
        std::isfinite(treePushAsideDistanceFactor) &&
        treePushAsideDistanceFactor > 0.0f;
    const float inversePushLength = validTreePush
        ? 1.0f / std::sqrt(pushLengthSquared) : 0.0f;
    const math::vec2 sanitizedTreePushDirection = validTreePush
        ? treePushAsideDirection * inversePushLength : math::vec2{};
    const float sanitizedTreePushAmount = validTreePush
        ? std::clamp(treePushAsideAmount, 0.0f, 1.0f) : 0.0f;
    const float sanitizedTreePushDistance = validTreePush
        ? treePushAsideDistanceFactor : 0.0f;
    const float sanitizedTreePushDarkening =
        validTreePush && std::isfinite(treePushAsideDarkeningFactor)
        ? treePushAsideDarkeningFactor : 0.0f;

    struct ResolvedVisibilityOverride final {
        container::String name;
        bool visible = true;
    };
    container::Vector<ResolvedVisibilityOverride> resolvedVisibility;
    resolvedVisibility.reserve(subObjectVisibility.size());
    for (const RenderSubObjectVisibility& source : subObjectVisibility) {
        container::String selected = source.name;
        if (source.nameIsPrefix) {
            container::Vector<container::String> numbered;
            numbered.reserve(8);
            for (uint32_t ordinal = 1; ordinal <= 99; ++ordinal) {
                container::String candidate = source.name;
                candidate.push_back(static_cast<char>('0' + ordinal / 10u));
                candidate.push_back(static_cast<char>('0' + ordinal % 10u));
                const bool exists = std::any_of(
                    m_impl->meshes.begin(), m_impl->meshes.end(),
                    [&candidate](const Impl::Mesh& candidateMesh) {
                        return std::any_of(
                            candidateMesh.visibilityControllerNames.begin(),
                            candidateMesh.visibilityControllerNames.end(),
                            [&candidate](container::StringView controller) {
                                return renderSubObjectNameMatches(
                                    controller, candidate);
                            });
                    });
                if (!exists) break;
                numbered.push_back(std::move(candidate));
            }
            if (!numbered.empty()) {
                if (source.nameSequenceOrdinal == 0u && !source.visible) {
                    for (container::String& candidate : numbered) {
                        resolvedVisibility.push_back({
                            .name = std::move(candidate),
                            .visible = false,
                        });
                    }
                    continue;
                }
                const uint32_t sequence = std::max<uint32_t>(
                    1u, source.nameSequenceOrdinal);
                selected = numbered[(sequence - 1u) % numbered.size()];
            } else if (!source.namePrefixFallsBackToBare) {
                continue;
            }
        }
        resolvedVisibility.push_back({
            .name = std::move(selected),
            .visible = source.visible,
        });
    }

    output.reserve(output.size() + m_impl->primitiveCount);
    for (const Impl::Mesh& mesh : m_impl->meshes) {
        std::optional<bool> explicitSubObjectVisibility;
        for (const ResolvedVisibilityOverride& overrideValue :
             resolvedVisibility) {
            if (std::any_of(
                    mesh.visibilityControllerNames.begin(),
                    mesh.visibilityControllerNames.end(),
                    [&overrideValue](container::StringView controllerName) {
                        return renderSubObjectNameMatches(
                            controllerName, overrideValue.name);
                    })) {
                explicitSubObjectVisibility = overrideValue.visible;
            }
        }
        if (!explicitSubObjectVisibility.value_or(!mesh.initiallyHidden)) continue;
        if (mesh.skinned && skinPalette.empty()) continue;
        // Classic bit channels control hierarchy-attached mesh parts. A skin
        // mesh can contain several pivots, so treating one pivot as the whole
        // mesh would be incorrect; retain it until per-vertex visibility is
        // implemented in the skin shader.
        if (!mesh.skinned && mesh.sourceBoneIndex < boneVisibility.size() &&
            boneVisibility[mesh.sourceBoneIndex] == 0 &&
            !explicitSubObjectVisibility.value_or(false)) continue;
        const math::transform world = mesh.localTransform * entityWorld;
        const bool attachedToPreparedBone = !mesh.skinned &&
            mesh.sourceBoneIndex < skinPalette.size();
        for (const StaticPrimitive& primitive : mesh.primitives) {
            if (primitive.materialIndex >= m_impl->materials.size()) continue;
            const Impl::Material& material = m_impl->materials[primitive.materialIndex];
            const auto textureOverride = std::find_if(
                materialTextureOverrides.begin(), materialTextureOverrides.end(),
                [&primitive](const W3dMaterialTextureOverride& value) {
                    return value.materialIndex == primitive.materialIndex;
                });

            StaticMeshDrawPacket packet;
            packet.vertexBuffer = mesh.vertexView;
            packet.indexBuffer = mesh.indexView;
            packet.sortingVertices = mesh.sortingVertices.data();
            packet.sortingVertexCount = static_cast<uint32_t>(
                mesh.sortingVertices.size());
            packet.sortingIndices = mesh.sortingIndices.data();
            packet.sortingIndexCount = static_cast<uint32_t>(
                mesh.sortingIndices.size());
            packet.textureSrv = m_impl->device->getSrvGpuHandle(
                textureOverride != materialTextureOverrides.end()
                    ? textureOverride->textureSrvIndex : material.textureSrvIndex);
            packet.detailTextureSrv = m_impl->device->getSrvGpuHandle(
                textureOverride != materialTextureOverrides.end() &&
                    textureOverride->overridesDetailTexture
                ? textureOverride->detailTextureSrvIndex
                : material.detailTextureSrvIndex);
            packet.textureMappers = material.textureMappers;
            const size_t meshSeparator = mesh.name.find_last_of('.');
            const container::StringView meshPart =
                meshSeparator == container::String::npos
                ? container::StringView{mesh.name}
                : container::StringView{mesh.name}.substr(meshSeparator + 1u);
            if (vehicleTreads.enabled &&
                startsWithAsciiInsensitive(meshPart, "TREADS")) {
                float treadOffset = vehicleTreads.middleOffset;
                if (meshPart.size() > 6u &&
                    (meshPart[6] == 'L' || meshPart[6] == 'l')) {
                    treadOffset = vehicleTreads.leftOffset;
                } else if (meshPart.size() > 6u &&
                           (meshPart[6] == 'R' || meshPart[6] == 'r')) {
                    treadOffset = vehicleTreads.rightOffset;
                }
                for (StaticTextureMapperDesc& mapper :
                     packet.textureMappers) {
                    if (mapper.type != StaticTextureMapperType::LinearOffset) {
                        continue;
                    }
                    // W3DTankDraw disables the asset mapper's automatic
                    // delta and installs its confirmed custom U offset.
                    mapper.uPerSecond = 0.0f;
                    mapper.vPerSecond = 0.0f;
                    mapper.uOffset += treadOffset;
                }
            }
            packet.visualTimeSeconds = sanitizedVisualTimeSeconds;
            packet.worldTransform = mesh.skinned
                ? math::transform::identity()
                : (attachedToPreparedBone ? skinPalette[mesh.sourceBoneIndex] : world);
            const bool previousAttachedBone = !mesh.skinned &&
                mesh.sourceBoneIndex < previousSkinPalette.size();
            const bool canInterpolate = previousEntityWorld &&
                sanitizedInterpolationAlpha < 1.0f &&
                (!mesh.skinned ||
                 previousSkinPalette.size() >= skinPalette.size()) &&
                (!attachedToPreparedBone || previousAttachedBone);
            packet.previousWorldTransform = canInterpolate
                ? (mesh.skinned
                    ? math::transform::identity()
                    : (previousAttachedBone
                        ? previousSkinPalette[mesh.sourceBoneIndex]
                        : mesh.localTransform * *previousEntityWorld))
                : packet.worldTransform;
            packet.interpolationAlpha = canInterpolate
                ? sanitizedInterpolationAlpha : 1.0f;
            packet.sortCenter = mesh.skinned
                ? entityWorld.translation()
                : packet.worldTransform.translation();
            packet.hasExplicitSortCenter = true;
            packet.diffuse = material.diffuse;
            packet.ambient = material.ambient;
            packet.specular = material.specular;
            packet.emissive = material.emissive;
            packet.shininess = material.shininess;
            packet.lightingEnabled = material.lightingEnabled;
            packet.primaryGradientDisabled =
                material.primaryGradientDisabled;
            packet.texturingEnabled = material.texturingEnabled;
            packet.lightmapPass = material.baseTextureIsLightmap;
            packet.requiresTriangleSorting =
                !packet.lightmapPass &&
                material.blendMode != StaticMeshBlendMode::Opaque &&
                material.alphaTestMode == StaticMeshAlphaTestMode::Disabled &&
                mesh.sortLevel <= 0;
            packet.sortLevel = mesh.sortLevel;
            packet.directionalLightScale = sanitizedDirectionalLightScale;
            packet.scriptFlashTint = sanitizedScriptFlashTint;
            const bool replacementHeatVision = heatVisionOnly &&
                sanitizedHeatVisionIntensity > 0.0f;
            // A friendly/hint heat response is a second additive EQUAL pass,
            // not a mutation of the base material.  Keep the base packet
            // heat-free here and append its overlay after all ordinary
            // material state has been resolved below.
            packet.heatVisionIntensity = replacementHeatVision
                ? sanitizedHeatVisionIntensity : 0.0f;
            packet.heatVisionOnly = replacementHeatVision;
            packet.objectOpacity = packet.heatVisionOnly
                ? 1.0f : sanitizedObjectOpacity;
            packet.treePushAsideDirection = sanitizedTreePushDirection;
            packet.treePushAsideAmount = sanitizedTreePushAmount;
            packet.treePushAsideDistanceFactor = sanitizedTreePushDistance;
            packet.treePushAsideDarkeningFactor =
                sanitizedTreePushDarkening;
            packet.scriptIndicatorColor = sanitizedScriptIndicatorColor;
            packet.hasScriptIndicatorColor = validScriptIndicatorColor;
            packet.houseColorVertexMaterial = recoloursHouseGeometry &&
                mesh.houseColorVertexMaterial;
            packet.houseColorTexture = recoloursHouseGeometry && material.houseColorTexture;
            packet.houseColorInverseAlphaMask = recoloursHouseGeometry &&
                material.houseColorInverseAlphaMask;
            packet.skinPalette = mesh.skinned ? skinPalette.data() : nullptr;
            packet.previousSkinPalette = mesh.skinned && canInterpolate
                ? previousSkinPalette.data() : packet.skinPalette;
            packet.skinBoneCount = mesh.skinned
                ? static_cast<uint32_t>(skinPalette.size())
                : 0;
            // RefCode's ordinary Mesh shadow path accepts opaque geometry by
            // default. MESH_FLAG_CAST_SHADOW is the author opt-in that also
            // permits an Alpha-blended mesh to cast; additive/multiply/screen
            // effects remain excluded by WorldRenderer's caster policy.
            packet.castsShadow = !packet.heatVisionOnly &&
                !packet.lightmapPass &&
                (material.blendMode == StaticMeshBlendMode::Opaque ||
                 mesh.forceShadow);
            packet.receivesShadow = !packet.lightmapPass;
            packet.receivesDynamicLights =
                !packet.lightmapPass && receivesDynamicLights;
            packet.receivesScenePointLights = !packet.lightmapPass;
            packet.dynamicLightReceiver =
                StaticMeshDynamicLightReceiver::Object;
            packet.twoSided = material.twoSided;
            packet.alphaTestMode = packet.heatVisionOnly
                ? StaticMeshAlphaTestMode::Disabled
                : material.alphaTestMode;
            packet.samplerMode = textureOverride != materialTextureOverrides.end()
                ? std::min<uint8_t>(textureOverride->samplerMode, 3u)
                : material.samplerMode;
            packet.detailSamplerMode = material.detailSamplerMode;
            packet.detailColorFunc = material.detailColorFunc;
            packet.detailAlphaFunc = material.detailAlphaFunc;
            packet.fogFunc = material.fogFunc;
            packet.hasDetailTexture =
                textureOverride != materialTextureOverrides.end() &&
                    textureOverride->overridesDetailTexture
                ? textureOverride->detailTextureSrvIndex != 0
                : material.hasDetailTexture;
            // RefCode's heat-vision-only material pass uses LEQUAL without
            // writing depth and disables the object's ordinary shadow.
            const bool ordinaryOpacityOverride =
                !packet.heatVisionOnly && packet.objectOpacity < 1.0f;
            packet.depthWrite = packet.heatVisionOnly ||
                    ordinaryOpacityOverride
                ? false : material.depthWrite;
            packet.depthCompare = packet.heatVisionOnly
                ? StaticMeshDepthCompare::LessEqual
                : material.depthCompare;
            packet.blendMode = packet.heatVisionOnly
                ? StaticMeshBlendMode::Additive
                : ordinaryOpacityOverride &&
                        material.blendMode == StaticMeshBlendMode::Opaque
                    ? StaticMeshBlendMode::Alpha
                    : material.blendMode;
            packet.worldLayer = packet.blendMode == StaticMeshBlendMode::Opaque
                ? StaticMeshWorldLayer::ObjectsOpaque
                : StaticMeshWorldLayer::ObjectsTransparent;
            packet.materialPass = material.materialPass;
            packet.firstIndex = primitive.firstIndex;
            packet.indexCount = primitive.indexCount;
            output.push_back(packet);
            if (!packet.lightmapPass &&
                sanitizedHeatVisionIntensity > 0.0f &&
                !replacementHeatVision) {
                StaticMeshDrawPacket overlay = packet;
                overlay.heatVisionIntensity = sanitizedHeatVisionIntensity;
                overlay.heatVisionOnly = true;
                overlay.objectOpacity = 1.0f;
                overlay.castsShadow = false;
                overlay.receivesShadow = false;
                overlay.receivesDynamicLights = false;
                overlay.dynamicLightReceiver =
                    StaticMeshDynamicLightReceiver::Object;
                overlay.alphaTestMode = StaticMeshAlphaTestMode::Disabled;
                overlay.depthWrite = false;
                overlay.depthCompare = StaticMeshDepthCompare::Equal;
                overlay.blendMode = StaticMeshBlendMode::Additive;
                overlay.worldLayer =
                    StaticMeshWorldLayer::ObjectsTransparent;
                output.push_back(std::move(overlay));
            }
        }
    }
}

void D3D12W3dModel::retire() noexcept {
    if (!m_impl || m_impl->isRetired) return;
    m_impl->isRetired = true;

    if (m_impl->device) {
        const d3d12::FrameFenceStatus useFence =
            m_impl->device->frameFenceStatus(m_impl->lastUsedFrame);
        for (Impl::Mesh& mesh : m_impl->meshes) {
            m_impl->device->retireStaticBufferAllocation(
                std::move(mesh.vertexBuffer), m_impl->retirementIdentity,
                m_impl->lastUsedFrame, useFence.fenceValue);
            m_impl->device->retireStaticBufferAllocation(
                std::move(mesh.indexBuffer), m_impl->retirementIdentity,
                m_impl->lastUsedFrame, useFence.fenceValue);
        }
    }
    if (m_impl->textures) {
        for (const Impl::Material& material : m_impl->materials) {
            if (material.ownsTextureReference) {
                m_impl->textures->release(material.textureName);
            }
            if (material.ownsDetailTextureReference) {
                m_impl->textures->release(material.detailTextureName);
            }
        }
    }
    m_impl->meshes.clear();
    m_impl->materials.clear();
    m_impl->materialTextureBindings.clear();
    m_impl->primitiveCount = 0;
    m_impl->skinnedMeshCount = 0;
    m_impl->residentBufferBytes = 0;
}

size_t D3D12W3dModel::meshCount() const noexcept {
    return m_impl && !m_impl->isRetired ? m_impl->meshes.size() : 0;
}

size_t D3D12W3dModel::skinnedMeshCount() const noexcept {
    return m_impl && !m_impl->isRetired ? m_impl->skinnedMeshCount : 0;
}

size_t D3D12W3dModel::primitiveCount() const noexcept {
    return m_impl && !m_impl->isRetired ? m_impl->primitiveCount : 0;
}

uint64_t D3D12W3dModel::residentBytes() const noexcept {
    return m_impl && !m_impl->isRetired
        ? m_impl->residentBufferBytes : 0u;
}

uint64_t D3D12W3dModel::lastUsedFrame() const noexcept {
    return m_impl ? m_impl->lastUsedFrame : 0u;
}

W3dGpuUseDiagnostic D3D12W3dModel::useDiagnostic() const noexcept {
    if (!m_impl || !m_impl->device || m_impl->lastUsedFrame == 0u) return {};
    const d3d12::FrameFenceStatus status =
        m_impl->device->frameFenceStatus(m_impl->lastUsedFrame);
    return {
        .frame = m_impl->lastUsedFrame,
        .fence = status.fenceValue,
        .completed = status.completed,
        .exactFence = status.exact,
    };
}

uint64_t D3D12W3dModel::retainedSortingBytes() const noexcept {
    return residentBytes();
}

bool D3D12W3dModel::retired() const noexcept {
    return !m_impl || m_impl->isRetired;
}

container::Span<const W3dMaterialTextureBinding>
D3D12W3dModel::materialTextureBindings() const noexcept {
    return m_impl && !m_impl->isRetired
        ? container::Span<const W3dMaterialTextureBinding>(
              m_impl->materialTextureBindings)
        : container::Span<const W3dMaterialTextureBinding>{};
}

void W3dRestPaletteFrameCache::clear() noexcept {
    m_entries.clear();
    m_stats = {};
}

container::Span<const math::transform> W3dRestPaletteFrameCache::resolve(
    container::SharedPtr<const Skeleton> skeleton,
    const math::transform& world) {
    if (!skeleton || skeleton->empty()) return {};
    const auto existing = std::find_if(
        m_entries.begin(), m_entries.end(),
        [&skeleton, &world](const Entry& entry) {
            return entry.skeleton == skeleton && entry.world == world;
        });
    if (existing != m_entries.end()) {
        if (m_stats.palettesReused != std::numeric_limits<uint32_t>::max()) {
            ++m_stats.palettesReused;
        }
        return existing->palette;
    }

    Entry entry{
        .skeleton = std::move(skeleton),
        .world = world,
        .palette = {},
    };
    const container::Span<const math::transform> modelRest =
        entry.skeleton->modelRestPose();
    entry.palette.assign(modelRest.begin(), modelRest.end());
    for (math::transform& bone : entry.palette) bone *= world;
    m_stats.jointsMaterialized += entry.palette.size();
    if (m_stats.palettesBuilt != std::numeric_limits<uint32_t>::max()) {
        ++m_stats.palettesBuilt;
    }
    m_entries.push_back(std::move(entry));
    return m_entries.back().palette;
}

size_t appendW3dModelGraphDrawPackets(
    W3dAssetCache& assets, W3dModelHandle rootHandle,
    const math::transform& entityWorld,
    container::Span<const math::transform> skinPalette,
    container::Span<const uint8_t> boneVisibility,
    container::Vector<StaticMeshDrawPacket>& output,
    const W3dModelGraphDrawOptions& options,
    W3dModelGraphTraversalStats* traversalStats) {
    constexpr size_t kMaximumAdditionalModelDepth = 16;
    const size_t packetStart = output.size();
    container::Vector<W3dModelHandle> activePath;
    activePath.reserve(kMaximumAdditionalModelDepth + 1u);
    const auto appendNode = [&](auto&& self, W3dModelHandle handle,
                                const math::transform& world,
                                container::Span<const math::transform> palette,
                                container::Span<const uint8_t> visibility,
                                size_t depth) -> void {
        if (traversalStats) ++traversalStats->requestedNodes;
        if (!handle) {
            if (traversalStats && depth != 0u) {
                ++traversalStats->missingChildren;
            }
            return;
        }
        if (depth > kMaximumAdditionalModelDepth) {
            if (traversalStats) ++traversalStats->depthRejected;
            return;
        }
        if (std::find(activePath.begin(), activePath.end(), handle) !=
            activePath.end()) {
            if (traversalStats) ++traversalStats->cycleRejected;
            return;
        }
        const std::optional<W3dAssetState> state = assets.state(handle);
        if (!state) {
            if (traversalStats && depth != 0u) {
                ++traversalStats->missingChildren;
            }
            return;
        }
        if (*state == W3dAssetState::CpuReady) {
            if (traversalStats) ++traversalStats->pendingNodes;
            assets.queueGpuUpload(handle, RenderAssetPriority::Visible);
            return;
        }
        if (*state == W3dAssetState::GpuUploadFailed) {
            if (traversalStats) ++traversalStats->pendingNodes;
            assets.queueGpuUpload(handle, RenderAssetPriority::Visible);
            return;
        }
        if (*state != W3dAssetState::GpuReady) {
            if (traversalStats) ++traversalStats->pendingNodes;
            return;
        }

        const container::SharedPtr<const CpuStaticModel> cpu =
            assets.cpuModel(handle);
        const auto gpu = std::dynamic_pointer_cast<const D3D12W3dModel>(
            assets.gpuModel(handle));
        if (!cpu || !gpu || gpu->retired()) {
            if (traversalStats) ++traversalStats->pendingNodes;
            return;
        }
        if (traversalStats) ++traversalStats->readyNodes;

        activePath.push_back(handle);
        container::Span<const math::transform> effectivePalette = palette;
        if (effectivePalette.empty() && cpu->skeleton &&
            !cpu->skeleton->empty() && options.restPalettes) {
            effectivePalette = options.restPalettes->resolve(
                cpu->skeleton, world);
        }
        gpu->appendDrawPackets(
            world, effectivePalette, visibility, output,
            options.visualTimeSeconds, options.directionalLightScale,
            options.materialTextureOverrides, options.scriptFlashTint,
            options.heatVisionIntensity, options.heatVisionOnly,
            options.objectOpacity, options.scriptIndicatorColor,
            options.hasScriptIndicatorColor, options.receivesDynamicLights,
            options.subObjectVisibility, options.treePushAsideDirection,
            options.treePushAsideAmount,
            options.treePushAsideDistanceFactor,
            options.treePushAsideDarkeningFactor, options.vehicleTreads,
            depth == 0u ? options.previousEntityWorld : nullptr,
            depth == 0u
                ? options.previousSkinPalette
                : container::Span<const math::transform>{},
            depth == 0u ? options.interpolationAlpha : 1.0f);

        const container::Span<const math::transform> attachmentPalette =
            effectivePalette;
        const container::Vector<W3dAdditionalModelInstance> children =
            resolveW3dAdditionalModelInstances(
                *cpu, world, attachmentPalette, visibility);
        for (const W3dAdditionalModelInstance& childReference : children) {
            const math::transform& childWorld = childReference.worldTransform;
            const W3dModelHandle childHandle = assets.requestAsync(
                childReference.modelName, false,
                RenderAssetPriority::Visible);
            if (!childHandle) {
                if (traversalStats) ++traversalStats->missingChildren;
                continue;
            }

            container::Span<const math::transform> childPalette{};
            const container::SharedPtr<const CpuStaticModel> childCpu =
                assets.cpuModel(childHandle);
            if (childCpu && childCpu->skeleton &&
                !childCpu->skeleton->empty() &&
                options.restPalettes) {
                childPalette = options.restPalettes->resolve(
                    childCpu->skeleton, childWorld);
            }
            self(self, childHandle, childWorld, childPalette, {}, depth + 1u);
        }
        activePath.pop_back();
    };

    appendNode(appendNode, rootHandle, entityWorld, skinPalette,
               boneVisibility, 0u);
    return output.size() - packetStart;
}

} // namespace engine::render
