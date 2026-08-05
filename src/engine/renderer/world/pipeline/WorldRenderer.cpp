#include "core/container/container_types.h"
#include "engine/renderer/world/pipeline/WorldRenderer.h"
#include "engine/renderer/world/pipeline/WorldRendererGpuLayout.h"
#include "engine/renderer/world/pipeline/WorldRendererPipelineStateContract.h"
#include "engine/renderer/world/pipeline/WorldRendererUploadOwner.h"
#include "engine/renderer/d3d12/runtime/D3D12QualitySettings.h"
#include "engine/renderer/d3d12/runtime/D3D12ShaderPackage.h"

#include "engine/renderer/world/model/W3dStaticModel.h"
#include "engine/renderer/d3d12/runtime/D3D12Device.h"
#include "debug/debug.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

#ifndef TD_WORLD_MAIN_SHADER_PACKAGE_VERSION
#define TD_WORLD_MAIN_SHADER_PACKAGE_VERSION 1
#endif
#ifndef TD_WORLD_MAIN_SHADER_SOURCE_SHA256
#define TD_WORLD_MAIN_SHADER_SOURCE_SHA256 \
    "0000000000000000000000000000000000000000000000000000000000000000"
#endif
#ifndef TD_WORLD_DIRECTIONAL_SHADOW_SHADER_PACKAGE_VERSION
#define TD_WORLD_DIRECTIONAL_SHADOW_SHADER_PACKAGE_VERSION 1
#endif
#ifndef TD_WORLD_DIRECTIONAL_SHADOW_SHADER_SOURCE_SHA256
#define TD_WORLD_DIRECTIONAL_SHADOW_SHADER_SOURCE_SHA256 \
    "0000000000000000000000000000000000000000000000000000000000000000"
#endif
namespace engine::render {

float worldDynamicPointLightAttenuation(
    float distanceToLight, float innerRadius, float outerRadius) noexcept {
    if (!std::isfinite(distanceToLight) || !std::isfinite(outerRadius) ||
        outerRadius <= 0.0f) {
        return 0.0f;
    }
    const float outer = std::max(outerRadius, 0.0f);
    const float inner = std::clamp(
        std::isfinite(innerRadius) ? innerRadius : 0.0f, 0.0f, outer);
    const float distance = std::max(distanceToLight, 0.0f);
    if (distance >= outer) return 0.0f;
    if (distance <= inner) return 1.0f;
    return (outer - distance) / (outer - inner);
}

ObjectDynamicLightEnvironment selectObjectPointLights(
    math::vec3 objectCenter,
    container::Span<const DynamicPointLightRenderData> dynamicLights,
    container::Span<const TerrainPointLightRenderData> sceneLights,
    bool includeDynamicLights,
    bool includeSceneLights) noexcept {
    ObjectDynamicLightEnvironment output;
    container::Array<float,
                     dynamic_lights::performance_limits::kObjectDiffuseMaximumLights>
        contributions{};
    if (!std::isfinite(objectCenter.x()) ||
        !std::isfinite(objectCenter.y()) ||
        !std::isfinite(objectCenter.z())) {
        return output;
    }
    const auto admit = [&](math::vec3 position, math::vec3 ambientColor,
                           math::vec3 diffuseColor, float innerRadius,
                           float outerRadius) {
        if (!std::isfinite(position.x()) || !std::isfinite(position.y()) ||
            !std::isfinite(position.z()) ||
            !std::isfinite(ambientColor.x()) ||
            !std::isfinite(ambientColor.y()) ||
            !std::isfinite(ambientColor.z()) ||
            !std::isfinite(diffuseColor.x()) ||
            !std::isfinite(diffuseColor.y()) ||
            !std::isfinite(diffuseColor.z()) ||
            !std::isfinite(outerRadius) || outerRadius <= 0.0f) {
            return;
        }
        ambientColor = {
            std::max(ambientColor.x(), 0.0f),
            std::max(ambientColor.y(), 0.0f),
            std::max(ambientColor.z(), 0.0f),
        };
        diffuseColor = {
            std::max(diffuseColor.x(), 0.0f),
            std::max(diffuseColor.y(), 0.0f),
            std::max(diffuseColor.z(), 0.0f),
        };
        if (ambientColor.x() < 0.05f && ambientColor.y() < 0.05f &&
            ambientColor.z() < 0.05f && diffuseColor.x() < 0.05f &&
            diffuseColor.y() < 0.05f && diffuseColor.z() < 0.05f) {
            return;
        }
        const float distance = (position - objectCenter).length();
        const float attenuation = worldDynamicPointLightAttenuation(
            distance, innerRadius, outerRadius);
        if (attenuation <= 0.0f) return;
        output.ambient += ambientColor * attenuation;
        const math::vec3 attenuatedDiffuse = diffuseColor * attenuation;
        const float contribution = attenuatedDiffuse.length_sq();
        if (contribution <= 0.0f) return;

        uint32_t insertion = output.diffuseLightCount;
        if (insertion < output.diffuseLights.size()) {
            ++output.diffuseLightCount;
        } else if (contribution <= contributions.back()) {
            return;
        } else {
            insertion = static_cast<uint32_t>(output.diffuseLights.size() - 1u);
        }
        while (insertion > 0u &&
               contribution > contributions[insertion - 1u]) {
            if (insertion < output.diffuseLights.size()) {
                output.diffuseLights[insertion] =
                    output.diffuseLights[insertion - 1u];
                contributions[insertion] = contributions[insertion - 1u];
            }
            --insertion;
        }
        output.diffuseLights[insertion] = {
            .position = position,
            .color = diffuseColor,
            .innerRadius = innerRadius,
            .outerRadius = outerRadius,
        };
        contributions[insertion] = contribution;
    };
    if (includeSceneLights) {
        for (const TerrainPointLightRenderData& light : sceneLights) {
            admit(light.position, light.ambient, light.diffuse,
                  light.innerRadius, light.outerRadius);
        }
    }
    if (includeDynamicLights) {
        for (const DynamicPointLightRenderData& light : dynamicLights) {
            // LightPulse configures identical ambient and diffuse colours;
            // W3DPoliceCarDraw deliberately uses half-strength ambient.
            admit(light.position,
                  light.hasSeparateAmbientColor
                      ? light.ambientColor : light.color,
                  light.color,
                  light.innerRadius, light.outerRadius);
        }
    }
    return output;
}

ObjectDynamicLightEnvironment selectObjectDynamicLights(
    math::vec3 objectCenter,
    container::Span<const DynamicPointLightRenderData> lights) noexcept {
    return selectObjectPointLights(objectCenter, lights, {}, true, false);
}

namespace {

struct DebugVertex {
    float position[3];
    float normal[3];
    float texcoord[2];
    float detailTexcoord[2]{};
    float color[4]{1.0f, 1.0f, 1.0f, 1.0f};
    uint32_t boneIndex = UINT32_MAX;
};
static_assert(std::is_standard_layout_v<StaticMeshVertex>);
static_assert(offsetof(StaticMeshVertex, position) == 0);
static_assert(offsetof(StaticMeshVertex, normal) == 12);
static_assert(offsetof(StaticMeshVertex, texcoord) == 24);
static_assert(offsetof(StaticMeshVertex, detailTexcoord) == 32);
static_assert(offsetof(StaticMeshVertex, color) == 40);
static_assert(offsetof(StaticMeshVertex, boneIndex) == 56);
static_assert(sizeof(StaticMeshVertex) == 60);
static_assert(sizeof(DebugVertex) == sizeof(StaticMeshVertex));

constexpr container::Array<DebugVertex, 24> kCubeVertices = {{
    {{ 1.0f, -1.0f, -1.0f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 1.0f}},
    {{ 1.0f, -1.0f,  1.0f}, { 1.0f,  0.0f,  0.0f}, {1.0f, 1.0f}},
    {{ 1.0f,  1.0f,  1.0f}, { 1.0f,  0.0f,  0.0f}, {1.0f, 0.0f}},
    {{ 1.0f,  1.0f, -1.0f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 0.0f}},
    {{-1.0f, -1.0f,  1.0f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 1.0f}},
    {{-1.0f, -1.0f, -1.0f}, {-1.0f,  0.0f,  0.0f}, {1.0f, 1.0f}},
    {{-1.0f,  1.0f, -1.0f}, {-1.0f,  0.0f,  0.0f}, {1.0f, 0.0f}},
    {{-1.0f,  1.0f,  1.0f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 0.0f}},
    {{-1.0f,  1.0f, -1.0f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 1.0f}},
    {{ 1.0f,  1.0f, -1.0f}, { 0.0f,  1.0f,  0.0f}, {1.0f, 1.0f}},
    {{ 1.0f,  1.0f,  1.0f}, { 0.0f,  1.0f,  0.0f}, {1.0f, 0.0f}},
    {{-1.0f,  1.0f,  1.0f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 0.0f}},
    {{-1.0f, -1.0f,  1.0f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 1.0f}},
    {{ 1.0f, -1.0f,  1.0f}, { 0.0f, -1.0f,  0.0f}, {1.0f, 1.0f}},
    {{ 1.0f, -1.0f, -1.0f}, { 0.0f, -1.0f,  0.0f}, {1.0f, 0.0f}},
    {{-1.0f, -1.0f, -1.0f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 0.0f}},
    {{ 1.0f, -1.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 1.0f}},
    {{-1.0f, -1.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}, {1.0f, 1.0f}},
    {{-1.0f,  1.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}, {1.0f, 0.0f}},
    {{ 1.0f,  1.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 0.0f}},
    {{-1.0f, -1.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 1.0f}},
    {{ 1.0f, -1.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}, {1.0f, 1.0f}},
    {{ 1.0f,  1.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}, {1.0f, 0.0f}},
    {{-1.0f,  1.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 0.0f}},
}};

constexpr container::Array<uint32_t, 36> kCubeIndices = {{
    0, 2, 1,  0, 3, 2,
    4, 6, 5,  4, 7, 6,
    8, 10, 9, 8, 11, 10,
    12, 14, 13, 12, 15, 14,
    16, 18, 17, 16, 19, 18,
    20, 22, 21, 20, 23, 22,
}};

// Main/shadow PSO creation must consume the same instance-stream ABI as the
// command-recording TUs. The private layout contract carries the offsets.
using world_renderer_detail::StaticMeshGpuInstance;

// Both embedded world shaders declare skinBones[256]. Keep a compile-time
// tripwire beside the CPU-side limit so changing only one side cannot silently
// alter the palette contract.
static_assert(world_renderer_detail::kMaximumSkinBones == 256);
static_assert(sizeof(math::transform) == sizeof(float) * 16);

// GlobalData's default legacy View shake strengths.  The script action shakes
// the tactical view at its own current position, so its distance attenuation
// is always 1.0 here.  The old code has a historical clamp quirk: it tests
// configured max 10 but assigns 3 when exceeded; retain that visible rule.

D3D12_HEAP_PROPERTIES makeUploadHeapProperties() {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    return properties;
}

D3D12_HEAP_PROPERTIES makeDefaultHeapProperties() {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    return properties;
}

D3D12_RESOURCE_DESC makeBufferDescription(UINT64 size) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return description;
}

bool isFiniteVector(const math::vec3& value) noexcept {
    return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
}

math::vec3 finiteVectorOrZero(const math::vec3& value) noexcept {
    return isFiniteVector(value) ? value : math::vec3{};
}

} // namespace


WorldLightEnvironment WorldLightEnvironment::fromTerrainGlobalLighting(
    const TerrainGlobalLightingRenderData& globalLighting) noexcept {
    // A present-but-malformed lighting chunk must not silently fall back to
    // the diagnostic sun.  Start from a black map environment and retain
    // only finite source values, so the failure stays local and visible.
    WorldLightEnvironment environment{};
    environment.ambient = {};
    for (WorldDirectionalLight& light : environment.directionalLights) {
        light.directionToLight = {0.0f, 0.0f, 1.0f};
        light.diffuse = {};
    }

    const auto& sourceLights = globalLighting.objectLights[globalLighting.terrainLightSlot()];
    // W3DDisplay applies only the primary object light's ambient as the
    // scene ambient; fill-light ambient values are explicitly zeroed by the
    // legacy light environment before its three directional lights are set.
    environment.ambient = finiteVectorOrZero(sourceLights.front().ambient);
    for (size_t index = 0; index < sourceLights.size(); ++index) {
        const TerrainLightingRenderData& source = sourceLights[index];
        WorldDirectionalLight& target = environment.directionalLights[index];
        target.diffuse = finiteVectorOrZero(source.diffuse);
        const float lengthSquared = source.direction.length_sq();
        if (std::isfinite(lengthSquared) && lengthSquared > 0.000001f) {
            // Map data stores RefCode's lightPos/ray direction. The lighting
            // equation consumes the direction from shaded surface to light.
            target.directionToLight = (-source.direction) / std::sqrt(lengthSquared);
        }
    }
    return environment;
}

WorldRenderer::WorldRenderer(d3d12::D3D12Device& device) noexcept
    : m_device(&device) {}

WorldRenderer::~WorldRenderer() {
    shutdown();
}

bool WorldRenderer::loadShaderPackages() {
    for (auto& bytecode : m_shaderBytecode) bytecode.clear();
    const auto loadPair = [this](
        container::StringView packageName, uint32_t packageVersion,
        container::StringView sourceSha256,
        container::StringView vertexFile,
        container::StringView pixelFile,
        ShaderBytecode vertexSlot,
        ShaderBytecode pixelSlot) {
        const container::Array<d3d12::ShaderPackageEntrySpec, 2> entries{{
            {"vertex_file", vertexFile, "vertex_profile", "vs_5_0"},
            {"pixel_file", pixelFile, "pixel_profile", "ps_5_0"},
        }};
        container::Vector<container::Vector<uint8_t>> loaded;
        if (!d3d12::loadShaderPackage(
                packageName, std::to_string(packageVersion), sourceSha256,
                {entries.data(), entries.size()}, loaded) ||
            loaded.size() != entries.size()) {
            TD_LOG_ERROR(
                "[WorldRenderer] precompiled '{}' shader package unavailable",
                packageName);
            return false;
        }
        m_shaderBytecode[static_cast<size_t>(vertexSlot)] =
            std::move(loaded[0]);
        m_shaderBytecode[static_cast<size_t>(pixelSlot)] =
            std::move(loaded[1]);
        return true;
    };

    return loadPair(
               "world_main", TD_WORLD_MAIN_SHADER_PACKAGE_VERSION,
               TD_WORLD_MAIN_SHADER_SOURCE_SHA256,
               "world_main_vs.cso", "world_main_ps.cso",
               ShaderBytecode::WorldVertex, ShaderBytecode::WorldPixel) &&
        loadPair(
               "world_directional_shadow",
               TD_WORLD_DIRECTIONAL_SHADOW_SHADER_PACKAGE_VERSION,
               TD_WORLD_DIRECTIONAL_SHADOW_SHADER_SOURCE_SHA256,
               "world_directional_shadow_vs.cso",
               "world_directional_shadow_ps.cso",
               ShaderBytecode::DirectionalShadowVertex,
               ShaderBytecode::DirectionalShadowPixel);
}

bool WorldRenderer::init(d3d12::D3D12Device& device) {
    shutdown();
    m_device = &device;
    return init();
}

bool WorldRenderer::init() {
    if (m_initialized) return true;
    if (!m_device || !m_device->getDevice()) {
        TD_LOG_ERROR("[WorldRenderer] Cannot initialize without a live D3D12 device");
        return false;
    }

    if (!loadShaderPackages() || !createRootSignature() ||
        !createPipelineStates() || !m_postProcessRenderer.init(*m_device) ||
        !createDebugCube()) {
        shutdown();
        return false;
    }

    m_directionalShadowAvailable =
        createDirectionalShadowResources() &&
        createDirectionalShadowPipelineStates();
    if (!m_directionalShadowAvailable) {
        releaseDirectionalShadowResources();
        TD_LOG_WARN(
            "[WorldRenderer] Directional shadow map unavailable; projected shadows remain active");
    }
    // The ordinary world root signature always exposes t2. Keep a
    // type-correct null R32_FLOAT descriptor bound when the optional shadow
    // map is unavailable or a frame has no valid casters; slot zero is an
    // RGBA UI fallback and is not a depth-comparison contract.
    if (!createDirectionalShadowFallbackSrv()) {
        shutdown();
        return false;
    }
    if (!createLocalVisibilityFallbackSrv()) {
        shutdown();
        return false;
    }

    m_initialized = true;
    TD_LOG_INFO("[WorldRenderer] Static world pass initialized");
    return true;
}

bool WorldRenderer::configureTextureSampling(
    uint32_t textureFilter, uint32_t anisotropyLevel,
    uint32_t sampleCount) {
    const d3d12::TextureSamplingQuality current =
        d3d12::textureSamplingQuality(
            m_textureFilter, m_anisotropyLevel);
    const d3d12::TextureSamplingQuality requested =
        d3d12::textureSamplingQuality(textureFilter, anisotropyLevel);
    if (current.filter == requested.filter &&
        current.maximumAnisotropy == requested.maximumAnisotropy &&
        current.maximumLod == requested.maximumLod &&
        m_sampleCount == sampleCount) {
        m_textureFilter = textureFilter;
        m_anisotropyLevel = anisotropyLevel;
        m_sampleCount = sampleCount;
        return true;
    }
    if (!m_initialized) {
        m_textureFilter = textureFilter;
        m_anisotropyLevel = anisotropyLevel;
        m_sampleCount = sampleCount;
        return true;
    }
    if (!m_device || !m_device->waitIdle()) {
        TD_LOG_ERROR(
            "[WorldRenderer] sampling reconfiguration could not synchronize the GPU");
        return false;
    }

    m_textureFilter = textureFilter;
    m_anisotropyLevel = anisotropyLevel;
    m_sampleCount = sampleCount;
    m_skeletonPipelineState.Reset();
    m_shadowOpaquePipelineState.backFaceCulled.Reset();
    m_shadowOpaquePipelineState.twoSided.Reset();
    m_shadowAlphaTestPipelineState.backFaceCulled.Reset();
    m_shadowAlphaTestPipelineState.twoSided.Reset();
    for (auto& blendPipelines : m_pipelineStates) {
        for (auto& depthWritePipelines : blendPipelines) {
            for (auto& pipelines : depthWritePipelines) {
                pipelines.backFaceCulled.Reset();
                pipelines.frontFaceCulled.Reset();
                pipelines.twoSided.Reset();
            }
        }
    }
    m_rootSignature.Reset();
    if (!createRootSignature() || !createPipelineStates()) {
        TD_LOG_ERROR(
            "[WorldRenderer] sampling-dependent root signature/PSO rebuild failed");
        m_initialized = false;
        return false;
    }
    if (m_directionalShadowAvailable &&
        !createDirectionalShadowPipelineStates()) {
        m_directionalShadowAvailable = false;
        m_directionalShadowValid = false;
        TD_LOG_WARN(
            "[WorldRenderer] directional shadow PSO rebuild failed after sampling change");
    }
    TD_LOG_INFO(
        "[WorldRenderer] texture sampling configured: filter={} anisotropy={}x",
        textureFilter, requested.maximumAnisotropy);
    return true;
}

void WorldRenderer::shutdown() {
    m_postProcessRenderer.shutdown();
    releaseLocalVisibilityResources();
    releaseWaterReflectionResources();
    releaseDirectionalShadowResources();
    m_cubeIndexBuffer.Reset();
    m_cubeVertexBuffer.Reset();
    m_skeletonPipelineState.Reset();
    for (auto& blendPipelines : m_pipelineStates) {
        for (auto& depthWritePipelines : blendPipelines) {
            for (auto& pipelines : depthWritePipelines) {
                pipelines.backFaceCulled.Reset();
                pipelines.frontFaceCulled.Reset();
                pipelines.twoSided.Reset();
            }
        }
    }
    m_rootSignature.Reset();
    for (auto& bytecode : m_shaderBytecode) bytecode.clear();
    m_cubeVertexBufferView = {};
    m_cubeIndexBufferView = {};
    m_scriptScreenShake = {};
    m_scriptCameraSlave = {};
    m_lastStaticMeshStats = {};
    m_initialized = false;
}

void WorldRenderer::resetPresentationEpoch(
    uint64_t presentationEpoch) noexcept {
    m_directionalShadowValid = false;
    m_waterReflectionValid = false;
    m_localVisibilityPresentationEpoch = presentationEpoch;
    m_localVisibilityRevision = 0;
    m_localVisibilityPolicyRevision = 0;
    m_localVisibilityLayoutRevision = 0;
    m_localVisibilityObserverPlayer = UINT8_MAX;
    m_localVisibilityEnabled = false;

    m_scriptScreenShake = {};
    m_scriptScreenShake.presentationEpoch = presentationEpoch;
    m_scriptCameraSlave = {};
    m_scriptCameraSlave.presentationEpoch = presentationEpoch;
    m_postProcessRenderer.resetPresentationEpoch(presentationEpoch);
    m_lastStaticMeshStats = {};
}

bool WorldRenderer::createRootSignature() {
    D3D12_DESCRIPTOR_RANGE textureRanges[7]{};
    for (uint32_t index = 0; index < std::size(textureRanges); ++index) {
        textureRanges[index].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        textureRanges[index].NumDescriptors = 1;
        textureRanges[index].BaseShaderRegister = index;
        textureRanges[index].RegisterSpace = 0;
        textureRanges[index].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    }

    D3D12_ROOT_PARAMETER parameters[10]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[1].Descriptor.ShaderRegister = 1;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[2].Descriptor.ShaderRegister = 2;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[3].DescriptorTable.NumDescriptorRanges = 1;
    parameters[3].DescriptorTable.pDescriptorRanges = &textureRanges[0];
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[4].DescriptorTable.NumDescriptorRanges = 1;
    parameters[4].DescriptorTable.pDescriptorRanges = &textureRanges[1];
    parameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[5].DescriptorTable.NumDescriptorRanges = 1;
    parameters[5].DescriptorTable.pDescriptorRanges = &textureRanges[2];
    parameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[6].DescriptorTable.NumDescriptorRanges = 1;
    parameters[6].DescriptorTable.pDescriptorRanges = &textureRanges[3];
    parameters[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[7].DescriptorTable.NumDescriptorRanges = 1;
    parameters[7].DescriptorTable.pDescriptorRanges = &textureRanges[4];
    parameters[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[8].DescriptorTable.NumDescriptorRanges = 1;
    parameters[8].DescriptorTable.pDescriptorRanges = &textureRanges[5];
    parameters[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[9].DescriptorTable.NumDescriptorRanges = 1;
    parameters[9].DescriptorTable.pDescriptorRanges = &textureRanges[6];
    parameters[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    container::Array<D3D12_STATIC_SAMPLER_DESC, 7> samplers{};
    const d3d12::TextureSamplingQuality sampling =
        d3d12::textureSamplingQuality(
            m_textureFilter, m_anisotropyLevel);
    for (uint32_t index = 0; index < 4; ++index) {
        auto& sampler = samplers[index];
        sampler.Filter = sampling.filter;
        sampler.AddressU = (index & 1) != 0 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressV = (index & 2) != 0 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MipLODBias = 0.0f;
        sampler.MaxAnisotropy = sampling.maximumAnisotropy;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        sampler.MinLOD = 0.0f;
        sampler.MaxLOD = sampling.maximumLod;
        sampler.ShaderRegister = index;
        sampler.RegisterSpace = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_STATIC_SAMPLER_DESC& shadowSampler = samplers[4];
    shadowSampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadowSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    shadowSampler.MinLOD = 0.0f;
    shadowSampler.MaxLOD = D3D12_FLOAT32_MAX;
    shadowSampler.ShaderRegister = 4;
    shadowSampler.RegisterSpace = 0;
    shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Terrain macro stages preserve the explicit RefCode filter contract
    // instead of inheriting the user base-texture sampler: cloud uses linear
    // min/mag with point mip, while TSNoiseUrb uses point min, linear mag and
    // linear mip. Both repeat in world space.
    D3D12_STATIC_SAMPLER_DESC& cloudSampler = samplers[5];
    cloudSampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    cloudSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    cloudSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    cloudSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    cloudSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    cloudSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    cloudSampler.MinLOD = 0.0f;
    cloudSampler.MaxLOD = D3D12_FLOAT32_MAX;
    cloudSampler.ShaderRegister = 5;
    cloudSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC& macroSampler = samplers[6];
    macroSampler = cloudSampler;
    macroSampler.Filter = D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR;
    macroSampler.ShaderRegister = 6;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(std::size(parameters));
    description.pParameters = parameters;
    description.NumStaticSamplers = static_cast<UINT>(samplers.size());
    description.pStaticSamplers = samplers.data();
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT serializeResult = D3D12SerializeRootSignature(
        &description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(serializeResult)) {
        if (errors) {
            TD_LOG_ERROR("[WorldRenderer] Root signature serialization failed: {}",
                         static_cast<const char*>(errors->GetBufferPointer()));
        } else {
            TD_LOG_ERROR("[WorldRenderer] Root signature serialization failed: 0x{:08X}",
                         static_cast<uint32_t>(serializeResult));
        }
        return false;
    }

    const HRESULT createResult = m_device->getDevice()->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature));
    if (FAILED(createResult)) {
        TD_LOG_ERROR("[WorldRenderer] CreateRootSignature failed: 0x{:08X}",
                     static_cast<uint32_t>(createResult));
        return false;
    }
    return true;
}

bool WorldRenderer::createPipelineStates() {
    const auto& vertexShader = m_shaderBytecode[
        static_cast<size_t>(ShaderBytecode::WorldVertex)];
    const auto& pixelShader = m_shaderBytecode[
        static_cast<size_t>(ShaderBytecode::WorldPixel)];
    if (vertexShader.empty() || pixelShader.empty()) return false;

    const D3D12_INPUT_ELEMENT_DESC inputElements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
         static_cast<UINT>(offsetof(StaticMeshVertex, position)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
         static_cast<UINT>(offsetof(StaticMeshVertex, normal)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
         static_cast<UINT>(offsetof(StaticMeshVertex, texcoord)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0,
         static_cast<UINT>(offsetof(StaticMeshVertex, detailTexcoord)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
         static_cast<UINT>(offsetof(StaticMeshVertex, color)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R32_UINT, 0,
         static_cast<UINT>(offsetof(StaticMeshVertex, boneIndex)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"INSTANCEWORLD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, world) + sizeof(float) * 0),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INSTANCEWORLD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, world) + sizeof(float) * 4),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INSTANCEWORLD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, world) + sizeof(float) * 8),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INSTANCEWORLD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, world) + sizeof(float) * 12),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INSTANCELIGHTING", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, directionalLightScale)),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INSTANCECOLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, scriptIndicatorColor)),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INSTANCEFLAGS", 0, DXGI_FORMAT_R32_UINT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, houseColorFlags)),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INSTANCEEFFECT", 0, DXGI_FORMAT_R32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, heatVisionIntensity)),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INSTANCEFLAGS", 1, DXGI_FORMAT_R32_UINT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, heatVisionMode)),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INSTANCEEFFECT", 1, DXGI_FORMAT_R32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, objectOpacity)),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INSTANCEEFFECT", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, treePushAsideDirection)),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INSTANCEEFFECT", 3, DXGI_FORMAT_R32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, treePushAsideDarkeningFactor)),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PREVIOUSWORLD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, previousWorld) + sizeof(float) * 0),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PREVIOUSWORLD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, previousWorld) + sizeof(float) * 4),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PREVIOUSWORLD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, previousWorld) + sizeof(float) * 8),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PREVIOUSWORLD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, previousWorld) + sizeof(float) * 12),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INTERPOLATION", 0, DXGI_FORMAT_R32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, interpolationAlpha)),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = m_rootSignature.Get();
    description.VS = {vertexShader.data(), vertexShader.size()};
    description.PS = {pixelShader.data(), pixelShader.size()};
    description.BlendState.AlphaToCoverageEnable = FALSE;
    description.BlendState.IndependentBlendEnable = FALSE;
    auto& renderTargetBlend = description.BlendState.RenderTarget[0];
    renderTargetBlend.BlendEnable = FALSE;
    renderTargetBlend.LogicOpEnable = FALSE;
    renderTargetBlend.SrcBlend = D3D12_BLEND_ONE;
    renderTargetBlend.DestBlend = D3D12_BLEND_ZERO;
    renderTargetBlend.BlendOp = D3D12_BLEND_OP_ADD;
    renderTargetBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
    renderTargetBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
    renderTargetBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    renderTargetBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
    renderTargetBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // WorldCamera now keeps WW3D's right-handed view/projection convention,
    // so source counter-clockwise W3D faces are front faces at rasterization.
    description.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    description.RasterizerState.FrontCounterClockwise = TRUE;
    description.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    description.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    description.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.RasterizerState.MultisampleEnable =
        m_sampleCount > 1u ? TRUE : FALSE;
    description.RasterizerState.AntialiasedLineEnable = FALSE;
    description.RasterizerState.ForcedSampleCount = 0;
    description.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    description.DepthStencilState.DepthEnable = TRUE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    description.DepthStencilState.StencilEnable = FALSE;
    description.InputLayout = {inputElements, static_cast<UINT>(std::size(inputElements))};
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = d3d12::D3D12Device::SWAP_FORMAT;
    description.DSVFormat = d3d12::D3D12Device::DEPTH_FORMAT;
    description.SampleDesc.Count = m_sampleCount;
    description.SampleDesc.Quality = 0;

    const auto createBlendPipelines = [this, &description, &renderTargetBlend](
        StaticMeshBlendMode mode, D3D12_BLEND sourceBlend, D3D12_BLEND destinationBlend) -> bool {
        const size_t index = static_cast<size_t>(mode);
        const bool opaque = mode == StaticMeshBlendMode::Opaque;
        renderTargetBlend.BlendEnable = opaque ? FALSE : TRUE;
        renderTargetBlend.SrcBlend = sourceBlend;
        renderTargetBlend.DestBlend = destinationBlend;
        // D3D12 does not permit color-only factors such as SRC_COLOR for the
        // alpha channel. World color is opaque today, so preserve RGB blend
        // semantics and write source alpha deterministically.
        renderTargetBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
        renderTargetBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
        for (size_t depthWriteIndex = 0; depthWriteIndex < 2; ++depthWriteIndex) {
            description.DepthStencilState.DepthWriteMask = depthWriteIndex != 0
                ? D3D12_DEPTH_WRITE_MASK_ALL
                : D3D12_DEPTH_WRITE_MASK_ZERO;
            for (size_t depthCompareIndex = 0;
                 depthCompareIndex <
                     world_renderer_detail::kDepthCompareFunctions.size();
                 ++depthCompareIndex) {
                description.DepthStencilState.DepthFunc =
                    world_renderer_detail::kDepthCompareFunctions[
                        depthCompareIndex];
                auto& pipelines = m_pipelineStates[index][depthWriteIndex][depthCompareIndex];
                description.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
                HRESULT result = m_device->getDevice()->CreateGraphicsPipelineState(
                    &description, IID_PPV_ARGS(&pipelines.backFaceCulled));
                if (FAILED(result)) {
                    TD_LOG_ERROR("[WorldRenderer] {} depth={} write={} back-face PSO failed: 0x{:08X}",
                                 static_cast<uint32_t>(mode), depthCompareIndex, depthWriteIndex,
                                 static_cast<uint32_t>(result));
                    return false;
                }

                description.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
                result = m_device->getDevice()->CreateGraphicsPipelineState(
                    &description, IID_PPV_ARGS(&pipelines.frontFaceCulled));
                if (FAILED(result)) {
                    TD_LOG_ERROR("[WorldRenderer] {} depth={} write={} front-face PSO failed: 0x{:08X}",
                                 static_cast<uint32_t>(mode), depthCompareIndex,
                                 depthWriteIndex, static_cast<uint32_t>(result));
                    return false;
                }

                description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                result = m_device->getDevice()->CreateGraphicsPipelineState(
                    &description, IID_PPV_ARGS(&pipelines.twoSided));
                if (FAILED(result)) {
                    TD_LOG_ERROR("[WorldRenderer] {} depth={} write={} two-sided PSO failed: 0x{:08X}",
                                 static_cast<uint32_t>(mode), depthCompareIndex, depthWriteIndex,
                                 static_cast<uint32_t>(result));
                    return false;
                }
            }
        }
        return true;
    };

    if (!createBlendPipelines(StaticMeshBlendMode::Opaque,
                              D3D12_BLEND_ONE, D3D12_BLEND_ZERO) ||
        !createBlendPipelines(StaticMeshBlendMode::Additive,
                              D3D12_BLEND_ONE, D3D12_BLEND_ONE) ||
        !createBlendPipelines(StaticMeshBlendMode::Alpha,
                              D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA) ||
        !createBlendPipelines(StaticMeshBlendMode::Multiply,
                              D3D12_BLEND_ZERO, D3D12_BLEND_SRC_COLOR) ||
        !createBlendPipelines(StaticMeshBlendMode::Screen,
                              D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC_COLOR)) {
        return false;
    }

    // The skeleton view must be useful precisely when a material, a texture
    // descriptor, or a terrain light is broken.  Give it a standalone
    // wireframe PSO rather than trying to inherit the many W3D blend modes.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC skeletonDescription = description;
    auto& skeletonBlend = skeletonDescription.BlendState.RenderTarget[0];
    skeletonBlend.BlendEnable = FALSE;
    skeletonBlend.LogicOpEnable = FALSE;
    skeletonBlend.SrcBlend = D3D12_BLEND_ONE;
    skeletonBlend.DestBlend = D3D12_BLEND_ZERO;
    skeletonBlend.BlendOp = D3D12_BLEND_OP_ADD;
    skeletonBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
    skeletonBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
    skeletonBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    skeletonBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
    skeletonBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    skeletonDescription.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    skeletonDescription.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    skeletonDescription.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    skeletonDescription.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    const HRESULT skeletonResult = m_device->getDevice()->CreateGraphicsPipelineState(
        &skeletonDescription, IID_PPV_ARGS(&m_skeletonPipelineState));
    if (FAILED(skeletonResult)) {
        TD_LOG_ERROR("[WorldRenderer] Skeleton wireframe PSO failed: 0x{:08X}",
                     static_cast<uint32_t>(skeletonResult));
        return false;
    }
    return true;
}

bool WorldRenderer::createDebugCube() {
    const auto createAndUpload = [this](const void* source, UINT64 size,
                                        Microsoft::WRL::ComPtr<ID3D12Resource>& resource) -> bool {
        const D3D12_HEAP_PROPERTIES heapProperties = makeUploadHeapProperties();
        const D3D12_RESOURCE_DESC description = makeBufferDescription(size);
        const HRESULT result = m_device->getDevice()->CreateCommittedResource(
            &heapProperties, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource));
        if (FAILED(result)) {
            TD_LOG_ERROR("[WorldRenderer] CreateCommittedResource for debug mesh failed: 0x{:08X}",
                         static_cast<uint32_t>(result));
            return false;
        }

        void* destination = nullptr;
        const D3D12_RANGE readRange{0, 0};
        const HRESULT mapResult = resource->Map(0, &readRange, &destination);
        if (FAILED(mapResult) || !destination) {
            TD_LOG_ERROR("[WorldRenderer] Map for debug mesh failed: 0x{:08X}",
                         static_cast<uint32_t>(mapResult));
            resource.Reset();
            return false;
        }
        std::memcpy(destination, source, static_cast<size_t>(size));
        resource->Unmap(0, nullptr);
        return true;
    };

    if (!createAndUpload(kCubeVertices.data(), sizeof(kCubeVertices), m_cubeVertexBuffer) ||
        !createAndUpload(kCubeIndices.data(), sizeof(kCubeIndices), m_cubeIndexBuffer)) {
        return false;
    }

    m_cubeVertexBufferView.BufferLocation = m_cubeVertexBuffer->GetGPUVirtualAddress();
    m_cubeVertexBufferView.SizeInBytes = static_cast<UINT>(sizeof(kCubeVertices));
    m_cubeVertexBufferView.StrideInBytes = sizeof(DebugVertex);
    m_cubeIndexBufferView.BufferLocation = m_cubeIndexBuffer->GetGPUVirtualAddress();
    m_cubeIndexBufferView.SizeInBytes = static_cast<UINT>(sizeof(kCubeIndices));
    m_cubeIndexBufferView.Format = DXGI_FORMAT_R32_UINT;
    return true;
}

void WorldRenderer::renderDebugScene(float elapsedSeconds) {
    math::transform world = math::transform::rotation_y(elapsedSeconds * 0.7f);
    world.rotate_x(elapsedSeconds * 0.35f);

    StaticMeshDrawPacket draw;
    draw.vertexBuffer = m_cubeVertexBufferView;
    draw.indexBuffer = m_cubeIndexBufferView;
    draw.worldTransform = world;
    draw.diffuse = {0.22f, 0.70f, 1.00f, 1.00f};
    draw.indexCount = static_cast<uint32_t>(kCubeIndices.size());
    renderStaticMeshes(container::Span<const StaticMeshDrawPacket>(&draw, 1),
                       m_debugCamera.toSnapshot(), {}, {}, elapsedSeconds);
}




} // namespace engine::render
