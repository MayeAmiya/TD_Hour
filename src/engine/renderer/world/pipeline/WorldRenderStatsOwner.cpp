#include "engine/renderer/world/pipeline/WorldRenderStatsOwner.h"
#include "core/debug/debug.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::render {

void WorldRenderStatsOwner::reset() noexcept {
    *this = {};
}

bool WorldRenderStatsOwner::assetSampleDue(
    uint64_t simulationFrame) const noexcept {
    const bool rewind =
        m_assetSampleValid && simulationFrame < m_assetSampleFrame;
    return !m_assetSampleValid || rewind ||
        simulationFrame - m_assetSampleFrame >=
            kAssetSampleIntervalFrames;
}

void WorldRenderStatsOwner::publishAssetSample(
    uint64_t simulationFrame,
    WorldTextureCache::Stats textures,
    W3dAssetCacheStats models,
    W3dAnimationCacheStats animations) noexcept {
    m_textureSample = textures;
    m_modelSample = models;
    m_animationSample = animations;
    m_assetSampleFrame = simulationFrame;
    m_assetSampleValid = true;
}

RenderAssetLifecycleStats WorldRenderStatsOwner::projectAssetLifecycle(
    const RenderAssetKindLifecycleStats& uiTextures,
    const RenderAssetKindLifecycleStats& glyphs,
    size_t traversalCycleRejects,
    size_t traversalDepthRejects) const noexcept {
    const auto boundedU32 = [](size_t value) noexcept {
        return static_cast<uint32_t>(std::min<size_t>(
            value, std::numeric_limits<uint32_t>::max()));
    };
    const auto& model = m_modelSample;
    const auto& animation = m_animationSample;
    const auto& texture = m_textureSample;
    RenderAssetLifecycleStats result;

    auto& modelLifecycle = result[RenderAssetKind::Model];
    modelLifecycle.tracked = boundedU32(model.assetCount);
    modelLifecycle.ownerReferences = static_cast<uint32_t>(
        std::min<uint64_t>(model.gpuExternalOwnerReferences,
                           std::numeric_limits<uint32_t>::max()));
    const auto projectModelState = [&](
        W3dAssetState source, RenderAssetLifecycleState destination) {
        modelLifecycle.currentStates[static_cast<size_t>(destination)] +=
            boundedU32(model.stateCounts[static_cast<size_t>(source)]);
    };
    projectModelState(W3dAssetState::CpuLoadQueued,
                      RenderAssetLifecycleState::IoQueued);
    projectModelState(W3dAssetState::CpuLoading,
                      RenderAssetLifecycleState::IoInFlight);
    projectModelState(W3dAssetState::CpuReady,
                      RenderAssetLifecycleState::CpuReady);
    projectModelState(W3dAssetState::UploadQueued,
                      RenderAssetLifecycleState::GpuQueued);
    projectModelState(W3dAssetState::Uploading,
                      RenderAssetLifecycleState::GpuInFlight);
    projectModelState(W3dAssetState::GpuReady,
                      RenderAssetLifecycleState::GpuResident);
    projectModelState(W3dAssetState::GpuUploadFailed,
                      RenderAssetLifecycleState::Failed);
    projectModelState(W3dAssetState::Failed,
                      RenderAssetLifecycleState::Failed);
    modelLifecycle.requests = model.requests;
    modelLifecycle.cacheHits = model.cacheHits;
    modelLifecycle.cacheMisses = model.cacheMisses;
    modelLifecycle.published = model.lifetimeUploadTotals.succeededUploads;
    modelLifecycle.failures = model.failedCpuLoadCompletions +
        model.failedSynchronousCpuLoads +
        model.lifetimeUploadTotals.failedUploads;
    modelLifecycle.cancelledQueued = model.cancelledQueuedCpuLoadJobs;
    modelLifecycle.cancelledPending =
        model.cancelledPendingCpuLoadCompletions;
    modelLifecycle.staleRejected =
        model.discardedStaleCpuLoadCompletions +
        model.lifetimeUploadTotals.discardedStaleUploads;
    modelLifecycle.evictions = model.evictions + model.gpuResidencyEvictions;
    modelLifecycle.resets = model.resets;
    modelLifecycle.gpuBytes = model.gpuResidentBytes;
    modelLifecycle.cpuBytes = model.cpuRetainedModelBytes;

    auto& hierarchyLifecycle = result[RenderAssetKind::Hierarchy];
    hierarchyLifecycle.tracked = boundedU32(model.externalHierarchyCount);
    hierarchyLifecycle.currentStates[static_cast<size_t>(
        RenderAssetLifecycleState::CpuReady)] =
        boundedU32(model.resolvedHierarchyCount);
    const size_t unresolvedHierarchies = model.externalHierarchyCount >
            model.resolvedHierarchyCount
        ? model.externalHierarchyCount - model.resolvedHierarchyCount
        : 0u;
    const size_t fallbackHierarchies = std::min(
        unresolvedHierarchies, model.skeletonFallbackCount);
    hierarchyLifecycle.currentStates[static_cast<size_t>(
        RenderAssetLifecycleState::Fallback)] =
        boundedU32(fallbackHierarchies);
    hierarchyLifecycle.currentStates[static_cast<size_t>(
        RenderAssetLifecycleState::Failed)] =
        boundedU32(unresolvedHierarchies - fallbackHierarchies);

    auto& animationLifecycle = result[RenderAssetKind::Animation];
    animationLifecycle.tracked = boundedU32(
        animation.clipCount + animation.failedSourceCount +
        animation.queuedJobs + animation.pendingCompletions +
        animation.loadsInFlight);
    animationLifecycle.currentStates[static_cast<size_t>(
        RenderAssetLifecycleState::IoQueued)] =
        boundedU32(animation.queuedJobs);
    animationLifecycle.currentStates[static_cast<size_t>(
        RenderAssetLifecycleState::IoInFlight)] =
        boundedU32(animation.loadsInFlight);
    animationLifecycle.currentStates[static_cast<size_t>(
        RenderAssetLifecycleState::CpuReady)] = boundedU32(
            animation.clipCount + animation.pendingReadyCompletions);
    animationLifecycle.currentStates[static_cast<size_t>(
        RenderAssetLifecycleState::Failed)] = boundedU32(
            animation.failedSourceCount + animation.pendingFailedCompletions);
    animationLifecycle.requests = animation.requests;
    animationLifecycle.cacheHits = animation.cacheHits;
    animationLifecycle.cacheMisses = animation.cacheMisses;
    animationLifecycle.published = animation.published;
    animationLifecycle.failures = animation.failures;
    animationLifecycle.cancelledQueued = animation.cancelledQueued;
    animationLifecycle.cancelledPending = animation.cancelledPending;
    animationLifecycle.staleRejected = animation.staleRejected;
    animationLifecycle.resets = animation.resets;
    animationLifecycle.cpuBytes = animation.retainedClipBytes;

    auto& sourceLifecycle = result[RenderAssetKind::TextureSource];
    sourceLifecycle.tracked = boundedU32(
        texture.decodedSources + texture.proceduralSources +
        texture.negativeSourceLookups + texture.sourcePendingSources);
    const size_t activeSources = std::min(
        texture.sourcePendingSources, texture.sourceActiveSourceJobs);
    sourceLifecycle.currentStates[static_cast<size_t>(
        RenderAssetLifecycleState::IoQueued)] =
        boundedU32(texture.sourcePendingSources - activeSources);
    sourceLifecycle.currentStates[static_cast<size_t>(
        RenderAssetLifecycleState::IoInFlight)] = boundedU32(activeSources);
    sourceLifecycle.currentStates[static_cast<size_t>(
        RenderAssetLifecycleState::CpuReady)] = boundedU32(
            texture.decodedSources + texture.proceduralSources);
    sourceLifecycle.currentStates[static_cast<size_t>(
        RenderAssetLifecycleState::Failed)] =
        boundedU32(texture.negativeSourceLookups);
    sourceLifecycle.requests = texture.sourceRequests;
    sourceLifecycle.cacheHits = texture.sourceCacheHits;
    sourceLifecycle.cacheMisses = texture.sourceCacheMisses;
    sourceLifecycle.published = texture.sourceDecodeSucceeded;
    sourceLifecycle.failures = texture.sourceDecodeFailed +
        texture.sourceMissing + texture.sourceUnsupported;
    sourceLifecycle.resets = texture.sourceResets;
    sourceLifecycle.cpuBytes = texture.sourceCpuBytes;

    auto& textureLifecycle = result[RenderAssetKind::TextureVariant];
    const size_t activeVariants = std::min(
        texture.sourcePendingVariants, texture.sourceActiveVariantJobs);
    const size_t residentOrQueued =
        texture.residentTextures + texture.queuedGpuUploads;
    const size_t cpuReadyVariants =
        texture.sourcePreparedVariants > residentOrQueued
        ? texture.sourcePreparedVariants - residentOrQueued
        : 0u;
    const size_t resolvedFailures = std::min(
        texture.sourceFailedVariants,
        texture.terminalFailureFallbackEntries);
    const size_t unresolvedFailures =
        texture.sourceFailedVariants - resolvedFailures;
    textureLifecycle.tracked = boundedU32(
        texture.sourcePendingVariants + cpuReadyVariants +
        texture.queuedGpuUploads + texture.residentTextures +
        texture.fallbackEntries + unresolvedFailures);
    textureLifecycle.ownerReferences = boundedU32(texture.ownerReferences);
    textureLifecycle.currentStates[static_cast<size_t>(
        RenderAssetLifecycleState::IoQueued)] =
        boundedU32(texture.sourcePendingVariants - activeVariants);
    textureLifecycle.currentStates[static_cast<size_t>(
        RenderAssetLifecycleState::IoInFlight)] = boundedU32(activeVariants);
    textureLifecycle.currentStates[static_cast<size_t>(
        RenderAssetLifecycleState::CpuReady)] =
        boundedU32(cpuReadyVariants);
    textureLifecycle.currentStates[static_cast<size_t>(
        RenderAssetLifecycleState::GpuResident)] =
        boundedU32(texture.residentTextures);
    textureLifecycle.currentStates[static_cast<size_t>(
        RenderAssetLifecycleState::Fallback)] =
        boundedU32(texture.fallbackEntries);
    textureLifecycle.currentStates[static_cast<size_t>(
        RenderAssetLifecycleState::GpuQueued)] =
        boundedU32(texture.queuedGpuUploads);
    textureLifecycle.currentStates[static_cast<size_t>(
        RenderAssetLifecycleState::Failed)] =
        boundedU32(unresolvedFailures);
    textureLifecycle.requests = texture.cacheHits + texture.cacheMisses;
    textureLifecycle.cacheHits = texture.cacheHits;
    textureLifecycle.cacheMisses = texture.cacheMisses;
    textureLifecycle.published = texture.gpuUploads;
    textureLifecycle.failures = texture.failedAcquisitions;
    textureLifecycle.evictions = texture.retiredTextures;
    textureLifecycle.resets = texture.resets;
    textureLifecycle.gpuBytes = texture.residentBytes;

    result[RenderAssetKind::UiTexture] = uiTextures;
    result[RenderAssetKind::Glyph] = glyphs;
    result.dependencies.nodes = boundedU32(model.dependencyNodeCount);
    result.dependencies.edges = boundedU32(model.dependencyEdgeCount);
    result.dependencies.requiredEdges =
        boundedU32(model.dependencyRequiredEdgeCount);
    result.dependencies.optionalEdges =
        boundedU32(model.dependencyOptionalEdgeCount);
    result.dependencies.fallbackAllowedEdges =
        boundedU32(model.dependencyFallbackAllowedEdgeCount);
    result.dependencies.missingRequired =
        boundedU32(model.dependencyMissingRequiredCount);
    result.dependencies.fallbackResolved =
        boundedU32(model.dependencyFallbackResolvedCount);
    result.dependencies.cycleRejected = boundedU32(
        model.dependencyCycleRejectedCount + traversalCycleRejects);
    result.dependencies.depthRejected = boundedU32(
        model.dependencyDepthRejectedCount + traversalDepthRejects);
    result.dependencies.sharedTargets =
        boundedU32(model.dependencySharedTargetCount);
    result.dependencies.incomingReferences =
        model.dependencyIncomingReferenceCount;
    return result;
}

void WorldRenderStatsOwner::publishRetainedScratchCapacity(
    uint64_t bytes) noexcept {
    if (bytes > m_retainedScratchCapacity &&
        m_retainedScratchGrowthFrames !=
            std::numeric_limits<uint32_t>::max()) {
        ++m_retainedScratchGrowthFrames;
    }
    m_retainedScratchCapacity = bytes;
    m_retainedScratchHighWater =
        std::max(m_retainedScratchHighWater, bytes);
}

float WorldRenderStatsOwner::beginPresentationFrame(
    std::chrono::steady_clock::time_point now,
    uint64_t gpuFrameMicroseconds) noexcept {
    float deltaSeconds = 0.0f;
    if (m_lastPresentationTime !=
        std::chrono::steady_clock::time_point{}) {
        deltaSeconds =
            std::chrono::duration<float>(now - m_lastPresentationTime)
                .count();
    }
    m_lastPresentationTime = now;
    if (deltaSeconds > 0.0f && deltaSeconds <= 0.25f) {
        constexpr double kDeltaEmaWeight = 0.05;
        m_presentationDeltaEmaSeconds =
            m_presentationDeltaEmaSeconds > 0.0
            ? std::lerp(
                  m_presentationDeltaEmaSeconds,
                  static_cast<double>(deltaSeconds), kDeltaEmaWeight)
            : static_cast<double>(deltaSeconds);
    }
    if (gpuFrameMicroseconds != 0u && gpuFrameMicroseconds <= 250'000u) {
        constexpr double kGpuEmaWeight = 0.10;
        m_gpuFrameMicrosecondsEma = m_gpuFrameMicrosecondsEma > 0.0
            ? std::lerp(
                  m_gpuFrameMicrosecondsEma,
                  static_cast<double>(gpuFrameMicroseconds), kGpuEmaWeight)
            : static_cast<double>(gpuFrameMicroseconds);
    }

    if (m_presentationDeltaEmaSeconds > 0.0) {
        const double measuredFps = 1.0 / m_presentationDeltaEmaSeconds;
        const double gpuUs = m_gpuFrameMicrosecondsEma;
        // A low measured FPS is not by itself evidence of overload: authored
        // client FPS limits and VSync can intentionally pace an otherwise
        // idle GPU. Preserve one global in-between sample in that case because
        // it materially smooths 30 Hz infantry animation. Drop to zero only
        // when measured GPU work itself consumes the logic-frame budget.
        uint32_t desiredSamples = 1u;
        if (measuredFps >= 110.0 && (gpuUs == 0.0 || gpuUs <= 7'500.0)) {
            desiredSamples = 3u;
        } else if (
            measuredFps >= 80.0 && (gpuUs == 0.0 || gpuUs <= 11'000.0)) {
            desiredSamples = 2u;
        } else if (gpuUs >= 33'000.0) {
            desiredSamples = 0u;
        }
        if (desiredSamples != m_interpolationCandidateSamples) {
            m_interpolationCandidateSamples = desiredSamples;
            m_interpolationCandidateFrames = 1u;
        } else if (m_interpolationCandidateFrames != UINT32_MAX) {
            ++m_interpolationCandidateFrames;
        }
        const uint32_t requiredFrames = desiredSamples <
                m_interpolationIntermediateSamples
            ? 30u : 180u;
        if (desiredSamples != m_interpolationIntermediateSamples &&
            m_interpolationCandidateFrames >= requiredFrames) {
            const uint32_t previous = m_interpolationIntermediateSamples;
            m_interpolationIntermediateSamples = desiredSamples;
            m_interpolationCandidateFrames = 0u;
            TD_LOG_INFO(
                "[InterpolationQuality] samples={} -> {} measured={:.1f}fps gpu={}us",
                previous, desiredSamples, measuredFps,
                static_cast<uint64_t>(gpuUs));
        }
    }
    return deltaSeconds;
}

bool WorldRenderStatsOwner::reportDue(
    uint64_t simulationFrame) const noexcept {
    return !m_hasReport ||
        simulationFrame < m_lastReportSimulationFrame ||
        simulationFrame >=
            m_lastReportSimulationFrame + kReportIntervalFrames;
}

void WorldRenderStatsOwner::markReported(
    uint64_t simulationFrame) noexcept {
    m_lastReportSimulationFrame = simulationFrame;
    m_hasReport = true;
}

bool WorldRenderStatsOwner::claimFirstFrame(
    WorldFirstFrameDiagnostic diagnostic) noexcept {
    const uint16_t bit = static_cast<uint16_t>(
        1u << static_cast<uint8_t>(diagnostic));
    if ((m_firstFrameMask & bit) != 0) return false;
    m_firstFrameMask |= bit;
    return true;
}

void WorldRenderStatsOwner::resetFirstFrame(
    WorldFirstFrameDiagnostic diagnostic) noexcept {
    const uint16_t bit = static_cast<uint16_t>(
        1u << static_cast<uint8_t>(diagnostic));
    m_firstFrameMask &= static_cast<uint16_t>(~bit);
}

} // namespace engine::render
