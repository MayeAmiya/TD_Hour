#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <thread>

namespace engine::resource {

enum class ResourceKind : uint8_t {
    Model,
    Texture,
    Animation,
    Audio,
    Glyph,
    Scene,
    Count,
};

enum class ResourceDemand : uint8_t {
    Optional,
    Prefetch,
    Visible,
    StartupRequired,
};

enum class ResourceLane : uint8_t {
    Resource,
    Scene,
};

struct ResourceJobKey final {
    ResourceKind kind = ResourceKind::Model;
    std::string canonicalIdentity;
    uint64_t variant = 0;
    // Zero denotes process-persistent work (for example UI glyphs) and is not
    // affected by the monotonic session-generation watermark.
    uint64_t generation = 0;
};

struct ResourceRequest final {
    ResourceJobKey key;
    ResourceDemand demand = ResourceDemand::Optional;
    ResourceLane lane = ResourceLane::Resource;
    uint64_t estimatedBytes = 0;
};

enum class ResourceJobState : uint8_t {
    Invalid,
    Queued,
    InFlight,
    Ready,
    Failed,
    Cancelled,
    Stale,
};

enum class StartupResourceState : uint8_t {
    NotApplicable,
    Pending,
    Ready,
    Failed,
    Cancelled,
    Stale,
};

enum class ResourceTaskResult : uint8_t {
    Ready,
    Failed,
};

class ResourceTaskContext final {
public:
    [[nodiscard]] bool stopRequested() const noexcept;
    [[nodiscard]] uint64_t generation() const noexcept;
    [[nodiscard]] uint64_t sequence() const noexcept;

private:
    struct Control;

    explicit ResourceTaskContext(std::shared_ptr<const Control> control) noexcept;

    std::shared_ptr<const Control> m_control;

    friend class ResourceScheduler;
};

using ResourceTask = std::function<ResourceTaskResult(const ResourceTaskContext&)>;

struct ResourceCompletion final {
    ResourceJobKey key;
    ResourceDemand demand = ResourceDemand::Optional;
    ResourceLane lane = ResourceLane::Resource;
    ResourceJobState state = ResourceJobState::Invalid;
    uint64_t sequence = 0;
    uint64_t estimatedBytes = 0;
};

// This callback always runs from pumpCompletions() (or shutdown()) on the
// scheduler owner thread, never from a resource worker. A cache should use it
// only to publish into that cache's own generation-safe owner mailbox.
using ResourceCompletionCallback = std::function<void(const ResourceCompletion&)>;

class ResourceTicket final {
public:
    ResourceTicket() = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] uint64_t sequence() const noexcept;
    [[nodiscard]] uint64_t generation() const noexcept;
    [[nodiscard]] ResourceJobState state() const noexcept;
    [[nodiscard]] StartupResourceState startupState() const noexcept;

private:
    struct State;

    explicit ResourceTicket(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> m_state;

    friend class ResourceScheduler;
};

enum class ResourceSubmitStatus : uint8_t {
    Accepted,
    InvalidRequest,
    QueueFull,
    EstimatedBytesTooLarge,
    StaleGeneration,
    ShuttingDown,
};

struct ResourceSubmitResult final {
    ResourceSubmitStatus status = ResourceSubmitStatus::InvalidRequest;
    ResourceTicket ticket;

    [[nodiscard]] bool accepted() const noexcept {
        return status == ResourceSubmitStatus::Accepted;
    }
};

struct ResourceKindLimits final {
    size_t maxQueued = 1024;
    size_t maxInFlight = 8;
    uint64_t maxInFlightBytes = 256ull * 1024ull * 1024ull;
};

struct ResourceSchedulerConfig final {
    size_t maxQueued = 4096;
    size_t maxInFlight = 32;
    uint64_t maxInFlightBytes = 512ull * 1024ull * 1024ull;
    uint64_t agingDispatchCycles = 8;
    std::array<ResourceKindLimits, static_cast<size_t>(ResourceKind::Count)> perKind{};
};

struct ResourceSchedulerStats final {
    size_t queued = 0;
    size_t inFlight = 0;
    uint64_t inFlightBytes = 0;
    uint64_t minimumGeneration = 0;
    bool accepting = false;
    std::array<size_t, static_cast<size_t>(ResourceKind::Count)> queuedPerKind{};
    std::array<size_t, static_cast<size_t>(ResourceKind::Count)> inFlightPerKind{};
    std::array<uint64_t, static_cast<size_t>(ResourceKind::Count)> inFlightBytesPerKind{};
};

// ResourceScheduler owns CPU resource admission only. It never performs GPU
// mutation. submit/cancel are thread-safe; dispatch and completion pumping are
// deliberately single-owner operations so ordering remains stable.
class ResourceScheduler final {
public:
    explicit ResourceScheduler(ResourceSchedulerConfig config = {});
    ~ResourceScheduler();

    ResourceScheduler(const ResourceScheduler&) = delete;
    ResourceScheduler& operator=(const ResourceScheduler&) = delete;
    ResourceScheduler(ResourceScheduler&&) = delete;
    ResourceScheduler& operator=(ResourceScheduler&&) = delete;

    [[nodiscard]] ResourceSubmitResult submit(
        ResourceRequest request,
        ResourceTask task,
        ResourceCompletionCallback completion = {});

    // Runs completion callbacks first and then admits work. maxJobs controls
    // this pump only; configured global/per-kind limits remain authoritative.
    [[nodiscard]] size_t pump(
        size_t maxJobs = std::numeric_limits<size_t>::max());
    [[nodiscard]] size_t pumpCompletions(
        size_t maxCallbacks = std::numeric_limits<size_t>::max());
    [[nodiscard]] size_t pumpDispatch(
        size_t maxJobs = std::numeric_limits<size_t>::max());

    [[nodiscard]] bool cancel(const ResourceTicket& ticket);
    void cancelGeneration(uint64_t generation);

    // Non-zero jobs older than minimumGeneration become Stale. Generation zero
    // is process-persistent. Equal/newer jobs remain valid; the watermark is
    // monotonic and submissions below it are rejected.
    void advanceMinimumGeneration(uint64_t minimumGeneration);

    [[nodiscard]] ResourceSchedulerStats stats() const;
    [[nodiscard]] bool isOwnerThread() const noexcept;

    // Stops admission, cancels queued/in-flight jobs, waits for every admitted
    // worker, and publishes terminal callbacks before returning.
    void shutdown();

private:
    struct SharedState;

    std::shared_ptr<SharedState> m_state;
    std::thread::id m_ownerThread;
};

} // namespace engine::resource
