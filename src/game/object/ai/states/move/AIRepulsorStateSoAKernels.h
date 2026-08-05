#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include "core/container/container_types.h"

#include "game/object/ai/runtime/AIStateFamilySoAStorage.h"
#include "game/object/ai/runtime/AIStateStep.h"

namespace engine::ai
{

// MoveAwayFromRepulsors and WanderInPlace intentionally share the approach
// path columns. They are mutually recursive descriptor states and never need
// live payloads at the same time for one stable object slot.
struct AIRepulsorStateSoAKernelInput final
{
    uint64_t confirmedTick = 0;
    container::Span<const uint8_t> scheduled{};
    container::Span<const uint8_t> effectivelyDead;
    container::Span<const uint8_t> mobile;
    container::Span<const uint8_t> canBeRepulsed;
    container::Span<const AIFixedPosition> subjectPosition;
    container::Span<const ObjectId> closestRepulsor;
    // Deterministic RNG is consumed by the caller in legacy X-then-Y order.
    // Kernels receive the resulting inclusive cell offsets, never own RNG.
    container::Span<const int32_t> wanderOffsetXCells;
    container::Span<const int32_t> wanderOffsetYCells;
    container::Span<const int64_t> wanderCellSizeRaw;
    container::Span<const uint32_t> ticksPerSecond;
    container::Span<const PathFeedback> pathFeedback;
    container::Span<const MovementFeedback> movementFeedback;
    container::Span<PathRequestBuffer> pathRequests;
    container::Span<MovementCommandBuffer> movementCommands;
    container::Span<AIStateStepResult> results;
};

namespace detail
{

[[nodiscard]] inline bool hasAlignedRepulsorStateSoASpans(
    const AIStateFamilySoAStorage& storage,
    const AIRepulsorStateSoAKernelInput& input) noexcept
{
    const size_t count = storage.size();
    return (input.scheduled.empty() || input.scheduled.size() == count) &&
           input.effectivelyDead.size() == count && input.mobile.size() == count &&
           input.canBeRepulsed.size() == count && input.subjectPosition.size() == count &&
           input.closestRepulsor.size() == count && input.wanderOffsetXCells.size() == count &&
           input.wanderOffsetYCells.size() == count && input.wanderCellSizeRaw.size() == count &&
           input.ticksPerSecond.size() == count && input.pathFeedback.size() == count &&
           input.movementFeedback.size() == count && input.pathRequests.size() == count &&
           input.movementCommands.size() == count &&
           input.results.size() == count;
}

[[nodiscard]] constexpr bool repulsorStateScheduled(
    const AIRepulsorStateSoAKernelInput& input,
    size_t slot) noexcept
{
    return input.scheduled.empty() || input.scheduled[slot] != 0;
}

[[nodiscard]] inline PathCorrelation repulsorCorrelation(
    ObjectId subject,
    const AIApproachPathStatePayload& payload) noexcept
{
    return {
        .subject = subject,
        .stateRequest = payload.request,
        .generation = payload.generation,
        .sourceOrderRevision = payload.sourceOrderRevision,
    };
}

[[nodiscard]] inline bool emitRepulsorPathRequest(
    PathRequestBuffer& output,
    ObjectId subject,
    const AIFixedPosition& start,
    const AIStateParameters& parameters,
    AIApproachPathStatePayload& payload,
    PathRequestKind kind) noexcept
{
    const PathRequest request{
        .correlation = repulsorCorrelation(subject, payload),
        .start = start,
        .originalGoal = payload.goal,
        .adjustDestinations = false,
        .ignoredObstacle = parameters.ignoredObstacle,
        .surfaceMask = parameters.pathSurfaceMask,
        .arrivalRadiusRaw = parameters.arrivalRadiusRaw,
        .kind = kind,
        .currentPath = payload.path,
        .safePathRepulsor = kind == PathRequestKind::Safe ? payload.repulsor : INVALID_OBJECT_ID,
        .safePathRepulsor2 = kind == PathRequestKind::Safe ? payload.repulsor2 : INVALID_OBJECT_ID,
        .traversalMode = AIPathTraversalMode::Navmesh,
        .waypointStart = {},
        .waypointGraphRevision = 0,
        .waypointHopLimit = 0,
        .polylineOffset = {},
        .extraDistanceRaw = 0,
        .pathThroughUnits = false,
        .preciseFinalZ = false,
    };
    if (!request.correlation.isValid() || !output.push(request))
        return false;
    payload.pathRequestIssued = kind != PathRequestKind::Cancel;
    return true;
}

[[nodiscard]] inline bool emitRepulsorMovement(
    MovementCommandBuffer& output,
    ObjectId subject,
    uint64_t confirmedTick,
    const AIApproachPathStatePayload& payload,
    ObjectId ignoredObstacle,
    MovementCommandKind kind,
    AIMovementMode mode) noexcept
{
    if (kind == MovementCommandKind::InstallPath && !payload.path)
        return false;
    return output.push({
        .correlation = repulsorCorrelation(subject, payload),
        .kind = kind,
        .path = payload.path,
        .ignoredObstacle = ignoredObstacle,
        .mode = mode,
        .panicking = mode == AIMovementMode::Panic,
        .clearGoal = kind == MovementCommandKind::EndMovement,
        .preserveUltraAccurateFinalPosition = kind == MovementCommandKind::EndMovement,
        .allowPathThroughUnits = false,
        .confirmedTick = confirmedTick,
    });
}

[[nodiscard]] constexpr AIFixedPosition wanderGoal(
    const AIFixedPosition& origin,
    int32_t xCells,
    int32_t yCells,
    int64_t cellSizeRaw) noexcept
{
    return {
        .xRaw = origin.xRaw + static_cast<int64_t>(xCells) * cellSizeRaw,
        .yRaw = origin.yRaw + static_cast<int64_t>(yCells) * cellSizeRaw,
        .zRaw = origin.zRaw,
    };
}

[[nodiscard]] inline bool beginWanderInPlaceSegment(
    PathRequestBuffer& pathOutput,
    MovementCommandBuffer& movementOutput,
    ObjectId subject,
    uint64_t confirmedTick,
    const AIFixedPosition& start,
    int32_t xCells,
    int32_t yCells,
    int64_t cellSizeRaw,
    const AIStateParameters& parameters,
    AIApproachPathStatePayload& payload,
    bool advanceGeneration,
    bool endPreviousMovement) noexcept
{
    const size_t movementCapacity = movementOutput.values.size();
    if (pathOutput.count >= pathOutput.values.size() ||
        (endPreviousMovement && movementOutput.count >= movementCapacity))
        return false;

    AIApproachPathStatePayload candidate = payload;
    if (advanceGeneration)
    {
        ++candidate.generation;
        if (candidate.generation == 0)
            ++candidate.generation;
    }
    candidate.goal = wanderGoal(candidate.origin, xCells, yCells, cellSizeRaw);
    candidate.path = {};
    candidate.phase = AIMoveToPhase::WaitingForPath;
    candidate.pathRequestIssued = false;
    candidate.adjustDestinations = false;

    // End uses the old generation; the new request uses the next generation.
    if (endPreviousMovement &&
        !emitRepulsorMovement(
            movementOutput, subject, confirmedTick, payload,
            parameters.ignoredObstacle, MovementCommandKind::EndMovement,
            AIMovementMode::Normal))
        return false;
    if (!emitRepulsorPathRequest(
            pathOutput, subject, start, parameters, candidate, PathRequestKind::New))
        return false;
    payload = candidate;
    return true;
}

[[nodiscard]] constexpr uint64_t nextRepulsorScanTick(ObjectId subject, uint64_t tick) noexcept
{
    const uint64_t waitFrames = 10 + (subject.value & 0x7U);
    const uint64_t delay = waitFrames + 1;
    return tick > std::numeric_limits<uint64_t>::max() - delay
               ? std::numeric_limits<uint64_t>::max()
               : tick + delay;
}

} // namespace detail

[[nodiscard]] inline bool enterMoveAwayFromRepulsorsSoA(
    AIStateFamilySoAStorage& storage,
    const AIRepulsorStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedRepulsorStateSoASpans(storage, input))
        return false;

    const auto subjects = storage.subjects();
    const auto runtimes = storage.runtimes();
    const auto states = storage.payloadStates();
    const auto parameters = storage.parameters();
    auto& columns = storage.approachPath();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::repulsorStateScheduled(input, slot) ||
            runtimes[slot].currentState != AIStateId::MoveAwayFromRepulsors)
            continue;
        if (states[slot] != AIStateId::MoveAwayFromRepulsors)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        if (input.effectivelyDead[slot] != 0)
        {
            input.results[slot] = AIStateStepResult::transitionTo(AIStateId::Dead);
            continue;
        }
        if (input.mobile[slot] == 0 || !input.closestRepulsor[slot])
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        if (!subjects[slot] || parameters[slot].sourceOrderRevision == 0)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        if (input.pathRequests[slot].count >= input.pathRequests[slot].values.size())
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        AIApproachPathStatePayload candidate = columns.load(slot);
        candidate.goal = input.subjectPosition[slot];
        candidate.origin = input.subjectPosition[slot];
        if (candidate.repulsor != input.closestRepulsor[slot])
            candidate.repulsor2 = candidate.repulsor;
        candidate.repulsor = input.closestRepulsor[slot];
        candidate.sourceOrderRevision = parameters[slot].sourceOrderRevision;
        candidate.generation = 1;
        candidate.repathsRemaining = 0;
        candidate.phase = AIMoveToPhase::WaitingForPath;
        candidate.path = {};
        candidate.pathRequestIssued = false;
        candidate.adjustDestinations = false;
        if (!detail::emitRepulsorPathRequest(input.pathRequests[slot],
                                             subjects[slot],
                                             input.subjectPosition[slot],
                                             parameters[slot],
                                             candidate,
                                             PathRequestKind::Safe))
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        columns.store(slot, candidate);
        input.results[slot] = AIStateStepResult::continueState();
    }
    return true;
}

[[nodiscard]] inline bool updateMoveAwayFromRepulsorsSoA(
    AIStateFamilySoAStorage& storage,
    const AIRepulsorStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedRepulsorStateSoASpans(storage, input))
        return false;

    const auto subjects = storage.subjects();
    const auto runtimes = storage.runtimes();
    const auto states = storage.payloadStates();
    const auto parameters = storage.parameters();
    auto& columns = storage.approachPath();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::repulsorStateScheduled(input, slot) ||
            runtimes[slot].currentState != AIStateId::MoveAwayFromRepulsors)
            continue;
        if (input.effectivelyDead[slot] != 0)
        {
            input.results[slot] = AIStateStepResult::transitionTo(AIStateId::Dead);
            continue;
        }
        if (states[slot] != AIStateId::MoveAwayFromRepulsors)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        AIApproachPathStatePayload payload = columns.load(slot);
        const PathCorrelation expected = detail::repulsorCorrelation(subjects[slot], payload);
        if (payload.phase == AIMoveToPhase::WaitingForPath)
        {
            const PathFeedback& feedback = input.pathFeedback[slot];
            if (!(feedback.correlation == expected) ||
                feedback.status == PathFeedbackStatus::Pending ||
                feedback.status == PathFeedbackStatus::Delayed)
            {
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
            if (feedback.status == PathFeedbackStatus::Ready)
            {
                if (!feedback.path)
                {
                    input.results[slot] = AIStateStepResult::unsupported();
                    continue;
                }
                AIApproachPathStatePayload candidate = payload;
                candidate.path = feedback.path;
                candidate.goal = feedback.adjustedGoal;
                candidate.pathRequestIssued = false;
                candidate.phase = AIMoveToPhase::FollowingPath;
                if (!detail::emitRepulsorMovement(input.movementCommands[slot],
                                                   subjects[slot],
                                                   input.confirmedTick,
                                                   candidate,
                                                   parameters[slot].ignoredObstacle,
                                                   MovementCommandKind::InstallPath,
                                                   AIMovementMode::Panic))
                {
                    input.results[slot] = AIStateStepResult::unsupported();
                    continue;
                }
                columns.store(slot, candidate);
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
            payload.pathRequestIssued = false;
            columns.store(slot, payload);
            input.results[slot] = feedback.status == PathFeedbackStatus::Unsupported
                                      ? AIStateStepResult::unsupported()
                                      : AIStateStepResult::failure();
            continue;
        }

        const MovementFeedback& feedback = input.movementFeedback[slot];
        if (!(feedback.correlation == expected) || isMovementActiveFeedback(feedback.status))
        {
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }
        if (feedback.status == MovementFeedbackStatus::Completed)
        {
            input.results[slot] = AIStateStepResult::success();
            continue;
        }
        if (feedback.status == MovementFeedbackStatus::Blocked)
        {
            const uint32_t maximum = std::numeric_limits<uint32_t>::max();
            const uint32_t threshold = input.ticksPerSecond[slot] > maximum / 2
                                           ? maximum
                                           : input.ticksPerSecond[slot] * 2;
            if (feedback.blockedTicks < threshold)
            {
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
        }
        input.results[slot] = feedback.status == MovementFeedbackStatus::Unsupported
                                  ? AIStateStepResult::unsupported()
                                  : AIStateStepResult::failure();
    }
    return true;
}

[[nodiscard]] inline bool enterWanderInPlaceSoA(
    AIStateFamilySoAStorage& storage,
    const AIRepulsorStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedRepulsorStateSoASpans(storage, input))
        return false;

    const auto subjects = storage.subjects();
    const auto runtimes = storage.runtimes();
    const auto states = storage.payloadStates();
    const auto parameters = storage.parameters();
    auto& columns = storage.approachPath();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::repulsorStateScheduled(input, slot) ||
            runtimes[slot].currentState != AIStateId::WanderInPlace)
            continue;
        if (states[slot] != AIStateId::WanderInPlace)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        if (input.effectivelyDead[slot] != 0)
        {
            input.results[slot] = AIStateStepResult::transitionTo(AIStateId::Dead);
            continue;
        }
        if (input.mobile[slot] == 0)
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        if (!subjects[slot] || parameters[slot].sourceOrderRevision == 0 ||
            input.wanderCellSizeRaw[slot] <= 0)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        if (input.pathRequests[slot].count >= input.pathRequests[slot].values.size())
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        AIApproachPathStatePayload candidate = columns.load(slot);
        candidate.origin = input.subjectPosition[slot];
        candidate.repulsor = INVALID_OBJECT_ID;
        candidate.repulsor2 = INVALID_OBJECT_ID;
        candidate.sourceOrderRevision = parameters[slot].sourceOrderRevision;
        candidate.generation = 1;
        candidate.repathsRemaining = 0;
        candidate.nextRepulsorScanTick = input.confirmedTick;
        if (!detail::beginWanderInPlaceSegment(input.pathRequests[slot],
                                               input.movementCommands[slot],
                                               subjects[slot],
                                               input.confirmedTick,
                                               input.subjectPosition[slot],
                                               input.wanderOffsetXCells[slot],
                                               input.wanderOffsetYCells[slot],
                                               input.wanderCellSizeRaw[slot],
                                               parameters[slot],
                                               candidate,
                                               false,
                                               false))
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        columns.store(slot, candidate);
        input.results[slot] = AIStateStepResult::continueState();
    }
    return true;
}

[[nodiscard]] inline bool updateWanderInPlaceSoA(
    AIStateFamilySoAStorage& storage,
    const AIRepulsorStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedRepulsorStateSoASpans(storage, input))
        return false;

    const auto subjects = storage.subjects();
    const auto runtimes = storage.runtimes();
    const auto states = storage.payloadStates();
    const auto parameters = storage.parameters();
    auto& columns = storage.approachPath();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::repulsorStateScheduled(input, slot) ||
            runtimes[slot].currentState != AIStateId::WanderInPlace)
            continue;
        if (input.effectivelyDead[slot] != 0)
        {
            input.results[slot] = AIStateStepResult::transitionTo(AIStateId::Dead);
            continue;
        }
        if (states[slot] != AIStateId::WanderInPlace)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        AIApproachPathStatePayload payload = columns.load(slot);
        if (input.canBeRepulsed[slot] != 0 && input.confirmedTick >= payload.nextRepulsorScanTick)
        {
            payload.nextRepulsorScanTick = detail::nextRepulsorScanTick(subjects[slot], input.confirmedTick);
            columns.store(slot, payload);
            if (input.closestRepulsor[slot])
            {
                input.results[slot] = AIStateStepResult::failure();
                continue;
            }
        }

        const PathCorrelation expected = detail::repulsorCorrelation(subjects[slot], payload);
        if (payload.phase == AIMoveToPhase::WaitingForPath)
        {
            const PathFeedback& feedback = input.pathFeedback[slot];
            if (!(feedback.correlation == expected) ||
                feedback.status == PathFeedbackStatus::Pending ||
                feedback.status == PathFeedbackStatus::Delayed)
            {
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
            if (feedback.status == PathFeedbackStatus::Ready)
            {
                if (!feedback.path)
                {
                    input.results[slot] = AIStateStepResult::unsupported();
                    continue;
                }
                AIApproachPathStatePayload candidate = payload;
                candidate.path = feedback.path;
                candidate.pathRequestIssued = false;
                candidate.phase = AIMoveToPhase::FollowingPath;
                if (!detail::emitRepulsorMovement(input.movementCommands[slot],
                                                   subjects[slot],
                                                   input.confirmedTick,
                                                   candidate,
                                                   parameters[slot].ignoredObstacle,
                                                   MovementCommandKind::InstallPath,
                                                   AIMovementMode::Wander))
                {
                    input.results[slot] = AIStateStepResult::unsupported();
                    continue;
                }
                columns.store(slot, candidate);
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
            if (feedback.status == PathFeedbackStatus::Unsupported)
            {
                input.results[slot] = AIStateStepResult::unsupported();
                continue;
            }
            payload.pathRequestIssued = false;
            input.results[slot] = detail::beginWanderInPlaceSegment(input.pathRequests[slot],
                                                                     input.movementCommands[slot],
                                                                     subjects[slot],
                                                                     input.confirmedTick,
                                                                     input.subjectPosition[slot],
                                                                     input.wanderOffsetXCells[slot],
                                                                     input.wanderOffsetYCells[slot],
                                                                     input.wanderCellSizeRaw[slot],
                                                                     parameters[slot],
                                                                     payload,
                                                                     true,
                                                                     false)
                                      ? AIStateStepResult::continueState()
                                      : AIStateStepResult::unsupported();
            columns.store(slot, payload);
            continue;
        }

        const MovementFeedback& feedback = input.movementFeedback[slot];
        if (!(feedback.correlation == expected) || isMovementActiveFeedback(feedback.status))
        {
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }
        if (feedback.status == MovementFeedbackStatus::Unsupported)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        if (feedback.status == MovementFeedbackStatus::Blocked)
        {
            const uint32_t maximum = std::numeric_limits<uint32_t>::max();
            const uint32_t threshold = input.ticksPerSecond[slot] > maximum / 2
                                           ? maximum
                                           : input.ticksPerSecond[slot] * 2;
            if (feedback.blockedTicks < threshold)
            {
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
        }
        input.results[slot] = detail::beginWanderInPlaceSegment(input.pathRequests[slot],
                                                                 input.movementCommands[slot],
                                                                 subjects[slot],
                                                                 input.confirmedTick,
                                                                 input.subjectPosition[slot],
                                                                 input.wanderOffsetXCells[slot],
                                                                 input.wanderOffsetYCells[slot],
                                                                 input.wanderCellSizeRaw[slot],
                                                                 parameters[slot],
                                                                 payload,
                                                                 true,
                                                                 true)
                                  ? AIStateStepResult::continueState()
                                  : AIStateStepResult::unsupported();
        columns.store(slot, payload);
    }
    return true;
}

[[nodiscard]] inline bool canExitRepulsorStateSoA(
    const AIStateFamilySoAStorage& storage,
    const AIRepulsorStateSoAKernelInput& input,
    AIStateId state) noexcept
{
    if (!detail::hasAlignedRepulsorStateSoASpans(storage, input))
        return false;
    const auto states = storage.payloadStates();
    const auto& columns = storage.approachPath();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::repulsorStateScheduled(input, slot) || states[slot] != state)
            continue;
        const AIApproachPathStatePayload payload = columns.load(slot);
        if (payload.pathRequestIssued &&
            input.pathRequests[slot].count >= input.pathRequests[slot].values.size())
            return false;
        if (input.movementCommands[slot].count >= input.movementCommands[slot].values.size())
            return false;
    }
    return true;
}

[[nodiscard]] inline bool exitRepulsorStateSoA(
    AIStateFamilySoAStorage& storage,
    const AIRepulsorStateSoAKernelInput& input,
    AIStateId state) noexcept
{
    if (!canExitRepulsorStateSoA(storage, input, state))
        return false;
    const auto subjects = storage.subjects();
    const auto states = storage.payloadStates();
    const auto parameters = storage.parameters();
    auto& columns = storage.approachPath();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::repulsorStateScheduled(input, slot) || states[slot] != state)
            continue;
        AIApproachPathStatePayload payload = columns.load(slot);
        if (payload.pathRequestIssued)
            static_cast<void>(detail::emitRepulsorPathRequest(input.pathRequests[slot],
                                                              subjects[slot],
                                                              input.subjectPosition[slot],
                                                              parameters[slot],
                                                              payload,
                                                              PathRequestKind::Cancel));
        static_cast<void>(detail::emitRepulsorMovement(input.movementCommands[slot],
                                                       subjects[slot],
                                                       input.confirmedTick,
                                                       payload,
                                                       parameters[slot].ignoredObstacle,
                                                       MovementCommandKind::EndMovement,
                                                       AIMovementMode::Normal));
        columns.store(slot, payload);
    }
    return true;
}

} // namespace engine::ai
