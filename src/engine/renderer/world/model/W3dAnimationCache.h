#pragma once

#include "core/container/hash_containers.h"
#include "engine/renderer/world/resource/RenderAssetLifecycle.h"
#include "engine/renderer/world/resource/RenderAssetScheduling.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <optional>
namespace io { class VFS; }

namespace engine::render {

class AnimationClip;

struct W3dAnimationDependency final {
    container::String logicalName;
    container::String hierarchyName;
    container::String sourcePath;
    uint64_t revision = 0;
    bool ready = false;
    container::String diagnostic;
};

struct W3dAnimationVersion final {
    container::String logicalName;
    uint64_t generation = 0;
    uint64_t revision = 0;

    [[nodiscard]] bool valid() const noexcept {
        return !logicalName.empty() && generation != 0 && revision != 0;
    }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
    friend bool operator==(
        const W3dAnimationVersion&, const W3dAnimationVersion&) = default;
};

struct W3dAnimationCacheStats final {
    size_t clipCount = 0;
    size_t loadedSourceCount = 0;
    size_t failedSourceCount = 0;
    size_t queuedJobs = 0;
    size_t pendingCompletions = 0;
    size_t pendingReadyCompletions = 0;
    size_t pendingFailedCompletions = 0;
    size_t loadsInFlight = 0;
    uint64_t generation = 0;
    uint64_t requests = 0;
    uint64_t cacheHits = 0;
    uint64_t cacheMisses = 0;
    uint64_t published = 0;
    uint64_t failures = 0;
    uint64_t cancelledQueued = 0;
    uint64_t cancelledPending = 0;
    uint64_t staleRejected = 0;
    uint64_t resets = 0;
    uint64_t readyBytesPublished = 0;
    uint64_t readyWorkerNanoseconds = 0;
    uint64_t readyPublishMicroseconds = 0;
    uint64_t readyDeferred = 0;
    uint64_t readyForcedOversized = 0;
    uint32_t maximumReadyAge = 0;
    uint64_t retainedClipBytes = 0;
    container::Array<uint64_t, kRenderAssetPriorityCount>
        publishedByPriority{};
};

// Renderer-owner-thread cache for hierarchy animation W3D files. Animation
// names in Generals INI use "HIERARCHY.CLIP" form (for example
// AIHERO_SKL.AIHERO_STA). request/query/publish/reset are serialized by that
// owner. Shared resource-executor tasks write only immutable completions; they
// never read a live snapshot/cache map or publish GPU work.
class W3dAnimationCache final {
public:
    W3dAnimationCache();
    explicit W3dAnimationCache(io::VFS& vfs);
    ~W3dAnimationCache();

    [[nodiscard]] container::SharedPtr<const AnimationClip> find(container::StringView animationName);
    [[nodiscard]] container::SharedPtr<const AnimationClip> findLoaded(
        container::StringView animationName) const;
    bool requestAsync(
        container::StringView animationName,
        RenderAssetPriority priority = RenderAssetPriority::Normal);
    size_t processLoads();
    size_t processLoads(const RenderAssetReadyBudget& budget);
    [[nodiscard]] container::String error(container::StringView animationName) const;
    [[nodiscard]] W3dAnimationDependency dependency(
        container::StringView animationName) const;
    [[nodiscard]] std::optional<W3dAnimationVersion> version(
        container::StringView animationName) const;
    [[nodiscard]] bool matches(const W3dAnimationVersion& version) const;
    [[nodiscard]] W3dAnimationCacheStats stats() const noexcept;
    [[nodiscard]] RenderAssetLifecycleRecord describeLifecycle(
        container::StringView animationName) const;
    void clear();

private:
    struct AsyncJob {
        container::String logicalName;
        uint64_t generation = 0;
        uint64_t enqueueSequence = 0;
        uint64_t estimatedBytes = 0;
        uint32_t deferredPasses = 0;
        RenderAssetPriority priority = RenderAssetPriority::Normal;
    };

    struct AsyncCompletion {
        container::String logicalName;
        container::SharedPtr<const AnimationClip> clip;
        W3dAnimationDependency dependency;
        uint64_t generation = 0;
        uint64_t enqueueSequence = 0;
        uint64_t estimatedBytes = 0;
        uint64_t workerNanoseconds = 0;
        uint32_t deferredPasses = 0;
        RenderAssetPriority priority = RenderAssetPriority::Normal;
    };

    struct AsyncState;

    bool loadSource(container::StringView hierarchyName);
    bool submitAsyncJob(AsyncJob job);
    static void runAsyncJob(
        container::SharedPtr<AsyncState> state, AsyncJob job) noexcept;
    void reapFinishedTasks() noexcept;
    void cancelAsyncTasks(bool shuttingDown) noexcept;

    container::HashMap<container::String, container::SharedPtr<const AnimationClip>> m_clips;
    container::HashMap<container::String, container::String> m_sourceErrors;
    container::HashMap<container::String, uint64_t> m_sourceRevisions;
    container::HashSet<container::String> m_loadedSources;
    container::SharedPtr<AsyncState> m_async;
    std::atomic<uint64_t> m_requests{0};
    std::atomic<uint64_t> m_cacheHits{0};
    std::atomic<uint64_t> m_cacheMisses{0};
    std::atomic<uint64_t> m_published{0};
    std::atomic<uint64_t> m_failures{0};
    std::atomic<uint64_t> m_cancelledQueued{0};
    std::atomic<uint64_t> m_cancelledPending{0};
    std::atomic<uint64_t> m_staleRejected{0};
    std::atomic<uint64_t> m_resets{0};
    uint64_t m_nextEnqueueSequence = 1;
    uint64_t m_readyBytesPublished = 0;
    uint64_t m_readyWorkerNanoseconds = 0;
    uint64_t m_readyPublishMicroseconds = 0;
    uint64_t m_readyDeferred = 0;
    uint64_t m_readyForcedOversized = 0;
    uint32_t m_maximumReadyAge = 0;
    container::Array<uint64_t, kRenderAssetPriorityCount>
        m_publishedByPriority{};
};

} // namespace engine::render
