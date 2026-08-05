#pragma once

#include "core/container/container_types.h"

#include "presentation/render/RenderWorldDescriptorContracts.h"
#include "engine/renderer/world/model/StaticTextureMapper.h"
#include "core/math/wwmath/base/wwmath.h"

#include <cstdint>
#include <optional>
namespace data::w3d { struct ParsedW3D; }

namespace engine::render {

class Skeleton;
class AnimationClip;

// Immutable CPU representation used between the packed W3D file format and
// backend-specific GPU resources. Packed W3D structs never escape into draw
// submission, and DirectXMath native types never enter this contract.
struct StaticMeshVertex {
    math::vec3 position{};
    math::vec3 normal{0.0f, 0.0f, 1.0f};
    math::vec2 texcoord{};
    math::vec2 detailTexcoord{};
    math::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    uint32_t boneIndex = UINT32_MAX;
};

struct StaticMaterialDesc {
    container::String name;
    container::String textureName;
    container::String detailTextureName;
    math::vec3 ambient{0.25f, 0.25f, 0.25f};
    math::vec4 diffuse{1.0f, 1.0f, 1.0f, 1.0f};
    math::vec3 specular{};
    math::vec3 emissive{};
    float shininess = 0.0f;
    uint32_t sourceVertexMaterialId = UINT32_MAX;
    uint32_t sourceShaderId = UINT32_MAX;
    uint32_t sourceTextureId = UINT32_MAX;
    uint32_t sourceDetailTextureId = UINT32_MAX;
    uint32_t materialPass = 0;
    StaticTextureMapperStages textureMappers{};
    bool twoSided = false;
    bool alphaTest = false;
    bool alphaTestInverted = false;
    // Encodes W3DTEXTURE_CLAMP_U/V as a compact shader sampler selection:
    // 0=wrap, 1=clamp U, 2=clamp V, 3=clamp UV.
    uint8_t samplerMode = 0;
    uint8_t detailSamplerMode = 0;
    uint8_t detailColorFunc = 0;
    uint8_t detailAlphaFunc = 0;
    // Preserve the file value for tooling/Mod diagnostics. Generals'
    // Convert_Shader path resolves ordinary mesh fog to disabled regardless
    // of this raw W3D field.
    uint8_t rawFogFunc = 0;
    uint8_t fogFunc = 0;
    // W3D's secondary-gradient shader bit is the authoritative specular
    // enable.  A vertex material may contain non-zero specular/shininess
    // data while this flag is off; RefCode then disables D3DRS_SPECULARENABLE
    // for that draw instead of treating the material values as opt-in.
    bool secondaryGradientEnabled = false;
    // W3DSHADER_PRIGRADIENT_DISABLE selects the texture directly instead of
    // modulating it by vertex/material/light diffuse.
    bool primaryGradientDisabled = false;
    bool texturingEnabled = true;
    // RefCode decides this at mesh installation time: PRELIT_VERTEX geometry
    // disables real-time lighting, while ordinary W3D mesh geometry enables
    // it. Keep the resolved, per-material value explicit across the CPU/GPU
    // boundary instead of inferring it in backend code.
    bool lightingEnabled = false;
    bool hasDetailTexture = false;
    // Prelit lightmap wrappers give their baked texture a semantic role that
    // is not interchangeable with an ordinary translucent/detail texture.
    // Multi-pass assets put the lightmap in the final pass' base stage;
    // multi-texture assets put it in the final texture stage.
    bool baseTextureIsLightmap = false;
    bool detailTextureIsLightmap = false;
    uint8_t sourceBlend = 1; // W3DSHADER_SRCBLENDFUNC_ONE
    uint8_t destinationBlend = 0; // W3DSHADER_DESTBLENDFUNC_ZERO
    uint8_t depthCompare = 3; // W3DSHADER_DEPTHCOMPARE_PASS_LEQUAL
    bool depthWrite = true;
};

struct StaticPrimitive {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t materialIndex = 0;
};

struct CpuStaticMesh {
    container::String name;
    uint32_t sourceBoneIndex = UINT32_MAX;
    bool skinned = false;
    // MESH_FLAG_HIDDEN is an initial visibility state, not an instruction to
    // discard geometry. A later ShowSubObject override can reveal this mesh.
    bool initiallyHidden = false;
    // W3D_MESH_FLAG_CAST_SHADOW is a force flag for otherwise translucent
    // geometry. Opaque packet casting is the default renderer policy.
    bool forceShadow = false;
    // Preserved MeshHeader3 static sort bin. Zero retains ordinary
    // camera/triangle sorting; positive levels use stable static-bin order.
    int32_t sortLevel = 0;
    math::transform localTransform{};
    math::aabb localBounds{};
    container::Vector<StaticMeshVertex> vertices;
    container::Vector<uint32_t> indices;
    container::Vector<StaticPrimitive> primitives;
};

struct CpuStaticModel {
    struct HierarchyModelReference final {
        container::String modelName;
        uint32_t boneIndex = 0;
    };
    struct HierarchyProxyReference final {
        container::String proxyName;
        uint32_t boneIndex = 0;
    };

    container::String name;
    math::aabb bounds{};
    container::Vector<StaticMaterialDesc> materials;
    container::Vector<CpuStaticMesh> meshes;
    // Preserved from the selected HLOD definition. Additional models are
    // render children attached to hierarchy bones. Proxy references are
    // application-defined data anchors and are never rendered implicitly.
    container::Vector<HierarchyModelReference> additionalModels;
    container::Vector<HierarchyProxyReference> proxies;
    container::SharedPtr<const Skeleton> skeleton;
    struct Animation {
        container::String name;
        container::SharedPtr<const AnimationClip> clip;
    };
    container::Vector<Animation> animations;
    container::String hierarchyName;
    bool externalHierarchyResolved = false;
    bool skeletonFallback = false;
    uint32_t invalidInfluenceCount = 0;
    uint32_t invalidSubObjectBoneCount = 0;
    uint32_t invalidHierarchyParentCount = 0;
    uint32_t hierarchyCycleNodeCount = 0;
    uint32_t hierarchyConflictCount = 0;
    container::Vector<container::String> diagnostics;

    [[nodiscard]] bool empty() const noexcept { return meshes.empty(); }
};

struct W3dAdditionalModelInstance final {
    container::StringView modelName;
    math::transform worldTransform{};
    uint32_t boneIndex = 0;
};

// Resolves HLOD AdditionalModels against the same sealed world-space palette
// used by the parent draw. Missing bones fall back to the parent root like the
// legacy attachment path; hidden hierarchy pivots suppress their child.
[[nodiscard]] container::Vector<W3dAdditionalModelInstance>
resolveW3dAdditionalModelInstances(
    const CpuStaticModel& model, const math::transform& entityWorld,
    container::Span<const math::transform> boneWorldTransforms,
    container::Span<const uint8_t> boneVisibility = {});

// Each retained primitive becomes one StaticMeshDrawPacket after a successful
// backend upload (assuming a required skin palette is available). This helper
// gives backend-neutral probes the exact initiallyHidden plus subobject
// override policy used by D3D12W3dModel before GPU packet construction.
[[nodiscard]] size_t countSubObjectVisibleDrawPackets(
    const CpuStaticModel& model,
    container::Span<const RenderSubObjectVisibility> overrides = {}) noexcept;

// RefCode applies ShowSubObject/HideSubObject to the named subobject and to
// every subobject attached below its hierarchy pivot. Keep that relationship
// backend-neutral so probes and GPU upload use one rule.
[[nodiscard]] bool subObjectVisibilityAffectsMesh(
    const CpuStaticModel& model, size_t controllerMeshIndex,
    size_t targetMeshIndex) noexcept;
[[nodiscard]] std::optional<bool> recursiveSubObjectVisibilityOverride(
    const CpuStaticModel& model, size_t targetMeshIndex,
    container::Span<const RenderSubObjectVisibility> overrides) noexcept;

struct W3dStaticModelBuildOptions {
    // Prototype/root name without a path or extension. Asset caches should set
    // this from the requested W3D name so multi-prototype files do not merge.
    container::String requestedPrototype;
    bool includeHiddenMeshes = true;
    bool includeCollisionMeshes = false;
    // Optional dependency file resolved by W3dAssetCache from the HLOD's
    // hierarchy name. The builder never performs VFS I/O itself.
    const data::w3d::ParsedW3D* externalHierarchySource = nullptr;
};

class W3dStaticModelBuilder final {
public:
    [[nodiscard]] static container::String requiredHierarchyName(
        const data::w3d::ParsedW3D& parsed,
        const W3dStaticModelBuildOptions& options = {});
    [[nodiscard]] static std::optional<CpuStaticModel> build(
        const data::w3d::ParsedW3D& parsed,
        const W3dStaticModelBuildOptions& options = {},
        container::String* error = nullptr);
};

} // namespace engine::render
