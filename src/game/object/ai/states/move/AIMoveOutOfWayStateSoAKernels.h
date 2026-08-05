#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include "core/container/container_types.h"

#include "game/object/ai/runtime/AIStateFamilySoAStorage.h"
#include "game/object/ai/runtime/AIStateStep.h"

namespace engine::ai
{

struct AIMoveOutOfWayStateSoAKernelInput final
{
    uint64_t confirmedTick = 0;
    container::Span<const uint8_t> scheduled{};
    container::Span<const uint8_t> effectivelyDead;
    container::Span<const uint8_t> mobile;
    container::Span<const uint32_t> ticksPerSecond;
    container::Span<const AIFixedPosition> subjectPosition;
    container::Span<const PathFeedback> pathFeedback;
    container::Span<const MovementFeedback> movementFeedback;
    container::Span<PathRequestBuffer> pathRequests;
    container::Span<MovementCommandBuffer> movementCommands;
    container::Span<AIStateStepResult> results;
};

namespace detail
{
[[nodiscard]] inline bool hasAlignedMoveOutSpans(const AIStateFamilySoAStorage& storage,
                                                 const AIMoveOutOfWayStateSoAKernelInput& input) noexcept
{
    const size_t count = storage.size();
    return (input.scheduled.empty() || input.scheduled.size() == count) && input.effectivelyDead.size() == count &&
           input.mobile.size() == count && input.ticksPerSecond.size() == count &&
           input.subjectPosition.size() == count &&
           input.pathFeedback.size() == count && input.movementFeedback.size() == count &&
           input.pathRequests.size() == count &&
           input.movementCommands.size() == count &&
           input.results.size() == count;
}
[[nodiscard]] constexpr bool moveOutScheduled(const AIMoveOutOfWayStateSoAKernelInput& input, size_t slot) noexcept
{ return input.scheduled.empty() || input.scheduled[slot] != 0; }
[[nodiscard]] inline PathCorrelation moveOutCorrelation(ObjectId subject,
                                                        const AIMoveOutOfWayStatePayload& payload) noexcept
{
    return {.subject = subject, .stateRequest = payload.request, .generation = payload.generation,
            .sourceOrderRevision = payload.sourceOrderRevision};
}
[[nodiscard]] inline bool emitMoveOutCommand(MovementCommandBuffer& output,
                                             ObjectId subject,
                                             uint64_t tick,
                                             const AIMoveOutOfWayStatePayload& payload,
                                             ObjectId ignoredObstacle,
                                             MovementCommandKind kind) noexcept
{
    return output.push({.correlation = moveOutCorrelation(subject, payload), .kind = kind, .path = payload.path,
                        .ignoredObstacle = ignoredObstacle,
                        .clearGoal = kind == MovementCommandKind::EndMovement,
                        .preserveUltraAccurateFinalPosition = kind == MovementCommandKind::EndMovement,
                        .allowPathThroughUnits = payload.allowPathThroughUnits, .confirmedTick = tick});
}

[[nodiscard]] inline bool emitMoveOutPathRequest(
    PathRequestBuffer& output, ObjectId subject,
    const AIFixedPosition& start, const AIStateParameters& parameters,
    AIMoveOutOfWayStatePayload& payload, PathRequestKind kind) noexcept
{
    const PathRequest request{
        .correlation = moveOutCorrelation(subject, payload),
        .start = start,
        .originalGoal = payload.goal,
        .adjustDestinations = true,
        .ignoredObstacle = parameters.ignoredObstacle,
        .surfaceMask = parameters.pathSurfaceMask,
        .arrivalRadiusRaw = parameters.arrivalRadiusRaw,
        .kind = kind,
        .currentPath = payload.path,
        .traversalMode = AIPathTraversalMode::Navmesh,
        .pathThroughUnits = payload.allowPathThroughUnits,
    };
    if (!request.correlation.isValid() || !output.push(request)) return false;
    payload.pathRequestIssued = kind != PathRequestKind::Cancel;
    return true;
}

} // namespace detail

[[nodiscard]] inline bool enterMoveOutOfWaySoA(AIStateFamilySoAStorage& storage,
                                               const AIMoveOutOfWayStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedMoveOutSpans(storage, input)) return false;
    const auto subjects = storage.subjects(); const auto runtimes = storage.runtimes();
    const auto states = storage.payloadStates(); const auto parameters = storage.parameters();
    auto& columns = storage.moveOutOfWay();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::moveOutScheduled(input, slot) || runtimes[slot].currentState != AIStateId::MoveOutOfTheWay) continue;
        if (states[slot] != AIStateId::MoveOutOfTheWay) { input.results[slot] = AIStateStepResult::unsupported(); continue; }
        if (!input.mobile[slot] || !parameters[slot].hasGoalPosition ||
            parameters[slot].sourceOrderRevision == 0)
        { input.results[slot] = AIStateStepResult::failure(); continue; }
        AIMoveOutOfWayStatePayload candidate = columns.load(slot);
        candidate.goal = parameters[slot].goalPosition;
        candidate.path = {};
        candidate.sourceOrderRevision = parameters[slot].sourceOrderRevision;
        const uint64_t maximum = std::numeric_limits<uint64_t>::max();
        const uint64_t duration = input.ticksPerSecond[slot] > maximum / 10u
            ? maximum
            : static_cast<uint64_t>(input.ticksPerSecond[slot]) * 10u;
        candidate.deadlineTick = input.confirmedTick > maximum - duration
            ? maximum : input.confirmedTick + duration;
        candidate.generation = 1;
        // RefCode privateMoveAwayFromUnit constructs a new path from the
        // blocking unit's current path *before* entering this temporary
        // state.  The detached runtime has no mutable Path pointer to transfer,
        // so preserve that semantic as one high-priority immutable MoveAside
        // request.  RebindExistingPath is invalid for the usual idle blocker,
        // which has no installed path of its own.
        candidate.phase = AIMoveToPhase::WaitingForPath;
        candidate.pathRequestIssued = false;
        candidate.allowPathThroughUnits = false;
        if (!detail::emitMoveOutPathRequest(
                input.pathRequests[slot], subjects[slot],
                input.subjectPosition[slot], parameters[slot], candidate,
                PathRequestKind::MoveAside))
        { input.results[slot] = AIStateStepResult::unsupported(); continue; }
        columns.store(slot, candidate); input.results[slot] = AIStateStepResult::continueState();
    }
    return true;
}

[[nodiscard]] inline bool updateMoveOutOfWaySoA(AIStateFamilySoAStorage& storage,
                                                const AIMoveOutOfWayStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedMoveOutSpans(storage, input)) return false;
    const auto subjects = storage.subjects(); const auto runtimes = storage.runtimes(); const auto states = storage.payloadStates();
    const auto parameters = storage.parameters();
    auto& columns = storage.moveOutOfWay();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::moveOutScheduled(input, slot) || runtimes[slot].currentState != AIStateId::MoveOutOfTheWay) continue;
        if (input.effectivelyDead[slot]) { input.results[slot] = AIStateStepResult::success(); continue; }
        if (states[slot] != AIStateId::MoveOutOfTheWay) { input.results[slot] = AIStateStepResult::unsupported(); continue; }
        AIMoveOutOfWayStatePayload payload = columns.load(slot);
        // RefCode installs this as a temporary state with a hard
        // 10*LOGICFRAMES_PER_SECOND lifetime.  A lost/delayed path feedback
        // must therefore release the remembered blocker and permit a later
        // collision to compute a fresh MoveAway path.
        if (payload.deadlineTick != 0 &&
            input.confirmedTick >= payload.deadlineTick)
        { input.results[slot] = AIStateStepResult::failure(); continue; }
        const PathCorrelation expected = detail::moveOutCorrelation(
            subjects[slot], payload);
        if (payload.phase == AIMoveToPhase::WaitingForPath)
        {
            if (!payload.pathRequestIssued)
            {
                input.results[slot] = detail::emitMoveOutPathRequest(
                    input.pathRequests[slot], subjects[slot],
                    input.subjectPosition[slot], parameters[slot], payload,
                    PathRequestKind::MoveAside)
                    ? AIStateStepResult::continueState()
                    : AIStateStepResult::unsupported();
                columns.store(slot, payload);
                continue;
            }
            const PathFeedback& path = input.pathFeedback[slot];
            if (!(path.correlation == expected))
            { input.results[slot] = AIStateStepResult::continueState(); continue; }
            if (path.status == PathFeedbackStatus::Pending ||
                path.status == PathFeedbackStatus::Delayed)
            { input.results[slot] = AIStateStepResult::continueState(); continue; }
            if (path.status == PathFeedbackStatus::Ready && path.path)
            {
                AIMoveOutOfWayStatePayload candidate = payload;
                candidate.path = path.path;
                candidate.phase = AIMoveToPhase::FollowingPath;
                candidate.pathRequestIssued = false;
                if (!detail::emitMoveOutCommand(
                        input.movementCommands[slot], subjects[slot],
                        input.confirmedTick, candidate,
                        parameters[slot].ignoredObstacle,
                        MovementCommandKind::InstallPath))
                { input.results[slot] = AIStateStepResult::unsupported(); continue; }
                columns.store(slot, candidate);
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
            if (path.status == PathFeedbackStatus::NoPath &&
                !payload.allowPathThroughUnits)
            {
                ++payload.generation;
                if (payload.generation == 0) ++payload.generation;
                payload.pathRequestIssued = false;
                payload.allowPathThroughUnits = true;
                input.results[slot] = detail::emitMoveOutPathRequest(
                    input.pathRequests[slot], subjects[slot],
                    input.subjectPosition[slot], parameters[slot], payload,
                    PathRequestKind::MoveAside)
                    ? AIStateStepResult::continueState()
                    : AIStateStepResult::unsupported();
                columns.store(slot, payload);
                continue;
            }
            payload.pathRequestIssued = false;
            columns.store(slot, payload);
            input.results[slot] = path.status == PathFeedbackStatus::Unsupported
                ? AIStateStepResult::unsupported()
                : AIStateStepResult::failure();
            continue;
        }
        const MovementFeedback& feedback = input.movementFeedback[slot];
        if (!(feedback.correlation == expected))
        { input.results[slot] = AIStateStepResult::continueState(); continue; }
        if (isMovementActiveFeedback(feedback.status))
        { input.results[slot] = AIStateStepResult::continueState(); continue; }
        if (feedback.status == MovementFeedbackStatus::Completed)
        { input.results[slot] = AIStateStepResult::success(); continue; }
        if (feedback.status == MovementFeedbackStatus::Cancelled)
        { input.results[slot] = AIStateStepResult::failure(); continue; }
        if (feedback.status == MovementFeedbackStatus::Unsupported)
        { input.results[slot] = AIStateStepResult::unsupported(); continue; }
        if (feedback.status == MovementFeedbackStatus::Blocked)
        { input.results[slot] = AIStateStepResult::continueState(); continue; }
        AIMoveOutOfWayStatePayload candidate = payload; candidate.allowPathThroughUnits = true;
        if (!detail::emitMoveOutCommand(input.movementCommands[slot], subjects[slot], input.confirmedTick, candidate,
                                        parameters[slot].ignoredObstacle,
                                        MovementCommandKind::RebindExistingPath))
        { input.results[slot] = AIStateStepResult::unsupported(); continue; }
        columns.store(slot, candidate); input.results[slot] = AIStateStepResult::continueState();
    }
    return true;
}

[[nodiscard]] inline bool canExitMoveOutOfWaySoA(const AIStateFamilySoAStorage& storage,
                                                 const AIMoveOutOfWayStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedMoveOutSpans(storage, input)) return false;
    const auto states = storage.payloadStates();
    const auto& columns = storage.moveOutOfWay();
    for (const size_t slot : storage.executionSlots())
        if (detail::moveOutScheduled(input, slot) &&
            states[slot] == AIStateId::MoveOutOfTheWay) {
            const AIMoveOutOfWayStatePayload payload = columns.load(slot);
            if (payload.pathRequestIssued && input.pathRequests[slot].count >=
                    input.pathRequests[slot].values.size()) return false;
            if (input.movementCommands[slot].count >=
                input.movementCommands[slot].values.size()) return false;
        }
    return true;
}

[[nodiscard]] inline bool exitMoveOutOfWaySoA(AIStateFamilySoAStorage& storage,
                                              const AIMoveOutOfWayStateSoAKernelInput& input) noexcept
{
    if (!canExitMoveOutOfWaySoA(storage, input)) return false;
    const auto subjects = storage.subjects(); const auto states = storage.payloadStates();
    const auto parameters = storage.parameters(); auto& columns = storage.moveOutOfWay();
    for (const size_t slot : storage.executionSlots())
        if (detail::moveOutScheduled(input, slot) &&
            states[slot] == AIStateId::MoveOutOfTheWay) {
            AIMoveOutOfWayStatePayload payload = columns.load(slot);
            if (payload.pathRequestIssued) {
                static_cast<void>(detail::emitMoveOutPathRequest(
                    input.pathRequests[slot], subjects[slot],
                    input.subjectPosition[slot], parameters[slot], payload,
                    PathRequestKind::Cancel));
            }
            static_cast<void>(detail::emitMoveOutCommand(input.movementCommands[slot], subjects[slot], input.confirmedTick,
                                                         payload, parameters[slot].ignoredObstacle,
                                                         MovementCommandKind::EndMovement));
            columns.store(slot, payload);
        }
    return true;
}

} // namespace engine::ai
