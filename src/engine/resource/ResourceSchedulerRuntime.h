#pragma once

#include "ResourceScheduler.h"

#include <memory>

namespace engine::resource {

// Process-wide admission owner used by the production application. The
// dedicated thread performs only deterministic queue arbitration and owner
// completion callbacks; CPU resource work remains on the existing resource
// and scene executors, and GPU mutation remains on the render thread.
class ResourceSchedulerRuntime final {
public:
    explicit ResourceSchedulerRuntime(ResourceSchedulerConfig config = {});
    ~ResourceSchedulerRuntime();

    ResourceSchedulerRuntime(const ResourceSchedulerRuntime&) = delete;
    ResourceSchedulerRuntime& operator=(const ResourceSchedulerRuntime&) = delete;
    ResourceSchedulerRuntime(ResourceSchedulerRuntime&&) = delete;
    ResourceSchedulerRuntime& operator=(ResourceSchedulerRuntime&&) = delete;

    [[nodiscard]] ResourceSubmitResult submit(
        ResourceRequest request,
        ResourceTask task,
        ResourceCompletionCallback completion = {});
    [[nodiscard]] bool cancel(const ResourceTicket& ticket);
    void cancelGeneration(uint64_t generation);
    void advanceMinimumGeneration(uint64_t minimumGeneration);
    [[nodiscard]] ResourceSchedulerStats stats() const;
    void shutdown() noexcept;

private:
    struct State;
    std::shared_ptr<State> m_state;
};

// ApplicationHost installs one runtime before engine subsystems initialize
// and removes it only after those subsystems have shut down. Callers outside a
// production host receive nullptr and must degrade without direct executor
// submission, preserving the single-admission contract.
void installResourceSchedulerRuntime(ResourceSchedulerRuntime* runtime) noexcept;
[[nodiscard]] ResourceSchedulerRuntime* activeResourceSchedulerRuntime() noexcept;

} // namespace engine::resource
