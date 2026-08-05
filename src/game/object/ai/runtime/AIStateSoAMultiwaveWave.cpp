#include "game/object/ai/runtime/AIStateSoAMultiwaveFamilyDispatch.h"

#include <algorithm>
#include <limits>

namespace engine::ai
{

// Executes updates and enter-generated transition chains until quiescent.
// Every wave follows collect -> arbitrate/commit runtime -> exit old payload ->
// activate target payload -> enter target. No allocation or AoS payload walk is
// hidden inside the runner.
[[nodiscard]] AIStateSoAMultiwaveReport runAIStateSoAMultiwave(
    AIStateFamilySoAStorage& storage,
    const AIStateSoAMultiwaveInput& input,
    const AIStateSoAMultiwaveScratch& scratch) noexcept
{
    AIStateSoAMultiwaveReport report;
    if (!detail::hasAlignedSoAMultiwaveSpans(storage, input, scratch))
    {
        report.spansRejected = true;
        return report;
    }

    const size_t count = storage.size();
    std::fill(scratch.results.begin(), scratch.results.end(), AIStateStepResult::continueState());
    std::fill(scratch.actionMask.begin(), scratch.actionMask.end(), uint8_t{0});

    // One result per slot is the upper bound for an enter/update pass. Check
    // before any kernel can mutate payload or append an output command.
    if (scratch.transitionEntries.size() < count)
    {
        report.transitionCapacityExceeded = true;
        return report;
    }

    AIStateSoATransitionQueue queue(scratch.transitionEntries);
    const auto processWaves = [&](const AIStateSoAMultiwaveInput& executionInput,
                                  uint64_t transitionTick) noexcept -> bool {
        while (true)
        {
            AIStateSoALifecycleWaveReport wave = collectAIStateSoASteps(
                storage,
                {.confirmedTick = transitionTick, .scheduled = scratch.actionMask, .results = scratch.results},
                queue);
            detail::mergeSoAWaveReport(report, wave);
            if (report.spansRejected || report.transitionCapacityExceeded)
                return false;
            if (queue.empty())
                return true;

            ++report.waves;
            commitAIStateSoALifecycleWave(
                storage,
                transitionTick,
                queue,
                {.exitMask = scratch.exitMask, .enterMask = scratch.enterMask},
                wave);
            report.transitionsCommitted += wave.transitionsCommitted;
            report.transitionsRejected += wave.transitionsRejected;
            report.transitionConflicts += wave.transitionConflicts;
            report.transitionBudgetExceeded += wave.transitionBudgetExceeded;
            if (wave.transitionsCommitted == 0)
                return true;

            // Ordinary waypoint completion is a success-only exit edge.
            // ZH's Exact state is intentionally different: once its enter
            // succeeded, every exit publishes the command's starting
            // waypoint (AIFollowWaypointPathExactState::m_lastWaypoint), even
            // for failure/cancellation. Preserve that retail compatibility
            // without widening ordinary/Wander/Panic completion semantics.
            for (const AIStateSoATransitionEntry& entry : queue.entries())
            {
                if (entry.result != AIStateSoATransitionResult::Committed || !entry.hasCommittedTransition ||
                    entry.request.slot >= storage.size() ||
                    !(detail::isWaypointState(storage.payloadStates()[entry.request.slot]) ||
                      storage.payloadStates()[entry.request.slot] == AIStateId::Wander ||
                      storage.payloadStates()[entry.request.slot] == AIStateId::Panic))
                    continue;
                AIWaypointPathStatePayload payload = storage.waypointPath().load(entry.request.slot);
                const AIStateId sourceState =
                    storage.payloadStates()[entry.request.slot];
                if (detail::waypointExactState(sourceState))
                {
                    const AIWaypointHandle start =
                        storage.parameters()[entry.request.slot].waypoint;
                    payload.completionTerminal = start;
                    payload.completionPending =
                        payload.request.isValid() && static_cast<bool>(start);
                }
                else
                {
                    payload.completionPending = payload.completionPending &&
                        entry.committedTransition.reason ==
                            AIStateTransitionReason::Success;
                }
                storage.waypointPath().store(entry.request.slot, payload);
            }

            if (!detail::dispatchSoAExits(storage, executionInput, scratch.exitMask, scratch.results))
            {
                report.exitBlocked = true;
                return false;
            }
            static_cast<void>(activateAIStateSoATargetPayloads(storage, queue));
            std::fill(scratch.exitMask.begin(), scratch.exitMask.end(), uint8_t{0});

            std::fill(scratch.results.begin(), scratch.results.end(), AIStateStepResult::continueState());
            if (!detail::dispatchSoAEnters(storage, executionInput, scratch.enterMask, scratch.results))
            {
                report.spansRejected = true;
                return false;
            }
            std::copy(scratch.enterMask.begin(), scratch.enterMask.end(), scratch.actionMask.begin());
            queue.clear();
        }
    };

    // A previous batch may have committed runtime but failed a transactional
    // Move exit. Retry that old payload before allowing any new state work.
    bool hasDeferred = false;
    uint64_t deferredTick = 0;
    for (size_t slot = 0; slot < count; ++slot)
    {
        // exitMask remains set only when the previous call returned from a
        // failed exit. It also preserves same-state reenter, where comparing
        // currentState and payloadState cannot reveal deferred activation.
        const bool deferred = scratch.exitMask[slot] != 0 ||
                              storage.runtimes()[slot].currentState != storage.payloadStates()[slot];
        scratch.actionMask[slot] = deferred ? uint8_t{1} : uint8_t{0};
        if (deferred)
        {
            const uint64_t slotTick = storage.runtimes()[slot].enteredTick;
            if (hasDeferred && slotTick != deferredTick)
            {
                report.spansRejected = true;
                return report;
            }
            deferredTick = slotTick;
        }
        hasDeferred = hasDeferred || deferred;
    }
    if (hasDeferred)
    {
        AIStateSoAMultiwaveInput recoveryInput = input;
        recoveryInput.confirmedTick = deferredTick;
        recoveryInput.requests = {};
        if (!detail::dispatchSoAExits(storage, recoveryInput, scratch.actionMask, scratch.results))
        {
            report.exitBlocked = true;
            return report;
        }
        for (size_t slot = 0; slot < count; ++slot)
        {
            const bool reenter = scratch.exitMask[slot] != 0 &&
                                 storage.runtimes()[slot].currentState == storage.payloadStates()[slot];
            if (scratch.actionMask[slot] != 0 &&
                AIStateSoATransitionQueue::activateDeferredSlot(storage, slot, reenter))
                ++report.deferredRetries;
        }
        std::fill(scratch.exitMask.begin(), scratch.exitMask.end(), uint8_t{0});
        if (!detail::dispatchSoAEnters(storage, recoveryInput, scratch.actionMask, scratch.results))
        {
            report.spansRejected = true;
            return report;
        }
        if (!processWaves(recoveryInput, deferredTick))
            return report;
        // A retry at the original tick completes only the pending lifecycle;
        // it must not immediately update the freshly entered state.
        if (input.confirmedTick <= deferredTick)
        {
            report.currentTickWorkDeferred = true;
            return report;
        }

        queue.clear();
        std::fill(scratch.results.begin(), scratch.results.end(), AIStateStepResult::continueState());
        std::fill(scratch.actionMask.begin(), scratch.actionMask.end(), uint8_t{0});
    }

    if (input.requests.size() > std::numeric_limits<size_t>::max() - count ||
        scratch.transitionEntries.size() < input.requests.size() + count)
    {
        report.transitionCapacityExceeded = true;
        return report;
    }
    for (const AIStateSoATransitionRequest& request : input.requests)
    {
        if (!queue.push(request))
        {
            report.transitionCapacityExceeded = true;
            return report;
        }
        ++report.transitionsRequested;
    }
    AIStateSoALifecycleWaveReport expiryReport;
    collectExpiredTemporaryAIStates(storage, input.confirmedTick, queue, expiryReport);
    detail::mergeSoAWaveReport(report, expiryReport);
    if (report.transitionCapacityExceeded)
        return report;

    auto runtimes = storage.runtimes();
    for (size_t slot = 0; slot < count; ++slot)
    {
        if (input.scheduled[slot] == 0 || !runtimes[slot].initialized ||
            AIStateMachine::isSleeping(runtimes[slot], input.confirmedTick))
        {
            scratch.actionMask[slot] = 0;
            continue;
        }
        if (runtimes[slot].wakeTick != 0 && input.confirmedTick >= runtimes[slot].wakeTick)
            AIStateMachine::wake(runtimes[slot], AIWakeReason::Deadline);
        scratch.actionMask[slot] = 1;
    }
    // Expiry wins before the sleep/update gate for its slot.
    for (const AIStateSoATransitionEntry& entry : queue.entries())
    {
        if (entry.request.operation == AIStateSoATransitionOperation::TemporaryExpired &&
            entry.request.slot < count)
        {
            scratch.actionMask[entry.request.slot] = 0;
        }
    }
    if (!detail::dispatchSoAUpdates(storage, input, scratch.actionMask, scratch.results))
    {
        report.spansRejected = true;
        return report;
    }
    static_cast<void>(processWaves(input, input.confirmedTick));
    return report;
}

} // namespace engine::ai
