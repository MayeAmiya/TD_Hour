#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include "core/container/container_types.h"

#include "game/object/ai/runtime/AIStateFamilySoAStorage.h"
#include "game/object/ai/runtime/AIStateStep.h"

namespace engine::ai
{

struct AIWanderPanicStateSoAKernelInput final
{
    uint64_t confirmedTick = 0;
    uint32_t repulsorWaitFrames = 10;
    container::Span<const uint8_t> scheduled{};
    container::Span<const uint8_t> effectivelyDead;
    container::Span<const uint8_t> mobile;
    container::Span<const uint8_t> groundMovement;
    container::Span<const uint8_t> projectile;
    container::Span<const uint8_t> canBeRepulsed;
    container::Span<const AIFixedPosition> subjectPosition;
    container::Span<const AIFixedPosition> wanderOffset;
    container::Span<const uint32_t> branchChoice;
    container::Span<const ObjectId> closestRepulsor;
    const AIWaypointGraphResolver* waypoints = nullptr;
    container::Span<const PathFeedback> pathFeedback;
    container::Span<const MovementFeedback> movementFeedback;
    container::Span<PathRequestBuffer> pathRequests;
    container::Span<MovementCommandBuffer> movementCommands;
    container::Span<AIWaypointCompletionBuffer> completions;
    container::Span<AIStateStepResult> results;
};

namespace wander_panic_detail
{

enum class MoveResult : uint8_t
{
    Continue,
    Success,
    Failure,
    Unsupported,
    // The pathfinder resolved the request as NoPath/Cancelled.  Distinct from
    // Failure because Panic deliberately treats a *movement* failure as
    // "keep panicking", but must still advance to the next waypoint when the
    // current one is simply unreachable — otherwise the slot stays parked in
    // WaitingForPath forever.
    PathUnavailable,
};

[[nodiscard]] constexpr bool isState(AIStateId state) noexcept
{
    return state == AIStateId::Wander || state == AIStateId::Panic;
}

[[nodiscard]] constexpr bool scheduled(const AIWanderPanicStateSoAKernelInput& input, size_t slot) noexcept
{
    return input.scheduled.empty() || input.scheduled[slot] != 0;
}

[[nodiscard]] constexpr bool fact(uint8_t value) noexcept
{
    return value != 0;
}

[[nodiscard]] constexpr int64_t saturatingAdd(int64_t left, int64_t right) noexcept
{
    if (right > 0 && left > std::numeric_limits<int64_t>::max() - right)
        return std::numeric_limits<int64_t>::max();
    if (right < 0 && left < std::numeric_limits<int64_t>::min() - right)
        return std::numeric_limits<int64_t>::min();
    return left + right;
}

[[nodiscard]] constexpr uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept
{
    return right > std::numeric_limits<uint64_t>::max() - left
               ? std::numeric_limits<uint64_t>::max()
               : left + right;
}

[[nodiscard]] inline bool hasAlignedSpans(const AIStateFamilySoAStorage& storage,
                                          const AIWanderPanicStateSoAKernelInput& input) noexcept
{
    const size_t count = storage.size();
    return input.waypoints != nullptr && (input.scheduled.empty() || input.scheduled.size() == count) &&
           input.effectivelyDead.size() == count && input.mobile.size() == count &&
           input.groundMovement.size() == count && input.projectile.size() == count &&
           input.canBeRepulsed.size() == count && input.subjectPosition.size() == count &&
           input.wanderOffset.size() == count && input.branchChoice.size() == count &&
           input.closestRepulsor.size() == count && input.pathFeedback.size() == count &&
           input.movementFeedback.size() == count && input.pathRequests.size() == count &&
           input.movementCommands.size() == count && input.completions.size() == count &&
           input.results.size() == count;
}

[[nodiscard]] inline PathCorrelation correlation(ObjectId subject,
                                                  const AIWaypointPathStatePayload& payload) noexcept
{
    return {.subject = subject,
            .stateRequest = payload.request,
            .generation = payload.generation,
            .sourceOrderRevision = payload.sourceOrderRevision};
}

[[nodiscard]] inline bool emitPath(PathRequestBuffer& output,
                                   ObjectId subject,
                                   const AIFixedPosition& start,
                                   const AIStateParameters& parameters,
                                   AIWaypointPathStatePayload& payload,
                                   PathRequestKind kind,
                                   bool quickPath) noexcept
{
    const PathRequest request{
        .correlation = correlation(subject, payload),
        .start = start,
        .originalGoal = payload.goal,
        .adjustDestinations = payload.adjustDestinations,
        .ignoredObstacle = parameters.ignoredObstacle,
        .surfaceMask = parameters.pathSurfaceMask,
        .arrivalRadiusRaw = parameters.arrivalRadiusRaw,
        .kind = kind,
        .currentPath = payload.path,
        .traversalMode = quickPath ? AIPathTraversalMode::DirectLine
                                   : AIPathTraversalMode::Navmesh,
        .waypointStart = {},
        .waypointGraphRevision = 0,
        .waypointHopLimit = 0,
        .polylineOffset = {},
        .extraDistanceRaw = payload.extraDistanceRaw,
        .pathThroughUnits = false,
        .preciseFinalZ = payload.preciseFinalZ,
    };
    if (!request.correlation.isValid() || !output.push(request))
        return false;
    payload.pathRequestIssued = kind != PathRequestKind::Cancel;
    return true;
}

[[nodiscard]] inline bool emitMovement(MovementCommandBuffer& output,
                                       ObjectId subject,
                                       uint64_t tick,
                                       const AIWaypointPathStatePayload& payload,
                                       ObjectId ignoredObstacle,
                                       MovementCommandKind kind,
                                       AIMovementMode mode) noexcept
{
    return output.push({.correlation = correlation(subject, payload),
                        .kind = kind,
                        .path = payload.path,
                        .ignoredObstacle = ignoredObstacle,
                        .extraDistanceRaw = payload.extraDistanceRaw,
                        .mode = mode,
                        .panicking = mode == AIMovementMode::Panic,
                        .clearGoal = kind == MovementCommandKind::EndMovement && payload.adjustDestinations,
                        .preserveUltraAccurateFinalPosition = kind == MovementCommandKind::EndMovement,
                        .allowPathThroughUnits = false,
                        .confirmedTick = tick});
}

[[nodiscard]] inline bool prepareGoal(const AIWanderPanicStateSoAKernelInput& input,
                                      const AIStateParameters& parameters,
                                      AIWaypointPathStatePayload& payload,
                                      size_t slot) noexcept
{
    const AIWaypointQuery query = input.waypoints->node(payload.current, payload.graphRevision);
    if (query.status != AIWaypointQueryStatus::Node)
        return false;

    payload.groupOffset = input.wanderOffset[slot];
    payload.goal = query.node.position;
    payload.goal.xRaw = saturatingAdd(payload.goal.xRaw, payload.groupOffset.xRaw);
    payload.goal.yRaw = saturatingAdd(payload.goal.yRaw, payload.groupOffset.yRaw);
    payload.extraDistanceRaw = query.node.lookAheadDistanceRaw;
    payload.adjustDestinations = query.node.linkCount == 0 && parameters.adjustDestinations &&
                                 fact(input.groundMovement[slot]);
    payload.preciseFinalZ = query.node.linkCount == 0 && fact(input.projectile[slot]);
    payload.phase = AIMoveToPhase::WaitingForPath;
    payload.path = {};
    payload.pathRequestIssued = false;
    return true;
}

[[nodiscard]] inline AIStateStepResult complete(AIWaypointPathStatePayload& payload) noexcept
{
    payload.completionTerminal = payload.current;
    payload.completionPending = true;
    return AIStateStepResult::success();
}

[[nodiscard]] inline AIStateStepResult advance(const AIWanderPanicStateSoAKernelInput& input,
                                               ObjectId subject,
                                               const AIStateParameters& parameters,
                                               AIWaypointPathStatePayload& payload,
                                               size_t slot) noexcept
{
    const AIWaypointQuery current = input.waypoints->node(payload.current, payload.graphRevision);
    if (current.status != AIWaypointQueryStatus::Node)
        return AIStateStepResult::unsupported();
    if (current.node.linkCount == 0)
        return complete(payload);

    const uint32_t selected = input.branchChoice[slot] % current.node.linkCount;
    const AIWaypointLinkQuery link = input.waypoints->link(payload.current, payload.graphRevision, selected);
    if (link.status != AIWaypointQueryStatus::Node || !link.target)
        return AIStateStepResult::unsupported();

    AIWaypointPathStatePayload candidate = payload;
    candidate.prior = candidate.current;
    candidate.current = link.target;
    ++candidate.generation;
    if (candidate.generation == 0)
        ++candidate.generation;
    if (!prepareGoal(input, parameters, candidate, slot) ||
        !emitPath(input.pathRequests[slot],
                  subject,
                  input.subjectPosition[slot],
                  parameters,
                  candidate,
                  PathRequestKind::New,
                  !fact(input.groundMovement[slot])))
    {
        return AIStateStepResult::unsupported();
    }
    payload = candidate;
    return AIStateStepResult::continueState();
}

[[nodiscard]] inline MoveResult updateMovement(const AIWanderPanicStateSoAKernelInput& input,
                                               ObjectId subject,
                                               AIWaypointPathStatePayload& payload,
                                               ObjectId ignoredObstacle,
                                               size_t slot,
                                               AIMovementMode mode) noexcept
{
    const PathCorrelation expected = correlation(subject, payload);
    if (payload.phase == AIMoveToPhase::WaitingForPath)
    {
        const PathFeedback& feedback = input.pathFeedback[slot];
        if (!(feedback.correlation == expected) || feedback.status == PathFeedbackStatus::Pending ||
            feedback.status == PathFeedbackStatus::Delayed)
        {
            return MoveResult::Continue;
        }
        if (feedback.status == PathFeedbackStatus::Ready)
        {
            if (!feedback.path)
                return MoveResult::Unsupported;
            AIWaypointPathStatePayload candidate = payload;
            candidate.path = feedback.path;
            candidate.pathRequestIssued = false;
            candidate.phase = AIMoveToPhase::FollowingPath;
            if (!emitMovement(input.movementCommands[slot],
                              subject,
                              input.confirmedTick,
                              candidate,
                              ignoredObstacle,
                              MovementCommandKind::InstallPath,
                              mode))
            {
                return MoveResult::Unsupported;
            }
            payload = candidate;
            return MoveResult::Continue;
        }
        if (feedback.status == PathFeedbackStatus::Unsupported)
            return MoveResult::Unsupported;
        // NoPath/Cancelled resolves the outstanding request, so drop the issued
        // flag: leaving it set kept the slot in WaitingForPath with a request
        // that could never be re-issued.
        payload.pathRequestIssued = false;
        return MoveResult::PathUnavailable;
    }

    const MovementFeedback& feedback = input.movementFeedback[slot];
    if (!(feedback.correlation == expected) || isMovementActiveFeedback(feedback.status) ||
        feedback.status == MovementFeedbackStatus::Blocked)
    {
        return MoveResult::Continue;
    }
    if (feedback.status == MovementFeedbackStatus::Completed)
        return MoveResult::Success;
    if (feedback.status == MovementFeedbackStatus::Unsupported)
        return MoveResult::Unsupported;
    return MoveResult::Failure;
}

[[nodiscard]] inline bool repulsorFailure(const AIWanderPanicStateSoAKernelInput& input,
                                          ObjectId subject,
                                          AIApproachPathStatePayload& scan,
                                          size_t slot) noexcept
{
    if (!fact(input.canBeRepulsed[slot]) || input.confirmedTick < scan.nextRepulsorScanTick)
        return false;

    const uint64_t wait = static_cast<uint64_t>(input.repulsorWaitFrames) + (subject.value & 0x7u) + 1u;
    scan.nextRepulsorScanTick = saturatingAdd(input.confirmedTick, wait);
    scan.repulsor = input.closestRepulsor[slot];
    return scan.repulsor.isValid();
}

} // namespace wander_panic_detail

[[nodiscard]] inline bool enterWanderPanicSoA(AIStateFamilySoAStorage& storage,
                                              AIStateId state,
                                              const AIWanderPanicStateSoAKernelInput& input) noexcept
{
    using namespace wander_panic_detail;
    if (!isState(state) || !hasAlignedSpans(storage, input))
        return false;

    const auto subjects = storage.subjects();
    const auto runtimes = storage.runtimes();
    const auto payloadStates = storage.payloadStates();
    const auto activationSequences = storage.activationSequences();
    const auto parameters = storage.parameters();
    auto& waypointColumns = storage.waypointPath();
    auto& scanColumns = storage.approachPath();

    for (const size_t slot : storage.executionSlots())
    {
        if (!scheduled(input, slot) || runtimes[slot].currentState != state)
            continue;
        if (fact(input.effectivelyDead[slot]))
        {
            input.results[slot] = AIStateStepResult::transitionTo(AIStateId::Dead);
            continue;
        }
        if (payloadStates[slot] != state)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        const AIStateParameters& parameter = parameters[slot];
        const AIStateRequestId request{runtimes[slot].enteredTick, activationSequences[slot]};
        if (!subjects[slot] || !request.isValid() || !fact(input.mobile[slot]) || !parameter.waypoint ||
            parameter.waypointGraphRevision == 0 || parameter.sourceOrderRevision == 0)
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        if (input.pathRequests[slot].count >= input.pathRequests[slot].values.size())
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        AIWaypointPathStatePayload payload{request};
        payload.current = parameter.waypoint;
        payload.graphRevision = parameter.waypointGraphRevision;
        payload.sourceOrderRevision = parameter.sourceOrderRevision;
        if (!prepareGoal(input, parameter, payload, slot) ||
            !emitPath(input.pathRequests[slot],
                      subjects[slot],
                      input.subjectPosition[slot],
                      parameter,
                      payload,
                      PathRequestKind::New,
                      !fact(input.groundMovement[slot])))
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        waypointColumns.store(slot, payload);
        AIApproachPathStatePayload scan{request};
        scan.nextRepulsorScanTick = input.confirmedTick;
        scanColumns.store(slot, scan);
        input.results[slot] = AIStateStepResult::continueState();
    }
    return true;
}

[[nodiscard]] inline bool updateWanderPanicSoA(AIStateFamilySoAStorage& storage,
                                               AIStateId state,
                                               const AIWanderPanicStateSoAKernelInput& input) noexcept
{
    using namespace wander_panic_detail;
    if (!isState(state) || !hasAlignedSpans(storage, input))
        return false;

    const auto subjects = storage.subjects();
    const auto runtimes = storage.runtimes();
    const auto payloadStates = storage.payloadStates();
    const auto parameters = storage.parameters();
    auto& waypointColumns = storage.waypointPath();
    auto& scanColumns = storage.approachPath();

    for (const size_t slot : storage.executionSlots())
    {
        if (!scheduled(input, slot) || runtimes[slot].currentState != state)
            continue;
        if (fact(input.effectivelyDead[slot]))
        {
            input.results[slot] = AIStateStepResult::transitionTo(AIStateId::Dead);
            continue;
        }
        if (!fact(input.mobile[slot]))
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        if (payloadStates[slot] != state)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        AIWaypointPathStatePayload payload = waypointColumns.load(slot);
        AIApproachPathStatePayload scan = scanColumns.load(slot);
        const AIMovementMode movementMode = state == AIStateId::Panic
            ? AIMovementMode::Panic : AIMovementMode::Wander;
        const MoveResult movement = updateMovement(
            input, subjects[slot], payload,
            parameters[slot].ignoredObstacle, slot, movementMode);
        const bool repulsed = repulsorFailure(input, subjects[slot], scan, slot);
        waypointColumns.store(slot, payload);
        scanColumns.store(slot, scan);

        if (repulsed)
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        if (movement == MoveResult::Unsupported)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        // Panic keeps running through an ordinary movement failure, but a
        // PathUnavailable result must fall through to advance() so the next
        // waypoint is tried — that is exactly what the non-Panic states do.
        if (movement == MoveResult::Continue || (movement == MoveResult::Failure && state == AIStateId::Panic))
        {
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }
        input.results[slot] = advance(input, subjects[slot], parameters[slot], payload, slot);
        waypointColumns.store(slot, payload);
    }
    return true;
}

[[nodiscard]] inline bool canExitWanderPanicSoA(const AIStateFamilySoAStorage& storage,
                                                const AIWanderPanicStateSoAKernelInput& input) noexcept
{
    using namespace wander_panic_detail;
    if (!hasAlignedSpans(storage, input))
        return false;

    const auto states = storage.payloadStates();
    const auto& columns = storage.waypointPath();
    for (const size_t slot : storage.executionSlots())
    {
        if (!scheduled(input, slot) || !isState(states[slot]))
            continue;
        const AIWaypointPathStatePayload payload = columns.load(slot);
        if ((payload.pathRequestIssued && input.pathRequests[slot].count >= input.pathRequests[slot].values.size()) ||
            input.movementCommands[slot].count >= input.movementCommands[slot].values.size() ||
            (payload.completionPending && input.completions[slot].count >= input.completions[slot].values.size()))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool exitWanderPanicSoA(AIStateFamilySoAStorage& storage,
                                             const AIWanderPanicStateSoAKernelInput& input) noexcept
{
    using namespace wander_panic_detail;
    if (!canExitWanderPanicSoA(storage, input))
        return false;

    const auto subjects = storage.subjects();
    const auto states = storage.payloadStates();
    const auto parameters = storage.parameters();
    auto& columns = storage.waypointPath();
    for (const size_t slot : storage.executionSlots())
    {
        if (!scheduled(input, slot) || !isState(states[slot]))
            continue;
        AIWaypointPathStatePayload payload = columns.load(slot);
        if (payload.pathRequestIssued)
        {
            static_cast<void>(emitPath(input.pathRequests[slot],
                                       subjects[slot],
                                       input.subjectPosition[slot],
                                       parameters[slot],
                                       payload,
                                       PathRequestKind::Cancel,
                                       !fact(input.groundMovement[slot])));
        }
        if (payload.completionPending)
        {
            static_cast<void>(input.completions[slot].push({.subject = subjects[slot],
                                                           .stateRequest = payload.request,
                                                           .terminal = payload.completionTerminal,
                                                           .confirmedTick = input.confirmedTick}));
            payload.completionPending = false;
        }
        static_cast<void>(emitMovement(input.movementCommands[slot],
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
