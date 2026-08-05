#include "engine/renderer/world/model/W3dAssetCache.h"

#include "VFS.h"
#include "core/platform/runtime_threads.h"
#include "engine/resource/ResourceSchedulerRuntime.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <mutex>
#include <utility>

namespace engine::render {
namespace {

uint64_t cpuLoadPriorityKey(W3dModelHandle handle) noexcept {
    return (static_cast<uint64_t>(handle.generation) << 32u) |
        handle.index;
}

uint64_t saturatedAdd(uint64_t lhs, uint64_t rhs) noexcept {
    return rhs > std::numeric_limits<uint64_t>::max() - lhs
        ? std::numeric_limits<uint64_t>::max()
        : lhs + rhs;
}

engine::resource::ResourceDemand resourceDemand(
    RenderAssetPriority priority) noexcept {
    switch (sanitizeRenderAssetPriority(priority)) {
    case RenderAssetPriority::Background:
        return engine::resource::ResourceDemand::Optional;
    case RenderAssetPriority::Preload:
        return engine::resource::ResourceDemand::Prefetch;
    case RenderAssetPriority::Normal:
    case RenderAssetPriority::Visible:
        return engine::resource::ResourceDemand::Visible;
    case RenderAssetPriority::Count:
        break;
    }
    return engine::resource::ResourceDemand::Visible;
}

constexpr uint64_t kW3dCpuLoadAdmissionEstimate = 4ull * 1024ull * 1024ull;

} // namespace

struct W3dAssetCache::CpuLoadWork final {
    W3dModelHandle handle;
    uint64_t revision = 0;
    uint64_t session = 0;
    std::optional<CpuLoadJob> job;
    std::optional<CpuLoadCompletion> result;
    engine::resource::ResourceJobState terminalState =
        engine::resource::ResourceJobState::Invalid;
    CpuLoadWork* nextCompleted = nullptr;
};

struct W3dAssetCache::CpuLoadMailbox final {
    std::mutex mutex;
    bool accepting = true;
    CpuLoadWork* completedHead = nullptr;
    CpuLoadWork* completedTail = nullptr;
};

size_t W3dAssetCache::processCpuLoads(size_t maxCompletions) {
    return processCpuLoads(RenderAssetReadyBudget{
        .maxItems = maxCompletions,
    });
}

size_t W3dAssetCache::processCpuLoads(
    const RenderAssetReadyBudget& budget) {
    collectFinishedCpuLoads();
    dispatchCpuLoads();
    if (budget.maxItems == 0u || budget.maxBytes == 0u ||
        budget.maxElapsedMicroseconds == 0u) {
        return 0;
    }
    container::Vector<CpuLoadCompletion> pending;
    pending.reserve(m_cpuLoadCompletions.size());
    while (!m_cpuLoadCompletions.empty()) {
        pending.push_back(std::move(m_cpuLoadCompletions.front()));
        m_cpuLoadCompletions.pop_front();
    }

    const auto started = std::chrono::steady_clock::now();
    uint64_t publishedBytes = 0;
    size_t published = 0;
    while (!pending.empty() && published < budget.maxItems) {
        const uint64_t elapsedMicroseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count());
        if (elapsedMicroseconds >= budget.maxElapsedMicroseconds) break;

        const uint64_t remainingBytes = publishedBytes < budget.maxBytes
            ? budget.maxBytes - publishedBytes : 0u;
        const auto better = [](const CpuLoadCompletion& candidate,
                               const CpuLoadCompletion& current) {
            const uint32_t candidatePriority = effectiveRenderAssetPriority(
                candidate.priority, candidate.deferredPasses);
            const uint32_t currentPriority = effectiveRenderAssetPriority(
                current.priority, current.deferredPasses);
            if (candidatePriority != currentPriority) {
                return candidatePriority > currentPriority;
            }
            if (candidate.deferredPasses != current.deferredPasses) {
                return candidate.deferredPasses > current.deferredPasses;
            }
            if (candidate.estimatedBytes != current.estimatedBytes) {
                return candidate.estimatedBytes < current.estimatedBytes;
            }
            return candidate.enqueueSequence < current.enqueueSequence;
        };
        size_t selected = pending.size();
        size_t oversized = pending.size();
        for (size_t index = 0; index < pending.size(); ++index) {
            if (pending[index].estimatedBytes <= remainingBytes &&
                (selected == pending.size() ||
                 better(pending[index], pending[selected]))) {
                selected = index;
            }
            if (pending[index].estimatedBytes > budget.maxBytes &&
                pending[index].deferredPasses >=
                    kRenderAssetOversizedProgressPasses &&
                (oversized == pending.size() ||
                 better(pending[index], pending[oversized]))) {
                oversized = index;
            }
        }
        bool forcedOversized = false;
        if (selected == pending.size() && published == 0u &&
            oversized != pending.size()) {
            selected = oversized;
            forcedOversized = true;
        }
        if (selected == pending.size()) break;

        CpuLoadCompletion completion = std::move(pending[selected]);
        pending.erase(pending.begin() +
                      static_cast<std::ptrdiff_t>(selected));
        Slot* slot = findSlot(completion.handle);
        if (completion.session != m_cpuLoadSession || !slot ||
            slot->revision != completion.revision ||
            (slot->state != W3dAssetState::CpuLoadQueued &&
             slot->state != W3dAssetState::CpuLoading)) {
            m_discardedStaleCpuLoadCompletions.fetch_add(
                1u, std::memory_order_relaxed);
            m_cpuLoadPriorities.erase(
                cpuLoadPriorityKey(completion.handle));
            continue;
        }
        unregisterFileDependencies(completion.handle.index);
        unregisterDependencyGraph(slot->dependencies);
        slot->dependencies = std::move(completion.dependencies);
        registerDependencyGraph(slot->dependencies);
        registerFileDependency(
            completion.handle.index, slot->dependencies.modelSourcePath);
        if (!slot->dependencies.hierarchySourcePath.empty()) {
            registerFileDependency(
                completion.handle.index,
                slot->dependencies.hierarchySourcePath);
        }
        slot->cpuModel = std::move(completion.model);
        slot->error = std::move(completion.error);
        slot->errorKind = completion.errorKind;
        if (slot->cpuModel) {
            slot->state = W3dAssetState::CpuReady;
            slot->error.clear();
            slot->errorKind = RenderAssetErrorKind::None;
            if (slot->queueGpuAfterCpuLoad) {
                queueGpuUpload(
                    completion.handle, slot->queueGpuAfterCpuLoadPriority);
            }
        } else {
            slot->state = W3dAssetState::Failed;
            if (slot->error.empty()) {
                slot->error = "asynchronous W3D CPU load failed";
            }
            if (slot->errorKind == RenderAssetErrorKind::None) {
                slot->errorKind = RenderAssetErrorKind::Build;
            }
            m_failedCpuLoadCompletions.fetch_add(
                1u, std::memory_order_relaxed);
        }
        slot->queueGpuAfterCpuLoad = false;
        slot->queueGpuAfterCpuLoadPriority = W3dGpuUploadPriority::Normal;
        m_publishedCpuLoadCompletions.fetch_add(
            1u, std::memory_order_relaxed);
        m_cpuLoadPriorities.erase(cpuLoadPriorityKey(completion.handle));
        publishedBytes = saturatedAdd(
            publishedBytes, completion.estimatedBytes);
        m_cpuReadyBytesPublished = saturatedAdd(
            m_cpuReadyBytesPublished, completion.estimatedBytes);
        m_cpuReadyWorkerNanoseconds = saturatedAdd(
            m_cpuReadyWorkerNanoseconds, completion.workerNanoseconds);
        m_maximumCpuReadyAge = std::max(
            m_maximumCpuReadyAge, completion.deferredPasses);
        if (forcedOversized) ++m_cpuReadyForcedOversized;
        ++published;
    }
    for (CpuLoadCompletion& completion : pending) {
        if (completion.deferredPasses !=
            std::numeric_limits<uint32_t>::max()) {
            ++completion.deferredPasses;
        }
        ++m_cpuReadyDeferred;
    }
    for (CpuLoadCompletion& completion : pending) {
        m_cpuLoadCompletions.push_back(std::move(completion));
    }
    m_cpuReadyPublishMicroseconds = saturatedAdd(
        m_cpuReadyPublishMicroseconds,
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count()));
    return published;
}

void W3dAssetCache::dispatchCpuLoads() {
    const auto publishRejected = [this](CpuLoadJob rejected,
                                        container::String error) {
        CpuLoadCompletion completion;
        completion.handle = rejected.handle;
        completion.revision = rejected.revision;
        completion.session = rejected.session;
        completion.error = std::move(error);
        completion.errorKind = RenderAssetErrorKind::Build;
        completion.enqueueSequence = rejected.enqueueSequence;
        completion.deferredPasses = rejected.deferredPasses;
        completion.priority = rejected.priority;
        m_cpuLoadCompletions.push_back(std::move(completion));
    };
    const auto better = [](const CpuLoadJob& candidate,
                           const CpuLoadJob& current) {
        const uint32_t candidatePriority = effectiveRenderAssetPriority(
            candidate.priority, candidate.deferredPasses);
        const uint32_t currentPriority = effectiveRenderAssetPriority(
            current.priority, current.deferredPasses);
        if (candidatePriority != currentPriority) {
            return candidatePriority > currentPriority;
        }
        if (candidate.deferredPasses != current.deferredPasses) {
            return candidate.deferredPasses > current.deferredPasses;
        }
        return candidate.enqueueSequence < current.enqueueSequence;
    };

    while (!m_cpuLoadJobs.empty()) {
        size_t selected = 0;
        for (size_t index = 1; index < m_cpuLoadJobs.size(); ++index) {
            if (better(m_cpuLoadJobs[index], m_cpuLoadJobs[selected])) {
                selected = index;
            }
        }
        CpuLoadJob job = std::move(m_cpuLoadJobs[selected]);
        m_cpuLoadJobs.erase(
            m_cpuLoadJobs.begin() + static_cast<std::ptrdiff_t>(selected));
        for (CpuLoadJob& waiting : m_cpuLoadJobs) {
            if (waiting.deferredPasses !=
                std::numeric_limits<uint32_t>::max()) {
                ++waiting.deferredPasses;
            }
        }

        engine::resource::ResourceSchedulerRuntime* scheduler =
            engine::resource::activeResourceSchedulerRuntime();
        if (!scheduler) {
            publishRejected(
                std::move(job), "W3D CPU resource scheduler is unavailable");
            continue;
        }

        if (!m_cpuLoadMailbox) {
            try {
                m_cpuLoadMailbox = std::make_shared<CpuLoadMailbox>();
            } catch (...) {
                m_cpuLoadJobs.insert(
                    m_cpuLoadJobs.begin() +
                        static_cast<std::ptrdiff_t>(selected),
                    std::move(job));
                break;
            }
        }

        const W3dModelHandle handle = job.handle;
        const uint64_t revision = job.revision;
        const uint64_t session = job.session;
        const uint64_t enqueueSequence = job.enqueueSequence;
        const RenderAssetPriority priority = job.priority;
        const container::String canonicalIdentity =
            job.key.sourcePath + "#" + job.key.prototype +
            (job.key.includeHiddenMeshes ? "#hidden" : "#visible-only") +
            (job.key.includeCollisionMeshes ? "#collision" : "#render");

        std::shared_ptr<CpuLoadWork> work;
        try {
            work = std::make_shared<CpuLoadWork>();
            work->handle = handle;
            work->revision = revision;
            work->session = session;
            work->job.emplace(std::move(job));
        } catch (...) {
            if (work && work->job) job = std::move(*work->job);
            m_cpuLoadJobs.insert(
                m_cpuLoadJobs.begin() +
                    static_cast<std::ptrdiff_t>(selected),
                std::move(job));
            break;
        }

        engine::resource::ResourceRequest request;
        request.key.kind = engine::resource::ResourceKind::Model;
        request.key.canonicalIdentity = canonicalIdentity;
        request.key.variant = enqueueSequence;
        request.key.generation = 0;
        request.demand = resourceDemand(priority);
        request.lane = engine::resource::ResourceLane::Resource;
        request.estimatedBytes = kW3dCpuLoadAdmissionEstimate;

        try {
            m_cpuLoadTasks.reserve(m_cpuLoadTasks.size() + 1u);
        } catch (...) {
            if (work->job) job = std::move(*work->job);
            m_cpuLoadJobs.insert(
                m_cpuLoadJobs.begin() +
                    static_cast<std::ptrdiff_t>(selected),
                std::move(job));
            break;
        }

        engine::resource::ResourceSubmitResult submitted;
        try {
            submitted = scheduler->submit(
                std::move(request),
                [vfs = m_vfs, work](
                    const engine::resource::ResourceTaskContext& context) {
                    if (!vfs || context.stopRequested() || !work->job) {
                        return engine::resource::ResourceTaskResult::Failed;
                    }
                    try {
                        work->result = runCpuLoad(*vfs, std::move(*work->job));
                        work->job.reset();
                    } catch (const std::exception& exception) {
                        CpuLoadCompletion failed;
                        failed.handle = work->handle;
                        failed.revision = work->revision;
                        failed.session = work->session;
                        failed.error = container::String{
                            "asynchronous W3D CPU load threw: "} +
                            exception.what();
                        failed.errorKind = RenderAssetErrorKind::Build;
                        work->result = std::move(failed);
                        work->job.reset();
                    } catch (...) {
                        CpuLoadCompletion failed;
                        failed.handle = work->handle;
                        failed.revision = work->revision;
                        failed.session = work->session;
                        failed.error = "asynchronous W3D CPU load threw";
                        failed.errorKind = RenderAssetErrorKind::Build;
                        work->result = std::move(failed);
                        work->job.reset();
                    }
                    if (context.stopRequested()) {
                        return engine::resource::ResourceTaskResult::Failed;
                    }
                    return work->result && work->result->model
                        ? engine::resource::ResourceTaskResult::Ready
                        : engine::resource::ResourceTaskResult::Failed;
                },
                [mailbox = m_cpuLoadMailbox, work](
                    const engine::resource::ResourceCompletion& completion) {
                    std::scoped_lock lock(mailbox->mutex);
                    if (!mailbox->accepting) return;
                    work->terminalState = completion.state;
                    work->nextCompleted = nullptr;
                    if (mailbox->completedTail) {
                        mailbox->completedTail->nextCompleted = work.get();
                    } else {
                        mailbox->completedHead = work.get();
                    }
                    mailbox->completedTail = work.get();
                });
        } catch (...) {
            submitted = {};
        }

        if (!submitted.accepted()) {
            if (work->job) job = std::move(*work->job);
            if (submitted.status ==
                engine::resource::ResourceSubmitStatus::QueueFull) {
                m_cpuLoadJobs.insert(
                    m_cpuLoadJobs.begin() +
                        static_cast<std::ptrdiff_t>(selected),
                    std::move(job));
                break;
            }
            publishRejected(
                std::move(job),
                "W3D CPU resource scheduler rejected the task");
            continue;
        }

        m_cpuLoadTasks.push_back({
            .handle = handle,
            .revision = revision,
            .session = session,
            .ticket = std::move(submitted.ticket),
            .work = std::move(work),
        });
        m_cpuLoadsInFlight.fetch_add(1u, std::memory_order_relaxed);
    }
}

void W3dAssetCache::collectFinishedCpuLoads() {
    if (!m_cpuLoadMailbox) return;
    CpuLoadWork* completed = nullptr;
    {
        std::scoped_lock lock(m_cpuLoadMailbox->mutex);
        completed = m_cpuLoadMailbox->completedHead;
        m_cpuLoadMailbox->completedHead = nullptr;
        m_cpuLoadMailbox->completedTail = nullptr;
    }

    while (completed) {
        CpuLoadWork* current = completed;
        completed = completed->nextCompleted;
        current->nextCompleted = nullptr;

        const auto task = std::find_if(
            m_cpuLoadTasks.begin(), m_cpuLoadTasks.end(),
            [current](const CpuLoadTask& candidate) {
                return candidate.work.get() == current;
            });
        if (task == m_cpuLoadTasks.end()) continue;

        std::shared_ptr<CpuLoadWork> work = task->work;
        m_cpuLoadsInFlight.fetch_sub(1u, std::memory_order_relaxed);
        m_cpuLoadTasks.erase(task);

        if (work->terminalState ==
                engine::resource::ResourceJobState::Cancelled ||
            work->terminalState ==
                engine::resource::ResourceJobState::Stale) {
            m_discardedStaleCpuLoadCompletions.fetch_add(
                1u, std::memory_order_relaxed);
            if (work->session == m_cpuLoadSession) {
                m_cpuLoadPriorities.erase(cpuLoadPriorityKey(work->handle));
            }
            continue;
        }

        CpuLoadCompletion completion;
        if (work->result) {
            completion = std::move(*work->result);
        } else {
            completion.handle = work->handle;
            completion.revision = work->revision;
            completion.session = work->session;
            completion.error =
                "W3D CPU resource task completed without a result";
            completion.errorKind = RenderAssetErrorKind::Build;
        }

        if (completion.session != m_cpuLoadSession) {
            m_discardedStaleCpuLoadCompletions.fetch_add(
                1u, std::memory_order_relaxed);
            m_cpuLoadPriorities.erase(cpuLoadPriorityKey(completion.handle));
            continue;
        }
        if (const auto requested = m_cpuLoadPriorities.find(
                cpuLoadPriorityKey(completion.handle));
            requested != m_cpuLoadPriorities.end()) {
            completion.priority = std::max(
                completion.priority, requested->second);
        }
        m_cpuLoadCompletions.push_back(std::move(completion));
    }
}

void W3dAssetCache::stopCpuLoads() noexcept {
    m_cancelledQueuedCpuLoadJobs.fetch_add(
        m_cpuLoadJobs.size(), std::memory_order_relaxed);
    m_cpuLoadJobs.clear();
    if (m_cpuLoadMailbox) {
        std::scoped_lock lock(m_cpuLoadMailbox->mutex);
        m_cpuLoadMailbox->accepting = false;
        m_cpuLoadMailbox->completedHead = nullptr;
        m_cpuLoadMailbox->completedTail = nullptr;
    }
    if (engine::resource::ResourceSchedulerRuntime* scheduler =
            engine::resource::activeResourceSchedulerRuntime()) {
        for (const CpuLoadTask& task : m_cpuLoadTasks) {
            static_cast<void>(scheduler->cancel(task.ticket));
        }
    }
    m_cpuLoadTasks.clear();
    m_cpuLoadMailbox.reset();
    m_cpuLoadsInFlight.store(0u, std::memory_order_relaxed);
}

W3dAssetCache::CpuLoadCompletion W3dAssetCache::runCpuLoad(
    io::VFS& vfs, CpuLoadJob job) {
    platform::runtime::ThreadRoleScope role(
        platform::runtime::ThreadRole::Resource);
    W3dAssetCache loader(vfs);
    W3dAssetRequest requestDescription;
    requestDescription.source = job.key.sourcePath;
    requestDescription.prototype = job.key.prototype;
    requestDescription.queueGpuUpload = false;
    requestDescription.includeHiddenMeshes = job.key.includeHiddenMeshes;
    requestDescription.includeCollisionMeshes = job.key.includeCollisionMeshes;
    const auto loadStarted = std::chrono::steady_clock::now();
    const W3dModelHandle workerHandle = loader.request(requestDescription);

    CpuLoadCompletion completion;
    completion.handle = job.handle;
    completion.revision = job.revision;
    completion.session = job.session;
    completion.enqueueSequence = job.enqueueSequence;
    completion.deferredPasses = job.deferredPasses;
    completion.priority = job.priority;
    completion.model = loader.cpuModel(workerHandle);
    completion.workerNanoseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - loadStarted).count());
    if (completion.model) {
        completion.estimatedBytes = estimateW3dGpuUploadBytes(
            *completion.model);
    }
    if (const auto dependencies = loader.dependencies(workerHandle)) {
        completion.dependencies = *dependencies;
    }
    if (!completion.model) {
        completion.error = loader.error(workerHandle);
        if (const auto lifecycle = loader.describeLifecycle(workerHandle)) {
            completion.errorKind = lifecycle->errorKind;
        }
    }
    return completion;
}

void W3dAssetCache::discardPendingCpuLoads(W3dModelHandle handle) {
    const size_t cancelledJobs = std::erase_if(
        m_cpuLoadJobs, [handle](const CpuLoadJob& pending) {
            return pending.handle == handle;
        });
    const size_t cancelledCompletions = std::erase_if(
        m_cpuLoadCompletions,
        [handle](const CpuLoadCompletion& pending) {
            return pending.handle == handle;
        });
    if (engine::resource::ResourceSchedulerRuntime* scheduler =
            engine::resource::activeResourceSchedulerRuntime()) {
        for (const CpuLoadTask& task : m_cpuLoadTasks) {
            if (task.handle == handle) {
                static_cast<void>(scheduler->cancel(task.ticket));
            }
        }
    }
    m_cpuLoadPriorities.erase(cpuLoadPriorityKey(handle));
    m_cancelledQueuedCpuLoadJobs.fetch_add(
        cancelledJobs, std::memory_order_relaxed);
    m_cancelledPendingCpuLoadCompletions.fetch_add(
        cancelledCompletions, std::memory_order_relaxed);
}

} // namespace engine::render
