#include "ResourceSchedulerRuntime.h"

#include "core/platform/runtime_threads.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace engine::resource {
namespace {

std::atomic<ResourceSchedulerRuntime*> g_activeRuntime{nullptr};

} // namespace

struct ResourceSchedulerRuntime::State final {
    explicit State(ResourceSchedulerConfig schedulerConfig)
        : config(std::move(schedulerConfig)) {}

    ResourceSchedulerConfig config;
    mutable std::mutex mutex;
    std::condition_variable wake;
    ResourceScheduler* scheduler = nullptr;
    std::jthread owner;
    uint64_t wakeSequence = 0;
    bool ready = false;
    bool accepting = true;
    bool stopped = false;
};

ResourceSchedulerRuntime::ResourceSchedulerRuntime(ResourceSchedulerConfig config)
    : m_state(std::make_shared<State>(std::move(config))) {
    const std::shared_ptr<State> state = m_state;
    state->owner = std::jthread([state](std::stop_token stopToken) {
        platform::runtime::ThreadRoleScope role(
            platform::runtime::ThreadRole::ResourceScheduler);
        ResourceScheduler scheduler(state->config);
        {
            std::scoped_lock lock(state->mutex);
            state->scheduler = &scheduler;
            state->ready = true;
        }
        state->wake.notify_all();

        uint64_t observedWake = 0;
        while (!stopToken.stop_requested()) {
            static_cast<void>(scheduler.pump());
            std::unique_lock lock(state->mutex);
            observedWake = state->wakeSequence;
            state->wake.wait_for(lock, std::chrono::milliseconds(1), [&] {
                return stopToken.stop_requested() ||
                    state->wakeSequence != observedWake;
            });
        }

        scheduler.shutdown();
        {
            std::scoped_lock lock(state->mutex);
            state->scheduler = nullptr;
            state->stopped = true;
        }
        state->wake.notify_all();
    });

    std::unique_lock lock(state->mutex);
    state->wake.wait(lock, [&] { return state->ready || state->stopped; });
}

ResourceSchedulerRuntime::~ResourceSchedulerRuntime() {
    shutdown();
    ResourceSchedulerRuntime* expected = this;
    static_cast<void>(g_activeRuntime.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel,
        std::memory_order_acquire));
}

ResourceSubmitResult ResourceSchedulerRuntime::submit(
    ResourceRequest request,
    ResourceTask task,
    ResourceCompletionCallback completion) {
    const std::shared_ptr<State> state = m_state;
    ResourceSubmitResult result;
    {
        std::scoped_lock lock(state->mutex);
        if (!state->accepting || !state->scheduler) {
            result.status = ResourceSubmitStatus::ShuttingDown;
            return result;
        }
        result = state->scheduler->submit(
            std::move(request), std::move(task), std::move(completion));
        if (result.accepted()) ++state->wakeSequence;
    }
    if (result.accepted()) state->wake.notify_one();
    return result;
}

bool ResourceSchedulerRuntime::cancel(const ResourceTicket& ticket) {
    const std::shared_ptr<State> state = m_state;
    bool cancelled = false;
    {
        std::scoped_lock lock(state->mutex);
        if (state->scheduler) {
            cancelled = state->scheduler->cancel(ticket);
            if (cancelled) ++state->wakeSequence;
        }
    }
    if (cancelled) state->wake.notify_one();
    return cancelled;
}

void ResourceSchedulerRuntime::cancelGeneration(uint64_t generation) {
    const std::shared_ptr<State> state = m_state;
    {
        std::scoped_lock lock(state->mutex);
        if (!state->scheduler) return;
        state->scheduler->cancelGeneration(generation);
        ++state->wakeSequence;
    }
    state->wake.notify_one();
}

void ResourceSchedulerRuntime::advanceMinimumGeneration(
    uint64_t minimumGeneration) {
    const std::shared_ptr<State> state = m_state;
    {
        std::scoped_lock lock(state->mutex);
        if (!state->scheduler) return;
        state->scheduler->advanceMinimumGeneration(minimumGeneration);
        ++state->wakeSequence;
    }
    state->wake.notify_one();
}

ResourceSchedulerStats ResourceSchedulerRuntime::stats() const {
    const std::shared_ptr<State> state = m_state;
    std::scoped_lock lock(state->mutex);
    return state->scheduler ? state->scheduler->stats()
                            : ResourceSchedulerStats{};
}

void ResourceSchedulerRuntime::shutdown() noexcept {
    const std::shared_ptr<State> state = m_state;
    if (!state) return;
    {
        std::scoped_lock lock(state->mutex);
        if (!state->accepting && state->stopped) return;
        state->accepting = false;
        ++state->wakeSequence;
        state->owner.request_stop();
    }
    state->wake.notify_all();
    if (state->owner.joinable()) state->owner.join();
}

void installResourceSchedulerRuntime(ResourceSchedulerRuntime* runtime) noexcept {
    g_activeRuntime.store(runtime, std::memory_order_release);
}

ResourceSchedulerRuntime* activeResourceSchedulerRuntime() noexcept {
    return g_activeRuntime.load(std::memory_order_acquire);
}

} // namespace engine::resource
