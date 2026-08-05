#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "core/container/container_types.h"
#include "game/object/ai/states/core/AICoreStateSoAKernels.h"
#include "game/object/ai/runtime/AIStateSoATransitionQueue.h"

namespace engine::ai
{

struct AICoreStateSoABatchScratch final
{
    container::Span<uint8_t> activeMask;
    container::Span<AIStateStepResult> results;
    container::Span<AIStateSoATransitionEntry> transitions;
};

struct AICoreStateSoABatchReport final
{
    size_t scheduled = 0;
    size_t stepsProcessed = 0;
    size_t transitionsCommitted = 0;
    size_t sleeping = 0;
    size_t unsupported = 0;
    size_t deferredToOtherFamily = 0;
    size_t transitionConflicts = 0;
    bool scratchRejected = false;
    bool transitionCapacityExceeded = false;
};

namespace detail
{

[[nodiscard]] constexpr bool isCoreSoAState(AIStateId state) noexcept
{
    switch (state)
    {
    case AIStateId::Idle:
    case AIStateId::Wait:
    case AIStateId::Busy:
    case AIStateId::Dead:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] inline bool runCoreSoAKernels(AIStateFamilySoAStorage& storage,
                                            const AICoreStateSoAKernelInput& input,
                                            bool entering) noexcept
{
    if (entering)
    {
        return (storage.activeStateCount(AIStateId::Idle) == 0 || enterIdleSoA(storage, input)) &&
               (storage.activeStateCount(AIStateId::Wait) == 0 || enterWaitSoA(storage, input)) &&
               (storage.activeStateCount(AIStateId::Busy) == 0 || enterBusySoA(storage, input)) &&
               (storage.activeStateCount(AIStateId::Dead) == 0 || enterDeadSoA(storage, input));
    }
    return (storage.activeStateCount(AIStateId::Idle) == 0 || updateIdleSoA(storage, input)) &&
           (storage.activeStateCount(AIStateId::Wait) == 0 || updateWaitSoA(storage, input)) &&
           (storage.activeStateCount(AIStateId::Busy) == 0 || updateBusySoA(storage, input)) &&
           (storage.activeStateCount(AIStateId::Dead) == 0 || updateDeadSoA(storage, input));
}

inline void applyCoreSoASteps(AIStateFamilySoAStorage& storage,
                              const AICoreStateSoAKernelInput& input,
                              AIStateSoATransitionQueue& queue,
                              AICoreStateSoABatchReport& report) noexcept
{
    auto runtimes = storage.runtimes();
    const auto subjects = storage.subjects();
    for (size_t slot = 0; slot < storage.size(); ++slot)
    {
        if (input.scheduled[slot] == 0 || !isCoreSoAState(runtimes[slot].currentState))
            continue;

        ++report.stepsProcessed;
        const AIStateStepResult& step = input.results[slot];
        switch (step.kind)
        {
        case AIStateStepKind::Continue:
            break;
        case AIStateStepKind::SleepUntil:
            AIStateMachine::sleepUntil(runtimes[slot], step.wakeTick);
            ++report.sleeping;
            break;
        case AIStateStepKind::Blocked:
            AIStateMachine::sleepUntil(runtimes[slot], std::numeric_limits<uint64_t>::max());
            ++report.sleeping;
            break;
        case AIStateStepKind::Unsupported:
            ++report.unsupported;
            break;
        case AIStateStepKind::Transition:
        case AIStateStepKind::Success:
        case AIStateStepKind::Failure:
        {
            AIStateSoATransitionRequest request{
                .slot = slot,
                .subject = subjects[slot],
                .expectedState = runtimes[slot].currentState,
                .expectedRevision = runtimes[slot].revision,
                .operation = AIStateSoATransitionOperation::Direct,
                .target = step.target,
                .authority = AIStateTransitionAuthority::Internal,
                .reenter = false,
                .terminalPriority = false,
            };
            if (step.kind == AIStateStepKind::Success)
            {
                request.operation = AIStateSoATransitionOperation::CompleteSuccess;
                request.target = AIStateId::Invalid;
            }
            else if (step.kind == AIStateStepKind::Failure)
            {
                request.operation = AIStateSoATransitionOperation::CompleteFailure;
                request.target = AIStateId::Invalid;
            }
            else if (step.target == AIStateId::Dead)
            {
                request.authority = AIStateTransitionAuthority::Terminal;
                request.terminalPriority = true;
            }
            if (!queue.push(request))
                report.transitionCapacityExceeded = true;
            break;
        }
        }
    }
}

} // namespace detail

// Processes one confirmed-tick core pass. The caller supplies all scratch
// storage; no allocation occurs. Sleeping slots are omitted unless explicitly
// woken before this call. Enter chains are bounded by the runtime transition
// budget and committed in stable ObjectId order.
[[nodiscard]] inline AICoreStateSoABatchReport updateCoreStateSoABatch(
    AIStateFamilySoAStorage& storage,
    AICoreStateSoAKernelInput input,
    const AICoreStateSoABatchScratch& scratch) noexcept
{
    AICoreStateSoABatchReport report;
    const size_t count = storage.size();
    if (scratch.activeMask.size() != count || scratch.results.size() != count || scratch.transitions.size() < count ||
        input.effectivelyDead.size() != count || input.idleAutoAcquireEnabled.size() != count ||
        input.idleTargetAvailable.size() != count ||
        (!input.idleTargetScanIntervalTicksBySlot.empty() &&
         input.idleTargetScanIntervalTicksBySlot.size() != count) ||
        (!input.scheduled.empty() && input.scheduled.size() != count))
    {
        report.scratchRejected = true;
        return report;
    }

    auto runtimes = storage.runtimes();
    for (size_t slot = 0; slot < count; ++slot)
    {
        const bool callerScheduled = input.scheduled.empty() || input.scheduled[slot] != 0;
        const bool due = callerScheduled && detail::isCoreSoAState(runtimes[slot].currentState) &&
                         !AIStateMachine::isSleeping(runtimes[slot], input.confirmedTick);
        scratch.activeMask[slot] = due ? uint8_t{1} : uint8_t{0};
        if (due)
        {
            ++report.scheduled;
            if (runtimes[slot].wakeTick != 0)
                AIStateMachine::wake(runtimes[slot], AIWakeReason::Deadline);
        }
    }

    input.scheduled = scratch.activeMask;
    input.results = scratch.results;
    if (!detail::runCoreSoAKernels(storage, input, false))
    {
        report.scratchRejected = true;
        return report;
    }

    AIStateSoATransitionQueue queue(scratch.transitions);
    detail::applyCoreSoASteps(storage, input, queue, report);

    for (uint8_t wave = 0; wave < AIStateMachineRuntime::MaximumTransitionsPerTick && !queue.empty(); ++wave)
    {
        const AIStateSoATransitionCommitReport commit = queue.commit(storage, input.confirmedTick);
        report.transitionsCommitted += commit.committed;
        report.transitionConflicts += commit.conflicts;

        std::fill(scratch.activeMask.begin(), scratch.activeMask.end(), uint8_t{0});
        for (const AIStateSoATransitionEntry& entry : queue.entries())
        {
            if (entry.result != AIStateSoATransitionResult::Committed)
                continue;
            const AIStateId state = storage.runtimes()[entry.request.slot].currentState;
            if (detail::isCoreSoAState(state))
                scratch.activeMask[entry.request.slot] = 1;
            else
                ++report.deferredToOtherFamily;
        }
        queue.clear();

        bool anyCoreEnter = false;
        for (uint8_t active : scratch.activeMask)
            anyCoreEnter = anyCoreEnter || active != 0;
        if (!anyCoreEnter)
            break;

        input.scheduled = scratch.activeMask;
        if (!detail::runCoreSoAKernels(storage, input, true))
        {
            report.scratchRejected = true;
            return report;
        }
        detail::applyCoreSoASteps(storage, input, queue, report);
    }

    return report;
}

} // namespace engine::ai
