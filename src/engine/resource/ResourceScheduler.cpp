#include "ResourceScheduler.h"

#include "core/platform/runtime_threads.h"

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <utility>
#include <vector>

namespace engine::resource {
namespace {

constexpr size_t kindIndex(ResourceKind kind) noexcept {
    return static_cast<size_t>(kind);
}

constexpr bool isTerminal(ResourceJobState state) noexcept {
    return state == ResourceJobState::Ready ||
           state == ResourceJobState::Failed ||
           state == ResourceJobState::Cancelled ||
           state == ResourceJobState::Stale;
}

constexpr uint8_t demandRank(ResourceDemand demand) noexcept {
    return static_cast<uint8_t>(demand);
}

ResourceSchedulerConfig sanitizeConfig(ResourceSchedulerConfig config) noexcept {
    assert(config.maxQueued > 0);
    assert(config.maxInFlight > 0);
    assert(config.maxInFlightBytes > 0);
    assert(config.agingDispatchCycles > 0);
    config.maxQueued = std::max<size_t>(config.maxQueued, 1u);
    config.maxInFlight = std::max<size_t>(config.maxInFlight, 1u);
    config.maxInFlightBytes = std::max<uint64_t>(
        config.maxInFlightBytes, 1u);
    config.agingDispatchCycles = std::max<uint64_t>(
        config.agingDispatchCycles, 1u);
    for (ResourceKindLimits& limits : config.perKind) {
        assert(limits.maxQueued > 0);
        assert(limits.maxInFlight > 0);
        assert(limits.maxInFlightBytes > 0);
        limits.maxQueued = std::max<size_t>(limits.maxQueued, 1u);
        limits.maxInFlight = std::max<size_t>(limits.maxInFlight, 1u);
        limits.maxInFlightBytes = std::max<uint64_t>(
            limits.maxInFlightBytes, 1u);
    }
    return config;
}

} // namespace

struct ResourceTaskContext::Control final {
    std::atomic_bool stopRequested{false};
    std::atomic_bool stale{false};
    uint64_t generation = 0;
    uint64_t sequence = 0;
};

struct ResourceTicket::State final {
    std::atomic<ResourceJobState> state{ResourceJobState::Invalid};
    ResourceDemand demand = ResourceDemand::Optional;
    uint64_t generation = 0;
    uint64_t sequence = 0;
};

struct ResourceScheduler::SharedState final {
    struct Job final {
        ResourceRequest request;
        uint64_t sequence = 0;
        uint64_t enqueueCycle = 0;
        ResourceTask task;
        ResourceCompletionCallback completion;
        std::shared_ptr<ResourceTaskContext::Control> control;
        std::shared_ptr<ResourceTicket::State> ticket;
    };

    struct CompletedJob final {
        std::shared_ptr<Job> job;
        ResourceJobState state = ResourceJobState::Failed;
    };

    explicit SharedState(ResourceSchedulerConfig schedulerConfig)
        : config(std::move(schedulerConfig)) {}

    ResourceSchedulerConfig config;
    mutable std::mutex mutex;
    std::condition_variable idleCondition;
    bool accepting = true;
    bool shutdownStarted = false;
    uint64_t nextSequence = 1;
    uint64_t dispatchCycle = 0;
    uint64_t minimumGeneration = 0;
    size_t inFlight = 0;
    uint64_t inFlightBytes = 0;
    size_t activeWorkers = 0;
    std::array<size_t, static_cast<size_t>(ResourceKind::Count)> queuedPerKind{};
    std::array<size_t, static_cast<size_t>(ResourceKind::Count)> inFlightPerKind{};
    std::array<uint64_t, static_cast<size_t>(ResourceKind::Count)> inFlightBytesPerKind{};
    std::deque<std::shared_ptr<Job>> queued;
    std::vector<std::shared_ptr<Job>> active;
    std::deque<CompletedJob> completed;
};

ResourceTaskContext::ResourceTaskContext(
    std::shared_ptr<const Control> control) noexcept
    : m_control(std::move(control)) {}

bool ResourceTaskContext::stopRequested() const noexcept {
    return !m_control || m_control->stopRequested.load(std::memory_order_acquire);
}

uint64_t ResourceTaskContext::generation() const noexcept {
    return m_control ? m_control->generation : 0;
}

uint64_t ResourceTaskContext::sequence() const noexcept {
    return m_control ? m_control->sequence : 0;
}

ResourceTicket::ResourceTicket(std::shared_ptr<State> state) noexcept
    : m_state(std::move(state)) {}

bool ResourceTicket::valid() const noexcept {
    return static_cast<bool>(m_state);
}

uint64_t ResourceTicket::sequence() const noexcept {
    return m_state ? m_state->sequence : 0;
}

uint64_t ResourceTicket::generation() const noexcept {
    return m_state ? m_state->generation : 0;
}

ResourceJobState ResourceTicket::state() const noexcept {
    return m_state
        ? m_state->state.load(std::memory_order_acquire)
        : ResourceJobState::Invalid;
}

StartupResourceState ResourceTicket::startupState() const noexcept {
    if (!m_state || m_state->demand != ResourceDemand::StartupRequired) {
        return StartupResourceState::NotApplicable;
    }

    switch (m_state->state.load(std::memory_order_acquire)) {
    case ResourceJobState::Queued:
    case ResourceJobState::InFlight:
        return StartupResourceState::Pending;
    case ResourceJobState::Ready:
        return StartupResourceState::Ready;
    case ResourceJobState::Failed:
        return StartupResourceState::Failed;
    case ResourceJobState::Cancelled:
        return StartupResourceState::Cancelled;
    case ResourceJobState::Stale:
        return StartupResourceState::Stale;
    case ResourceJobState::Invalid:
    default:
        return StartupResourceState::NotApplicable;
    }
}

ResourceScheduler::ResourceScheduler(ResourceSchedulerConfig config)
    : m_state(std::make_shared<SharedState>(sanitizeConfig(std::move(config)))),
      m_ownerThread(std::this_thread::get_id()) {}

ResourceScheduler::~ResourceScheduler() {
    shutdown();
}

ResourceSubmitResult ResourceScheduler::submit(
    ResourceRequest request,
    ResourceTask task,
    ResourceCompletionCallback completion) {
    ResourceSubmitResult result;
    const size_t index = kindIndex(request.key.kind);
    if (index >= kindIndex(ResourceKind::Count) ||
        request.key.canonicalIdentity.empty() || !task) {
        return result;
    }

    const std::shared_ptr<SharedState> state = m_state;
    std::scoped_lock lock(state->mutex);
    if (!state->accepting) {
        result.status = ResourceSubmitStatus::ShuttingDown;
        return result;
    }
    if (request.key.generation != 0 &&
        request.key.generation < state->minimumGeneration) {
        result.status = ResourceSubmitStatus::StaleGeneration;
        return result;
    }

    const ResourceKindLimits& limits = state->config.perKind[index];
    if (state->queued.size() >= state->config.maxQueued ||
        state->queuedPerKind[index] >= limits.maxQueued) {
        result.status = ResourceSubmitStatus::QueueFull;
        return result;
    }
    if (request.estimatedBytes > state->config.maxInFlightBytes ||
        request.estimatedBytes > limits.maxInFlightBytes) {
        result.status = ResourceSubmitStatus::EstimatedBytesTooLarge;
        return result;
    }

    auto ticketState = std::make_shared<ResourceTicket::State>();
    ticketState->state.store(ResourceJobState::Queued, std::memory_order_relaxed);
    ticketState->demand = request.demand;
    ticketState->generation = request.key.generation;
    ticketState->sequence = state->nextSequence++;

    auto control = std::make_shared<ResourceTaskContext::Control>();
    control->generation = request.key.generation;
    control->sequence = ticketState->sequence;

    auto job = std::make_shared<SharedState::Job>();
    job->request = std::move(request);
    job->sequence = ticketState->sequence;
    job->enqueueCycle = state->dispatchCycle;
    job->task = std::move(task);
    job->completion = std::move(completion);
    job->control = std::move(control);
    job->ticket = ticketState;

    state->queued.push_back(std::move(job));
    ++state->queuedPerKind[index];
    result.status = ResourceSubmitStatus::Accepted;
    result.ticket = ResourceTicket(std::move(ticketState));
    return result;
}

size_t ResourceScheduler::pump(size_t maxJobs) {
    assert(isOwnerThread());
    static_cast<void>(pumpCompletions());
    return pumpDispatch(maxJobs);
}

size_t ResourceScheduler::pumpCompletions(size_t maxCallbacks) {
    assert(isOwnerThread());
    const std::shared_ptr<SharedState> state = m_state;
    std::vector<SharedState::CompletedJob> completed;
    {
        std::scoped_lock lock(state->mutex);
        const size_t count = std::min(maxCallbacks, state->completed.size());
        completed.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            completed.push_back(std::move(state->completed.front()));
            state->completed.pop_front();
        }
    }

    for (SharedState::CompletedJob& item : completed) {
        const std::shared_ptr<SharedState::Job>& job = item.job;
        job->ticket->state.store(item.state, std::memory_order_release);
        if (job->completion) {
            ResourceCompletion completion;
            completion.key = job->request.key;
            completion.demand = job->request.demand;
            completion.lane = job->request.lane;
            completion.state = item.state;
            completion.sequence = job->sequence;
            completion.estimatedBytes = job->request.estimatedBytes;
            try {
                job->completion(completion);
            } catch (...) {
                // One cache publication failure must not prevent later
                // completions from reaching their terminal owner state.
            }
        }
    }
    return completed.size();
}

size_t ResourceScheduler::pumpDispatch(size_t maxJobs) {
    assert(isOwnerThread());
    const std::shared_ptr<SharedState> state = m_state;
    size_t dispatched = 0;

    while (dispatched < maxJobs) {
        std::shared_ptr<SharedState::Job> job;
        {
            std::scoped_lock lock(state->mutex);
            if (!state->accepting || state->queued.empty() ||
                state->inFlight >= state->config.maxInFlight) {
                break;
            }

            ++state->dispatchCycle;
            auto best = state->queued.end();
            uint8_t bestPriority = 0;
            uint64_t bestAgeBand = 0;

            for (auto it = state->queued.begin(); it != state->queued.end(); ++it) {
                const std::shared_ptr<SharedState::Job>& candidate = *it;
                const size_t index = kindIndex(candidate->request.key.kind);
                const ResourceKindLimits& limits = state->config.perKind[index];
                if (state->inFlightPerKind[index] >= limits.maxInFlight ||
                    candidate->request.estimatedBytes >
                        state->config.maxInFlightBytes - state->inFlightBytes ||
                    candidate->request.estimatedBytes >
                        limits.maxInFlightBytes - state->inFlightBytesPerKind[index]) {
                    continue;
                }

                const uint64_t age = state->dispatchCycle - candidate->enqueueCycle;
                const uint64_t ageBand = age / state->config.agingDispatchCycles;
                const uint8_t priority = static_cast<uint8_t>(std::min<uint64_t>(
                    demandRank(candidate->request.demand) + ageBand,
                    demandRank(ResourceDemand::StartupRequired)));

                const bool better = best == state->queued.end() ||
                    priority > bestPriority ||
                    (priority == bestPriority && ageBand > bestAgeBand) ||
                    (priority == bestPriority && ageBand == bestAgeBand &&
                     candidate->request.estimatedBytes < (*best)->request.estimatedBytes) ||
                    (priority == bestPriority && ageBand == bestAgeBand &&
                     candidate->request.estimatedBytes == (*best)->request.estimatedBytes &&
                     candidate->sequence < (*best)->sequence);
                if (better) {
                    best = it;
                    bestPriority = priority;
                    bestAgeBand = ageBand;
                }
            }

            if (best == state->queued.end()) {
                break;
            }

            job = std::move(*best);
            state->queued.erase(best);
            const size_t index = kindIndex(job->request.key.kind);
            --state->queuedPerKind[index];
            ++state->inFlight;
            state->inFlightBytes += job->request.estimatedBytes;
            ++state->inFlightPerKind[index];
            state->inFlightBytesPerKind[index] += job->request.estimatedBytes;
            ++state->activeWorkers;
            state->active.push_back(job);
            job->ticket->state.store(ResourceJobState::InFlight, std::memory_order_release);
        }

        auto worker = [state, job] {
            platform::runtime::ThreadRoleScope role(
                platform::runtime::ThreadRole::Resource);
            ResourceJobState terminal = ResourceJobState::Failed;
            if (!job->control->stopRequested.load(std::memory_order_acquire)) {
                try {
                    const ResourceTaskContext context(job->control);
                    terminal = job->task(context) == ResourceTaskResult::Ready
                        ? ResourceJobState::Ready
                        : ResourceJobState::Failed;
                } catch (const std::exception&) {
                    terminal = ResourceJobState::Failed;
                } catch (...) {
                    terminal = ResourceJobState::Failed;
                }
            }

            if (job->control->stopRequested.load(std::memory_order_acquire)) {
                terminal = job->control->stale.load(std::memory_order_acquire)
                    ? ResourceJobState::Stale
                    : ResourceJobState::Cancelled;
            }

            {
                std::scoped_lock lock(state->mutex);
                // Cancellation/generation invalidation can race the task's
                // final cooperative check. Re-evaluate while holding the
                // same mutex used by cancel paths before publishing Ready.
                if (job->control->stopRequested.load(
                        std::memory_order_acquire)) {
                    terminal = job->control->stale.load(
                            std::memory_order_acquire)
                        ? ResourceJobState::Stale
                        : ResourceJobState::Cancelled;
                }
                const size_t index = kindIndex(job->request.key.kind);
                --state->inFlight;
                state->inFlightBytes -= job->request.estimatedBytes;
                --state->inFlightPerKind[index];
                state->inFlightBytesPerKind[index] -= job->request.estimatedBytes;
                const auto active = std::find(state->active.begin(), state->active.end(), job);
                if (active != state->active.end()) {
                    state->active.erase(active);
                }
                state->completed.push_back({job, terminal});
                --state->activeWorkers;
            }
            state->idleCondition.notify_all();
        };

        if (job->request.lane == ResourceLane::Scene) {
            static_cast<void>(platform::runtime::sceneResourceExecutor().async(std::move(worker)));
        } else {
            static_cast<void>(platform::runtime::resourceExecutor().async(std::move(worker)));
        }
        ++dispatched;
    }

    return dispatched;
}

bool ResourceScheduler::cancel(const ResourceTicket& ticket) {
    if (!ticket.m_state || isTerminal(ticket.state())) {
        return false;
    }

    const std::shared_ptr<SharedState> state = m_state;
    std::scoped_lock lock(state->mutex);
    for (auto it = state->queued.begin(); it != state->queued.end(); ++it) {
        if ((*it)->ticket == ticket.m_state) {
            const std::shared_ptr<SharedState::Job> job = std::move(*it);
            state->queued.erase(it);
            --state->queuedPerKind[kindIndex(job->request.key.kind)];
            job->control->stopRequested.store(true, std::memory_order_release);
            state->completed.push_back({job, ResourceJobState::Cancelled});
            return true;
        }
    }
    for (const std::shared_ptr<SharedState::Job>& job : state->active) {
        if (job->ticket == ticket.m_state) {
            job->control->stopRequested.store(true, std::memory_order_release);
            return true;
        }
    }
    return false;
}

void ResourceScheduler::cancelGeneration(uint64_t generation) {
    const std::shared_ptr<SharedState> state = m_state;
    std::scoped_lock lock(state->mutex);
    for (auto it = state->queued.begin(); it != state->queued.end();) {
        const std::shared_ptr<SharedState::Job>& job = *it;
        if (job->request.key.generation != generation) {
            ++it;
            continue;
        }
        job->control->stopRequested.store(true, std::memory_order_release);
        --state->queuedPerKind[kindIndex(job->request.key.kind)];
        state->completed.push_back({job, ResourceJobState::Cancelled});
        it = state->queued.erase(it);
    }
    for (const std::shared_ptr<SharedState::Job>& job : state->active) {
        if (job->request.key.generation == generation) {
            job->control->stopRequested.store(true, std::memory_order_release);
        }
    }
}

void ResourceScheduler::advanceMinimumGeneration(uint64_t minimumGeneration) {
    const std::shared_ptr<SharedState> state = m_state;
    std::scoped_lock lock(state->mutex);
    if (minimumGeneration <= state->minimumGeneration) {
        return;
    }
    state->minimumGeneration = minimumGeneration;

    for (auto it = state->queued.begin(); it != state->queued.end();) {
        const std::shared_ptr<SharedState::Job>& job = *it;
        if (job->request.key.generation == 0 ||
            job->request.key.generation >= minimumGeneration) {
            ++it;
            continue;
        }
        job->control->stale.store(true, std::memory_order_release);
        job->control->stopRequested.store(true, std::memory_order_release);
        --state->queuedPerKind[kindIndex(job->request.key.kind)];
        state->completed.push_back({job, ResourceJobState::Stale});
        it = state->queued.erase(it);
    }
    for (const std::shared_ptr<SharedState::Job>& job : state->active) {
        if (job->request.key.generation != 0 &&
            job->request.key.generation < minimumGeneration) {
            job->control->stale.store(true, std::memory_order_release);
            job->control->stopRequested.store(true, std::memory_order_release);
        }
    }
}

ResourceSchedulerStats ResourceScheduler::stats() const {
    const std::shared_ptr<SharedState> state = m_state;
    std::scoped_lock lock(state->mutex);
    ResourceSchedulerStats result;
    result.queued = state->queued.size();
    result.inFlight = state->inFlight;
    result.inFlightBytes = state->inFlightBytes;
    result.minimumGeneration = state->minimumGeneration;
    result.accepting = state->accepting;
    result.queuedPerKind = state->queuedPerKind;
    result.inFlightPerKind = state->inFlightPerKind;
    result.inFlightBytesPerKind = state->inFlightBytesPerKind;
    return result;
}

bool ResourceScheduler::isOwnerThread() const noexcept {
    return std::this_thread::get_id() == m_ownerThread;
}

void ResourceScheduler::shutdown() {
    assert(isOwnerThread());
    const std::shared_ptr<SharedState> state = m_state;
    {
        std::unique_lock lock(state->mutex);
        if (state->shutdownStarted) {
            state->idleCondition.wait(lock, [&] { return state->activeWorkers == 0; });
        } else {
            state->shutdownStarted = true;
            state->accepting = false;
            for (const std::shared_ptr<SharedState::Job>& job : state->queued) {
                job->control->stopRequested.store(true, std::memory_order_release);
                state->completed.push_back({job, ResourceJobState::Cancelled});
            }
            state->queued.clear();
            state->queuedPerKind.fill(0);
            for (const std::shared_ptr<SharedState::Job>& job : state->active) {
                job->control->stopRequested.store(true, std::memory_order_release);
            }
            state->idleCondition.wait(lock, [&] { return state->activeWorkers == 0; });
        }
    }
    static_cast<void>(pumpCompletions());
}

} // namespace engine::resource
