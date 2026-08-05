#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "core/container/container_types.h"
#include "game/object/ai/runtime/AIStateSoATransitionQueue.h"
#include "game/object/ai/runtime/AIStateStep.h"

namespace engine::ai
{

struct AIStateSoALifecycleWaveInput final
{
    uint64_t confirmedTick = 0;
    container::Span<const uint8_t> scheduled;
    container::Span<const AIStateStepResult> results;
};

struct AIStateSoALifecycleWaveScratch final
{
    container::Span<uint8_t> exitMask;
    container::Span<uint8_t> enterMask;
};

struct AIStateSoALifecycleWaveReport final
{
    size_t stepsProcessed = 0;
    size_t sleeping = 0;
    size_t unsupported = 0;
    size_t transitionsRequested = 0;
    size_t transitionsCommitted = 0;
    size_t transitionsRejected = 0;
    size_t transitionConflicts = 0;
    size_t transitionBudgetExceeded = 0;
    bool spansRejected = false;
    bool transitionCapacityExceeded = false;
};

// Temporary expiry precedes the sleep gate and must inspect every temporary
// slot, including those sleeping forever.
inline void collectExpiredTemporaryAIStates(AIStateFamilySoAStorage& storage,
                                            uint64_t confirmedTick,
                                            AIStateSoATransitionQueue& queue,
                                            AIStateSoALifecycleWaveReport& report) noexcept
{
    const auto subjects = storage.subjects();
    const auto runtimes = storage.runtimes();
    for (size_t slot = 0; slot < storage.size(); ++slot)
    {
        const AIStateMachineRuntime& runtime = runtimes[slot];
        if (!runtime.initialized || !runtime.temporaryActive || confirmedTick < runtime.temporaryEndTickExclusive)
        {
            continue;
        }

        AIStateSoATransitionRequest request;
        request.slot = slot;
        request.subject = subjects[slot];
        request.expectedState = runtime.currentState;
        request.expectedRevision = runtime.revision;
        request.operation = AIStateSoATransitionOperation::TemporaryExpired;
        request.authority = AIStateTransitionAuthority::Internal;
        if (!queue.push(request))
            report.transitionCapacityExceeded = true;
        else
            ++report.transitionsRequested;
    }
}

// Reduces state-family results into direct runtime effects and a shared
// transition queue. No state payload is activated here.
[[nodiscard]] inline AIStateSoALifecycleWaveReport collectAIStateSoASteps(AIStateFamilySoAStorage& storage,
                                                                          const AIStateSoALifecycleWaveInput& input,
                                                                          AIStateSoATransitionQueue& queue) noexcept
{
    AIStateSoALifecycleWaveReport report;
    const size_t count = storage.size();
    if (input.scheduled.size() != count || input.results.size() != count || queue.sealed())
    {
        report.spansRejected = true;
        return report;
    }

    auto runtimes = storage.runtimes();
    const auto subjects = storage.subjects();
    for (size_t slot = 0; slot < count; ++slot)
    {
        if (input.scheduled[slot] == 0)
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
            AIStateSoATransitionRequest request;
            request.slot = slot;
            request.subject = subjects[slot];
            request.expectedState = runtimes[slot].currentState;
            request.expectedRevision = runtimes[slot].revision;
            request.operation = AIStateSoATransitionOperation::Direct;
            request.target = step.target;
            request.authority = AIStateTransitionAuthority::Internal;
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
            else
                ++report.transitionsRequested;
            break;
        }
        }
    }
    return report;
}

// Commits runtime transitions but deliberately leaves old payload metadata in
// place. The caller must run all relevant exit kernels using exitMask, then
// call activateAIStateSoATargetPayloads() before processing enterMask.
inline void commitAIStateSoALifecycleWave(AIStateFamilySoAStorage& storage,
                                          uint64_t confirmedTick,
                                          AIStateSoATransitionQueue& queue,
                                          const AIStateSoALifecycleWaveScratch& scratch,
                                          AIStateSoALifecycleWaveReport& report) noexcept
{
    const size_t count = storage.size();
    if (scratch.exitMask.size() != count || scratch.enterMask.size() != count)
    {
        report.spansRejected = true;
        return;
    }
    // commit() returns its cached aggregate report after sealing. Treat a
    // repeated lifecycle call as a no-op so caller-owned counters remain
    // deltas rather than being added twice during exit retry handling.
    if (queue.sealed())
        return;
    std::fill(scratch.exitMask.begin(), scratch.exitMask.end(), uint8_t{0});
    std::fill(scratch.enterMask.begin(), scratch.enterMask.end(), uint8_t{0});

    const AIStateSoATransitionCommitReport committed =
        queue.commit(storage, confirmedTick, AIStateSoAPayloadCommitMode::DeferUntilAfterExit);
    report.transitionsCommitted += committed.committed;
    report.transitionsRejected += committed.rejected;
    report.transitionConflicts += committed.conflicts;
    for (const AIStateSoATransitionEntry& entry : queue.entries())
    {
        if (entry.result == AIStateSoATransitionResult::TransitionBudgetExceeded)
            ++report.transitionBudgetExceeded;
        if (entry.result != AIStateSoATransitionResult::Committed || !entry.hasCommittedTransition)
            continue;
        scratch.exitMask[entry.request.slot] = isValidState(entry.committedTransition.from) ? uint8_t{1} : uint8_t{0};
        scratch.enterMask[entry.request.slot] = 1;
    }
}

[[nodiscard]] inline size_t activateAIStateSoATargetPayloads(AIStateFamilySoAStorage& storage,
                                                             AIStateSoATransitionQueue& queue) noexcept
{
    return queue.activateCommittedPayloads(storage);
}

} // namespace engine::ai
