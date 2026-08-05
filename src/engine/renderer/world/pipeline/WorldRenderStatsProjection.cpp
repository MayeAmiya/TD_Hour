#include "engine/renderer/world/pipeline/WorldRenderStatsOwner.h"

#include "engine/fx/runtime/ParticleRuntime.h"
#include "engine/renderer/world/particle/ParticleRenderer.h"

#include <algorithm>
#include <limits>
#include <type_traits>

namespace engine::render {
namespace {

template <typename Value>
[[nodiscard]] uint32_t boundedU32(Value value) noexcept {
    using Common = std::common_type_t<Value, uint64_t>;
    return static_cast<uint32_t>(std::min<Common>(
        static_cast<Common>(value),
        static_cast<Common>(std::numeric_limits<uint32_t>::max())));
}

} // namespace

void WorldRenderStatsOwner::projectParticleFrameStats(
    const ParticleRenderDrawList& drawList,
    size_t particleDrawCount,
    size_t smudgeDrawCount,
    const fx::ParticleRuntimePhaseProfile& phase,
    const GpuParticleSimulationRenderStats& gpu,
    const ParticleRenderExecutionStats& execution) noexcept {
    WorldFrameRenderStats& stats = m_frame;
    stats.particleInstances = boundedU32(particleDrawCount);
    stats.particleBatches = boundedU32(drawList.batches.size());
    stats.smudgeInstances = boundedU32(smudgeDrawCount);
    stats.particleSourceCount = boundedU32(drawList.stats.sourceParticles);
    stats.particleRejectedInvalid = boundedU32(
        drawList.stats.rejectedInvalid);
    stats.particleRejectedVisibility = boundedU32(
        drawList.stats.rejectedVisibility);
    stats.particleRejectedColor = boundedU32(
        drawList.stats.rejectedColor);
    stats.particleRejectedSourceBudget = boundedU32(
        drawList.stats.rejectedSourceBudget);
    stats.particleRejectedBudget = boundedU32(
        drawList.stats.rejectedBudget);
    stats.particleRoutedDrawable = boundedU32(
        drawList.stats.routedDrawableParticles);
    stats.particleInstanceCapacity = boundedU32(drawList.instances.capacity());
    stats.particleBatchCapacity = boundedU32(drawList.batches.capacity());
    stats.smudgeInstanceCapacity =
        boundedU32(drawList.smudgeInstances.capacity());
    stats.particleSourcePriorityScratchCapacity =
        boundedU32(drawList.stats.sourcePriorityScratchCapacity);
    stats.particleSourceOrdinalScratchCapacity =
        boundedU32(drawList.stats.sourceOrdinalScratchCapacity);
    stats.particleCandidateScratchCapacity =
        boundedU32(drawList.stats.candidateScratchCapacity);
    stats.particleStreakPointScratchCapacity =
        boundedU32(drawList.stats.streakPointScratchCapacity);
    stats.particleSourcePriorityScratchHighWater =
        boundedU32(drawList.stats.sourcePriorityScratchHighWater);
    stats.particleSourceOrdinalScratchHighWater =
        boundedU32(drawList.stats.sourceOrdinalScratchHighWater);
    stats.particleCandidateScratchHighWater =
        boundedU32(drawList.stats.candidateScratchHighWater);
    stats.particleStreakPointScratchHighWater =
        boundedU32(drawList.stats.streakPointScratchHighWater);
    stats.particleScratchCapacityGrowths =
        boundedU32(drawList.stats.scratchCapacityGrowths);
    stats.particleScratchContainersReused =
        boundedU32(drawList.stats.scratchContainersReused);
    stats.particleScratchHardCapRejected =
        boundedU32(drawList.stats.scratchHardCapRejected);
    stats.particleGpuReferenceEligibleSources =
        boundedU32(drawList.stats.gpuCompatibleEligibleSources);
    stats.particleGpuReferenceSelectedInstances =
        boundedU32(drawList.stats.gpuCompatibleSelectedInstances);
    stats.particleSourceSelectionMicroseconds =
        drawList.stats.sourceSelectionMicroseconds;
    stats.particleExpansionMicroseconds = drawList.stats.expansionMicroseconds;
    stats.particleSortMicroseconds = drawList.stats.sortMicroseconds;
    stats.particlePackMicroseconds = drawList.stats.packMicroseconds;
    stats.particleTextureBindingMicroseconds =
        drawList.stats.textureBindingMicroseconds;
    stats.particleInstanceUploadBytes = execution.instanceUploadBytes;
    stats.particleInstanceUploadMicroseconds =
        execution.instanceUploadMicroseconds;
    stats.particleDrawRecordMicroseconds = execution.drawRecordMicroseconds;
    stats.particleSmudgeUploadBytes = execution.smudgeUploadBytes;
    stats.particleSmudgeUploadMicroseconds =
        execution.smudgeUploadMicroseconds;
    stats.particleSmudgeDrawRecordMicroseconds =
        execution.smudgeDrawRecordMicroseconds;
    stats.particleCpuDrawCalls = execution.cpuDrawCalls;
    stats.particleGpuIndirectDrawCalls = execution.gpuIndirectDrawCalls;
    stats.particleSmudgeDrawCalls = execution.smudgeDrawCalls;
    stats.particleIntegrated = boundedU32(phase.integratedParticles);
    stats.particleCompacted = boundedU32(phase.compactedDeadParticles);
    stats.particlePhaseSampleOrdinal = phase.sampleOrdinal;
    stats.particleAuthoredFrames = phase.authoredFrames;
    stats.particleIntegrationBlocks = boundedU32(phase.activeBlocks);
    stats.particleIntegrationTasks = boundedU32(phase.integrationTasks);
    stats.particleEmitterUpdateMicroseconds =
        phase.emitterUpdateNanoseconds / 1000u;
    stats.particleIntegrationMicroseconds =
        phase.integrationNanoseconds / 1000u;
    stats.particleCompactMicroseconds =
        phase.serialCompactNanoseconds / 1000u;
    stats.particleParallelIntegration = phase.parallelIntegration;
    stats.gpuParticles = gpu;
}

void WorldRenderStatsOwner::projectAssetCacheFrameStats(
    const W3dGpuUploadBatchStats& upload) noexcept {
    WorldFrameRenderStats& stats = m_frame;
    const auto& texture = m_textureSample;
    const auto& model = m_modelSample;
    const auto& animation = m_animationSample;
    stats.residentTextures = boundedU32(texture.residentTextures);
    stats.residentTextureBytes = texture.residentBytes;
    stats.textureLatestUsedFrame = texture.latestUsedFrame;
    stats.textureLatestUsedFence = texture.latestUsedFence;
    stats.textureCacheHits = texture.cacheHits;
    stats.textureCacheMisses = texture.cacheMisses;
    stats.textureGpuUploads = texture.gpuUploads;
    stats.textureFallbackResolutions = texture.fallbackResolutions;
    stats.textureFailedAcquisitions = texture.failedAcquisitions;
    stats.textureCpuQueuedJobs = boundedU32(texture.sourceQueuedJobs);
    stats.textureCpuActiveJobs = boundedU32(texture.sourceActiveJobs);
    stats.textureCpuPendingVariants =
        boundedU32(texture.sourcePendingVariants);
    stats.textureCpuPreparedVariants =
        boundedU32(texture.sourcePreparedVariants);
    stats.textureCpuFailedVariants =
        boundedU32(texture.sourceFailedVariants);
    stats.textureCpuStaleCompletions = texture.sourceStaleCompletions;
    stats.textureCpuCompletedJobs = texture.sourceCompletedCpuJobs;
    stats.textureCpuPreparedBytes = texture.sourcePreparedBytes;
    stats.textureCpuWorkerNanoseconds = texture.sourceWorkerNanoseconds;
    stats.textureCpuCancelledVariants = texture.sourceCancelledVariants;
    stats.textureCpuCancelledReady = texture.sourceCancelledReady;
    stats.textureCpuCancelRequestedActive =
        texture.sourceCancelRequestedActive;
    stats.textureCpuMaximumQueueAge = texture.sourceMaximumQueueAge;
    stats.textureCpuRetainedPreparedBytes =
        texture.sourceRetainedPreparedBytes;
    stats.textureCpuReclaimedPreparedBytes =
        texture.sourceReclaimedPreparedBytes;
    stats.textureCpuReclaimedSourceBytes = texture.sourceReclaimedBytes;
    stats.textureCpuReclaimedSources = texture.sourceReclaimedCount;
    stats.textureGpuQueuedUploads = boundedU32(texture.queuedGpuUploads);
    stats.textureGpuUploadAttempts = texture.gpuUploadAttempts;
    stats.textureGpuUploadDeferred = texture.gpuUploadDeferred;
    stats.textureGpuUploadForcedOversized =
        texture.gpuUploadForcedOversized;
    stats.textureGpuUploadAttemptedBytes = texture.gpuUploadAttemptedBytes;
    stats.textureGpuUploadDeferredBytes = texture.gpuUploadDeferredBytes;
    stats.textureGpuUploadNanoseconds = texture.gpuUploadNanoseconds;
    stats.textureGpuUploadCancelled = texture.gpuUploadCancelled;
    stats.textureGpuUploadMaximumAge = texture.gpuUploadMaximumAge;
    stats.textureResidencyEvictions = texture.residencyEvictions;
    stats.textureResidencyEvictedBytes = texture.residencyEvictedBytes;
    stats.textureResidencyOwnerRejects = texture.residencyOwnerRejects;
    stats.textureResidencyPinnedRejects = texture.residencyPinnedRejects;
    stats.textureResidencyPins = texture.residencyPins;

    stats.modelGpuLatestUsedFrame = model.gpuLatestUsedFrame;
    stats.modelGpuLatestUsedFence = model.gpuLatestUsedFence;
    stats.modelGpuUseFencesInFlight = model.gpuUseFencesInFlight;
    stats.modelGpuCompletedUsesWithoutExactFence =
        model.gpuCompletedUseFramesWithoutExactFence;
    stats.modelResidencyEvictions = model.gpuResidencyEvictions;
    stats.modelResidencyEvictedBytes = model.gpuResidencyEvictedBytes;
    stats.modelResidencyPinnedRejects = model.gpuResidencyPinnedRejects;
    stats.modelResidencyReferencedRejects =
        model.gpuResidencyReferencedRejects;
    stats.modelResidencyPins = model.gpuResidencyPins;
    stats.modelRetainedSortingBytes = model.gpuRetainedSortingBytes;
    stats.modelUploadAttempts = boundedU32(upload.attemptedUploads);
    stats.modelUploadSucceeded = boundedU32(upload.succeededUploads);
    stats.modelUploadFailed = boundedU32(upload.failedUploads);
    stats.modelUploadDeferred = boundedU32(upload.deferredUploads);
    stats.modelUploadForcedOversized =
        boundedU32(upload.forcedOversizedUploads);
    stats.modelUploadEstimatedBytes = upload.attemptedEstimatedBytes;
    stats.modelUploadDeferredBytes = upload.deferredEstimatedBytes;
    stats.modelUploadMicroseconds =
        upload.attemptedElapsedNanoseconds / 1000u;
    stats.modelCpuLoadQueuedJobs = boundedU32(model.queuedCpuLoadJobs);
    stats.modelCpuLoadPendingCompletions =
        boundedU32(model.pendingCpuLoadCompletions);
    stats.modelCpuLoadsInFlight = boundedU32(model.cpuLoadsInFlight);
    stats.modelCpuLoadPublishedCompletions =
        model.publishedCpuLoadCompletions;
    stats.modelCpuLoadFailedCompletions = model.failedCpuLoadCompletions;
    stats.modelCpuLoadCancelledJobs = model.cancelledQueuedCpuLoadJobs;
    stats.modelCpuLoadCancelledCompletions =
        model.cancelledPendingCpuLoadCompletions;
    stats.modelCpuLoadDiscardedStaleCompletions =
        model.discardedStaleCpuLoadCompletions;
    stats.modelCpuLoadMaximumQueueAge = model.maximumCpuLoadQueueAge;
    stats.modelCpuReadyBytes = model.cpuReadyBytesPublished;
    stats.modelCpuReadyWorkerNanoseconds = model.cpuReadyWorkerNanoseconds;
    stats.modelCpuReadyPublishMicroseconds = model.cpuReadyPublishMicroseconds;
    stats.modelCpuReadyDeferred = model.cpuReadyDeferred;
    stats.modelCpuReadyForcedOversized = model.cpuReadyForcedOversized;
    stats.modelCpuReadyMaximumAge = model.maximumCpuReadyAge;
    stats.animationReadyBytes = animation.readyBytesPublished;
    stats.animationReadyWorkerNanoseconds = animation.readyWorkerNanoseconds;
    stats.animationReadyPublishMicroseconds =
        animation.readyPublishMicroseconds;
    stats.animationReadyDeferred = animation.readyDeferred;
    stats.animationReadyForcedOversized = animation.readyForcedOversized;
    stats.animationMaximumReadyAge = animation.maximumReadyAge;
}

} // namespace engine::render
