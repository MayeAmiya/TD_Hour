#include "engine/renderer/world/model/W3dAssetCache.h"

#include <algorithm>

namespace engine::render {
namespace {

size_t priorityIndex(W3dGpuUploadPriority priority) noexcept {
    return std::min(static_cast<size_t>(priority),
                    kW3dGpuUploadPriorityCount - 1u);
}

} // namespace

W3dAssetCacheStats W3dAssetCache::stats() const noexcept {
    W3dAssetCacheStats result;
    result.parsedFileCount = static_cast<size_t>(std::count_if(
        m_fileCache.begin(), m_fileCache.end(), [](const auto& entry) {
            return static_cast<bool>(entry.second.parsed);
        }));
    result.queuedUploadCount = m_uploadQueue.size();
    result.lastUploadBatch = m_lastUploadBatch;
    result.lifetimeUploadTotals = m_lifetimeUploadTotals;
    result.queuedCpuLoadJobs = m_cpuLoadJobs.size();
    for (const CpuLoadJob& job : m_cpuLoadJobs) {
        result.maximumCpuLoadQueueAge = std::max(
            result.maximumCpuLoadQueueAge, job.deferredPasses);
        ++result.queuedCpuLoadsByPriority[priorityIndex(job.priority)];
    }
    result.pendingCpuLoadCompletions = m_cpuLoadCompletions.size();
    result.queuedCpuLoadCount = result.queuedCpuLoadJobs +
        result.pendingCpuLoadCompletions;
    result.cpuLoadsInFlight = m_cpuLoadsInFlight.load(
        std::memory_order_relaxed);
    result.publishedCpuLoadCompletions =
        m_publishedCpuLoadCompletions.load(std::memory_order_relaxed);
    result.failedCpuLoadCompletions =
        m_failedCpuLoadCompletions.load(std::memory_order_relaxed);
    result.failedSynchronousCpuLoads = m_failedSynchronousCpuLoads;
    result.cancelledQueuedCpuLoadJobs =
        m_cancelledQueuedCpuLoadJobs.load(std::memory_order_relaxed);
    result.cancelledPendingCpuLoadCompletions =
        m_cancelledPendingCpuLoadCompletions.load(
            std::memory_order_relaxed);
    result.discardedStaleCpuLoadCompletions =
        m_discardedStaleCpuLoadCompletions.load(
            std::memory_order_relaxed);
    result.cpuReadyBytesPublished = m_cpuReadyBytesPublished;
    result.cpuReadyWorkerNanoseconds = m_cpuReadyWorkerNanoseconds;
    result.cpuReadyPublishMicroseconds = m_cpuReadyPublishMicroseconds;
    result.cpuReadyDeferred = m_cpuReadyDeferred;
    result.cpuReadyForcedOversized = m_cpuReadyForcedOversized;
    result.maximumCpuReadyAge = m_maximumCpuReadyAge;
    result.requests = m_requests;
    result.cacheHits = m_cacheHits;
    result.cacheMisses = m_cacheMisses;
    result.reloads = m_reloads;
    result.evictions = m_evictions;
    result.resets = m_resets;
    result.gpuResidencyEvictions = m_gpuResidencyEvictions;
    result.gpuResidencyEvictedBytes = m_gpuResidencyEvictedBytes;
    result.gpuResidencyPinnedRejects = m_gpuResidencyPinnedRejects;
    result.gpuResidencyReferencedRejects =
        m_gpuResidencyReferencedRejects;
    for (const Slot& slot : m_slots) {
        if (!slot.occupied) continue;
        if (slot.residencyPinMask != 0u) ++result.gpuResidencyPins;
        ++result.assetCount;
        const W3dModelHandle slotHandle{
            static_cast<uint32_t>(&slot - m_slots.data()), slot.generation};
        const bool isActiveCpuLoad = std::any_of(
            m_cpuLoadTasks.begin(), m_cpuLoadTasks.end(),
            [this, slotHandle, &slot](const CpuLoadTask& task) {
                return task.session == m_cpuLoadSession &&
                    task.handle == slotHandle &&
                    task.revision == slot.revision &&
                    task.ticket.state() ==
                        engine::resource::ResourceJobState::InFlight;
            });
        const size_t stateIndex = static_cast<size_t>(
            isActiveCpuLoad ? W3dAssetState::CpuLoading : slot.state);
        if (stateIndex < result.stateCounts.size()) {
            ++result.stateCounts[stateIndex];
        }
        if (slot.state == W3dAssetState::GpuReady) ++result.gpuReadyCount;
        if (slot.cpuModel) {
            result.cpuRetainedModelBytes +=
                estimateW3dGpuUploadBytes(*slot.cpuModel);
        }
        if (slot.gpuModel) {
            const long sharedOwners = slot.gpuModel.use_count();
            if (sharedOwners > 1) {
                result.gpuExternalOwnerReferences +=
                    static_cast<uint64_t>(sharedOwners - 1);
            }
            result.gpuResidentBytes += slot.gpuModel->residentBytes();
            result.gpuRetainedSortingBytes +=
                slot.gpuModel->retainedSortingBytes();
            result.gpuLatestUsedFrame = std::max(
                result.gpuLatestUsedFrame,
                slot.gpuModel->lastUsedFrame());
            const W3dGpuUseDiagnostic use = slot.gpuModel->useDiagnostic();
            result.gpuLatestUsedFence = std::max(
                result.gpuLatestUsedFence, use.fence);
            if (use.frame != 0u && !use.completed) {
                ++result.gpuUseFencesInFlight;
            } else if (use.frame != 0u && !use.exactFence && use.completed) {
                ++result.gpuCompletedUseFramesWithoutExactFence;
            }
        }
        if (slot.state == W3dAssetState::Failed ||
            slot.state == W3dAssetState::GpuUploadFailed) {
            ++result.failedCount;
        }
        if (slot.dependencies.externalHierarchyRequired) {
            ++result.externalHierarchyCount;
        }
        if (slot.dependencies.externalHierarchyResolved) {
            ++result.resolvedHierarchyCount;
        }
        if (slot.dependencies.skeletonFallback) {
            ++result.skeletonFallbackCount;
        }
        result.dependencyDiagnosticCount +=
            slot.dependencies.diagnostics.size();
        result.dependencyNodeCount += slot.dependencies.nodes.size();
        result.dependencyEdgeCount += slot.dependencies.edges.size();
        for (const W3dDependencyEdge& edge : slot.dependencies.edges) {
            switch (edge.policy) {
            case RenderAssetDependencyPolicy::Required:
                ++result.dependencyRequiredEdgeCount;
                break;
            case RenderAssetDependencyPolicy::Optional:
                ++result.dependencyOptionalEdgeCount;
                break;
            case RenderAssetDependencyPolicy::FallbackAllowed:
                ++result.dependencyFallbackAllowedEdgeCount;
                break;
            }
            if (edge.resolution ==
                RenderAssetDependencyResolution::Fallback) {
                ++result.dependencyFallbackResolvedCount;
            } else if (edge.resolution ==
                       RenderAssetDependencyResolution::CycleRejected) {
                ++result.dependencyCycleRejectedCount;
            } else if (edge.resolution ==
                       RenderAssetDependencyResolution::DepthRejected) {
                ++result.dependencyDepthRejectedCount;
            } else if (edge.policy ==
                           RenderAssetDependencyPolicy::Required &&
                       edge.resolution ==
                           RenderAssetDependencyResolution::Missing) {
                ++result.dependencyMissingRequiredCount;
            }
        }
    }
    for (const auto& [identity, references] :
         m_dependencyTargetReferences) {
        static_cast<void>(identity);
        result.dependencyIncomingReferenceCount += references;
        if (references >= 2u) ++result.dependencySharedTargetCount;
    }
    return result;
}

std::optional<RenderAssetLifecycleRecord>
W3dAssetCache::describeLifecycle(W3dModelHandle handle) const {
    const Slot* slot = findSlot(handle);
    if (!slot) return std::nullopt;

    RenderAssetLifecycleRecord result;
    result.identity.kind = RenderAssetKind::Model;
    result.identity.logicalName = slot->key.prototype.empty()
        ? slot->key.sourcePath : slot->key.prototype;
    result.identity.canonicalSource = slot->key.sourcePath;
    result.identity.variant =
        container::String(slot->key.includeHiddenMeshes
            ? "hidden=1" : "hidden=0") +
        (slot->key.includeCollisionMeshes
            ? ";collision=1" : ";collision=0");
    result.identity.generation = slot->generation;
    result.identity.revision = slot->revision;
    result.diagnostic = slot->error;
    const bool activeCpuLoad = std::any_of(
        m_cpuLoadTasks.begin(), m_cpuLoadTasks.end(),
        [this, handle, slot](const CpuLoadTask& task) {
            return task.session == m_cpuLoadSession &&
                task.handle == handle && task.revision == slot->revision &&
                task.ticket.state() ==
                    engine::resource::ResourceJobState::InFlight;
        });
    switch (activeCpuLoad ? W3dAssetState::CpuLoading : slot->state) {
    case W3dAssetState::CpuLoadQueued:
        result.state = RenderAssetLifecycleState::IoQueued;
        break;
    case W3dAssetState::CpuLoading:
        result.state = RenderAssetLifecycleState::IoInFlight;
        break;
    case W3dAssetState::CpuReady:
        result.state = RenderAssetLifecycleState::CpuReady;
        break;
    case W3dAssetState::UploadQueued:
        result.state = RenderAssetLifecycleState::GpuQueued;
        break;
    case W3dAssetState::Uploading:
        result.state = RenderAssetLifecycleState::GpuInFlight;
        break;
    case W3dAssetState::GpuReady:
        result.state = RenderAssetLifecycleState::GpuResident;
        break;
    case W3dAssetState::GpuUploadFailed:
        result.state = RenderAssetLifecycleState::Failed;
        result.errorKind = slot->errorKind;
        break;
    case W3dAssetState::Failed:
        result.state = RenderAssetLifecycleState::Failed;
        result.errorKind = slot->errorKind;
        break;
    }
    return result;
}

} // namespace engine::render
