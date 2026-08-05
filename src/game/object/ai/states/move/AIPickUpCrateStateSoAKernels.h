#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include "core/container/container_types.h"

#include "game/object/ai/states/move/AIMoveStateSoAKernels.h"

namespace engine::ai
{

inline constexpr uint8_t AI_PICK_UP_CRATE_DELAY_UPDATES = 3;

// PickUpCrate uses the MoveTo payload and path/movement service protocol. The
// delay is a dedicated storage-owned SoA column. Like the other SoA kernels,
// every non-empty input span is indexed by the storage's stable slot.
struct AIPickUpCrateStateSoAKernelInput final
{
    uint64_t confirmedTick = 0;
    bool childMode = false;
    container::Span<const uint8_t> scheduled{};
    container::Span<const uint8_t> effectivelyDead;
    container::Span<const uint8_t> mobile;
    container::Span<const uint8_t> moveTargetValid;
    container::Span<const uint8_t> groundMovement;
    container::Span<const AIFixedPosition> subjectPosition;
    container::Span<const AIFixedPosition> resolvedMoveTarget;
    container::Span<const uint32_t> ticksPerSecond;
    container::Span<const PathFeedback> pathFeedback;
    container::Span<const MovementFeedback> movementFeedback;
    container::Span<PathRequestBuffer> pathRequests;
    container::Span<MovementCommandBuffer> movementCommands;
    container::Span<AIStateStepResult> results;
};

namespace detail
{

[[nodiscard]] inline AIMoveStateSoAKernelInput pickUpCrateMoveInput(
    const AIPickUpCrateStateSoAKernelInput& input) noexcept
{
    return {
        .confirmedTick = input.confirmedTick,
        .scheduled = input.scheduled,
        .effectivelyDead = input.effectivelyDead,
        .mobile = input.mobile,
        .moveTargetValid = input.moveTargetValid,
        .groundMovement = input.groundMovement,
        .subjectPosition = input.subjectPosition,
        .resolvedMoveTarget = input.resolvedMoveTarget,
        .ticksPerSecond = input.ticksPerSecond,
        .pathFeedback = input.pathFeedback,
        .movementFeedback = input.movementFeedback,
        .pathRequests = input.pathRequests,
        .movementCommands = input.movementCommands,
        .results = input.results,
    };
}

[[nodiscard]] inline bool hasAlignedPickUpCrateStateSoASpans(
    const AIStateFamilySoAStorage& storage,
    const AIPickUpCrateStateSoAKernelInput& input) noexcept
{
    return hasAlignedMoveStateSoASpans(storage, pickUpCrateMoveInput(input));
}

[[nodiscard]] constexpr bool pickUpCrateStateSoAScheduled(
    const AIPickUpCrateStateSoAKernelInput& input,
    size_t slot) noexcept
{
    return input.scheduled.empty() || input.scheduled[slot] != 0;
}

inline void updatePickUpCrateMovement(AIStateFamilySoAStorage& storage,
                                      const AIPickUpCrateStateSoAKernelInput& input,
                                      size_t slot) noexcept
{
    const auto subjects = storage.subjects();
    const auto parameters = storage.parameters();
    auto& columns = storage.moveTo();
    AIMoveToStatePayload payload = columns.load(slot);
    struct Writeback final
    {
        AIMoveToSoAColumns& columns;
        size_t slot;
        AIMoveToStatePayload& payload;
        ~Writeback() { columns.store(slot, payload); }
    } writeback{columns, slot, payload};

    if (!moveStateSoAFact(input.mobile[slot]))
    {
        input.results[slot] = AIStateStepResult::failure();
        return;
    }

    const AIStateParameters& parameter = parameters[slot];
    const PathCorrelation expected = moveStateSoACorrelation(subjects[slot], payload);
    if (payload.phase == AIMoveToPhase::WaitingForPath)
    {
        if (!payload.pathRequestIssued)
        {
            input.results[slot] = emitMoveStateSoAPathRequest(input.pathRequests[slot],
                                                              subjects[slot],
                                                              input.subjectPosition[slot],
                                                              parameter,
                                                              payload,
                                                              PathRequestKind::New,
                                                              !moveStateSoAFact(
                                                                  input.groundMovement[slot]))
                                      ? AIStateStepResult::continueState()
                                      : AIStateStepResult::unsupported();
            return;
        }

        const PathFeedback& feedback = input.pathFeedback[slot];
        if (!(feedback.correlation == expected))
        {
            input.results[slot] = AIStateStepResult::continueState();
            return;
        }

        switch (feedback.status)
        {
        case PathFeedbackStatus::Pending:
        case PathFeedbackStatus::Delayed:
            input.results[slot] = AIStateStepResult::continueState();
            return;
        case PathFeedbackStatus::Ready:
            if (!feedback.path)
            {
                input.results[slot] = AIStateStepResult::unsupported();
                return;
            }
            {
                AIMoveToStatePayload candidate = payload;
                candidate.pathRequestIssued = false;
                candidate.path = feedback.path;
                candidate.adjustedGoal = feedback.adjustedGoal;
                candidate.adjustedLayer = feedback.adjustedLayer;
                candidate.phase = AIMoveToPhase::FollowingPath;
                if (emitMoveStateSoAInstall(
                        input.movementCommands[slot], subjects[slot], input.confirmedTick,
                        candidate, parameter.ignoredObstacle))
                {
                    payload = candidate;
                    input.results[slot] = AIStateStepResult::continueState();
                }
                else
                {
                    input.results[slot] = AIStateStepResult::unsupported();
                }
            }
            return;
        case PathFeedbackStatus::NoPath:
        case PathFeedbackStatus::Cancelled:
            payload.pathRequestIssued = false;
            input.results[slot] = AIStateStepResult::failure();
            return;
        case PathFeedbackStatus::Unsupported:
            payload.pathRequestIssued = false;
            input.results[slot] = AIStateStepResult::unsupported();
            return;
        }
    }

    const MovementFeedback& feedback = input.movementFeedback[slot];
    if (!(feedback.correlation == expected))
    {
        input.results[slot] = AIStateStepResult::continueState();
        return;
    }

    switch (feedback.status)
    {
    case MovementFeedbackStatus::Started:
    case MovementFeedbackStatus::Moving:
        input.results[slot] = AIStateStepResult::continueState();
        break;
    case MovementFeedbackStatus::Completed:
        input.results[slot] = AIStateStepResult::success();
        break;
    case MovementFeedbackStatus::Blocked:
    {
        const uint32_t maximum = std::numeric_limits<uint32_t>::max();
        const uint32_t repathTicks =
            input.ticksPerSecond[slot] > maximum / 2 ? maximum : input.ticksPerSecond[slot] * 2;
        if (feedback.blockedTicks < repathTicks)
        {
            input.results[slot] = AIStateStepResult::continueState();
            break;
        }
        input.results[slot] = beginMoveStateSoARepath(
                                  input.pathRequests[slot],
                                  subjects[slot],
                                  input.subjectPosition[slot],
                                  parameter,
                                  payload,
                                  !moveStateSoAFact(
                                      input.groundMovement[slot]))
                                  ? AIStateStepResult::continueState()
                                  : AIStateStepResult::unsupported();
        break;
    }
    case MovementFeedbackStatus::Stuck:
        input.results[slot] = beginMoveStateSoARepath(input.pathRequests[slot],
                                                      subjects[slot],
                                                      input.subjectPosition[slot],
                                                      parameter,
                                                      payload,
                                                      !moveStateSoAFact(
                                                          input.groundMovement[slot]))
                                  ? AIStateStepResult::continueState()
                                  : AIStateStepResult::unsupported();
        break;
    case MovementFeedbackStatus::Cancelled:
        input.results[slot] = AIStateStepResult::failure();
        break;
    case MovementFeedbackStatus::Unsupported:
        input.results[slot] = AIStateStepResult::unsupported();
        break;
    }
}

} // namespace detail

// Entry snapshots the required crate object's position and starts a three
// update delay. No path or movement service output is produced on entry.
[[nodiscard]] inline bool enterPickUpCrateSoA(AIStateFamilySoAStorage& storage,
                                              const AIPickUpCrateStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedPickUpCrateStateSoASpans(storage, input))
        return false;

    const auto subjects = storage.subjects();
    const auto runtimes = storage.runtimes();
    const auto payloadStates = storage.payloadStates();
    auto parameters = storage.parameters();
    auto& columns = storage.moveTo();
    auto delayUpdatesRemaining = storage.pickUpCrateDelayUpdates();

    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::pickUpCrateStateSoAScheduled(input, slot) ||
            (!input.childMode &&
             runtimes[slot].currentState != AIStateId::PickUpCrate))
        {
            continue;
        }
        if (detail::moveStateSoAFact(input.effectivelyDead[slot]))
        {
            input.results[slot] = AIStateStepResult::transitionTo(AIStateId::Dead);
            continue;
        }
        if (!input.childMode &&
            payloadStates[slot] != AIStateId::PickUpCrate)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        AIStateParameters& parameter = parameters[slot];
        const AIMoveToStatePayload active = columns.load(slot);
        if (!subjects[slot] || !active.request.isValid() || parameter.sourceOrderRevision == 0)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        if (!parameter.goalObject || !detail::moveStateSoAFact(input.moveTargetValid[slot]))
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }

        parameter.goalPosition = input.resolvedMoveTarget[slot];
        parameter.hasGoalPosition = true;
        parameter.adjustDestinations = true;

        AIMoveToStatePayload candidate{active.request};
        candidate.resolvedGoal = parameter.goalPosition;
        candidate.sourceOrderRevision = parameter.sourceOrderRevision;
        candidate.adjustDestinations = true;
        columns.store(slot, candidate);
        delayUpdatesRemaining[slot] = AI_PICK_UP_CRATE_DELAY_UPDATES;
        input.results[slot] = AIStateStepResult::continueState();
    }
    return true;
}

[[nodiscard]] inline bool updatePickUpCrateSoA(AIStateFamilySoAStorage& storage,
                                               const AIPickUpCrateStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedPickUpCrateStateSoASpans(storage, input))
        return false;

    const auto runtimes = storage.runtimes();
    const auto payloadStates = storage.payloadStates();
    auto delayUpdatesRemaining = storage.pickUpCrateDelayUpdates();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::pickUpCrateStateSoAScheduled(input, slot) ||
            (!input.childMode &&
             runtimes[slot].currentState != AIStateId::PickUpCrate))
        {
            continue;
        }
        if (detail::moveStateSoAFact(input.effectivelyDead[slot]))
        {
            input.results[slot] = AIStateStepResult::transitionTo(AIStateId::Dead);
            continue;
        }
        if (!input.childMode &&
            payloadStates[slot] != AIStateId::PickUpCrate)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        uint8_t& remaining = delayUpdatesRemaining[slot];
        if (remaining != 0)
        {
            --remaining;
            if (remaining != 0)
            {
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
        }

        detail::updatePickUpCrateMovement(storage, input, slot);
    }
    return true;
}

[[nodiscard]] inline bool canExitPickUpCrateSoA(
    const AIStateFamilySoAStorage& storage,
    const AIPickUpCrateStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedPickUpCrateStateSoASpans(storage, input))
        return false;

    const auto payloadStates = storage.payloadStates();
    const auto& columns = storage.moveTo();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::pickUpCrateStateSoAScheduled(input, slot) ||
            (!input.childMode &&
             payloadStates[slot] != AIStateId::PickUpCrate))
        {
            continue;
        }
        if (columns.load(slot).pathRequestIssued &&
            input.pathRequests[slot].count >= input.pathRequests[slot].values.size())
        {
            return false;
        }
        if (input.movementCommands[slot].count >= input.movementCommands[slot].values.size())
            return false;
    }
    return true;
}

// Cleanup is transactional across the whole scheduled batch. Capacity is
// preflighted before either Cancel or EndMovement is emitted for any slot.
[[nodiscard]] inline bool exitPickUpCrateSoA(AIStateFamilySoAStorage& storage,
                                             const AIPickUpCrateStateSoAKernelInput& input) noexcept
{
    if (!canExitPickUpCrateSoA(storage, input))
        return false;

    const auto subjects = storage.subjects();
    const auto payloadStates = storage.payloadStates();
    const auto parameters = storage.parameters();
    auto& columns = storage.moveTo();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::pickUpCrateStateSoAScheduled(input, slot) ||
            (!input.childMode &&
             payloadStates[slot] != AIStateId::PickUpCrate))
        {
            continue;
        }

        AIMoveToStatePayload payload = columns.load(slot);
        if (payload.pathRequestIssued)
        {
            static_cast<void>(detail::emitMoveStateSoAPathRequest(input.pathRequests[slot],
                                                                  subjects[slot],
                                                                  input.subjectPosition[slot],
                                                                  parameters[slot],
                                                                  payload,
                                                                  PathRequestKind::Cancel,
                                                                  !detail::moveStateSoAFact(
                                                                      input.groundMovement[slot])));
        }
        static_cast<void>(input.movementCommands[slot].push({
            .correlation = detail::moveStateSoACorrelation(subjects[slot], payload),
            .kind = MovementCommandKind::EndMovement,
            .path = payload.path,
            .clearGoal = payload.adjustDestinations,
            .preserveUltraAccurateFinalPosition = true,
            .confirmedTick = input.confirmedTick,
        }));
        columns.store(slot, payload);
    }
    return true;
}

} // namespace engine::ai
