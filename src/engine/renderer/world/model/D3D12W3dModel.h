#pragma once

#include "core/container/container_types.h"

#include "engine/renderer/world/model/W3dAssetCache.h"
#include "engine/renderer/world/pipeline/WorldRenderer.h"

#include <cstddef>
#include <cstdint>
namespace engine::d3d12 {
class D3D12Device;
struct GpuRetirementIdentity;
}

namespace engine::render {

class WorldTextureCache;

// Immutable material-to-SRV snapshot used by renderer diagnostics.  It keeps
// GPU ownership private while making a missing VFS texture distinguishable
// from a W3D material that deliberately has no texture stage.
struct W3dMaterialTextureBinding {
    uint32_t materialIndex = 0;
    container::String materialName;
    container::String textureName;
    uint32_t textureSrvIndex = 0;
    container::String detailTextureName;
    uint32_t detailTextureSrvIndex = 0;
};

// Per-draw texture substitution. This is intentionally an append-packet
// value, not a mutation of D3D12W3dModel's immutable material table: RefCode
// has one new_skybox instance, whereas a modern asset cache may share every
// other W3D model across the whole world.
struct W3dMaterialTextureOverride {
    uint32_t materialIndex = UINT32_MAX;
    uint32_t textureSrvIndex = 0;
    // W3DTEXTURE_CLAMP_U | W3DTEXTURE_CLAMP_V in the existing renderer
    // sampler encoding. Skybox callers force 3 to eliminate corner seams.
    uint8_t samplerMode = 3;
    // W3DTreeBuffer binds its atlas on stage zero and explicitly clears
    // stage one. Other override users leave the immutable detail stage intact.
    bool overridesDetailTexture = false;
    uint32_t detailTextureSrvIndex = 0;
};

struct W3dRestPaletteFrameStats final {
    uint32_t palettesBuilt = 0;
    uint32_t palettesReused = 0;
    uint64_t jointsMaterialized = 0;
};

// Frame-local owner for immutable skeleton rest poses transformed into world
// space. Exact skeleton/world matches share one stable palette allocation, so
// packet generation and the existing GPU upload cache see the same pointer.
class W3dRestPaletteFrameCache final {
public:
    void clear() noexcept;
    [[nodiscard]] container::Span<const math::transform> resolve(
        container::SharedPtr<const Skeleton> skeleton,
        const math::transform& world);
    [[nodiscard]] const W3dRestPaletteFrameStats& stats() const noexcept {
        return m_stats;
    }

private:
    struct Entry final {
        container::SharedPtr<const Skeleton> skeleton;
        math::transform world;
        container::Vector<math::transform> palette;
    };
    container::Vector<Entry> m_entries;
    W3dRestPaletteFrameStats m_stats;
};

// Value-only options shared by a root W3D and its HLOD AdditionalModels.
// Child models inherit Drawable presentation channels just as they do in the
// original HLodClass scene graph. Runtime-generated rest palettes must be
// retained until command recording has consumed the emitted packet pointers.
struct W3dModelGraphDrawOptions final {
    float visualTimeSeconds = 0.0f;
    float directionalLightScale = 1.0f;
    container::Span<const W3dMaterialTextureOverride>
        materialTextureOverrides{};
    math::vec3 scriptFlashTint{};
    float heatVisionIntensity = 0.0f;
    bool heatVisionOnly = false;
    float objectOpacity = 1.0f;
    math::vec3 scriptIndicatorColor{};
    bool hasScriptIndicatorColor = false;
    bool receivesDynamicLights = true;
    container::Span<const RenderSubObjectVisibility>
        subObjectVisibility{};
    math::vec2 treePushAsideDirection{};
    float treePushAsideAmount = 0.0f;
    float treePushAsideDistanceFactor = 0.0f;
    float treePushAsideDarkeningFactor = 0.0f;
    RenderVehicleTreadState vehicleTreads{};
    W3dRestPaletteFrameCache* restPalettes = nullptr;
    const math::transform* previousEntityWorld = nullptr;
    container::Span<const math::transform> previousSkinPalette{};
    float interpolationAlpha = 1.0f;
};

// D3D12 immutable representation of one CpuStaticModel.  The CPU asset cache
// stores it through the backend-neutral W3dGpuModel base; draw submission
// performs one checked cast at the renderer boundary.
class D3D12W3dModel final : public W3dGpuModel {
public:
    ~D3D12W3dModel() override;

    D3D12W3dModel(const D3D12W3dModel&) = delete;
    D3D12W3dModel& operator=(const D3D12W3dModel&) = delete;
    D3D12W3dModel(D3D12W3dModel&&) = delete;
    D3D12W3dModel& operator=(D3D12W3dModel&&) = delete;

    // Records DEFAULT-heap VB/IB and texture uploads on the current frame.
    [[nodiscard]] static container::SharedPtr<D3D12W3dModel> upload(
        d3d12::D3D12Device& device,
        container::SharedPtr<WorldTextureCache> textures,
        const CpuStaticModel& cpuModel,
        const d3d12::GpuRetirementIdentity& retirementIdentity,
        RenderAssetPriority priority = RenderAssetPriority::Normal,
        container::String* error = nullptr,
        bool* deferred = nullptr);

    // Converts immutable mesh primitives into transient packets. W3D part
    // transforms precede the entity transform under the row-vector contract.
    void appendDrawPackets(const math::transform& entityWorld,
                           container::Span<const math::transform> skinPalette,
                           container::Span<const uint8_t> boneVisibility,
                           container::Vector<StaticMeshDrawPacket>& output,
                           float visualTimeSeconds = 0.0f,
                           float directionalLightScale = 1.0f,
                           container::Span<const W3dMaterialTextureOverride>
                               materialTextureOverrides = {},
                           math::vec3 scriptFlashTint = {},
                           float heatVisionIntensity = 0.0f,
                           bool heatVisionOnly = false,
                           float objectOpacity = 1.0f,
                           math::vec3 scriptIndicatorColor = {},
                           bool hasScriptIndicatorColor = false,
                           bool receivesDynamicLights = true,
                           container::Span<const RenderSubObjectVisibility>
                               subObjectVisibility = {},
                           math::vec2 treePushAsideDirection = {},
                           float treePushAsideAmount = 0.0f,
                           float treePushAsideDistanceFactor = 0.0f,
                           float treePushAsideDarkeningFactor = 0.0f,
                           RenderVehicleTreadState vehicleTreads = {},
                           const math::transform* previousEntityWorld = nullptr,
                           container::Span<const math::transform>
                               previousSkinPalette = {},
                           float interpolationAlpha = 1.0f) const;

    // Idempotent render-thread retirement. Resources are handed to the
    // device's fence queue; texture references return to the shared cache.
    void retire() noexcept;

    [[nodiscard]] size_t meshCount() const noexcept;
    [[nodiscard]] size_t skinnedMeshCount() const noexcept;
    [[nodiscard]] size_t primitiveCount() const noexcept;
    [[nodiscard]] uint64_t residentBytes() const noexcept override;
    [[nodiscard]] uint64_t lastUsedFrame() const noexcept override;
    [[nodiscard]] W3dGpuUseDiagnostic useDiagnostic() const noexcept override;
    [[nodiscard]] uint64_t retainedSortingBytes() const noexcept override;
    [[nodiscard]] bool retired() const noexcept;

    // Immutable cached view built once with the GPU material table. A
    // non-empty texture
    // name with SRV 0 means WorldTextureCache had to use its white fallback;
    // an empty name means the W3D material has no corresponding texture stage.
    [[nodiscard]] container::Span<const W3dMaterialTextureBinding>
    materialTextureBindings() const noexcept;

private:
    struct Impl;
    explicit D3D12W3dModel(container::UniquePtr<Impl> impl) noexcept;

    container::UniquePtr<Impl> m_impl;
};

// Appends the ready portion of an immutable HLOD graph. Missing/pending child
// assets are requested through the normal asynchronous cache path and appear
// once ready; malformed cycles and graphs deeper than the legacy-safe bound
// are ignored without affecting the root model. Projected-size LOD selection
// is intentionally outside this function (B12).
struct W3dModelGraphTraversalStats final {
    uint32_t requestedNodes = 0;
    uint32_t readyNodes = 0;
    uint32_t pendingNodes = 0;
    uint32_t missingChildren = 0;
    uint32_t cycleRejected = 0;
    uint32_t depthRejected = 0;
};

[[nodiscard]] size_t appendW3dModelGraphDrawPackets(
    W3dAssetCache& assets, W3dModelHandle rootHandle,
    const math::transform& entityWorld,
    container::Span<const math::transform> skinPalette,
    container::Span<const uint8_t> boneVisibility,
    container::Vector<StaticMeshDrawPacket>& output,
    const W3dModelGraphDrawOptions& options = {},
    W3dModelGraphTraversalStats* traversalStats = nullptr);

} // namespace engine::render
