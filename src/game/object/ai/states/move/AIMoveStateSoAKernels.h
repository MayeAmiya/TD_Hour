#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include "core/container/container_types.h"

#include "game/object/ai/runtime/AIStateFamilySoAStorage.h"
#include "game/object/ai/runtime/AIStateStep.h"

namespace engine::ai
{

// All non-empty spans are indexed by the stable AIStateFamilySoAStorage slot.
// Feedback absence is represented by an invalid/non-matching correlation in
// that slot. Output buffers are caller-owned, bounded, and dedicated per slot;
// kernels append without clearing them, so the caller controls each batch's
// consumption boundary. Filtered slots are not read or written.
struct AIMoveStateSoAKernelInput final
{
    uint64_t confirmedTick = 0;
    // MoveTo body is reused by the AttackMoveTo wrapper while the wrapper
    // retains the top-level runtime/payload state and opportunity semantics.
    AIStateId activeState = AIStateId::MoveTo;
    // Wrapper states such as Guard retain the top-level runtime/payload state
    // while borrowing MoveTo's detached path/movement body.
    bool childMode = false;
    container::Span<const uint8_t> scheduled{};
    container::Span<const uint8_t> effectivelyDead;
    container::Span<const uint8_t> mobile;
    container::Span<const uint8_t> moveTargetValid;
    // This is the sampled Object::isDoingGroundMovement() fact, not merely
    // "has a locomotor". Airborne movement bypasses the ground pathfinder.
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

[[nodiscard]] inline bool hasAlignedMoveStateSoASpans(const AIStateFamilySoAStorage& storage,
                                                      const AIMoveStateSoAKernelInput& input) noexcept
{
    const size_t count = storage.size();
    return (input.scheduled.empty() || input.scheduled.size() == count) && input.effectivelyDead.size() == count &&
           input.mobile.size() == count && input.moveTargetValid.size() == count &&
           input.groundMovement.size() == count &&
           input.subjectPosition.size() == count && input.resolvedMoveTarget.size() == count &&
           input.ticksPerSecond.size() == count && input.pathFeedback.size() == count &&
           input.movementFeedback.size() == count && input.pathRequests.size() == count &&
           input.movementCommands.size() == count && input.results.size() == count;
}

[[nodiscard]] constexpr bool moveStateSoAScheduled(const AIMoveStateSoAKernelInput& input, size_t slot) noexcept
{
    return input.scheduled.empty() || input.scheduled[slot] != 0;
}

[[nodiscard]] constexpr bool moveStateSoAFact(uint8_t value) noexcept
{
    return value != 0;
}

[[nodiscard]] inline PathCorrelation moveStateSoACorrelation(ObjectId subject,
                                                             const AIMoveToStatePayload& payload) noexcept
{
    return {
        .subject = subject,
        .stateRequest = payload.request,
        .generation = payload.generation,
        .sourceOrderRevision = payload.sourceOrderRevision,
    };
}

[[nodiscard]] inline bool emitMoveStateSoAPathRequest(PathRequestBuffer& output,
                                                      ObjectId subject,
                                                      const AIFixedPosition& start,
                                                      const AIStateParameters& parameters,
                                                      AIMoveToStatePayload& payload,
                                                      PathRequestKind kind,
                                                      bool quickPath) noexcept
{
    const PathRequest request{
        .correlation = moveStateSoACorrelation(subject, payload),
        // Always retain the actor's real confirmed position.  Ordinal zero is
        // selected as the real unit nearest the group centre, so it is also a
        // valid centerline start.  Followers need their own start when the
        // shared route is unavailable or rejected and Navigation falls back
        // to an ordinary per-object search.
        .start = start,
        .originalGoal = parameters.goalPosition,
        .adjustDestinations = payload.adjustDestinations,
        .ignoredObstacle = parameters.ignoredObstacle,
        .surfaceMask = parameters.pathSurfaceMask,
        .arrivalRadiusRaw = parameters.arrivalRadiusRaw,
        .kind = kind,
        .currentPath = payload.path,
        // RefCode AIUpdateInterface::canComputeQuickPath() takes this branch
        // for an AIR-capable locomotor whenever it is not doing ground
        // movement. The DirectLine contract is immutable and still reaches
        // the normal Movement owner; it merely bypasses ground A*.
        .traversalMode = quickPath ? AIPathTraversalMode::DirectLine
                                   : AIPathTraversalMode::Navmesh,
        .waypointStart = {},
        .waypointGraphRevision = 0,
        .waypointHopLimit = 0,
        .polylineOffset = {},
        .groupPathId = kind == PathRequestKind::New
            ? parameters.groupPathId : 0,
        .groupPathMemberOrdinal = kind == PathRequestKind::New
            ? parameters.groupPathMemberOrdinal : 0,
        .groupPathMemberCount = kind == PathRequestKind::New
            ? parameters.groupPathMemberCount : 0,
        .groupPathOffset = kind == PathRequestKind::New
            ? parameters.groupPathOffset : AIFixedPosition{},
        .extraDistanceRaw = 0,
        .pathThroughUnits = false,
        .preciseFinalZ = false,
    };
    if (!request.correlation.isValid() || !output.push(request))
        return false;
    payload.pathRequestIssued = kind != PathRequestKind::Cancel;
    return true;
}

[[nodiscard]] inline bool emitMoveStateSoAInstall(MovementCommandBuffer& output,
                                                  ObjectId subject,
                                                  uint64_t confirmedTick,
                                                  const AIMoveToStatePayload& payload,
                                                  ObjectId ignoredObstacle) noexcept
{
    if (!payload.path)
        return false;
    return output.push({
        .correlation = moveStateSoACorrelation(subject, payload),
        .kind = MovementCommandKind::InstallPath,
        .path = payload.path,
        .ignoredObstacle = ignoredObstacle,
        .clearGoal = false,
        .preserveUltraAccurateFinalPosition = false,
        .confirmedTick = confirmedTick,
    });
}

[[nodiscard]] inline bool beginMoveStateSoARepath(PathRequestBuffer& output,
                                                  ObjectId subject,
                                                   const AIFixedPosition& start,
                                                   const AIStateParameters& parameters,
                                                   AIMoveToStatePayload& payload,
                                                   bool quickPath) noexcept
{
    AIMoveToStatePayload candidate = payload;
    ++candidate.generation;
    if (candidate.generation == 0)
        ++candidate.generation;
    candidate.phase = AIMoveToPhase::WaitingForPath;
    candidate.pathRequestIssued = false;
    if (!emitMoveStateSoAPathRequest(
            output, subject, start, parameters, candidate,
            PathRequestKind::Patch, quickPath))
        return false;
    payload = candidate;
    return true;
}

} // namespace detail

// A false return means the caller-provided spans were not slot-aligned. No
// payload, parameter, result, or output buffer is modified in that case.
[[nodiscard]] inline bool enterMoveToSoA(AIStateFamilySoAStorage& storage,
                                         const AIMoveStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedMoveStateSoASpans(storage, input))
        return false;

    const auto subjects = storage.subjects();
    const auto runtimes = storage.runtimes();
    const auto payloadStates = storage.payloadStates();
    auto parameters = storage.parameters();
    auto& columns = storage.moveTo();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::moveStateSoAScheduled(input, slot) ||
            (!input.childMode &&
             runtimes[slot].currentState != input.activeState))
            continue;
        if (detail::moveStateSoAFact(input.effectivelyDead[slot]))
        {
            input.results[slot] = AIStateStepResult::transitionTo(AIStateId::Dead);
            continue;
        }
        if (!detail::moveStateSoAFact(input.mobile[slot]))
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        if (!input.childMode && payloadStates[slot] != input.activeState)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        AIStateParameters& parameter = parameters[slot];
        AIMoveToStatePayload payload = columns.load(slot);
        if (!subjects[slot] || parameter.sourceOrderRevision == 0)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        payload.sourceOrderRevision = parameter.sourceOrderRevision;
        payload.adjustDestinations = parameter.adjustDestinations;
        if (parameter.goalObject)
        {
            if (!detail::moveStateSoAFact(input.moveTargetValid[slot]))
            {
                input.results[slot] = AIStateStepResult::failure();
                continue;
            }
            parameter.goalPosition = input.resolvedMoveTarget[slot];
            parameter.hasGoalPosition = true;
        }
        else if (!parameter.hasGoalPosition)
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        payload.resolvedGoal = parameter.goalPosition;

        input.results[slot] = detail::emitMoveStateSoAPathRequest(input.pathRequests[slot],
                                                                  subjects[slot],
                                                                   input.subjectPosition[slot],
                                                                   parameter,
                                                                   payload,
                                                                   PathRequestKind::New,
                                                                   !detail::moveStateSoAFact(
                                                                       input.groundMovement[slot]))
                                  ? AIStateStepResult::continueState()
                                  : AIStateStepResult::unsupported();
        columns.store(slot, payload);
    }
    return true;
}

[[nodiscard]] inline bool updateMoveToSoA(AIStateFamilySoAStorage& storage,
                                          const AIMoveStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedMoveStateSoASpans(storage, input))
        return false;

    const auto subjects = storage.subjects();
    const auto runtimes = storage.runtimes();
    const auto payloadStates = storage.payloadStates();
    const auto parameters = storage.parameters();
    auto& columns = storage.moveTo();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::moveStateSoAScheduled(input, slot) ||
            (!input.childMode &&
             runtimes[slot].currentState != input.activeState))
            continue;
        if (detail::moveStateSoAFact(input.effectivelyDead[slot]))
        {
            input.results[slot] = AIStateStepResult::transitionTo(AIStateId::Dead);
            continue;
        }
        if (!detail::moveStateSoAFact(input.mobile[slot]))
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        if (!input.childMode && payloadStates[slot] != input.activeState)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        const AIStateParameters& parameter = parameters[slot];
        AIMoveToStatePayload payload = columns.load(slot);
        struct Writeback final
        {
            AIMoveToSoAColumns& columns;
            size_t slot;
            AIMoveToStatePayload& payload;
            ~Writeback() { columns.store(slot, payload); }
        } writeback{columns, slot, payload};
        if (parameter.goalObject)
        {
            if (!detail::moveStateSoAFact(input.moveTargetValid[slot]))
            {
                input.results[slot] = AIStateStepResult::failure();
                continue;
            }
            payload.resolvedGoal = input.resolvedMoveTarget[slot];
        }

        const PathCorrelation expected = detail::moveStateSoACorrelation(subjects[slot], payload);
        if (payload.phase == AIMoveToPhase::WaitingForPath)
        {
            if (!payload.pathRequestIssued)
            {
                input.results[slot] = detail::emitMoveStateSoAPathRequest(input.pathRequests[slot],
                                                                          subjects[slot],
                                                                           input.subjectPosition[slot],
                                                                           parameter,
                                                                           payload,
                                                                           PathRequestKind::New,
                                                                           !detail::moveStateSoAFact(
                                                                               input.groundMovement[slot]))
                                          ? AIStateStepResult::continueState()
                                          : AIStateStepResult::unsupported();
                continue;
            }
            const PathFeedback& feedback = input.pathFeedback[slot];
            if (!(feedback.correlation == expected))
            {
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }

            switch (feedback.status)
            {
            case PathFeedbackStatus::Pending:
            case PathFeedbackStatus::Delayed:
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            case PathFeedbackStatus::Ready:
                if (!feedback.path)
                {
                    input.results[slot] = AIStateStepResult::unsupported();
                    continue;
                }
                // Install output is part of the phase transition. Preserve
                // WaitingForPath on overflow so the same Ready feedback can
                // be retried without losing the command.
                {
                    AIMoveToStatePayload candidate = payload;
                    candidate.pathRequestIssued = false;
                    candidate.path = feedback.path;
                    candidate.adjustedGoal = feedback.adjustedGoal;
                    candidate.adjustedLayer = feedback.adjustedLayer;
                    candidate.phase = AIMoveToPhase::FollowingPath;
                    if (detail::emitMoveStateSoAInstall(
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
                continue;
            case PathFeedbackStatus::NoPath:
            case PathFeedbackStatus::Cancelled:
                payload.pathRequestIssued = false;
                input.results[slot] = AIStateStepResult::failure();
                continue;
            case PathFeedbackStatus::Unsupported:
                payload.pathRequestIssued = false;
                input.results[slot] = AIStateStepResult::unsupported();
                continue;
            }
        }

        const MovementFeedback& feedback = input.movementFeedback[slot];
        if (!(feedback.correlation == expected))
        {
            input.results[slot] = AIStateStepResult::continueState();
            continue;
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
            input.results[slot] =
                detail::beginMoveStateSoARepath(
                    input.pathRequests[slot], subjects[slot], input.subjectPosition[slot],
                    parameter, payload,
                    !detail::moveStateSoAFact(input.groundMovement[slot]))
                    ? AIStateStepResult::continueState()
                    : AIStateStepResult::unsupported();
            break;
        }
        case MovementFeedbackStatus::Stuck:
            input.results[slot] =
                detail::beginMoveStateSoARepath(
                    input.pathRequests[slot], subjects[slot], input.subjectPosition[slot],
                    parameter, payload,
                    !detail::moveStateSoAFact(input.groundMovement[slot]))
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
    return true;
}

[[nodiscard]] inline bool canExitMoveToSoA(const AIStateFamilySoAStorage& storage,
                                           const AIMoveStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedMoveStateSoASpans(storage, input))
        return false;

    const auto payloadStates = storage.payloadStates();
    const auto& columns = storage.moveTo();

    // Cleanup is transactional across the batch: never emit only one half of
    // Cancel + EndMovement and then lose the old payload correlation.
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::moveStateSoAScheduled(input, slot) ||
            (!input.childMode && payloadStates[slot] != input.activeState))
            continue;
        const AIMoveToStatePayload payload = columns.load(slot);
        if (payload.pathRequestIssued && input.pathRequests[slot].count >= input.pathRequests[slot].values.size())
            return false;
        if (input.movementCommands[slot].count >= input.movementCommands[slot].values.size())
            return false;
    }
    return true;
}

// Exit emits cleanup only. It never writes results or commits a transition.
[[nodiscard]] inline bool exitMoveToSoA(AIStateFamilySoAStorage& storage,
                                        const AIMoveStateSoAKernelInput& input) noexcept
{
    if (!canExitMoveToSoA(storage, input))
        return false;

    const auto subjects = storage.subjects();
    const auto payloadStates = storage.payloadStates();
    const auto parameters = storage.parameters();
    auto& columns = storage.moveTo();

    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::moveStateSoAScheduled(input, slot) ||
            (!input.childMode && payloadStates[slot] != input.activeState))
            continue;

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
