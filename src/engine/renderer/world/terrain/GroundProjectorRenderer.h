#pragma once

#include "core/container/hash_containers.h"

#include "engine/renderer/world/pipeline/WorldRenderPipeline.h"
#include "engine/renderer/world/pipeline/WorldRenderer.h"
#include "engine/renderer/world/resource/WorldTextureCache.h"
#include "presentation/render/GroundDecalPerformanceSettings.h"
#include "presentation/render/GroundDecalVisualSettings.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <cstddef>
#include <cstdint>
#include <optional>
namespace engine::d3d12 {
class D3D12Device;
}

namespace engine::render {

struct TerrainPrimaryCellTopologyResolver;

namespace ground_projector_limits {
inline constexpr float kMinimumTerrainScorchRadius = 0.15f;
// RefCode does not clamp authored scorch radii. Stock map/content values are
// 9..209 world units; terrain bounds and the renderer's independent instance
// budget provide the operational bound.
inline constexpr float kMaximumStockTerrainScorchRadius = 209.0f;
} // namespace ground_projector_limits

enum class GroundProjectorBlendMode : uint8_t {
    Alpha,
    Additive,
    Multiply,
    Count,
};

struct GroundProjectorInstance final {
    container::Array<RenderVector, 4> corners;
    math::vec4 color{0.0f, 0.0f, 0.0f, 0.45f};
    float edgeSoftness = ground_decals::visual_defaults::kRadialEdgeSoftness;
    container::String textureName;
    // Affine sub-rectangle applied after the projectors' canonical 0..1 UV.
    // Keeping this on the shared contract lets terrain scorches, road/decal
    // atlases and future authored projectors use the same renderer path.
    math::vec2 uvScale{1.0f, 1.0f};
    math::vec2 uvOffset{};
    // Terrain-clipped triangles are not necessarily an affine sub-rectangle.
    // When enabled, these exact per-corner UVs replace uvScale/uvOffset.
    container::Array<math::vec2, 4> cornerUvs{};
    GroundProjectorBlendMode blendMode = GroundProjectorBlendMode::Alpha;
    bool radialMask = true;
    // Terrain scorches/general decals participate in the late shroud
    // composite. Object projected shadows retain RefCode's caster-level
    // fully-obscured policy and therefore leave this false.
    bool receivesVisibility = false;
    bool explicitCornerUvs = false;
    // Heightfield cells may use the opposite diagonal. Existing quad
    // producers retain false; cell-conforming producers publish the terrain
    // flip explicitly so geometry and depth use identical triangles.
    bool triangleFlip = false;
    bool visible = true;
    uint64_t expireSimulationFrame = 0;
};

struct GroundProjectorRenderStats final {
    uint32_t submittedInstances = 0;
    uint32_t renderedInstances = 0;
    uint32_t rejectedInstances = 0;
    uint32_t budgetRejectedInstances = 0;
    uint32_t drawCalls = 0;
    uint32_t textureBatches = 0;
    uint32_t residentTextures = 0;
};

// Shared terrain-conforming ground projector. Authored DECAL/PROJECTION
// shadows and short-lived scorch/decal content share this value-only
// instance contract; VOLUME geometry remains in the directional shadow pass.
class GroundProjectorRenderer final {
public:
    GroundProjectorRenderer() = default;
    GroundProjectorRenderer(
        d3d12::D3D12Device& device,
        container::SharedPtr<WorldTextureCache> textures = {});
    ~GroundProjectorRenderer();

    GroundProjectorRenderer(const GroundProjectorRenderer&) = delete;
    GroundProjectorRenderer& operator=(const GroundProjectorRenderer&) = delete;

    bool init(d3d12::D3D12Device& device,
              container::SharedPtr<WorldTextureCache> textures = {});
    void shutdown();
    // Drops only cache-owned texture references. Pipeline objects remain
    // valid, so a map/session texture-generation reset does not require a
    // shader/PSO rebuild.
    void resetTextureCache();

    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }
    [[nodiscard]] const GroundProjectorRenderStats& stats() const noexcept {
        return m_stats;
    }

    // Session-frozen operational limits. Values are clamped to the immutable
    // hard ceilings in GroundDecalPerformanceSettings; lowering the texture
    // budget drops the old cache so no previous session can retain capacity.
    void configureOperationalBudget(uint32_t maximumInstancesPerFrame,
                                    uint32_t maximumResidentTextures);
    [[nodiscard]] bool configureTextureSampling(
        uint32_t textureFilter, uint32_t anisotropyLevel,
        uint32_t sampleCount);

    [[nodiscard]] static container::Vector<GroundProjectorInstance>
    buildProjectedShadows(
        container::Span<const PreparedRenderInstance> instances,
        container::Span<const PreparedProjectileRenderSnapshot> projectiles,
        const TerrainRenderSnapshot* terrain,
        const WorldLightEnvironment& lights);

    // Caller-owned hot-path form. Existing contents are preserved and new
    // projectors are appended, allowing a frame-retained aggregate to clear
    // once and reuse its capacity across all ground-projector producers.
    static void appendProjectedShadows(
        container::Vector<GroundProjectorInstance>& output,
        container::Span<const PreparedRenderInstance> instances,
        container::Span<const PreparedProjectileRenderSnapshot> projectiles,
        const TerrainRenderSnapshot* terrain,
        const WorldLightEnvironment& lights);

    [[nodiscard]] static container::Vector<GroundProjectorInstance>
    buildTerrainScorches(
        container::Span<const TerrainScorchRenderData> scorches,
        const TerrainRenderSnapshot* terrain);

    static void appendTerrainScorches(
        container::Vector<GroundProjectorInstance>& output,
        container::Span<const TerrainScorchRenderData> scorches,
        const TerrainRenderSnapshot* terrain,
        const RenderCameraSnapshot* camera = nullptr,
        float viewportAspectRatio = 4.0f / 3.0f);

    [[nodiscard]] static std::optional<GroundProjectorInstance>
    buildTexturedDecal(
        RenderVector position, float radius, float yawRadians,
        container::String textureName, math::vec4 color,
        GroundProjectorBlendMode blendMode,
        const TerrainRenderSnapshot* terrain,
        uint64_t expireSimulationFrame = 0);

    // Persistent/general decals use a tessellated square so every generated
    // corner samples the heightfield. UVs remain continuous across tiles;
    // authored radius is clipped by map bounds, not a visual-size clamp.
    [[nodiscard]] static container::Vector<GroundProjectorInstance>
    buildTexturedDecals(
        RenderVector position, float radius, float yawRadians,
        container::StringView textureName, math::vec4 color,
        GroundProjectorBlendMode blendMode,
        const TerrainRenderSnapshot* terrain,
        uint64_t expireSimulationFrame = 0);

    static void appendTexturedDecals(
        container::Vector<GroundProjectorInstance>& output,
        RenderVector position, float radius, float yawRadians,
        container::StringView textureName, math::vec4 color,
        GroundProjectorBlendMode blendMode,
        const TerrainRenderSnapshot* terrain,
        uint64_t expireSimulationFrame = 0,
        const TerrainPrimaryCellTopologyResolver* topology = nullptr);

    // Renderer-neutral authored rectangular projector contract. sizeX/sizeY
    // are full world-space dimensions; legacy ShadowOffsetX shifts opposite
    // object X while ShadowOffsetY shifts along object Y before tessellation.
    // Every tile samples terrain
    // independently while its UV sub-rectangle remains in the common 0..1
    // footprint, so adjacent tiles cannot restart or repeat the texture.
    [[nodiscard]] static container::Vector<GroundProjectorInstance>
    buildTexturedRectDecals(
        RenderVector position, float sizeX, float sizeY,
        float offsetX, float offsetY, float yawRadians,
        container::StringView textureName, math::vec4 color,
        GroundProjectorBlendMode blendMode,
        const TerrainRenderSnapshot* terrain,
        uint64_t expireSimulationFrame = 0);

    static void appendTexturedRectDecals(
        container::Vector<GroundProjectorInstance>& output,
        RenderVector position, float sizeX, float sizeY,
        float offsetX, float offsetY, float yawRadians,
        container::StringView textureName, math::vec4 color,
        GroundProjectorBlendMode blendMode,
        const TerrainRenderSnapshot* terrain,
        uint64_t expireSimulationFrame = 0,
        const TerrainPrimaryCellTopologyResolver* topology = nullptr);

    [[nodiscard]] size_t render(
        container::Span<const GroundProjectorInstance> instances,
        const RenderCameraSnapshot& camera,
        uint64_t simulationFrame = 0,
        const WorldLocalVisibilityGpuBinding& visibility = {});

private:
    struct GpuInstance final {
        float corners[4][3]{};
        float color[4]{};
        float uvScale[2]{1.0f, 1.0f};
        float uvOffset[2]{};
        float edgeSoftness = 0.30f;
        uint32_t radialMask = 1;
        uint32_t blendMode = 0;
        uint32_t receivesVisibility = 0;
        float cornerUvs[4][2]{};
        uint32_t explicitCornerUvs = 0;
        uint32_t triangleFlip = 0;
    };

    struct PreparedInstance final {
        GpuInstance gpu;
        uint32_t textureSrv = 0;
        GroundProjectorBlendMode blendMode = GroundProjectorBlendMode::Alpha;
        float distanceSquared = 0.0f;
    };

    bool createRootSignature();
    bool loadShaderPackage();
    bool createPipelineStates();
    [[nodiscard]] uint32_t textureSrv(container::StringView textureName);
    void releaseTextures();

    d3d12::D3D12Device* m_device = nullptr;
    container::SharedPtr<WorldTextureCache> m_textures;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    container::Array<Microsoft::WRL::ComPtr<ID3D12PipelineState>,
               static_cast<size_t>(GroundProjectorBlendMode::Count)>
        m_pipelineStates;
    container::Array<container::Vector<uint8_t>, 2> m_shaderBytecode;
    container::Vector<GpuInstance> m_gpuInstances;
    container::Vector<PreparedInstance> m_preparedInstances;
    container::HashMap<container::String, uint32_t> m_textureSrvs;
    GroundProjectorRenderStats m_stats;
    uint32_t m_maximumInstancesPerFrame =
        ground_decals::performance_limits::kHardMaximumInstancesPerFrame;
    uint32_t m_maximumResidentTextures =
        ground_decals::performance_limits::kHardMaximumResidentTextures;
    uint32_t m_textureFilter = 2;
    uint32_t m_anisotropyLevel = 2;
    uint32_t m_sampleCount = 1;
    bool m_initialized = false;
};

} // namespace engine::render
