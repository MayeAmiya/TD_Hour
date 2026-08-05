#pragma once

#include "core/container/hash_containers.h"

#include "engine/renderer/world/model/W3dStaticModel.h"
#include "engine/renderer/world/resource/RenderAssetLifecycle.h"
#include "engine/renderer/world/resource/RenderAssetScheduling.h"
#include "engine/resource/ResourceScheduler.h"

#include <cstddef>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
namespace data::w3d { struct ParsedW3D; }
namespace io { class VFS; }

namespace engine::render {

struct W3dModelHandle {
    static constexpr uint32_t InvalidIndex = std::numeric_limits<uint32_t>::max();

    uint32_t index = InvalidIndex;
    uint32_t generation = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != InvalidIndex && generation != 0;
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

    friend constexpr bool operator==(const W3dModelHandle&, const W3dModelHandle&) = default;
};

// Stable content-version token for renderer registrations and asynchronous
// hand-offs. Handle generation distinguishes slot reuse; revision
// distinguishes reloads within that generation. Neither GPU residency nor an
// SRV index is part of content identity.
struct W3dAssetVersion final {
    W3dModelHandle handle;
    uint64_t revision = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return handle.valid() && revision != 0;
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return valid();
    }
    friend constexpr bool operator==(
        const W3dAssetVersion&, const W3dAssetVersion&) = default;
};

enum class W3dAssetState : uint8_t {
    CpuLoadQueued,
    CpuLoading,
    CpuReady,
    UploadQueued,
    Uploading,
    GpuReady,
    GpuUploadFailed,
    Failed,
};
inline constexpr size_t kW3dAssetStateCount =
    static_cast<size_t>(W3dAssetState::Failed) + 1u;

// Upload priority is renderer scheduling policy only. CPU parsing is admitted
// by the process resource scheduler and every GPU mutation remains inside
// processGpuUploads() during the renderer-owned begin-frame window.
using W3dGpuUploadPriority = RenderAssetPriority;

inline constexpr size_t kW3dGpuUploadPriorityCount =
    kRenderAssetPriorityCount;

struct W3dAssetRequest {
    // A logical prototype ("PMWLDCRATE" or "AVHUMMER.CHASSIS"), a W3D
    // filename, or a canonical VFS path such as "Art/W3D/PMWldCrate.w3d".
    container::String source;

    // Optional exact HLOD/mesh prototype. Dotted mesh prototype names are
    // preserved; they are not treated as filename extensions.
    container::String prototype;

    bool queueGpuUpload = true;
    W3dGpuUploadPriority gpuUploadPriority = W3dGpuUploadPriority::Normal;
    // Hidden is an authored initial state; normal renderer requests retain the
    // geometry so ShowSubObject can reveal it later. Offline consumers may
    // explicitly opt out when they only need the initially-visible surface.
    bool includeHiddenMeshes = true;
    bool includeCollisionMeshes = false;
};

// Backend-owned immutable GPU representation. A D3D12 implementation derives
// from this marker without leaking D3D types into the asset/cache contract.
struct W3dGpuUseDiagnostic final {
    uint64_t frame = 0;
    uint64_t fence = 0;
    bool completed = false;
    bool exactFence = false;
};

class W3dGpuModel {
public:
    virtual ~W3dGpuModel() = default;
    [[nodiscard]] virtual uint64_t residentBytes() const noexcept { return 0; }
    [[nodiscard]] virtual uint64_t lastUsedFrame() const noexcept { return 0; }
    [[nodiscard]] virtual W3dGpuUseDiagnostic useDiagnostic() const noexcept {
        return {};
    }
    [[nodiscard]] virtual uint64_t retainedSortingBytes() const noexcept {
        return 0;
    }
};

struct W3dGpuUploadRequest {
    W3dModelHandle handle;
    uint64_t revision = 0;
    container::SharedPtr<const CpuStaticModel> cpuModel;
    uint64_t estimatedBytes = 0;
    W3dGpuUploadPriority priority = W3dGpuUploadPriority::Normal;
};

struct W3dGpuUploadResult {
    container::SharedPtr<const W3dGpuModel> model;
    container::String error;
    // CPU texture preparation is still in flight. Keep the immutable model in
    // the upload queue; this is neither a content error nor a GPU failure.
    bool deferred = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(model);
    }
};

struct W3dGpuUploadBudget final {
    size_t maxUploads = std::numeric_limits<size_t>::max();
    uint64_t maxEstimatedBytes = std::numeric_limits<uint64_t>::max();
    uint64_t maxElapsedMicroseconds = std::numeric_limits<uint64_t>::max();
};

struct W3dGpuUploadBatchStats final {
    size_t attemptedUploads = 0;
    size_t succeededUploads = 0;
    size_t failedUploads = 0;
    size_t deferredUploads = 0;
    size_t discardedStaleUploads = 0;
    size_t forcedOversizedUploads = 0;
    uint64_t attemptedEstimatedBytes = 0;
    uint64_t deferredEstimatedBytes = 0;
    uint64_t attemptedElapsedNanoseconds = 0;
    // Whole scheduling-pass wall time, including queue ranking/stat updates.
    uint64_t elapsedNanoseconds = 0;
    std::array<size_t, kW3dGpuUploadPriorityCount> attemptedByPriority{};
    std::array<size_t, kW3dGpuUploadPriorityCount> deferredByPriority{};
};

// Geometry bytes match D3D12W3dModel's immutable VB/IB copies. Material and
// mesh metadata are included as a deterministic scheduling proxy. Texture
// payload sizes are not resident in CpuStaticModel and remain the shared
// WorldTextureCache's responsibility.
[[nodiscard]] uint64_t estimateW3dGpuUploadBytes(
    const CpuStaticModel& model) noexcept;

struct W3dAssetCacheStats {
    size_t assetCount = 0;
    size_t parsedFileCount = 0;
    size_t queuedUploadCount = 0;
    // Legacy aggregate retained for existing diagnostics. It is exactly the
    // sum of queuedCpuLoadJobs and pendingCpuLoadCompletions; in-flight work
    // is reported separately and is never hidden inside this value.
    size_t queuedCpuLoadCount = 0;
    size_t queuedCpuLoadJobs = 0;
    size_t pendingCpuLoadCompletions = 0;
    size_t cpuLoadsInFlight = 0;
    uint64_t publishedCpuLoadCompletions = 0;
    uint64_t failedCpuLoadCompletions = 0;
    uint64_t failedSynchronousCpuLoads = 0;
    uint64_t cancelledQueuedCpuLoadJobs = 0;
    uint64_t cancelledPendingCpuLoadCompletions = 0;
    uint64_t discardedStaleCpuLoadCompletions = 0;
    uint32_t maximumCpuLoadQueueAge = 0;
    container::Array<size_t, kRenderAssetPriorityCount>
        queuedCpuLoadsByPriority{};
    uint64_t cpuReadyBytesPublished = 0;
    uint64_t cpuReadyWorkerNanoseconds = 0;
    uint64_t cpuReadyPublishMicroseconds = 0;
    uint64_t cpuReadyDeferred = 0;
    uint64_t cpuReadyForcedOversized = 0;
    uint32_t maximumCpuReadyAge = 0;
    size_t gpuReadyCount = 0;
    uint64_t gpuExternalOwnerReferences = 0;
    uint64_t gpuResidentBytes = 0;
    uint64_t gpuLatestUsedFrame = 0;
    uint64_t gpuLatestUsedFence = 0;
    uint64_t gpuUseFencesInFlight = 0;
    uint64_t gpuCompletedUseFramesWithoutExactFence = 0;
    uint64_t gpuResidencyEvictions = 0;
    uint64_t gpuResidencyEvictedBytes = 0;
    uint64_t gpuResidencyPinnedRejects = 0;
    uint64_t gpuResidencyReferencedRejects = 0;
    uint32_t gpuResidencyPins = 0;
    uint64_t cpuRetainedModelBytes = 0;
    uint64_t gpuRetainedSortingBytes = 0;
    size_t failedCount = 0;
    size_t externalHierarchyCount = 0;
    size_t resolvedHierarchyCount = 0;
    size_t skeletonFallbackCount = 0;
    size_t dependencyDiagnosticCount = 0;
    size_t dependencyNodeCount = 0;
    size_t dependencyEdgeCount = 0;
    size_t dependencyRequiredEdgeCount = 0;
    size_t dependencyOptionalEdgeCount = 0;
    size_t dependencyFallbackAllowedEdgeCount = 0;
    size_t dependencyMissingRequiredCount = 0;
    size_t dependencyFallbackResolvedCount = 0;
    size_t dependencyCycleRejectedCount = 0;
    size_t dependencyDepthRejectedCount = 0;
    size_t dependencySharedTargetCount = 0;
    uint64_t dependencyIncomingReferenceCount = 0;
    std::array<size_t, kW3dAssetStateCount> stateCounts{};
    uint64_t requests = 0;
    uint64_t cacheHits = 0;
    uint64_t cacheMisses = 0;
    uint64_t reloads = 0;
    uint64_t evictions = 0;
    uint64_t resets = 0;
    W3dGpuUploadBatchStats lastUploadBatch;
    W3dGpuUploadBatchStats lifetimeUploadTotals;
};

enum class W3dDependencyKind : uint8_t {
    Model,
    Hierarchy,
    Animation,
    Texture,
};

enum class W3dDependencyState : uint8_t {
    Ready,
    Missing,
    Fallback,
    Referenced,
};

struct W3dDependencyNode final {
    W3dDependencyKind kind = W3dDependencyKind::Model;
    W3dDependencyState state = W3dDependencyState::Referenced;
    RenderAssetErrorKind errorKind = RenderAssetErrorKind::None;
    container::String logicalName;
    container::String canonicalIdentity;
    uint64_t revision = 0;
    uint32_t incomingReferences = 0;
    container::Vector<container::String> diagnostics;
};

using W3dDependencyEdge = RenderAssetDependencyEdge;

struct W3dAssetDependencies final {
    container::String modelSourcePath;
    container::String hierarchyName;
    container::String hierarchySourcePath;
    bool externalHierarchyRequired = false;
    bool externalHierarchyResolved = false;
    bool skeletonFallback = false;
    container::Vector<container::String> animations;
    container::Vector<container::String> textures;
    container::Vector<container::String> diagnostics;
    container::Vector<W3dDependencyNode> nodes;
    container::Vector<W3dDependencyEdge> edges;
};

// Renderer-owner-thread cache for immutable static W3D assets. request/query,
// completion publish, GPU upload admission and reset are serialized by that
// owner. Resource-scheduler tasks return only immutable completion records;
// they never mutate cache maps or D3D12 state. GPU creation is explicitly
// deferred to the command-recording window after D3D12Device::beginFrame().
// Shared immutable CPU/GPU objects may be retained by readers after a lookup.
class W3dAssetCache final {
public:
    using UploadFunction = std::function<W3dGpuUploadResult(const W3dGpuUploadRequest&)>;
    using RetireFunction = std::function<void(container::SharedPtr<const W3dGpuModel>)>;

    W3dAssetCache();
    explicit W3dAssetCache(io::VFS& vfs) noexcept;
    ~W3dAssetCache();

    W3dAssetCache(const W3dAssetCache&) = delete;
    W3dAssetCache& operator=(const W3dAssetCache&) = delete;
    W3dAssetCache(W3dAssetCache&&) = delete;
    W3dAssetCache& operator=(W3dAssetCache&&) = delete;

    // Equal canonical source/prototype/options return the same live handle.
    // A valid handle is also returned for load failures so callers can inspect
    // state/error and explicitly retry with reload().
    [[nodiscard]] W3dModelHandle request(const W3dAssetRequest& request);
    [[nodiscard]] W3dModelHandle request(container::StringView logicalPrototype,
                                         bool queueGpuUpload = true);
    // Non-blocking renderer path. CPU VFS read/parse/build runs on the unified
    // resource scheduler; processCpuLoads() publishes immutable results at a
    // renderer-owned frame boundary.
    [[nodiscard]] W3dModelHandle requestAsync(const W3dAssetRequest& request);
    [[nodiscard]] W3dModelHandle requestAsync(
        container::StringView logicalPrototype, bool queueGpuUpload = true,
        RenderAssetPriority priority = RenderAssetPriority::Normal);
    size_t processCpuLoads(
        size_t maxCompletions = std::numeric_limits<size_t>::max());
    size_t processCpuLoads(const RenderAssetReadyBudget& budget);

    [[nodiscard]] bool contains(W3dModelHandle handle) const noexcept;
    [[nodiscard]] std::optional<W3dAssetState> state(W3dModelHandle handle) const noexcept;
    [[nodiscard]] uint64_t revision(W3dModelHandle handle) const noexcept;
    [[nodiscard]] std::optional<W3dAssetVersion> version(
        W3dModelHandle handle) const noexcept;
    [[nodiscard]] bool matches(W3dAssetVersion version) const noexcept;
    [[nodiscard]] container::SharedPtr<const CpuStaticModel> cpuModel(W3dModelHandle handle) const;
    [[nodiscard]] container::SharedPtr<const W3dGpuModel> gpuModel(W3dModelHandle handle);
    [[nodiscard]] container::String error(W3dModelHandle handle) const;
    [[nodiscard]] container::String sourcePath(W3dModelHandle handle) const;
    [[nodiscard]] container::String prototype(W3dModelHandle handle) const;
    [[nodiscard]] std::optional<W3dAssetDependencies> dependencies(
        W3dModelHandle handle) const;
    void recordAnimationDependency(
        W3dModelHandle handle,
        container::StringView logicalName,
        container::StringView hierarchyName,
        container::StringView sourcePath,
        uint64_t revision,
        bool ready,
        container::StringView diagnostic = {});
    void recordTextureDependency(
        W3dModelHandle handle,
        container::StringView textureName,
        bool resident);

    // Idempotently puts a CPU-ready asset into the GPU queue. Failed GPU
    // uploads can be retried without reparsing the W3D.
    bool queueGpuUpload(
        W3dModelHandle handle,
        W3dGpuUploadPriority priority = W3dGpuUploadPriority::Normal);

    // These are frame-phase notifications only; the cache never begins or
    // submits a device frame itself. processGpuUploads() is a no-op until the
    // begin-frame completion notification opens the upload window.
    void notifyBeginFrameComplete() noexcept;
    void notifyFrameSubmitted() noexcept;
    [[nodiscard]] bool gpuUploadWindowOpen() const noexcept;
    size_t processGpuUploads(const UploadFunction& upload,
                             size_t maxUploads = std::numeric_limits<size_t>::max());
    size_t processGpuUploads(const UploadFunction& upload,
                             const W3dGpuUploadBudget& budget);
    [[nodiscard]] const W3dGpuUploadBatchStats& lastGpuUploadBatchStats()
        const noexcept { return m_lastUploadBatch; }

    void setRetireFunction(RetireFunction retire);

    // Reload preserves the generation handle and advances revision. Eviction
    // invalidates the handle and advances generation before its slot is reused.
    bool reload(W3dModelHandle handle, bool queueUpload = true);
    bool evict(W3dModelHandle handle);
    bool setGpuResidencyPinned(
        W3dModelHandle handle, RenderAssetPinScope scope,
        bool pinned) noexcept;
    void beginResidencyFrame(uint64_t frameOrdinal) noexcept;
    size_t trimGpuResidency(
        size_t maximumModels, uint64_t maximumBytes,
        uint64_t graceFrames);
    void clear();

    [[nodiscard]] W3dAssetCacheStats stats() const noexcept;
    [[nodiscard]] std::optional<RenderAssetLifecycleRecord>
    describeLifecycle(W3dModelHandle handle) const;

private:
    struct ModelKey {
        container::String sourcePath;
        container::String prototype;
        bool includeHiddenMeshes = true;
        bool includeCollisionMeshes = false;

        friend bool operator==(const ModelKey&, const ModelKey&) = default;
    };

    struct ModelKeyHash {
        size_t operator()(const ModelKey& key) const noexcept;
    };

    struct Slot {
        ModelKey key;
        uint32_t generation = 1;
        uint64_t revision = 0;
        bool occupied = false;
        W3dAssetState state = W3dAssetState::Failed;
        container::SharedPtr<const CpuStaticModel> cpuModel;
        container::SharedPtr<const W3dGpuModel> gpuModel;
        W3dAssetDependencies dependencies;
        container::String error;
        RenderAssetErrorKind errorKind = RenderAssetErrorKind::None;
        bool queueGpuAfterCpuLoad = false;
        uint8_t residencyPinMask = 0;
        uint64_t residentSinceFrame = 0;
        uint64_t lastReachableFrame = 0;
        W3dGpuUploadPriority queueGpuAfterCpuLoadPriority =
            W3dGpuUploadPriority::Normal;
    };

    struct PendingUpload {
        W3dModelHandle handle;
        uint64_t revision = 0;
        uint64_t estimatedBytes = 0;
        uint64_t enqueueSequence = 0;
        uint32_t deferredPasses = 0;
        W3dGpuUploadPriority priority = W3dGpuUploadPriority::Normal;
    };

    struct ParsedFileNode {
        container::SharedPtr<const data::w3d::ParsedW3D> parsed;
        container::String error;
        RenderAssetErrorKind errorKind = RenderAssetErrorKind::None;
        uint64_t revision = 1;
    };

    struct CpuLoadJob {
        W3dModelHandle handle;
        uint64_t revision = 0;
        uint64_t session = 0;
        ModelKey key;
        uint64_t enqueueSequence = 0;
        uint32_t deferredPasses = 0;
        RenderAssetPriority priority = RenderAssetPriority::Normal;
    };

    struct CpuLoadCompletion {
        W3dModelHandle handle;
        uint64_t revision = 0;
        uint64_t session = 0;
        container::SharedPtr<const CpuStaticModel> model;
        W3dAssetDependencies dependencies;
        container::String error;
        RenderAssetErrorKind errorKind = RenderAssetErrorKind::None;
        uint64_t enqueueSequence = 0;
        uint64_t estimatedBytes = 0;
        uint64_t workerNanoseconds = 0;
        uint32_t deferredPasses = 0;
        RenderAssetPriority priority = RenderAssetPriority::Normal;
    };

    struct CpuLoadWork;
    struct CpuLoadMailbox;

    struct CpuLoadTask {
        W3dModelHandle handle;
        uint64_t revision = 0;
        uint64_t session = 0;
        engine::resource::ResourceTicket ticket;
        std::shared_ptr<CpuLoadWork> work;
    };

    [[nodiscard]] static std::optional<ModelKey> resolveKey(
        const W3dAssetRequest& request, container::String& error);
    [[nodiscard]] Slot* findSlot(W3dModelHandle handle) noexcept;
    [[nodiscard]] const Slot* findSlot(W3dModelHandle handle) const noexcept;
    [[nodiscard]] W3dModelHandle allocateSlot(ModelKey key);
    [[nodiscard]] container::SharedPtr<const data::w3d::ParsedW3D> loadParsedFile(
        container::StringView sourcePath, container::String& error,
        uint64_t* revision = nullptr,
        RenderAssetErrorKind* errorKind = nullptr);
    bool loadCpuAsset(uint32_t slotIndex);
    void unregisterFileDependencies(uint32_t slotIndex);
    void registerFileDependency(uint32_t slotIndex, container::StringView canonicalPath);
    void registerDependencyGraph(const W3dAssetDependencies& dependencies);
    void unregisterDependencyGraph(const W3dAssetDependencies& dependencies);
    void dispatchCpuLoads();
    void collectFinishedCpuLoads();
    void stopCpuLoads() noexcept;
    [[nodiscard]] static CpuLoadCompletion runCpuLoad(
        io::VFS& vfs, CpuLoadJob job);
    void discardPendingCpuLoads(W3dModelHandle handle);
    void discardPendingUploads(W3dModelHandle handle);
    void retireGpuModel(container::SharedPtr<const W3dGpuModel> model);
    static uint32_t nextGeneration(uint32_t generation) noexcept;

    io::VFS* m_vfs = nullptr;
    container::Vector<Slot> m_slots;
    container::Vector<uint32_t> m_freeSlots;
    container::HashMap<ModelKey, uint32_t, ModelKeyHash> m_modelLookup;
    container::HashMap<container::String, ParsedFileNode> m_fileCache;
    container::HashMap<container::String, container::HashSet<uint32_t>> m_fileDependents;
    container::HashMap<container::String, uint32_t>
        m_dependencyTargetReferences;
    container::Deque<PendingUpload> m_uploadQueue;
    container::Deque<CpuLoadJob> m_cpuLoadJobs;
    container::Deque<CpuLoadCompletion> m_cpuLoadCompletions;
    container::HashMap<uint64_t, RenderAssetPriority> m_cpuLoadPriorities;
    container::Vector<CpuLoadTask> m_cpuLoadTasks;
    std::shared_ptr<CpuLoadMailbox> m_cpuLoadMailbox;
    uint64_t m_cpuLoadSession = 1;
    std::atomic<size_t> m_cpuLoadsInFlight{0};
    std::atomic<uint64_t> m_publishedCpuLoadCompletions{0};
    std::atomic<uint64_t> m_failedCpuLoadCompletions{0};
    uint64_t m_failedSynchronousCpuLoads = 0;
    std::atomic<uint64_t> m_cancelledQueuedCpuLoadJobs{0};
    std::atomic<uint64_t> m_cancelledPendingCpuLoadCompletions{0};
    std::atomic<uint64_t> m_discardedStaleCpuLoadCompletions{0};
    uint64_t m_nextCpuLoadSequence = 1;
    uint64_t m_cpuReadyBytesPublished = 0;
    uint64_t m_cpuReadyWorkerNanoseconds = 0;
    uint64_t m_cpuReadyPublishMicroseconds = 0;
    uint64_t m_cpuReadyDeferred = 0;
    uint64_t m_cpuReadyForcedOversized = 0;
    uint32_t m_maximumCpuReadyAge = 0;
    RetireFunction m_retire;
    bool m_uploadWindowOpen = false;
    uint64_t m_nextUploadSequence = 1;
    W3dGpuUploadBatchStats m_lastUploadBatch;
    W3dGpuUploadBatchStats m_lifetimeUploadTotals;
    uint64_t m_requests = 0;
    uint64_t m_cacheHits = 0;
    uint64_t m_cacheMisses = 0;
    uint64_t m_reloads = 0;
    uint64_t m_evictions = 0;
    uint64_t m_gpuResidencyEvictions = 0;
    uint64_t m_gpuResidencyEvictedBytes = 0;
    uint64_t m_gpuResidencyPinnedRejects = 0;
    uint64_t m_gpuResidencyReferencedRejects = 0;
    uint64_t m_residencyFrame = 0;
    uint64_t m_resets = 0;
};

} // namespace engine::render
