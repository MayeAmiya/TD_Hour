#include "core/container/container_types.h"
#include "engine/renderer/world/model/W3dAnimationCache.h"

#include "engine/renderer/world/model/Skeleton.h"
#include "engine/renderer/runtime/RenderPerformanceSettings.h"
#include "VFS.h"
#include "LocaleResourceLocator.h"
#include "data/w3d/W3dAssetIdentity.h"
#include "data/w3d/W3dLoader.h"
#include "core/platform/runtime_threads.h"
#include "engine/resource/ResourceSchedulerRuntime.h"
#include "debug/debug.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <limits>
#include <stdexcept>
namespace engine::render {
namespace {

container::String lowerAscii(container::String value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

container::String boundedName(const char* data, size_t capacity) {
    size_t length = 0;
    while (length < capacity && data[length] != '\0') ++length;
    return container::String(data, length);
}

size_t priorityIndex(RenderAssetPriority priority) noexcept {
    return std::min(static_cast<size_t>(sanitizeRenderAssetPriority(priority)),
                    kRenderAssetPriorityCount - 1u);
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

template <typename Work>
bool betterReadyCandidate(const Work& candidate, const Work& current) noexcept {
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
}

} // namespace

struct W3dAnimationCache::AsyncState final {
    io::VFS* vfs = nullptr;
    std::mutex mutex;
    std::condition_variable idle;
    container::HashSet<container::String> pendingAnimations;
    container::HashMap<container::String, RenderAssetPriority>
        pendingPriorities;
    container::Deque<AsyncJob> jobs;
    container::Deque<AsyncCompletion> completions;
    container::HashSet<container::String> activeAnimations;
    std::atomic<bool> shuttingDown{false};
    std::atomic<uint64_t> generation{1};
    std::atomic<size_t> loadsInFlight{0};
    size_t scheduledTasks = 0;
};

W3dAnimationCache::W3dAnimationCache()
    : W3dAnimationCache(io::VFS::instance()) {}

W3dAnimationCache::W3dAnimationCache(io::VFS& vfs)
    : m_async(std::make_shared<AsyncState>()) {
    m_async->vfs = &vfs;
}

W3dAnimationCache::~W3dAnimationCache() {
    cancelAsyncTasks(true);
}

container::SharedPtr<const AnimationClip> W3dAnimationCache::find(container::StringView animationName) {
    m_requests.fetch_add(1u, std::memory_order_relaxed);
    const container::String key = lowerAscii(container::String(animationName));
    if (key.empty()) {
        m_cacheMisses.fetch_add(1u, std::memory_order_relaxed);
        return nullptr;
    }
    if (const auto found = m_clips.find(key); found != m_clips.end()) {
        m_cacheHits.fetch_add(1u, std::memory_order_relaxed);
        return found->second;
    }
    m_cacheMisses.fetch_add(1u, std::memory_order_relaxed);

    const container::String source =
        data::w3d::w3dAnimationFileStem(animationName);
    if (source.empty() || !loadSource(source)) return nullptr;
    if (const auto found = m_clips.find(key); found != m_clips.end()) return found->second;
    return nullptr;
}

container::SharedPtr<const AnimationClip> W3dAnimationCache::findLoaded(
    container::StringView animationName) const {
    const container::String key = lowerAscii(container::String(animationName));
    const auto found = m_clips.find(key);
    return found == m_clips.end() ? nullptr : found->second;
}

bool W3dAnimationCache::requestAsync(
    container::StringView animationName, RenderAssetPriority priority) {
    m_requests.fetch_add(1u, std::memory_order_relaxed);
    const container::String key = lowerAscii(container::String(animationName));
    if (key.empty()) {
        m_cacheMisses.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    if (m_clips.contains(key)) {
        m_cacheHits.fetch_add(1u, std::memory_order_relaxed);
        return true;
    }
    const container::String source =
        data::w3d::w3dAnimationFileStem(animationName);
    if (source.empty() || m_sourceErrors.contains(source)) {
        m_cacheMisses.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    if (m_async->shuttingDown.load(std::memory_order_acquire)) return false;
    reapFinishedTasks();
    priority = sanitizeRenderAssetPriority(priority);
    AsyncJob job;
    {
        std::lock_guard lock(m_async->mutex);
        if (!m_async->pendingAnimations.insert(key).second) {
            auto& requestedPriority = m_async->pendingPriorities[key];
            requestedPriority = std::max(requestedPriority, priority);
            for (AsyncJob& job : m_async->jobs) {
                if (lowerAscii(job.logicalName) == key) {
                    job.priority = std::max(job.priority, priority);
                }
            }
            for (AsyncCompletion& completion : m_async->completions) {
                if (lowerAscii(completion.logicalName) == key) {
                    completion.priority = std::max(
                        completion.priority, priority);
                }
            }
            m_cacheHits.fetch_add(1u, std::memory_order_relaxed);
            return true;
        }
        m_cacheMisses.fetch_add(1u, std::memory_order_relaxed);
        m_async->pendingPriorities[key] = priority;
        job = {
            .logicalName = container::String(animationName),
            .generation = m_async->generation.load(std::memory_order_acquire),
            .enqueueSequence = m_nextEnqueueSequence++,
            .priority = priority,
        };
        m_async->jobs.push_back(job);
    }
    return submitAsyncJob(std::move(job));
}

size_t W3dAnimationCache::processLoads() {
    return processLoads(RenderAssetReadyBudget{
        .maxItems = performance_limits::kAnimationReadyPublishesPerFrame,
        .maxBytes = performance_limits::kAnimationReadyBytesPerFrame,
        .maxElapsedMicroseconds =
            performance_limits::kAnimationReadyMicrosecondsPerFrame,
    });
}

size_t W3dAnimationCache::processLoads(
    const RenderAssetReadyBudget& budget) {
    reapFinishedTasks();
    container::Vector<AsyncCompletion> pending;
    {
        std::lock_guard lock(m_async->mutex);
        pending.reserve(m_async->completions.size());
        while (!m_async->completions.empty()) {
            pending.push_back(std::move(m_async->completions.front()));
            m_async->completions.pop_front();
        }
    }

    const auto started = std::chrono::steady_clock::now();
    uint64_t publishedBytes = 0;
    size_t publishedCount = 0;
    while (!pending.empty() && publishedCount < budget.maxItems) {
        const uint64_t elapsedMicroseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count());
        if (elapsedMicroseconds >= budget.maxElapsedMicroseconds) break;

        const uint64_t remainingBytes = publishedBytes < budget.maxBytes
            ? budget.maxBytes - publishedBytes : 0u;
        size_t selected = pending.size();
        size_t oversized = pending.size();
        for (size_t index = 0; index < pending.size(); ++index) {
            const AsyncCompletion& candidate = pending[index];
            if (candidate.estimatedBytes <= remainingBytes &&
                (selected == pending.size() ||
                 betterReadyCandidate(candidate, pending[selected]))) {
                selected = index;
            }
            if (candidate.estimatedBytes > budget.maxBytes &&
                candidate.deferredPasses >=
                    kRenderAssetOversizedProgressPasses &&
                (oversized == pending.size() ||
                 betterReadyCandidate(candidate, pending[oversized]))) {
                oversized = index;
            }
        }
        bool forcedOversized = false;
        if (selected == pending.size() && publishedCount == 0u &&
            oversized != pending.size()) {
            selected = oversized;
            forcedOversized = true;
        }
        if (selected == pending.size()) break;

        AsyncCompletion completion = std::move(pending[selected]);
        pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(selected));
        const container::String key = lowerAscii(completion.logicalName);
        if (completion.generation !=
            m_async->generation.load(std::memory_order_acquire)) {
            m_staleRejected.fetch_add(1u, std::memory_order_relaxed);
            std::lock_guard lock(m_async->mutex);
            m_async->pendingAnimations.erase(key);
            m_async->pendingPriorities.erase(key);
            continue;
        }
        const container::String source =
            data::w3d::w3dAnimationFileStem(completion.logicalName);
        if (completion.clip) {
            m_clips[key] = std::move(completion.clip);
            m_loadedSources.insert(source);
            m_sourceErrors.erase(source);
            m_published.fetch_add(1u, std::memory_order_relaxed);
        } else if (!completion.dependency.diagnostic.empty()) {
            m_sourceErrors[source] = completion.dependency.diagnostic;
            m_failures.fetch_add(1u, std::memory_order_relaxed);
        }
        m_sourceRevisions[source] = std::max<uint64_t>(
            completion.dependency.revision, 1u);
        std::lock_guard lock(m_async->mutex);
        m_async->pendingAnimations.erase(key);
        m_async->pendingPriorities.erase(key);
        ++publishedCount;
        publishedBytes += completion.estimatedBytes;
        m_readyBytesPublished += completion.estimatedBytes;
        m_readyWorkerNanoseconds += completion.workerNanoseconds;
        m_maximumReadyAge = std::max(
            m_maximumReadyAge, completion.deferredPasses);
        ++m_publishedByPriority[priorityIndex(completion.priority)];
        if (forcedOversized) ++m_readyForcedOversized;
    }
    for (AsyncCompletion& completion : pending) {
        if (completion.deferredPasses != std::numeric_limits<uint32_t>::max()) {
            ++completion.deferredPasses;
        }
        ++m_readyDeferred;
    }
    {
        std::lock_guard lock(m_async->mutex);
        for (AsyncCompletion& completion : pending) {
            m_async->completions.push_back(std::move(completion));
        }
    }
    m_readyPublishMicroseconds += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count());
    return publishedCount;
}

container::String W3dAnimationCache::error(container::StringView animationName) const {
    const container::String source =
        data::w3d::w3dAnimationFileStem(animationName);
    if (const auto found = m_sourceErrors.find(source); found != m_sourceErrors.end()) return found->second;
    return {};
}

W3dAnimationDependency W3dAnimationCache::dependency(
    container::StringView animationName) const {
    W3dAnimationDependency result;
    result.logicalName = container::String(animationName);
    const container::String key = lowerAscii(container::String(animationName));
    const container::String source =
        data::w3d::w3dAnimationFileStem(animationName);
    result.sourcePath = data::w3d::w3dAnimationSourcePath(animationName);
    if (const auto revision = m_sourceRevisions.find(source);
        revision != m_sourceRevisions.end()) {
        result.revision = revision->second;
    }
    if (const auto clip = m_clips.find(key); clip != m_clips.end() && clip->second) {
        result.ready = true;
        result.hierarchyName = clip->second->hierarchyName();
    }
    if (const auto failure = m_sourceErrors.find(source);
        failure != m_sourceErrors.end()) {
        result.diagnostic = failure->second;
    }
    return result;
}

std::optional<W3dAnimationVersion> W3dAnimationCache::version(
    container::StringView animationName) const {
    const W3dAnimationDependency current = dependency(animationName);
    if (!current.ready || current.revision == 0) return std::nullopt;
    W3dAnimationVersion result{
        .logicalName = lowerAscii(container::String(animationName)),
        .generation = m_async->generation.load(std::memory_order_acquire),
        .revision = current.revision,
    };
    return result.valid()
        ? std::optional<W3dAnimationVersion>(std::move(result))
        : std::nullopt;
}

bool W3dAnimationCache::matches(
    const W3dAnimationVersion& expected) const {
    if (!expected || expected.generation !=
            m_async->generation.load(std::memory_order_acquire)) {
        return false;
    }
    const std::optional<W3dAnimationVersion> current =
        version(expected.logicalName);
    return current && *current == expected;
}

void W3dAnimationCache::clear() {
    m_resets.fetch_add(1u, std::memory_order_relaxed);
    cancelAsyncTasks(false);
    m_clips.clear();
    m_sourceErrors.clear();
    m_sourceRevisions.clear();
    m_loadedSources.clear();
}

W3dAnimationCacheStats W3dAnimationCache::stats() const noexcept {
    W3dAnimationCacheStats result;
    result.clipCount = m_clips.size();
    result.loadedSourceCount = m_loadedSources.size();
    result.failedSourceCount = m_sourceErrors.size();
    {
        std::lock_guard lock(m_async->mutex);
        result.queuedJobs = m_async->jobs.size();
        result.pendingCompletions = m_async->completions.size();
        for (const AsyncCompletion& completion : m_async->completions) {
            if (completion.clip) ++result.pendingReadyCompletions;
            else ++result.pendingFailedCompletions;
        }
    }
    result.loadsInFlight = m_async->loadsInFlight.load(
        std::memory_order_relaxed);
    result.generation = m_async->generation.load(std::memory_order_relaxed);
    result.requests = m_requests.load(std::memory_order_relaxed);
    result.cacheHits = m_cacheHits.load(std::memory_order_relaxed);
    result.cacheMisses = m_cacheMisses.load(std::memory_order_relaxed);
    result.published = m_published.load(std::memory_order_relaxed);
    result.failures = m_failures.load(std::memory_order_relaxed);
    result.cancelledQueued = m_cancelledQueued.load(
        std::memory_order_relaxed);
    result.cancelledPending = m_cancelledPending.load(
        std::memory_order_relaxed);
    result.staleRejected = m_staleRejected.load(
        std::memory_order_relaxed);
    result.resets = m_resets.load(std::memory_order_relaxed);
    result.readyBytesPublished = m_readyBytesPublished;
    result.readyWorkerNanoseconds = m_readyWorkerNanoseconds;
    result.readyPublishMicroseconds = m_readyPublishMicroseconds;
    result.readyDeferred = m_readyDeferred;
    result.readyForcedOversized = m_readyForcedOversized;
    result.maximumReadyAge = m_maximumReadyAge;
    for (const auto& [name, clip] : m_clips) {
        static_cast<void>(name);
        if (clip) result.retainedClipBytes += clip->estimatedByteSize();
    }
    result.publishedByPriority = m_publishedByPriority;
    return result;
}

RenderAssetLifecycleRecord W3dAnimationCache::describeLifecycle(
    container::StringView animationName) const {
    RenderAssetLifecycleRecord result;
    result.identity.kind = RenderAssetKind::Animation;
    result.identity.logicalName = container::String(animationName);
    const container::String key = lowerAscii(container::String(animationName));
    const container::String source =
        data::w3d::w3dAnimationFileStem(animationName);
    result.identity.canonicalSource =
        data::w3d::w3dAnimationSourcePath(animationName);
    result.identity.generation = m_async->generation.load(
        std::memory_order_relaxed);
    if (const auto revision = m_sourceRevisions.find(source);
        revision != m_sourceRevisions.end()) {
        result.identity.revision = revision->second;
    }
    if (const auto clip = m_clips.find(key);
        clip != m_clips.end() && clip->second) {
        result.state = RenderAssetLifecycleState::CpuReady;
        result.identity.variant = clip->second->hierarchyName();
        return result;
    }
    if (const auto failure = m_sourceErrors.find(source);
        failure != m_sourceErrors.end()) {
        result.state = RenderAssetLifecycleState::Failed;
        result.diagnostic = failure->second;
        result.errorKind = result.diagnostic.find("VFS read") !=
            container::String::npos
            ? RenderAssetErrorKind::Io
            : RenderAssetErrorKind::Parse;
        return result;
    }
    {
        std::lock_guard lock(m_async->mutex);
        if (m_async->activeAnimations.contains(key)) {
            result.state = RenderAssetLifecycleState::IoInFlight;
            return result;
        }
        const auto completion = std::find_if(
            m_async->completions.begin(), m_async->completions.end(),
            [&key](const AsyncCompletion& value) {
                return lowerAscii(value.logicalName) == key;
            });
        if (completion != m_async->completions.end()) {
            result.state = completion->clip
                ? RenderAssetLifecycleState::CpuReady
                : RenderAssetLifecycleState::Failed;
            result.errorKind = completion->clip
                ? RenderAssetErrorKind::None
                : (completion->dependency.diagnostic.find("VFS read") !=
                       container::String::npos
                    ? RenderAssetErrorKind::Io
                    : RenderAssetErrorKind::Parse);
            result.diagnostic = completion->dependency.diagnostic;
            return result;
        }
        if (m_async->pendingAnimations.contains(key)) {
            result.state = RenderAssetLifecycleState::IoQueued;
            return result;
        }
    }
    result.state = RenderAssetLifecycleState::Requested;
    return result;
}

bool W3dAnimationCache::loadSource(container::StringView hierarchyName) {
    const container::String source(hierarchyName);
    m_sourceRevisions.try_emplace(source, 1);
    if (m_loadedSources.contains(source)) return true;
    if (m_sourceErrors.contains(source)) return false;

    container::String path = data::w3d::w3dAnimationSourcePath(source);
    bool sourceAvailable = m_async->vfs != nullptr;
    if (const auto locator = io::acquireLocaleResourceLocator()) {
        const std::optional<container::String> resolved = locator->resolve(
            io::LocaleResourceKind::W3d, path);
        sourceAvailable = resolved.has_value();
        if (resolved) path = *resolved;
    }
    container::Vector<uint8_t> bytes;
    if (!sourceAvailable || !m_async->vfs ||
        !m_async->vfs->readToBuffer(path, bytes) ||
        bytes.empty()) {
        const container::String message = "W3D animation VFS read failed: " + path;
        m_sourceErrors.emplace(source, message);
        m_failures.fetch_add(1u, std::memory_order_relaxed);
        TD_LOG_WARN("[W3dAnimationCache] {}", message);
        return false;
    }

    data::w3d::W3dLoader loader;
    if (!loader.loadFromMemory(bytes.data(), bytes.size())) {
        const container::String message = "W3D animation parse failed for " + path + ": " + loader.error();
        m_sourceErrors.emplace(source, message);
        m_failures.fetch_add(1u, std::memory_order_relaxed);
        TD_LOG_WARN("[W3dAnimationCache] {}", message);
        return false;
    }

    size_t added = 0;
    for (const data::w3d::ParsedAnimation& animation : loader.result().animations) {
        const container::String hierarchy = boundedName(animation.hierarchyName, data::w3d::NAME_LEN);
        const container::String name = boundedName(animation.name, data::w3d::NAME_LEN);
        if (hierarchy.empty() || name.empty()) continue;
        const container::String key = lowerAscii(hierarchy + "." + name);
        m_clips.emplace(key, AnimationClip::fromW3d(animation));
        ++added;
    }
    if (added == 0) {
        const container::String message = "W3D animation file contains no supported clips: " + path;
        m_sourceErrors.emplace(source, message);
        m_failures.fetch_add(1u, std::memory_order_relaxed);
        TD_LOG_WARN("[W3dAnimationCache] {}", message);
        return false;
    }
    m_loadedSources.emplace(source);
    m_published.fetch_add(added, std::memory_order_relaxed);
    TD_LOG_INFO("[W3dAnimationCache] Loaded '{}' clips={}", source, added);
    return true;
}

bool W3dAnimationCache::submitAsyncJob(AsyncJob job) {
    const container::String logicalName = job.logicalName;
    const container::String key = lowerAscii(logicalName);
    const uint64_t enqueueSequence = job.enqueueSequence;
    bool taskReservationOwned = false;
    try {
        engine::resource::ResourceSchedulerRuntime* scheduler =
            engine::resource::activeResourceSchedulerRuntime();
        if (!scheduler) throw std::runtime_error(
            "resource scheduler is unavailable");
        engine::resource::ResourceRequest request;
        request.key.kind = engine::resource::ResourceKind::Animation;
        request.key.canonicalIdentity =
            data::w3d::w3dAnimationSourcePath(job.logicalName);
        request.key.variant = enqueueSequence;
        request.demand = resourceDemand(job.priority);
        request.estimatedBytes = std::max<uint64_t>(job.estimatedBytes, 1u);
        const container::SharedPtr<AsyncState> state = m_async;
        {
            std::lock_guard lock(state->mutex);
            ++state->scheduledTasks;
            taskReservationOwned = true;
        }
        const engine::resource::ResourceSubmitResult submitted = scheduler->submit(
            std::move(request),
            [state, job = std::move(job)](
                const engine::resource::ResourceTaskContext& context) mutable {
                if (context.stopRequested()) {
                    return engine::resource::ResourceTaskResult::Failed;
                }
                runAsyncJob(state, std::move(job));
                return engine::resource::ResourceTaskResult::Ready;
            },
            [state](const engine::resource::ResourceCompletion&) {
                {
                    std::lock_guard lock(state->mutex);
                    --state->scheduledTasks;
                }
                state->idle.notify_all();
            });
        if (!submitted.accepted()) {
            {
                std::lock_guard lock(state->mutex);
                --state->scheduledTasks;
                taskReservationOwned = false;
            }
            state->idle.notify_all();
            throw std::runtime_error(
                "resource scheduler rejected animation task");
        }
        taskReservationOwned = false;
        return true;
    } catch (const std::exception& exception) {
        static_cast<void>(exception);
        std::lock_guard lock(m_async->mutex);
        if (taskReservationOwned) {
            --m_async->scheduledTasks;
            m_async->idle.notify_all();
        }
        const auto queued = std::find_if(
            m_async->jobs.begin(), m_async->jobs.end(),
            [enqueueSequence](const AsyncJob& candidate) {
                return candidate.enqueueSequence == enqueueSequence;
            });
        if (queued != m_async->jobs.end()) m_async->jobs.erase(queued);
        m_async->pendingAnimations.erase(key);
        m_async->pendingPriorities.erase(key);
        TD_LOG_ERROR("[W3dAnimationCache] Could not submit '{}' to resource executor: {}",
                     logicalName, exception.what());
        return false;
    } catch (...) {
        std::lock_guard lock(m_async->mutex);
        if (taskReservationOwned) {
            --m_async->scheduledTasks;
            m_async->idle.notify_all();
        }
        const auto queued = std::find_if(
            m_async->jobs.begin(), m_async->jobs.end(),
            [enqueueSequence](const AsyncJob& candidate) {
                return candidate.enqueueSequence == enqueueSequence;
            });
        if (queued != m_async->jobs.end()) m_async->jobs.erase(queued);
        m_async->pendingAnimations.erase(key);
        m_async->pendingPriorities.erase(key);
        TD_LOG_ERROR("[W3dAnimationCache] Could not submit '{}' to resource executor",
                     logicalName);
        return false;
    }
}

void W3dAnimationCache::runAsyncJob(
    container::SharedPtr<AsyncState> state, AsyncJob job) noexcept {
    platform::runtime::ThreadRoleScope role(
        platform::runtime::ThreadRole::Resource);
    const container::String key = lowerAscii(job.logicalName);
    {
        std::lock_guard lock(state->mutex);
        const auto queued = std::find_if(
            state->jobs.begin(), state->jobs.end(),
            [&job](const AsyncJob& candidate) {
                return candidate.enqueueSequence == job.enqueueSequence;
            });
        if (queued == state->jobs.end() ||
            job.generation != state->generation.load(std::memory_order_acquire) ||
            state->shuttingDown.load(std::memory_order_acquire)) {
            return;
        }
        job.priority = queued->priority;
        job.deferredPasses = queued->deferredPasses;
        state->jobs.erase(queued);
        for (AsyncJob& waiting : state->jobs) {
            if (waiting.deferredPasses !=
                std::numeric_limits<uint32_t>::max()) {
                ++waiting.deferredPasses;
            }
        }
        state->activeAnimations.insert(key);
    }

    AsyncCompletion completion;
    completion.logicalName = job.logicalName;
    completion.generation = job.generation;
    completion.enqueueSequence = job.enqueueSequence;
    completion.deferredPasses = job.deferredPasses;
    completion.priority = job.priority;
    state->loadsInFlight.fetch_add(1u, std::memory_order_relaxed);
    const auto started = std::chrono::steady_clock::now();
    try {
        if (state->vfs) {
            W3dAnimationCache loader(*state->vfs);
            completion.clip = loader.find(job.logicalName);
            completion.dependency = loader.dependency(job.logicalName);
        } else {
            completion.dependency.logicalName = job.logicalName;
            completion.dependency.diagnostic =
                "W3D animation cache has no VFS";
        }
    } catch (const std::exception& exception) {
        completion.dependency.logicalName = job.logicalName;
        completion.dependency.sourcePath =
            data::w3d::w3dAnimationSourcePath(job.logicalName);
        completion.dependency.revision = 1u;
        completion.dependency.diagnostic =
            "W3D animation worker failed for " + job.logicalName +
            ": " + exception.what();
    } catch (...) {
        completion.dependency.logicalName = job.logicalName;
        completion.dependency.sourcePath =
            data::w3d::w3dAnimationSourcePath(job.logicalName);
        completion.dependency.revision = 1u;
        completion.dependency.diagnostic =
            "W3D animation worker failed for " + job.logicalName;
    }
    completion.workerNanoseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count());
    if (completion.clip) {
        completion.estimatedBytes = completion.clip->estimatedByteSize();
    }
    state->loadsInFlight.fetch_sub(1u, std::memory_order_relaxed);

    std::lock_guard lock(state->mutex);
    state->activeAnimations.erase(key);
    if (completion.generation !=
            state->generation.load(std::memory_order_acquire) ||
        state->shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    if (const auto requested = state->pendingPriorities.find(key);
        requested != state->pendingPriorities.end()) {
        completion.priority = std::max(
            completion.priority, requested->second);
    }
    state->completions.push_back(std::move(completion));
}

void W3dAnimationCache::reapFinishedTasks() noexcept {
    // Scheduler completion callbacks retire admitted tasks. Kept as a named
    // lifecycle hook for request/process call sites.
}

void W3dAnimationCache::cancelAsyncTasks(bool shuttingDown) noexcept {
    {
        std::lock_guard lock(m_async->mutex);
        if (shuttingDown) {
            m_async->shuttingDown.store(true, std::memory_order_release);
        }
        m_async->generation.fetch_add(1u, std::memory_order_acq_rel);
        m_cancelledQueued.fetch_add(
            m_async->jobs.size(), std::memory_order_relaxed);
        m_cancelledPending.fetch_add(
            m_async->completions.size() + m_async->activeAnimations.size(),
            std::memory_order_relaxed);
        m_async->jobs.clear();
        m_async->completions.clear();
        m_async->pendingAnimations.clear();
        m_async->pendingPriorities.clear();
    }

    const container::SharedPtr<AsyncState> state = m_async;
    std::unique_lock lock(state->mutex);
    state->idle.wait(lock, [state] { return state->scheduledTasks == 0; });
    state->activeAnimations.clear();
}

} // namespace engine::render
