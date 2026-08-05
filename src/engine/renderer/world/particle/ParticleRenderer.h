#pragma once

#include "core/container/hash_containers.h"

#include "engine/fx/runtime/ParticleRuntime.h"
#include "engine/renderer/runtime/RendererStats.h"
#include "engine/renderer/world/particle/GpuParticleMaterialBins.h"
#include "engine/renderer/world/particle/GpuParticlePresentationGate.h"
#include "engine/renderer/world/particle/GpuParticleBillboardContract.h"
#include "engine/renderer/world/particle/ParticleVisibilityContract.h"
#include "engine/renderer/world/pipeline/WorldCamera.h"
#include "engine/renderer/d3d12/runtime/D3D12PerformanceSettings.h"
#include "presentation/render/TerrainRenderSnapshot.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
namespace engine::d3d12 {
class D3D12Device;
}

namespace engine::render {

class WorldTextureCache;
class GpuParticleSimulator;
class W3dAssetCache;
class W3dRestPaletteFrameCache;
struct GpuParticleSimulationRenderStats;
struct ParticleRendererScratch;
struct StaticMeshDrawPacket;
struct W3dModelGraphTraversalStats;

// Renderer-internal allocation ceilings. These are safety/performance limits,
// not user-facing particle-detail settings: Feature quality may lower the
// working set but cannot raise these bounds.
namespace particle_render::performance_limits {
inline constexpr size_t kHardMaximumSourceParticles = 8192;
inline constexpr size_t kMaximumExpansionPerSource = 6;
inline constexpr size_t kHardMaximumCandidates =
    kHardMaximumSourceParticles * kMaximumExpansionPerSource;
} // namespace particle_render::performance_limits

// AoS stream consumed by the GPU after ParticleRuntime's CPU AoSoA update.
// Slot 1 advances once per instance; slot 0 is one immutable four-vertex quad.
struct ParticleRenderInstance final {
    float position[3]{};
    float size = 0.0f;
    float endPosition[3]{};
    float endSize = 0.0f;
    float color[4]{};
    float endColor[4]{};
    float angleRadians = 0.0f;
    uint32_t flags = 0;
};

static_assert(sizeof(ParticleRenderInstance) == 72);

struct SmudgeRenderInstance final {
    float position[3]{};
    float size = 0.0f;
    float opacity = 0.0f;
    float uvOffset[2]{};
};

static_assert(sizeof(SmudgeRenderInstance) == 28);

struct GpuParticleReferenceSample final {
    uint32_t stateSlot = 0;
    uint32_t particleGeneration = 0;
    float position[3]{};
    float previousPosition[3]{};
    float size = 0.0f;
    float angle = 0.0f;
    float color[4]{};
};

inline constexpr uint32_t kParticleRenderGroundAligned = 1u << 0u;
inline constexpr uint32_t kParticleRenderStreak = 1u << 1u;
inline constexpr uint32_t kParticleRenderVolumeLayer = 1u << 2u;

// The reference route identifies CPU-produced instances that can be compared
// against the shadow GPU backend.  Both routes remain CPU-rendered until the
// presentation gate explicitly transfers visible authority.
enum class ParticleRenderRoute : uint8_t {
    CpuOnly,
    GpuCompatibleReference,
    Count,
};

struct ParticleRenderBatch final {
    fx::ParticleShader shader = fx::ParticleShader::None;
    ParticleRenderRoute route = ParticleRenderRoute::CpuOnly;
    container::String textureName;
    uint32_t textureSrvIndex = 0;
    uint32_t firstInstance = 0;
    uint32_t instanceCount = 0;
};

struct ParticleRenderDrawList final {
    uint64_t textureBindingGeneration = 0;
    float interpolationAlpha = 1.0f;
    struct Stats final {
        size_t sourceParticles = 0;
        size_t rejectedInvalid = 0;
        size_t rejectedVisibility = 0;
        size_t rejectedColor = 0;
        size_t rejectedSourceBudget = 0;
        size_t rejectedBudget = 0;
        size_t billboardInstances = 0;
        size_t streakInstances = 0;
        size_t volumeInstances = 0;
        // DRAWABLE ParticleSystems are submitted as W3D static-mesh packets
        // by appendDrawableParticleDrawPackets(), not omitted from the
        // renderer. This counts sources routed away from billboards.
        size_t routedDrawableParticles = 0;
        size_t smudgeInstances = 0;
        size_t gpuCompatibleEligibleSources = 0;
        size_t gpuCompatibleSelectedInstances = 0;
        uint64_t sourceSelectionMicroseconds = 0;
        uint64_t expansionMicroseconds = 0;
        uint64_t sortMicroseconds = 0;
        uint64_t packMicroseconds = 0;
        uint64_t textureBindingMicroseconds = 0;
        container::Array<size_t, static_cast<size_t>(
            fx::GpuParticleCompatibilityReason::Count)>
            compatibilityReasonSourceParticles{};
        container::Array<size_t, static_cast<size_t>(
            fx::GpuParticleCompatibilityReason::Count)>
            compatibilityReasonSelectedInstances{};
        size_t sourcePriorityScratchCapacity = 0;
        size_t sourceOrdinalScratchCapacity = 0;
        size_t candidateScratchCapacity = 0;
        size_t streakPointScratchCapacity = 0;
        size_t sourcePriorityScratchHighWater = 0;
        size_t sourceOrdinalScratchHighWater = 0;
        size_t candidateScratchHighWater = 0;
        size_t streakPointScratchHighWater = 0;
        size_t scratchCapacityGrowths = 0;
        size_t scratchContainersReused = 0;
        size_t scratchHardCapRejected = 0;
    } stats;

    container::Vector<ParticleRenderInstance> instances;
    container::Vector<ParticleRenderBatch> batches;
    container::Vector<GpuParticleVisibilityGeneration>
        gpuVisibilityGenerations;
    // Screen-space distortion consumes these after opaque/transparent world
    // rendering has produced a scene-color source. They must never enter the
    // ordinary billboard batches.
    container::Vector<SmudgeRenderInstance> smudgeInstances;
    container::Array<GpuParticleReferenceSample,
        d3d12::performance_limits::kGpuParticleAbStateSampleCapacity>
        gpuReferenceSamples{};
    uint32_t gpuReferenceSampleCount = 0;
};

struct ParticleRenderExecutionStats final {
    uint64_t instanceUploadBytes = 0;
    uint64_t instanceUploadMicroseconds = 0;
    uint64_t drawRecordMicroseconds = 0;
    uint64_t smudgeUploadBytes = 0;
    uint64_t smudgeUploadMicroseconds = 0;
    uint64_t smudgeDrawRecordMicroseconds = 0;
    uint32_t cpuDrawCalls = 0;
    uint32_t gpuIndirectDrawCalls = 0;
    uint32_t smudgeDrawCalls = 0;
};

// Non-UI qualification published by a renderer benchmark/validation owner.
// Revision changes invalidate the prior approval atomically; gameplay and
// authored particle values never participate in this policy.
struct GpuParticlePresentationQualification final {
    uint64_t revision = 0;
    bool outputParityApproved = false;
    bool profileApproved = false;
    size_t minimumParticleCount = 0;
};

// Separate D3D12 particle pass. It owns only static quad/PSO resources and
// borrows the shared world texture cache; ParticleRuntime remains a client
// presentation service and never receives a device or descriptor handle.
class ParticleRenderer final {
public:
    ParticleRenderer();
    ParticleRenderer(d3d12::D3D12Device& device,
                     container::SharedPtr<WorldTextureCache> textures);
    ~ParticleRenderer();

    ParticleRenderer(const ParticleRenderer&) = delete;
    ParticleRenderer& operator=(const ParticleRenderer&) = delete;

    bool init(d3d12::D3D12Device& device,
              container::SharedPtr<WorldTextureCache> textures);
    void shutdown();
    void resetTextureCache();
    [[nodiscard]] bool configureTextureSampling(
        uint32_t textureFilter, uint32_t anisotropyLevel,
        uint32_t sampleCount);
    // Builds the opt-in Compute + indirect shadow infrastructure without
    // changing simulation or visible-render authority. The GPU path executes
    // with color/depth writes suppressed until A/B and profile gates pass.
    [[nodiscard]] bool configureGpuSimulationInfrastructure(bool enabled);
    void requestGpuSimulationReset(uint64_t authorityEpoch) noexcept;
    // Queues CPU-authoritative lifecycle commands for the renderer-owned GPU
    // shadow state. This does not switch update or draw authority away from
    // ParticleRuntime; the batch is consumed before the ordinary CPU draw.
    void queueGpuSimulationCommands(
        fx::ParticleGpuCommandBatch batch,
        uint64_t authoredSampleOrdinal,
        uint32_t authoredFrames);
    [[nodiscard]] bool gpuSimulationInfrastructureReady() const noexcept;
    // True only when the offline graphics package and its root/PSO/command
    // signature are ready. It does not mean GPU presentation is effective.
    [[nodiscard]] bool gpuBillboardShaderPackageReady() const noexcept {
        return m_gpuBillboardShaderPackageReady;
    }
    [[nodiscard]] GpuParticleSimulationRenderStats
        gpuSimulationStats() const noexcept;
    [[nodiscard]] const SceneColorRenderStats& sceneColorStats() const noexcept {
        return m_sceneColorStats;
    }
    // Re-evaluated from the current dynamic particle budget every frame.
    // Until visibility/parity/profile qualifications are proven, this gate
    // deliberately keeps CPU presentation effective and exposes why.
    void updateGpuPresentationGate(
        bool requested, size_t particleCount, size_t particleBudget) noexcept;
    [[nodiscard]] GpuParticlePresentationGateDecision
        gpuPresentationGate() const noexcept {
        return m_gpuPresentationGate;
    }
    void configureGpuPresentationQualification(
        GpuParticlePresentationQualification qualification) noexcept;
    [[nodiscard]] static GpuParticleMaterialBinCompilation
        compileGpuMaterialBins(
            const fx::ParticleSystemCatalog& catalog,
            size_t maximumBins = particle_render::performance_limits::
                kHardMaximumSourceParticles);
    // Safe-frame catalog publication. The CPU descriptor list remains useful
    // before GPU count/prefix/scatter is enabled and is retained across frames.
    void configureGpuMaterialBins(
        const fx::ParticleSystemCatalog& catalog);
    [[nodiscard]] container::Span<const GpuParticleMaterialBin>
        gpuMaterialBins() const noexcept { return m_gpuMaterialBins.bins; }

    [[nodiscard]] static ParticleRenderDrawList buildDrawList(
        const fx::ParticleRuntime& runtime,
        const fx::ParticleSystemCatalog& catalog,
        math::vec3 cameraPosition,
        size_t maximumInstances,
        float interpolationAlpha = 1.0f,
        size_t maximumSourceParticles = std::numeric_limits<size_t>::max(),
        const LocalVisibilityRenderSnapshot& localVisibility = {});
    // Renderer-owned output can be retained across frames; clearing here
    // preserves instance/batch/smudge capacity while all values remain
    // frame-local and detached from ParticleRuntime.
    static void buildDrawListInto(
        ParticleRenderDrawList& output,
        const fx::ParticleRuntime& runtime,
        const fx::ParticleSystemCatalog& catalog,
        math::vec3 cameraPosition,
        size_t maximumInstances,
        float interpolationAlpha = 1.0f,
        size_t maximumSourceParticles = std::numeric_limits<size_t>::max(),
        const LocalVisibilityRenderSnapshot& localVisibility = {});
    // Production path: unlike the static probe/value API above, this retains
    // all bounded source-selection, expansion and streak scratch across
    // frames. Scratch is cleared before returning, so it never owns a runtime
    // particle, catalog pointer or other frame-lifetime reference.
    void buildDrawListIntoRetained(
        ParticleRenderDrawList& output,
        const fx::ParticleRuntime& runtime,
        const fx::ParticleSystemCatalog& catalog,
        math::vec3 cameraPosition,
        size_t maximumInstances,
        float interpolationAlpha = 1.0f,
        size_t maximumSourceParticles = std::numeric_limits<size_t>::max(),
        const LocalVisibilityRenderSnapshot& localVisibility = {});

    // DRAWABLE ParticleSystems are W3D instances, not camera-facing quads.
    // Keep their resource lifetime in the renderer's shared W3D cache and
    // append packets to the normal static-mesh pass.  This is deliberately
    // separate from the billboard draw list so `Size = 0` retains the model's
    // native scale rather than becoming an invisible quad.
    [[nodiscard]] static size_t appendDrawableParticleDrawPackets(
        const fx::ParticleRuntime& runtime,
        const fx::ParticleSystemCatalog& catalog,
        W3dAssetCache& assets,
        container::Vector<StaticMeshDrawPacket>& output,
        W3dRestPaletteFrameCache& restPalettes,
        float visualTimeSeconds,
        float interpolationAlpha,
        const LocalVisibilityRenderSnapshot& localVisibility = {},
        W3dModelGraphTraversalStats* traversalStats = nullptr);

    [[nodiscard]] bool publishGpuVisibilityAuthority(
        const ParticleRenderDrawList& drawList);

    // Resolve texture names once after the device-independent draw list has
    // been built. Keeping this separate preserves buildDrawList for probes
    // while ensuring the command-recording loop consumes integer SRV slots.
    void prepareTextureBindings(ParticleRenderDrawList& drawList);

    // Record after opaque/models/water and before snow/view filters. All modes
    // depth-test against the world; only the opaque AlphaTest mode writes depth.
    [[nodiscard]] size_t render(const ParticleRenderDrawList& drawList,
                                const RenderCameraSnapshot& camera,
                                const LocalVisibilityRenderSnapshot&
                                    localVisibility = {});
    // Runs after the multisample world target has been resolved and before
    // snow/tactical filters/UI. It copies scene color once, then composites
    // bounded view-facing distortion fans. Disabled heat effects produce no
    // draw and never fall back to a textured particle.
    [[nodiscard]] size_t renderSmudges(
        const ParticleRenderDrawList& drawList,
        const RenderCameraSnapshot& camera,
        bool heatEffectsEnabled,
        const LocalVisibilityRenderSnapshot& localVisibility = {});
    [[nodiscard]] const ParticleRenderExecutionStats& executionStats()
        const noexcept { return m_executionStats; }

    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }
    [[nodiscard]] size_t cachedTextureCount() const noexcept { return m_textureSrvs.size(); }
    [[nodiscard]] size_t textureCacheHighWater() const noexcept { return m_textureCacheHighWater; }

private:
    struct QuadVertex final {
        float corner[2];
        float uv[2];
    };

    struct GpuAbCountExpectation final {
        uint64_t authorityEpoch = 0;
        uint32_t cpuCompatibleCount = 0;
        uint32_t visibilitySignatureA = 0;
        uint32_t visibilitySignatureB = 0;
        bool valid = false;
    };

    struct GpuAbStateExpectation final {
        uint64_t authorityEpoch = 0;
        uint32_t count = 0;
        container::Array<GpuParticleReferenceSample,
            d3d12::performance_limits::kGpuParticleAbStateSampleCapacity>
            samples{};
        bool valid = false;
    };

    bool createRootSignature();
    bool loadShaderPackage();
    bool createPipelineStates();
    bool createGpuBillboardPipeline();
    bool createStaticQuad();
    bool createSmudgePipeline();
    [[nodiscard]] bool loadGpuBillboardShaderPackage();
    [[nodiscard]] std::optional<D3D12_GPU_VIRTUAL_ADDRESS>
    prepareGpuBillboardIndirect(
        const GpuParticleBillboardConstants& constants);
    [[nodiscard]] size_t recordGpuBillboardIndirectStage(
        D3D12_GPU_VIRTUAL_ADDRESS constants,
        fx::ParticleShader shader, bool visible);
    bool ensureSmudgeSceneTargets(uint32_t width, uint32_t height);
    void releaseSmudgeSceneTargets();
    [[nodiscard]] uint32_t textureSrv(container::StringView textureName);

    d3d12::D3D12Device* m_device = nullptr;
    container::SharedPtr<WorldTextureCache> m_textures;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_smudgeRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_gpuBillboardRootSignature;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature>
        m_gpuBillboardCommandSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_smudgePipelineState;
    container::Array<Microsoft::WRL::ComPtr<ID3D12PipelineState>,
               static_cast<size_t>(fx::ParticleShader::Count)> m_pipelineStates;
    container::Array<Microsoft::WRL::ComPtr<ID3D12PipelineState>,
               static_cast<size_t>(fx::ParticleShader::Count)>
        m_gpuBillboardPipelineStates;
    // Executes the full indirect/root/SRV path while suppressing color/depth
    // writes. CPU remains presentation authority until A/B parity and profile
    // gates pass.
    container::Array<Microsoft::WRL::ComPtr<ID3D12PipelineState>,
               static_cast<size_t>(fx::ParticleShader::Count)>
        m_gpuBillboardShadowPipelineStates;
    // Build-generated bytecode: particle VS/PS/alpha-test PS followed by
    // SMUDGE VS/PS. Retaining the blobs supports sampler/PSO rebuilds without
    // re-reading the package or invoking a runtime compiler.
    container::Array<container::Vector<uint8_t>, 5> m_shaderBytecode;
    container::Array<container::Vector<uint8_t>, 3>
        m_gpuBillboardShaderBytecode;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_quadVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_quadIndexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_quadVertexView{};
    D3D12_INDEX_BUFFER_VIEW m_quadIndexView{};
    container::Array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2>
        m_smudgeSceneTargets;
    container::Array<uint64_t, 2> m_smudgeSceneTargetAllocationBytes{};
    container::Array<uint32_t, 2> m_smudgeSceneSrvs{UINT32_MAX, UINT32_MAX};
    container::Array<D3D12_RESOURCE_STATES, 2> m_smudgeSceneStates{
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_COPY_DEST};
    uint32_t m_smudgeSceneWidth = 0;
    uint32_t m_smudgeSceneHeight = 0;
    SceneColorRenderStats m_sceneColorStats;
    container::HashMap<container::String, uint32_t> m_textureSrvs;
    uint64_t m_textureBindingGeneration = 1;
    size_t m_textureCacheHighWater = 0;
    uint32_t m_textureFilter = 2;
    uint32_t m_anisotropyLevel = 2;
    uint32_t m_sampleCount = 1;
    bool m_reportedGpuResetFailure = false;
    bool m_reportedGpuCommandFailure = false;
    bool m_reportedGpuDrawFailure = false;
    bool m_gpuBillboardShaderPackageReady = false;
    uint64_t m_gpuBillboardIndirectShadowExecuteCalls = 0;
    uint64_t m_gpuBillboardIndirectVisibleExecuteCalls = 0;
    uint64_t m_gpuPresentationFallbackFrames = 0;
    bool m_initialized = false;
    ParticleRenderExecutionStats m_executionStats;
    container::UniquePtr<ParticleRendererScratch> m_buildScratch;
    container::UniquePtr<GpuParticleSimulator> m_gpuSimulator;
    fx::ParticleGpuCommandBatch m_pendingGpuCommands;
    fx::ParticleGpuCommandBatch m_gpuCommandNormalizationWork;
    fx::ParticleGpuCommandNormalizationScratch
        m_gpuCommandNormalizationScratch;
    uint64_t m_lastQueuedGpuAuthoredSample = 0;
    uint32_t m_pendingGpuAuthoredFrames = 0;
    GpuParticleMaterialBinCompilation m_gpuMaterialBins;
    container::Vector<uint32_t> m_gpuMaterialBinTextureSrvs;
    uint64_t m_gpuMaterialBinTextureGeneration = 0;
    GpuParticlePresentationGateDecision m_gpuPresentationGate{};
    bool m_gpuVisibilityContractReady = false;
    bool m_gpuOutputParityVerified = false;
    bool m_gpuProfileApproved = false;
    size_t m_gpuProfileMinimumParticleCount = 0;
    uint64_t m_gpuQualificationRevision = 0;
    size_t m_gpuPresentationParticleCount = 0;
    size_t m_gpuPresentationParticleBudget = 0;
    container::Array<GpuAbCountExpectation, 2> m_gpuAbCountExpectations{};
    uint64_t m_gpuAbCountSamples = 0;
    uint64_t m_gpuAbCountMatches = 0;
    uint64_t m_gpuAbCountMismatches = 0;
    uint64_t m_gpuAbCountStaleSkipped = 0;
    uint32_t m_gpuAbCountConsecutiveFrames = 0;
    uint32_t m_gpuAbVisibilityConsecutiveFrames = 0;
    uint32_t m_gpuAbLastCpuCount = 0;
    uint32_t m_gpuAbLastGpuCount = 0;
    bool m_gpuAbLastCountValid = false;
    bool m_gpuPresentationRequested = false;
    container::Array<GpuAbStateExpectation, 2> m_gpuAbStateExpectations{};
    uint64_t m_gpuAbStateSamples = 0;
    uint64_t m_gpuAbStateMatches = 0;
    uint64_t m_gpuAbStateMismatches = 0;
    uint64_t m_gpuAbStateStaleSkipped = 0;
    uint32_t m_gpuAbStateConsecutiveFrames = 0;
    bool m_gpuAbLastStateValid = false;
};

} // namespace engine::render
