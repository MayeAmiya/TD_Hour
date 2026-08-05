#pragma once

#include "engine/renderer/world/resource/RenderAssetLifecycle.h"

#include <array>
#include <cstdint>

namespace engine::render {

// Fixed device-level timestamp ranges. The stable ordinals are shared by the
// command-recording API and its value-only result, so renderer passes can add
// markers without owning query heaps or readback resources.
enum class GpuTimestampRange : uint8_t {
    Frame = 0,
    OpaqueWorld,
    ParticleCompute,
    ParticleDraw,
    SceneColorCopy,
    Fxaa,
    Count,
};

inline constexpr uint32_t kGpuTimestampRangeCount =
    static_cast<uint32_t>(GpuTimestampRange::Count);

// Most recently consumed asynchronous GPU timestamp result. sourceFrameOrdinal
// names the submitted frame whose FRAME_COUNT ring slot became reusable; it is
// intentionally older than the CPU frame publishing this structure.
struct GpuTimestampRenderStats final {
    uint64_t sourceFrameOrdinal = 0;
    uint64_t timestampFrequencyHz = 0;
    uint64_t validRangeMask = 0;
    std::array<uint64_t, kGpuTimestampRangeCount> rangeMicroseconds{};
    bool infrastructureReady = false;
};

// Lifetime diagnostics for one renderer-owned scene-color copy pool. A
// resource remains resident here only while the owner retains it; handing it
// to D3D12Device's fence retirement queue decreases residentAllocationBytes
// immediately and is reported separately as a retirement request. Byte
// values come from ID3D12Device::GetResourceAllocationInfo, including the
// copiedBytes value for whole-resource CopyResource calls.
//
// Aggregation is intended for independent owners (currently the world-filter
// capture and particle SMUDGE captures). Current resident bytes are exact at
// the sample point. The aggregated lifetime high-water is the sum of each
// owner's independently observed peak, not a claim that all peaks overlapped.
struct SceneColorRenderStats final {
    uint64_t residentAllocationBytes = 0;
    uint64_t lifetimeResidentHighWaterBytes = 0;
    uint64_t allocationAttempts = 0;
    uint64_t allocationFailures = 0;
    uint64_t copyCalls = 0;
    uint64_t copiedBytes = 0;
    uint64_t releaseCalls = 0;
    uint64_t releasedAllocationBytes = 0;
    uint64_t retirementRequests = 0;
    uint64_t retirementRequestedBytes = 0;

    constexpr SceneColorRenderStats& operator+=(
        const SceneColorRenderStats& other) noexcept {
        residentAllocationBytes += other.residentAllocationBytes;
        lifetimeResidentHighWaterBytes +=
            other.lifetimeResidentHighWaterBytes;
        allocationAttempts += other.allocationAttempts;
        allocationFailures += other.allocationFailures;
        copyCalls += other.copyCalls;
        copiedBytes += other.copiedBytes;
        releaseCalls += other.releaseCalls;
        releasedAllocationBytes += other.releasedAllocationBytes;
        retirementRequests += other.retirementRequests;
        retirementRequestedBytes += other.retirementRequestedBytes;
        return *this;
    }
};

[[nodiscard]] constexpr SceneColorRenderStats operator+(
    SceneColorRenderStats left,
    const SceneColorRenderStats& right) noexcept {
    left += right;
    return left;
}

// Raw D3D12 command-list API calls recorded during one CPU frame. These are
// intentionally observation-only counters: repeated binds remain repeated
// calls here, so this value type can establish a baseline before any D06
// state-cache work is considered.
struct RenderBindingStats final {
    uint64_t frameOrdinal = 0;
    uint64_t pipelineStateCalls = 0;
    uint64_t graphicsRootSignatureCalls = 0;
    uint64_t computeRootSignatureCalls = 0;
    uint64_t graphicsDescriptorTableCalls = 0;
    uint64_t computeDescriptorTableCalls = 0;
    uint64_t vertexBufferCalls = 0;
    uint64_t indexBufferCalls = 0;
    uint64_t drawCalls = 0;
    uint64_t dispatchCalls = 0;
    uint64_t executeIndirectCalls = 0;
};

// Renderer-owner vectors that remain live across command-recording frames.
// A growth is counted once for a frame in which their aggregate retained
// capacity increased.  This deliberately observes reuse without prescribing
// a central allocator or changing any gameplay/snapshot ownership.
struct RetainedRenderScratchStats final {
    uint64_t capacityBytes = 0;
    uint64_t lifetimeHighWaterBytes = 0;
    uint32_t capacityGrowthFrames = 0;
};

// Renderer-owned counters for one sealed world-frame preparation. These are
// deliberately value-only diagnostics: no Taskflow task, ECS entity, GPU
// resource, or backend pointer crosses the public renderer boundary.
struct WorldPreparationStats final {
    uint64_t simulationFrame = 0;
    uint32_t inputEntities = 0;
    uint32_t chunkTaskCount = 0;
    uint32_t scheduledTaskCount = 0;
    uint32_t completedTaskCount = 0;
    uint32_t executorWorkerCapacity = 0;
    uint32_t activeWorkerCount = 0;
    bool activeWorkerObservationClamped = false;
    uint32_t entityGrain = 0;
    uint32_t preparedInstances = 0;
    uint32_t animatedInstances = 0;
    uint32_t hiddenInstances = 0;
    uint32_t invalidInstances = 0;
    uint32_t distanceCulledInstances = 0;
    uint32_t frustumCulledInstances = 0;
    uint32_t poseEvaluations = 0;
    uint32_t poseReuses = 0;
    uint32_t ordinaryPoseEvaluations = 0;
    uint32_t cameraPoseEvaluations = 0;
    uint32_t emitterPoseEvaluations = 0;
    uint32_t trackMarkPoseEvaluations = 0;
    uint32_t cameraPoseReuses = 0;
    uint32_t emitterPoseReuses = 0;
    uint32_t completionFallbacks = 0;
    uint32_t cameraPoseFallbacks = 0;
    uint32_t emitterRootFallbacks = 0;
    uint64_t poseJointsEvaluated = 0;
    uint64_t poseAnimationChannelsSampled = 0;
    uint64_t poseControlsApplied = 0;
    uint64_t poseEvaluationMicroseconds = 0;
    uint64_t ordinaryPoseMicroseconds = 0;
    uint64_t cameraPoseMicroseconds = 0;
    uint64_t emitterPoseMicroseconds = 0;
    uint64_t trackMarkPoseMicroseconds = 0;
    uint64_t poseArenaRequestedJoints = 0;
    uint64_t poseArenaAllocatedJoints = 0;
    uint32_t poseArenaRejectedInstances = 0;
    uint32_t poseArenaGrowths = 0;
    uint32_t visibilityArenaGrowths = 0;
    uint64_t poseArenaCapacityBytes = 0;
    uint64_t visibilityArenaCapacityBytes = 0;
    uint64_t poseArenaCapacityHighWaterBytes = 0;
    uint64_t visibilityArenaCapacityHighWaterBytes = 0;
    uint64_t preparedPaletteBytes = 0;
    uint64_t preparedVisibilityBytes = 0;
    // Retained outer-container storage used by extraction/preparation. Nested
    // pose/visibility payload bytes are reported separately above.
    uint64_t retainedContainerCapacityBytes = 0;
    uint64_t retainedContainerCapacityHighWaterBytes = 0;
    uint32_t containerCapacityGrowths = 0;
    uint64_t retainedNestedCapacityBytes = 0;
    uint64_t retainedNestedCapacityHighWaterBytes = 0;
    uint32_t nestedCapacityGrowthFrames = 0;
    uint64_t elapsedMicroseconds = 0;
};

// Command-recording counters for the ordinary W3D/terrain world pass. A
// packet is the CPU material/primitive request; a draw call may consume many
// compatible rigid packets after renderer-local instancing.
struct StaticMeshRenderStats final {
    uint32_t submittedPackets = 0;
    uint32_t validPackets = 0;
    uint32_t skippedPackets = 0;
    uint32_t drawCalls = 0;
    uint32_t instancedDrawCalls = 0;
    uint32_t renderedInstances = 0;
    uint32_t pipelineStateChanges = 0;
    uint64_t renderedTriangles = 0;
    uint32_t instanceScratchCapacity = 0;
    uint32_t instanceScratchCapacityHighWater = 0;
    uint32_t instanceScratchCapacityGrowths = 0;
    uint32_t skinPaletteScratchCapacity = 0;
    uint32_t skinPaletteScratchCapacityHighWater = 0;
    uint32_t skinPaletteScratchCapacityGrowths = 0;
    uint32_t skinPaletteUploadHits = 0;
    uint32_t skinPaletteUploadMisses = 0;
    uint64_t skinPaletteUploadBytes = 0;
    uint32_t restPalettesBuilt = 0;
    uint32_t restPalettesReused = 0;
    uint64_t restPaletteJointsMaterialized = 0;
    uint32_t poseBindingGenerationRejects = 0;
    uint32_t shadowCasterPackets = 0;
    uint32_t shadowDrawCalls = 0;
    uint64_t shadowTriangles = 0;
    uint32_t shadowCasterScratchCapacity = 0;
    uint32_t shadowCasterScratchCapacityHighWater = 0;
    uint32_t shadowCasterScratchCapacityGrowths = 0;
    uint32_t shadowPolicyRejectedPackets = 0;
    uint32_t shadowInvalidPackets = 0;
    uint32_t dynamicPointLights = 0;
    uint32_t dynamicLightReceivingPackets = 0;
    uint32_t scenePointLights = 0;
    uint32_t sceneLightReceivingPackets = 0;
    bool shadowAvailable = false;
    bool shadowValid = false;
    bool shadowFallbackBound = true;
    uint64_t visibilityRevision = 0;
    uint32_t visibilityTextureWidth = 0;
    uint32_t visibilityTextureHeight = 0;
    uint32_t visibilityUploadedCells = 0;
    uint64_t visibilityUploadedBytes = 0;
    bool visibilityEnabled = false;
    bool visibilityFallbackBound = true;
};

// One frame's generic upload-arena use. The primary segment is persistently
// mapped with the device; spill pages are renderer infrastructure shared by
// particles, W3D instances, skin palettes, trails, shadows and future passes.
struct FrameUploadRenderStats final {
    uint64_t frameOrdinal = 0;
    uint32_t primaryCapacityBytes = 0;
    uint32_t primaryBytesUsed = 0;
    uint32_t spillCapacityBytes = 0;
    uint32_t spillBytesUsed = 0;
    uint64_t requestedBytes = 0;
    uint64_t uploadedBytes = 0;
    uint64_t rejectedBytes = 0;
    uint32_t allocationCount = 0;
    uint32_t spillAllocationCount = 0;
    uint32_t failedAllocationCount = 0;
    uint32_t spillPageCount = 0;
    uint32_t spillPageGrowthCount = 0;
    uint32_t spillPageReuseAllocationCount = 0;
    uint64_t allocationCpuNanoseconds = 0;
    uint64_t copyCpuNanoseconds = 0;
    uint32_t lifetimePrimaryHighWaterBytes = 0;
    uint32_t lifetimeSpillHighWaterBytes = 0;
};

struct SrvDescriptorRenderStats final {
    uint32_t capacity = 0;
    uint32_t allocated = 0;
    uint32_t retiring = 0;
    uint32_t available = 0;
    uint32_t lifetimeHighWater = 0;
    uint64_t allocationFailures = 0;
    uint64_t pressureWarnings = 0;
};

// Device-owned fence retirement ledger. "Pending" has been retired by an
// owner while the current submission still has no fence; "fenced" has a
// concrete last-possible-use fence and is waiting for GPU completion.
struct GpuRetirementRenderStats final {
    uint64_t frameOrdinal = 0;
    uint32_t pendingResources = 0;
    uint32_t fencedResources = 0;
    uint64_t pendingResourceBytes = 0;
    uint64_t fencedResourceBytes = 0;
    uint32_t pendingSrvDescriptors = 0;
    uint32_t fencedSrvDescriptors = 0;
    uint64_t latestSrvUseFrame = 0;
    uint64_t latestSrvUseFence = 0;
    uint64_t lastSealedRetirementFence = 0;
    uint64_t completedFence = 0;
    uint64_t oldestRetireRequestFrame = 0;
    uint64_t reclaimedResources = 0;
    uint64_t reclaimedResourceBytes = 0;
    uint64_t reclaimedSrvDescriptors = 0;
    uint64_t rejectedRetirements = 0;
    uint32_t attributedResources = 0;
    uint32_t attributedSrvDescriptors = 0;
    uint64_t latestResourceUseFrame = 0;
    uint64_t latestResourceUseFence = 0;
    uint32_t completedResourceUsesWithoutExactFence = 0;
};

// Per-frame ledger for the device-owned world color target. These counters
// describe calls and actual state transitions separately, so a 1x path can
// prove that the explicit post-process boundary was reached without claiming
// that a multisample resolve or its barriers executed.
struct WorldResourceStateRenderStats final {
    uint64_t frameOrdinal = 0;
    uint32_t sampleCount = 1;
    uint32_t beginCalls = 0;
    uint32_t beginFailures = 0;
    uint32_t resolveCalls = 0;
    uint32_t resolveExecutions = 0;
    uint32_t resolveNoopSingleSample = 0;
    uint32_t resolveNoopInactive = 0;
    uint32_t resolveFailures = 0;
    uint32_t transitionBarriers = 0;
    bool worldPassBegan = false;
    bool frameOpen = false;
    bool multisamplePassActive = false;
    bool presentationTargetRestored = false;
};

// Capability-resolved Display settings and the last output state SDL/D3D12
// actually accepted. Keeping both sides prevents a failed fullscreen/mode
// change from being reported as effective merely because normalization
// succeeded before the platform call.
struct DisplayOutputRenderStats final {
    uint64_t revision = 0;
    uint32_t requestedWidth = 0;
    uint32_t requestedHeight = 0;
    uint32_t requestedMode = 0;
    uint32_t requestedRefreshRateHz = 0;
    uint32_t effectiveWidth = 0;
    uint32_t effectiveHeight = 0;
    uint32_t effectiveMode = 0;
    uint32_t effectiveRefreshRateHz = 0;
    uint32_t appliedWidth = 0;
    uint32_t appliedHeight = 0;
    uint32_t appliedMode = 0;
    uint32_t appliedRefreshRateHz = 0;
    uint32_t pixelWidth = 0;
    uint32_t pixelHeight = 0;
    uint32_t changeMask = 0;
    uint32_t capabilityFallbackMask = 0;
    uint64_t appliedOutputRevision = 0;
    uint64_t lastOutputAttemptRevision = 0;
    uint64_t outputApplyAttempts = 0;
    uint64_t outputApplySucceeded = 0;
    uint64_t outputApplyFailed = 0;
    bool lastOutputApplySucceeded = false;
    bool hasAppliedOutput = false;
    bool pixelExtentValid = false;
    bool appliedMatchesEffective = false;
};

// Cumulative diagnostics for the renderer-owned GPU particle shadow state.
// The effective simulation/render backend remains CPU until visibility,
// A/B and profile gates pass; these counters prove Compute/indirect command
// recording, resource ordering and authority-epoch synchronization.
struct GpuParticleSimulationRenderStats final {
    uint64_t resetDispatches = 0;
    uint64_t commandDispatches = 0;
    uint64_t integrationDispatches = 0;
    uint64_t aliveCompactDispatches = 0;
    uint64_t aliveCompactCounterResetDispatches = 0;
    uint64_t visibleCompactDispatches = 0;
    uint64_t visibleCompactCounterResetDispatches = 0;
    uint64_t materialBinResetDispatches = 0;
    uint64_t materialBinCountDispatches = 0;
    uint64_t materialBinPrefixDispatches = 0;
    uint64_t materialBinScatterDispatches = 0;
    uint64_t indirectArgumentBuilds = 0;
    uint64_t indirectArgumentTransitions = 0;
    uint64_t indirectDrawResourceTransitions = 0;
    uint64_t indirectShadowExecuteCalls = 0;
    uint64_t indirectVisibleExecuteCalls = 0;
    uint64_t presentationFallbackFrames = 0;
    uint64_t visibilityAuthorityPublishes = 0;
    uint64_t visibilityAuthorityEntries = 0;
    uint64_t visibilityAuthorityBytes = 0;
    uint64_t visibilityAuthorityRejects = 0;
    uint64_t abCountReadbacks = 0;
    uint64_t abCountSamples = 0;
    uint64_t abCountMatches = 0;
    uint64_t abCountMismatches = 0;
    uint64_t abCountStaleSkipped = 0;
    uint32_t abLastCpuCount = 0;
    uint32_t abLastGpuCount = 0;
    bool abLastCountValid = false;
    uint32_t abCountConsecutiveFrames = 0;
    uint32_t abVisibilityConsecutiveFrames = 0;
    uint64_t abStateReadbacks = 0;
    uint64_t abStateCopies = 0;
    uint64_t abStateSamples = 0;
    uint64_t abStateMatches = 0;
    uint64_t abStateMismatches = 0;
    uint64_t abStateStaleSkipped = 0;
    bool abLastStateValid = false;
    uint32_t abStateConsecutiveFrames = 0;
    uint64_t submittedBirthCommands = 0;
    uint64_t submittedRetireCommands = 0;
    uint64_t rejectedCommands = 0;
    uint64_t transitionBarriers = 0;
    uint64_t uavBarriers = 0;
    size_t normalizationSlotScratchCapacity = 0;
    size_t normalizationSlotScratchHighWater = 0;
    size_t normalizationBirthScratchCapacity = 0;
    size_t normalizationRetireScratchCapacity = 0;
    uint64_t normalizationScratchCapacityGrowths = 0;
    uint32_t aliveIndexCapacity = 0;
    uint32_t visibleIndexCapacity = 0;
    uint32_t materialTemplateMapCount = 0;
    uint32_t materialBinCount = 0;
    uint32_t materialBinCapacity = 0;
    // Alive/overflow remain GPU-resident. These values are intentionally not
    // populated by the runtime because doing so would impose a frame readback.
    uint32_t diagnosticAliveCount = 0;
    uint32_t diagnosticAliveOverflow = 0;
    bool diagnosticAliveCountsValid = false;
    uint32_t diagnosticVisibleCount = 0;
    uint32_t diagnosticVisibleOverflow = 0;
    bool diagnosticVisibleCountsValid = false;
    uint32_t capacity = 0;
    uint64_t authorityEpoch = 0;
    bool infrastructureReady = false;
    bool indirectShadowDrawReady = false;
    uint32_t presentationRejectionMask = 0;
    uint32_t presentationParticleCount = 0;
    uint32_t presentationParticleBudget = 0;
    bool effectiveGpuPresentation = false;
    uint64_t qualificationRevision = 0;
    uint32_t profileMinimumParticleCount = 0;
    bool outputParityApproved = false;
    bool profileApproved = false;
};

struct WorldPassTimingRenderStats final {
    uint64_t particleUpdateMicroseconds = 0;
    uint64_t particlePrepareMicroseconds = 0;
    uint64_t terrainResourceMicroseconds = 0;
    bool terrainResourceSkipped = false;
    uint64_t skyboxMicroseconds = 0;
    bool skyboxSkipped = false;
    uint64_t terrainPacketMicroseconds = 0;
    bool terrainPacketSkipped = false;
    uint64_t terrainBridgeMicroseconds = 0;
    bool terrainBridgeSkipped = false;
    uint64_t objectPacketMicroseconds = 0;
    bool objectPacketSkipped = false;
    uint64_t worldOverlayPacketMicroseconds = 0;
    bool worldOverlayPacketSkipped = false;
    uint64_t opaqueWorldMicroseconds = 0;
    bool opaqueWorldSkipped = false;
    uint64_t projectorPrepareMicroseconds = 0;
    bool projectorPrepareSkipped = false;
    uint64_t worldEffectsMicroseconds = 0;
    bool worldEffectsSkipped = false;
    uint64_t worldPostMicroseconds = 0;
    bool worldPostSkipped = false;
    uint64_t statsPublicationMicroseconds = 0;
    bool statsPublicationSkipped = false;
};

enum class StartupSceneTicketState : uint8_t {
    Inactive,
    Pending,
    Ready,
    Degraded,
    Failed,
};

// Generation-bound startup barrier. Required products gate Loading; optional
// visible assets may continue streaming after the first world frame and only
// contribute degraded diagnostics when they fail terminally.
struct StartupSceneTicketRenderStats final {
    uint64_t presentationEpoch = 0;
    uint64_t sessionRevision = 0;
    uint64_t loadingRevision = 0;
    StartupSceneTicketState state = StartupSceneTicketState::Inactive;
    uint32_t requiredTotal = 0;
    uint32_t requiredReady = 0;
    uint32_t requiredPending = 0;
    uint32_t requiredFailed = 0;
    uint32_t optionalTotal = 0;
    uint32_t optionalReady = 0;
    uint32_t optionalPending = 0;
    uint32_t optionalDegraded = 0;
};

// Aggregated presentation diagnostics for the most recently submitted world
// frame. UI batching remains device-local; this contract covers the world
// systems whose budgets and batching decisions need gameplay-scale tuning.
struct WorldFrameRenderStats final {
    uint64_t simulationFrame = 0;
    uint64_t worldRevision = 0;
    uint64_t viewRevision = 0;
    float interpolationAlpha = 1.0f;
    uint64_t presentationEpoch = 0;
    uint64_t sessionRevision = 0;
    uint64_t loadingRevision = 0;
    uint64_t terrainRevision = 0;
    bool startupSceneReady = false;
    // Terminal startup failure is generation-scoped.  It is deliberately
    // separate from ordinary optional asset fallbacks: only a required world
    // product such as the initial terrain may fail the Loading barrier.
    bool startupSceneFailed = false;
    StartupSceneTicketRenderStats startupSceneTicket;
    WorldPassTimingRenderStats passTimings;
    WorldResourceStateRenderStats worldResources;
    GpuTimestampRenderStats gpuTimestamps;
    RenderBindingStats renderBindings;
    SceneColorRenderStats sceneColor;
    RetainedRenderScratchStats retainedScratch;
    DisplayOutputRenderStats displayOutput;
    RenderAssetLifecycleStats assets;
    WorldPreparationStats preparation;
    StaticMeshRenderStats staticMeshes;
    FrameUploadRenderStats frameUpload;
    SrvDescriptorRenderStats srvDescriptors;
    GpuRetirementRenderStats gpuRetirement;
    uint32_t staticBufferPageCount = 0;
    uint32_t staticBufferOversizedPageCount = 0;
    uint32_t staticBufferActiveSliceCount = 0;
    uint32_t staticBufferRetiringSliceCount = 0;
    uint64_t staticBufferPageCapacityBytes = 0;
    uint64_t staticBufferLiveLogicalBytes = 0;
    uint32_t staticBufferPendingCopyCount = 0;
    uint64_t staticBufferPendingCopyBytes = 0;
    uint32_t particleInstances = 0;
    uint32_t particleBatches = 0;
    uint32_t smudgeInstances = 0;
    uint32_t particleSourceCount = 0;
    uint32_t particleRejectedInvalid = 0;
    uint32_t particleRejectedVisibility = 0;
    uint32_t particleRejectedColor = 0;
    uint32_t particleRejectedSourceBudget = 0;
    uint32_t particleRejectedBudget = 0;
    uint32_t particleRoutedDrawable = 0;
    uint32_t particleInstanceCapacity = 0;
    uint32_t particleBatchCapacity = 0;
    uint32_t smudgeInstanceCapacity = 0;
    uint32_t particleSourcePriorityScratchCapacity = 0;
    uint32_t particleSourceOrdinalScratchCapacity = 0;
    uint32_t particleCandidateScratchCapacity = 0;
    uint32_t particleStreakPointScratchCapacity = 0;
    uint32_t particleSourcePriorityScratchHighWater = 0;
    uint32_t particleSourceOrdinalScratchHighWater = 0;
    uint32_t particleCandidateScratchHighWater = 0;
    uint32_t particleStreakPointScratchHighWater = 0;
    uint32_t particleScratchCapacityGrowths = 0;
    uint32_t particleScratchContainersReused = 0;
    uint32_t particleScratchHardCapRejected = 0;
    uint32_t particleGpuReferenceEligibleSources = 0;
    uint32_t particleGpuReferenceSelectedInstances = 0;
    uint64_t particleSourceSelectionMicroseconds = 0;
    uint64_t particleExpansionMicroseconds = 0;
    uint64_t particleSortMicroseconds = 0;
    uint64_t particlePackMicroseconds = 0;
    uint64_t particleTextureBindingMicroseconds = 0;
    uint64_t particleInstanceUploadBytes = 0;
    uint64_t particleInstanceUploadMicroseconds = 0;
    uint64_t particleDrawRecordMicroseconds = 0;
    uint64_t particleSmudgeUploadBytes = 0;
    uint64_t particleSmudgeUploadMicroseconds = 0;
    uint64_t particleSmudgeDrawRecordMicroseconds = 0;
    uint32_t particleCpuDrawCalls = 0;
    uint32_t particleGpuIndirectDrawCalls = 0;
    uint32_t particleSmudgeDrawCalls = 0;
    uint32_t particleIntegrated = 0;
    uint32_t particleCompacted = 0;
    uint64_t particlePhaseSampleOrdinal = 0;
    uint32_t particleAuthoredFrames = 0;
    uint32_t particleIntegrationBlocks = 0;
    uint32_t particleIntegrationTasks = 0;
    uint64_t particleEmitterUpdateMicroseconds = 0;
    uint64_t particleIntegrationMicroseconds = 0;
    uint64_t particleCompactMicroseconds = 0;
    bool particleParallelIntegration = false;
    GpuParticleSimulationRenderStats gpuParticles;
    uint32_t fxaaPasses = 0;
    bool fxaaEnabled = false;
    uint32_t terrainVisibleChunks = 0;
    uint32_t terrainCulledChunks = 0;
    // Dedicated semantic overlay passes, kept separate from ordinary static
    // mesh packet counts so bridge/Bib device probes can prove that the
    // correct pass survived culling, upload and shroud binding.
    uint32_t terrainBridgeDrawPackets = 0;
    uint32_t terrainBibDrawPackets = 0;
    uint32_t residentTextures = 0;
    uint64_t residentTextureBytes = 0;
    uint64_t textureLatestUsedFrame = 0;
    uint64_t textureLatestUsedFence = 0;
    uint64_t textureCacheHits = 0;
    uint64_t textureCacheMisses = 0;
    uint64_t textureGpuUploads = 0;
    uint64_t textureFallbackResolutions = 0;
    uint64_t textureFailedAcquisitions = 0;
    uint32_t textureCpuQueuedJobs = 0;
    uint32_t textureCpuActiveJobs = 0;
    uint32_t textureCpuPendingVariants = 0;
    uint32_t textureCpuPreparedVariants = 0;
    uint32_t textureCpuFailedVariants = 0;
    uint64_t textureCpuStaleCompletions = 0;
    uint64_t textureCpuCompletedJobs = 0;
    uint64_t textureCpuPreparedBytes = 0;
    uint64_t textureCpuWorkerNanoseconds = 0;
    uint64_t textureCpuCancelledVariants = 0;
    uint64_t textureCpuCancelledReady = 0;
    uint64_t textureCpuCancelRequestedActive = 0;
    uint32_t textureCpuMaximumQueueAge = 0;
    uint64_t textureCpuRetainedPreparedBytes = 0;
    uint64_t textureCpuReclaimedPreparedBytes = 0;
    uint64_t textureCpuReclaimedSourceBytes = 0;
    uint64_t textureCpuReclaimedSources = 0;
    uint32_t textureGpuQueuedUploads = 0;
    uint64_t textureGpuUploadAttempts = 0;
    uint64_t textureGpuUploadDeferred = 0;
    uint64_t textureGpuUploadForcedOversized = 0;
    uint64_t textureGpuUploadAttemptedBytes = 0;
    uint64_t textureGpuUploadDeferredBytes = 0;
    uint64_t textureGpuUploadNanoseconds = 0;
    uint64_t textureGpuUploadCancelled = 0;
    uint32_t textureGpuUploadMaximumAge = 0;
    uint64_t textureResidencyEvictions = 0;
    uint64_t textureResidencyEvictedBytes = 0;
    uint64_t textureResidencyOwnerRejects = 0;
    uint64_t textureResidencyPinnedRejects = 0;
    uint32_t textureResidencyPins = 0;
    uint64_t modelGpuLatestUsedFrame = 0;
    uint64_t modelGpuLatestUsedFence = 0;
    uint64_t modelGpuUseFencesInFlight = 0;
    uint64_t modelGpuCompletedUsesWithoutExactFence = 0;
    uint64_t modelResidencyEvictions = 0;
    uint64_t modelResidencyEvictedBytes = 0;
    uint64_t modelResidencyPinnedRejects = 0;
    uint64_t modelResidencyReferencedRejects = 0;
    uint32_t modelResidencyPins = 0;
    uint64_t modelRetainedSortingBytes = 0;
    uint32_t modelUploadAttempts = 0;
    uint32_t modelUploadSucceeded = 0;
    uint32_t modelUploadFailed = 0;
    uint32_t modelUploadDeferred = 0;
    uint32_t modelUploadForcedOversized = 0;
    uint64_t modelUploadEstimatedBytes = 0;
    uint64_t modelUploadDeferredBytes = 0;
    uint64_t modelUploadMicroseconds = 0;
    uint32_t modelCpuLoadQueuedJobs = 0;
    uint32_t modelCpuLoadPendingCompletions = 0;
    uint32_t modelCpuLoadsInFlight = 0;
    uint64_t modelCpuLoadPublishedCompletions = 0;
    uint64_t modelCpuLoadFailedCompletions = 0;
    uint64_t modelCpuLoadCancelledJobs = 0;
    uint64_t modelCpuLoadCancelledCompletions = 0;
    uint64_t modelCpuLoadDiscardedStaleCompletions = 0;
    uint32_t modelCpuLoadMaximumQueueAge = 0;
    uint64_t modelCpuReadyBytes = 0;
    uint64_t modelCpuReadyWorkerNanoseconds = 0;
    uint64_t modelCpuReadyPublishMicroseconds = 0;
    uint64_t modelCpuReadyDeferred = 0;
    uint64_t modelCpuReadyForcedOversized = 0;
    uint32_t modelCpuReadyMaximumAge = 0;
    uint64_t animationReadyBytes = 0;
    uint64_t animationReadyWorkerNanoseconds = 0;
    uint64_t animationReadyPublishMicroseconds = 0;
    uint64_t animationReadyDeferred = 0;
    uint64_t animationReadyForcedOversized = 0;
    uint32_t animationMaximumReadyAge = 0;
    uint32_t projectileTrails = 0;
    uint32_t projectileTrailSegments = 0;
    uint32_t trackMarkStreams = 0;
    uint32_t trackMarkSegments = 0;
    uint32_t projectedShadows = 0;
    uint32_t projectedShadowDrawCalls = 0;
    uint32_t groundProjectorTextureBatches = 0;
    uint32_t groundProjectorResidentTextures = 0;
    uint32_t groundProjectorBudgetRejected = 0;
    uint32_t snowflakeInstances = 0;
    uint32_t visibilityHiddenEntities = 0;
    uint32_t visibilityHiddenProjectiles = 0;
    uint32_t visibilityRejectedFxObjects = 0;
    uint32_t visibilityRejectedFxInvocations = 0;
};

} // namespace engine::render
