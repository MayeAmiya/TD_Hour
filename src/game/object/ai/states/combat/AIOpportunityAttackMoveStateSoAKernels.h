#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "core/container/container_types.h"
#include "game/object/ai/states/move/AIMoveStateSoAKernels.h"
#include "game/object/ai/states/combat/AIOpportunityAttackMoveStateData.h"
#include "game/object/ai/states/move/AIWaypointStateSoAKernels.h"

namespace engine::ai
{
// movementResults is the per-slot result produced by the existing movement
// body for this confirmed tick. The adapter may evaluate the MoveTo/Waypoint
// kernel before this wrapper and expose that result as this immutable view.
struct AIOpportunityAttackMoveStateSoAKernelInput final
{
    uint64_t confirmedTick = 0;
    container::Span<const uint8_t> scheduled{};
    AIExecutionSlotRange executionSlots{};
    container::Span<const AIStateId> states;
    container::Span<const ObjectId> subjects;
    container::Span<const uint64_t> sourceOrderRevisions;
    container::Span<const uint8_t> effectivelyDead;
    container::Span<const uint8_t> mobile;
    container::Span<const uint8_t> groundMovement;
    container::Span<const AIFixedPosition> subjectPositions;
    container::Span<const uint32_t> ticksPerSecond;
    container::Span<const int64_t> pathfindCellSizeRaw;
    container::Span<const AIStateParameters> parameters;
    AIMoveToSoAColumns* moveToColumns = nullptr;
    AIWaypointPathSoAColumns* waypointColumns = nullptr;
    // Filled by the shared MoveTo/Waypoint body immediately before this
    // wrapper runs, then read as an immutable per-slot result by wrapper code.
    container::Span<AIStateStepResult> movementResults;
    container::Span<const AIOpportunityAttackMoveQueryFeedbackBuffer> queryFeedback;
    container::Span<const AIOpportunityAttackMoveChildFeedbackBuffer> childFeedback;
    container::Span<AIOpportunityAttackMoveQueryCommandBuffer> queryCommands;
    container::Span<AIOpportunityAttackMoveChildCommandBuffer> childCommands;
    container::Span<PathRequestBuffer> pathRequests;
    container::Span<MovementCommandBuffer> movementCommands;
    container::Span<AIWaypointCompletionBuffer> completions;
    container::Span<AIStateStepResult> results;
};

namespace opportunity_attack_move_detail
{

[[nodiscard]] constexpr bool fact(uint8_t value) noexcept
{
    return value != 0;
}

[[nodiscard]] inline bool scheduled(const AIOpportunityAttackMoveStateSoAKernelInput& input, size_t slot) noexcept
{
    return input.scheduled.empty() || input.scheduled[slot] != 0;
}

[[nodiscard]] inline bool columnsAligned(const AIOpportunityAttackMoveSoAColumns& columns, size_t count) noexcept
{
    return columns.requestIssuedTick.size() == count && columns.requestSequence.size() == count &&
           columns.sourceOrderRevision.size() == count && columns.state.size() == count &&
           columns.active.size() == count && columns.phase.size() == count && columns.phaseRevision.size() == count &&
           columns.nextOperationRevision.size() == count && columns.scanOperation.size() == count &&
           columns.queryPending.size() == count && columns.queryRevision.size() == count &&
           columns.childOperation.size() == count && columns.childActive.size() == count &&
           columns.childRevision.size() == count && columns.childTarget.size() == count &&
           columns.movementPaused.size() == count && columns.resumeRequired.size() == count &&
           columns.resumeScanComplete.size() == count && columns.forceRetarget.size() == count &&
           columns.retriesRemaining.size() == count && columns.retryWakeTick.size() == count &&
           columns.movementTerminal.size() == count;
}

[[nodiscard]] inline bool aligned(const AIOpportunityAttackMoveSoAColumns& columns,
                                  const AIOpportunityAttackMoveStateSoAKernelInput& input) noexcept
{
    const size_t count = input.states.size();
    return columnsAligned(columns, count) && (input.scheduled.empty() || input.scheduled.size() == count) &&
           input.subjects.size() == count && input.sourceOrderRevisions.size() == count &&
           input.effectivelyDead.size() == count && input.mobile.size() == count &&
           input.groundMovement.size() == count &&
           input.subjectPositions.size() == count && input.ticksPerSecond.size() == count &&
           input.pathfindCellSizeRaw.size() == count && input.parameters.size() == count &&
           input.moveToColumns != nullptr && input.moveToColumns->size() == count && input.waypointColumns != nullptr &&
           input.waypointColumns->size() == count &&
           input.movementResults.size() == count && input.queryFeedback.size() == count &&
           input.childFeedback.size() == count && input.queryCommands.size() == count &&
           input.childCommands.size() == count && input.pathRequests.size() == count &&
           input.movementCommands.size() == count && input.completions.size() == count && input.results.size() == count;
}

template <typename Buffer>
[[nodiscard]] inline bool available(const Buffer& buffer, size_t additional = 1) noexcept
{
    return buffer.count <= buffer.values.size() && additional <= buffer.values.size() - buffer.count;
}

[[nodiscard]] constexpr uint32_t nextRevision(uint32_t value) noexcept
{
    ++value;
    return value == 0 ? 1 : value;
}

[[nodiscard]] constexpr uint64_t saturatingAdd(uint64_t lhs, uint64_t rhs) noexcept
{
    return rhs > std::numeric_limits<uint64_t>::max() - lhs ? std::numeric_limits<uint64_t>::max() : lhs + rhs;
}

[[nodiscard]] constexpr uint64_t saturatingMultiply(uint64_t lhs, uint64_t rhs) noexcept
{
    return lhs != 0 && rhs > std::numeric_limits<uint64_t>::max() / lhs ? std::numeric_limits<uint64_t>::max()
                                                                        : lhs * rhs;
}

[[nodiscard]] constexpr uint64_t unsignedDistance(int64_t lhs, int64_t rhs) noexcept
{
    return lhs >= rhs ? static_cast<uint64_t>(lhs) - static_cast<uint64_t>(rhs)
                      : static_cast<uint64_t>(rhs) - static_cast<uint64_t>(lhs);
}

[[nodiscard]] constexpr uint64_t saturatingSquare(uint64_t value) noexcept
{
    return saturatingMultiply(value, value);
}

[[nodiscard]] inline bool withinMoveToRetryDistance(const AIFixedPosition& subject,
                                                    const AIFixedPosition& goal,
                                                    int64_t cellSizeRaw) noexcept
{
    if (cellSizeRaw <= 0)
        return false;
    const uint64_t dx = unsignedDistance(subject.xRaw, goal.xRaw);
    const uint64_t dy = unsignedDistance(subject.yRaw, goal.yRaw);
    const uint64_t distanceSquared = saturatingAdd(saturatingSquare(dx), saturatingSquare(dy));
    const uint64_t threshold =
        saturatingMultiply(static_cast<uint64_t>(cellSizeRaw), AI_OPPORTUNITY_ATTACK_MOVE_CLOSE_ENOUGH_CELLS);
    return distanceSquared < saturatingSquare(threshold);
}

inline void resetSlot(AIOpportunityAttackMoveSoAColumns& columns, size_t slot) noexcept
{
    columns.store(slot, AIOpportunityAttackMoveStatePayload{});
}

inline void setPhase(AIOpportunityAttackMoveSoAColumns& columns,
                     size_t slot,
                     AIOpportunityAttackMovePhase phase) noexcept
{
    if (columns.phaseAt(slot) == phase)
        return;
    columns.phase[slot] = static_cast<uint8_t>(phase);
    columns.phaseRevision[slot] = nextRevision(columns.phaseRevision[slot]);
}

[[nodiscard]] inline uint32_t allocateOperation(AIOpportunityAttackMoveSoAColumns& columns, size_t slot) noexcept
{
    const uint32_t revision = columns.nextOperationRevision[slot] == 0 ? 1 : columns.nextOperationRevision[slot];
    columns.nextOperationRevision[slot] = nextRevision(revision);
    return revision;
}

[[nodiscard]] inline AIOpportunityAttackMoveCorrelation correlation(
    const AIOpportunityAttackMoveSoAColumns& columns,
    const AIOpportunityAttackMoveStateSoAKernelInput& input,
    size_t slot,
    AIOpportunityAttackMoveOperation operation,
    uint32_t operationRevision) noexcept
{
    return {.subject = input.subjects[slot],
            .stateRequest = columns.requestAt(slot),
            .state = columns.state[slot],
            .phase = columns.phaseAt(slot),
            .operation = operation,
            .sourceOrderRevision = columns.sourceOrderRevision[slot],
            .phaseRevision = columns.phaseRevision[slot],
            .operationRevision = operationRevision};
}

[[nodiscard]] inline PathCorrelation movementCorrelation(const AIOpportunityAttackMovePolicy& policy,
                                                         const AIOpportunityAttackMoveStateSoAKernelInput& input,
                                                         size_t slot) noexcept
{
    return policy.movement == AIOpportunityAttackMoveMovement::MoveTo
               ? detail::moveStateSoACorrelation(input.subjects[slot], input.moveToColumns->load(slot))
               : detail::waypointCorrelation(input.subjects[slot], input.waypointColumns->load(slot));
}

[[nodiscard]] inline bool emitPath(AIOpportunityAttackMoveStateSoAKernelInput const& input,
                                   const AIOpportunityAttackMovePolicy& policy,
                                   size_t slot,
                                   PathRequestKind kind,
                                   AIMoveToStatePayload& move,
                                   AIWaypointPathStatePayload& waypoint) noexcept
{
    if (policy.movement == AIOpportunityAttackMoveMovement::MoveTo)
        return detail::emitMoveStateSoAPathRequest(input.pathRequests[slot],
                                                   input.subjects[slot],
                                                   input.subjectPositions[slot],
                                                   input.parameters[slot],
                                                   move,
                                                   kind,
                                                   !fact(input.groundMovement[slot]));
    return detail::emitWaypointPath(input.pathRequests[slot],
                                    input.subjects[slot],
                                    input.subjectPositions[slot],
                                    input.parameters[slot],
                                    waypoint,
                                    kind,
                                    !fact(input.groundMovement[slot]));
}

[[nodiscard]] inline bool initializeMovement(const AIOpportunityAttackMoveStateSoAKernelInput& input,
                                             const AIOpportunityAttackMovePolicy& policy,
                                             size_t slot,
                                             AIStateRequestId request) noexcept
{
    AIMoveToStatePayload move = input.moveToColumns->load(slot);
    AIWaypointPathStatePayload waypoint = input.waypointColumns->load(slot);
    if (policy.movement == AIOpportunityAttackMoveMovement::MoveTo)
    {
        if (!input.parameters[slot].hasGoalPosition)
            return false;
        move = AIMoveToStatePayload{request};
        move.resolvedGoal = input.parameters[slot].goalPosition;
        move.sourceOrderRevision = input.sourceOrderRevisions[slot];
        move.adjustDestinations = input.parameters[slot].adjustDestinations;
    }
    else
    {
        if (!waypoint.current || waypoint.graphRevision == 0 || (policy.moveAsTeam && !waypoint.team))
            return false;
        waypoint.request = request;
        waypoint.sourceOrderRevision = input.sourceOrderRevisions[slot];
        waypoint.generation = 1;
        waypoint.phase = AIMoveToPhase::WaitingForPath;
        waypoint.path = {};
        waypoint.pathRequestIssued = false;
        waypoint.moveAsTeam = policy.moveAsTeam;
        waypoint.exactPolyline = false;
        waypoint.awaitingTeamProgress = false;
        waypoint.completionPending = false;
    }
    if (!emitPath(input, policy, slot, PathRequestKind::New, move, waypoint))
        return false;
    input.moveToColumns->store(slot, move);
    input.waypointColumns->store(slot, waypoint);
    return true;
}

[[nodiscard]] inline const AIOpportunityAttackMoveQueryFeedback* matchingQueryFeedback(
    const AIOpportunityAttackMoveQueryFeedbackBuffer& buffer,
    const AIOpportunityAttackMoveCorrelation& expected,
    uint64_t confirmedTick) noexcept
{
    const size_t count = buffer.count < buffer.values.size() ? buffer.count : buffer.values.size();
    const AIOpportunityAttackMoveQueryFeedback* selected = nullptr;
    for (size_t index = 0; index < count; ++index)
    {
        const auto& candidate = buffer.values[index];
        if (candidate.correlation == expected && candidate.confirmedTick <= confirmedTick &&
            (!selected || candidate.confirmedTick >= selected->confirmedTick))
            selected = &candidate;
    }
    return selected;
}

[[nodiscard]] inline const AIOpportunityAttackMoveChildFeedback* matchingChildFeedback(
    const AIOpportunityAttackMoveChildFeedbackBuffer& buffer,
    const AIOpportunityAttackMoveCorrelation& expected,
    uint64_t confirmedTick) noexcept
{
    const size_t count = buffer.count < buffer.values.size() ? buffer.count : buffer.values.size();
    const AIOpportunityAttackMoveChildFeedback* selected = nullptr;
    for (size_t index = 0; index < count; ++index)
    {
        const auto& candidate = buffer.values[index];
        if (candidate.correlation == expected && candidate.confirmedTick <= confirmedTick &&
            (!selected || candidate.confirmedTick >= selected->confirmedTick))
            selected = &candidate;
    }
    return selected;
}

[[nodiscard]] inline bool issueQuery(AIOpportunityAttackMoveSoAColumns& columns,
                                     const AIOpportunityAttackMoveStateSoAKernelInput& input,
                                     size_t slot,
                                     AIOpportunityAttackMoveOperation operation) noexcept
{
    if (!available(input.queryCommands[slot]))
        return false;
    const uint32_t revision = allocateOperation(columns, slot);
    const auto expected = correlation(columns, input, slot, operation, revision);
    const auto kind = operation == AIOpportunityAttackMoveOperation::FindCrate
                          ? AIOpportunityAttackMoveQueryCommandKind::FindCrate
                          : AIOpportunityAttackMoveQueryCommandKind::FindMoodTarget;
    if (!input.queryCommands[slot].push(
            {.correlation = expected,
             .kind = kind,
             .confirmedTick = input.confirmedTick}))
        return false;
    columns.scanOperation[slot] = static_cast<uint8_t>(operation);
    columns.queryRevision[slot] = revision;
    columns.queryPending[slot] = 1;
    return true;
}

[[nodiscard]] inline bool pauseAndBeginChild(AIOpportunityAttackMoveSoAColumns& columns,
                                             const AIOpportunityAttackMoveStateSoAKernelInput& input,
                                             const AIOpportunityAttackMovePolicy& policy,
                                             size_t slot,
                                             AIOpportunityAttackMoveOperation child,
                                             ObjectId target,
                                             AIFixedPosition targetPosition,
                                             bool targetPositionValid) noexcept
{
    AIMoveToStatePayload move = input.moveToColumns->load(slot);
    AIWaypointPathStatePayload waypoint = input.waypointColumns->load(slot);
    const bool pathPending = policy.movement == AIOpportunityAttackMoveMovement::MoveTo ? move.pathRequestIssued
                                                                                        : waypoint.pathRequestIssued;
    if ((pathPending && !available(input.pathRequests[slot])) || !available(input.movementCommands[slot]) ||
        !available(input.childCommands[slot]))
        return false;

    const PathCorrelation movement = policy.movement == AIOpportunityAttackMoveMovement::MoveTo
                                         ? detail::moveStateSoACorrelation(input.subjects[slot], move)
                                         : detail::waypointCorrelation(input.subjects[slot], waypoint);
    if (pathPending)
        static_cast<void>(emitPath(input, policy, slot, PathRequestKind::Cancel, move, waypoint));
    static_cast<void>(input.movementCommands[slot].push(
        {.correlation = movement,
         .kind = MovementCommandKind::EndMovement,
         .path = policy.movement == AIOpportunityAttackMoveMovement::MoveTo ? move.path : waypoint.path,
         .clearGoal = false,
         .preserveUltraAccurateFinalPosition = true,
         .confirmedTick = input.confirmedTick}));

    setPhase(columns, slot, AIOpportunityAttackMovePhase::Engaging);
    const uint32_t revision = allocateOperation(columns, slot);
    const auto expected = correlation(columns, input, slot, child, revision);
    static_cast<void>(
        input.childCommands[slot].push({.correlation = expected,
                                        .kind = child == AIOpportunityAttackMoveOperation::Attack
                                                    ? AIOpportunityAttackMoveChildCommandKind::BeginAttack
                                                    : AIOpportunityAttackMoveChildCommandKind::BeginPickUpCrate,
                                        .target = target,
                                        .targetPosition = targetPosition,
                                        .targetPositionValid = targetPositionValid,
                                        .commandSourceIsAI = child == AIOpportunityAttackMoveOperation::Attack,
                                        .confirmedTick = input.confirmedTick}));

    input.moveToColumns->store(slot, move);
    input.waypointColumns->store(slot, waypoint);
    columns.queryPending[slot] = 0;
    columns.queryRevision[slot] = 0;
    columns.childOperation[slot] = static_cast<uint8_t>(child);
    columns.childActive[slot] = 1;
    columns.childRevision[slot] = revision;
    columns.childTarget[slot] = target;
    columns.movementPaused[slot] = 1;
    columns.resumeRequired[slot] = 0;
    columns.resumeScanComplete[slot] = 0;
    columns.movementTerminal[slot] = 0;
    return true;
}

inline void finishTargetlessScan(AIOpportunityAttackMoveSoAColumns& columns, size_t slot) noexcept
{
    columns.queryPending[slot] = 0;
    columns.queryRevision[slot] = 0;
    columns.scanOperation[slot] = static_cast<uint8_t>(AIOpportunityAttackMoveOperation::FindCrate);
    columns.forceRetarget[slot] = 0;
    if (fact(columns.resumeRequired[slot]))
    {
        columns.resumeScanComplete[slot] = 1;
        setPhase(columns, slot, AIOpportunityAttackMovePhase::Resuming);
    }
    else
        setPhase(columns, slot, AIOpportunityAttackMovePhase::Moving);
}

[[nodiscard]] inline AIStateStepResult updateScanning(AIOpportunityAttackMoveSoAColumns& columns,
                                                      const AIOpportunityAttackMoveStateSoAKernelInput& input,
                                                      const AIOpportunityAttackMovePolicy& policy,
                                                      size_t slot) noexcept
{
    if (!fact(columns.movementPaused[slot]))
    {
        if (input.movementResults[slot].kind == AIStateStepKind::Success)
            columns.movementTerminal[slot] = 1;
        else if (input.movementResults[slot].kind == AIStateStepKind::Failure)
            columns.movementTerminal[slot] = 2;
    }
    const auto operation = static_cast<AIOpportunityAttackMoveOperation>(columns.scanOperation[slot]);
    if (!fact(columns.queryPending[slot]))
        return issueQuery(columns, input, slot, operation) ? AIStateStepResult::continueState()
                                                           : AIStateStepResult::blocked();

    const auto expected = correlation(columns, input, slot, operation, columns.queryRevision[slot]);
    const auto* feedback = matchingQueryFeedback(input.queryFeedback[slot], expected, input.confirmedTick);
    if (!feedback || feedback->kind == AIOpportunityAttackMoveQueryFeedbackKind::None)
        return AIStateStepResult::continueState();
    if (feedback->kind == AIOpportunityAttackMoveQueryFeedbackKind::Unsupported)
        return AIStateStepResult::unsupported();
    if (feedback->kind == AIOpportunityAttackMoveQueryFeedbackKind::Target && feedback->target)
    {
        // The query protocol has no excluded-target id. Keep the last Attack
        // child in scalar SoA state and reject that exact mood target locally,
        // so force-retarget never asks GameSession to infer mutable side state.
        const bool repeatsExcludedAttack =
            operation == AIOpportunityAttackMoveOperation::FindMoodTarget && fact(columns.forceRetarget[slot]) &&
            !feedback->commonTeamTarget &&
            static_cast<AIOpportunityAttackMoveOperation>(columns.childOperation[slot]) ==
                AIOpportunityAttackMoveOperation::Attack &&
            columns.childTarget[slot] == feedback->target;
        if (repeatsExcludedAttack)
        {
            finishTargetlessScan(columns, slot);
            return AIStateStepResult::continueState();
        }
        const auto child = operation == AIOpportunityAttackMoveOperation::FindCrate
                               ? AIOpportunityAttackMoveOperation::PickUpCrate
                               : AIOpportunityAttackMoveOperation::Attack;
        return pauseAndBeginChild(columns, input, policy, slot, child,
                                  feedback->target, feedback->targetPosition,
                                  feedback->targetPositionValid)
                   ? AIStateStepResult::continueState()
                   : AIStateStepResult::blocked();
    }
    if (operation == AIOpportunityAttackMoveOperation::FindCrate)
    {
        if (!available(input.queryCommands[slot]))
            return AIStateStepResult::blocked();
        columns.queryPending[slot] = 0;
        columns.queryRevision[slot] = 0;
        return issueQuery(columns, input, slot, AIOpportunityAttackMoveOperation::FindMoodTarget)
                   ? AIStateStepResult::continueState()
                   : AIStateStepResult::blocked();
    }
    finishTargetlessScan(columns, slot);
    return AIStateStepResult::continueState();
}

[[nodiscard]] inline AIStateStepResult updateEngaging(AIOpportunityAttackMoveSoAColumns& columns,
                                                      const AIOpportunityAttackMoveStateSoAKernelInput& input,
                                                      size_t slot) noexcept
{
    const auto operation = static_cast<AIOpportunityAttackMoveOperation>(columns.childOperation[slot]);
    const auto expected = correlation(columns, input, slot, operation, columns.childRevision[slot]);
    const auto* feedback = matchingChildFeedback(input.childFeedback[slot], expected, input.confirmedTick);
    if (!feedback || feedback->kind == AIOpportunityAttackMoveChildFeedbackKind::None ||
        feedback->kind == AIOpportunityAttackMoveChildFeedbackKind::Progress)
        return AIStateStepResult::continueState();
    if (feedback->kind == AIOpportunityAttackMoveChildFeedbackKind::Unsupported)
        return AIStateStepResult::unsupported();

    columns.childActive[slot] = 0;
    // Retain the terminal Attack operation/target as the force-retarget
    // exclusion key. Pick-up children have no combat exclusion semantics.
    if (operation != AIOpportunityAttackMoveOperation::Attack)
    {
        columns.childOperation[slot] = static_cast<uint8_t>(AIOpportunityAttackMoveOperation::None);
        columns.childTarget[slot] = INVALID_OBJECT_ID;
    }
    columns.childRevision[slot] = 0;
    columns.resumeRequired[slot] = 1;
    columns.resumeScanComplete[slot] = 0;
    columns.forceRetarget[slot] = 1;
    columns.scanOperation[slot] = static_cast<uint8_t>(AIOpportunityAttackMoveOperation::FindCrate);
    setPhase(columns, slot, AIOpportunityAttackMovePhase::Resuming);
    return AIStateStepResult::continueState();
}

[[nodiscard]] inline bool forceRepath(AIOpportunityAttackMoveSoAColumns& columns,
                                      const AIOpportunityAttackMoveStateSoAKernelInput& input,
                                      const AIOpportunityAttackMovePolicy& policy,
                                      size_t slot) noexcept
{
    if (!available(input.pathRequests[slot]))
        return false;
    AIMoveToStatePayload move = input.moveToColumns->load(slot);
    AIWaypointPathStatePayload waypoint = input.waypointColumns->load(slot);
    if (policy.movement == AIOpportunityAttackMoveMovement::MoveTo)
    {
        move.generation = nextRevision(move.generation);
        move.phase = AIMoveToPhase::WaitingForPath;
        move.pathRequestIssued = false;
        move.path = {};
    }
    else
    {
        waypoint.generation = nextRevision(waypoint.generation);
        waypoint.phase = AIMoveToPhase::WaitingForPath;
        waypoint.pathRequestIssued = false;
        waypoint.path = {};
    }
    if (!emitPath(input, policy, slot, PathRequestKind::Patch, move, waypoint))
        return false;
    input.moveToColumns->store(slot, move);
    input.waypointColumns->store(slot, waypoint);
    columns.movementPaused[slot] = 0;
    columns.resumeRequired[slot] = 0;
    columns.resumeScanComplete[slot] = 0;
    columns.retryWakeTick[slot] = 0;
    columns.movementTerminal[slot] = 0;
    setPhase(columns, slot, AIOpportunityAttackMovePhase::Scanning);
    return true;
}

[[nodiscard]] inline AIStateStepResult updateResuming(AIOpportunityAttackMoveSoAColumns& columns,
                                                      const AIOpportunityAttackMoveStateSoAKernelInput& input,
                                                      const AIOpportunityAttackMovePolicy& policy,
                                                      size_t slot) noexcept
{
    if (!fact(columns.resumeScanComplete[slot]))
    {
        if (!available(input.queryCommands[slot]))
            return AIStateStepResult::blocked();
        setPhase(columns, slot, AIOpportunityAttackMovePhase::Scanning);
        static_cast<void>(issueQuery(columns, input, slot, AIOpportunityAttackMoveOperation::FindCrate));
        return AIStateStepResult::continueState();
    }
    if (columns.retryWakeTick[slot] != 0 && input.confirmedTick < columns.retryWakeTick[slot])
    {
        if (!available(input.queryCommands[slot]))
            return AIStateStepResult::blocked();
        columns.resumeScanComplete[slot] = 0;
        setPhase(columns, slot, AIOpportunityAttackMovePhase::Scanning);
        static_cast<void>(issueQuery(columns, input, slot, AIOpportunityAttackMoveOperation::FindCrate));
        return AIStateStepResult::continueState();
    }
    return forceRepath(columns, input, policy, slot) ? AIStateStepResult::continueState()
                                                     : AIStateStepResult::blocked();
}

[[nodiscard]] inline AIStateStepResult updateMoving(AIOpportunityAttackMoveSoAColumns& columns,
                                                    const AIOpportunityAttackMoveStateSoAKernelInput& input,
                                                    const AIOpportunityAttackMovePolicy& policy,
                                                    size_t slot) noexcept
{
    AIStateStepResult movement = input.movementResults[slot];
    if (columns.movementTerminal[slot] == 1)
        movement = AIStateStepResult::success();
    else if (columns.movementTerminal[slot] == 2)
        movement = AIStateStepResult::failure();
    if (movement.kind == AIStateStepKind::Blocked || movement.kind == AIStateStepKind::Unsupported ||
        movement.kind == AIStateStepKind::Transition)
        return movement;
    if (movement.kind == AIStateStepKind::Success || movement.kind == AIStateStepKind::Failure)
    {
        if (policy.movement == AIOpportunityAttackMoveMovement::MoveTo && columns.retriesRemaining[slot] != 0 &&
            !withinMoveToRetryDistance(
                input.subjectPositions[slot], input.parameters[slot].goalPosition, input.pathfindCellSizeRaw[slot]))
        {
            --columns.retriesRemaining[slot];
            const uint64_t delay =
                saturatingMultiply(input.ticksPerSecond[slot], AI_OPPORTUNITY_ATTACK_MOVE_RETRY_DELAY_SECONDS);
            columns.retryWakeTick[slot] = saturatingAdd(input.confirmedTick, delay);
            columns.movementPaused[slot] = 1;
            columns.resumeRequired[slot] = 1;
            columns.resumeScanComplete[slot] = 0;
            columns.scanOperation[slot] = static_cast<uint8_t>(AIOpportunityAttackMoveOperation::FindCrate);
            columns.movementTerminal[slot] = 0;
            setPhase(columns, slot, AIOpportunityAttackMovePhase::Scanning);
            return AIStateStepResult::continueState();
        }
        return movement;
    }

    columns.scanOperation[slot] = static_cast<uint8_t>(AIOpportunityAttackMoveOperation::FindCrate);
    setPhase(columns, slot, AIOpportunityAttackMovePhase::Scanning);
    return AIStateStepResult::continueState();
}

} // namespace opportunity_attack_move_detail

[[nodiscard]] inline bool enterOpportunityAttackMoveSoA(
    AIOpportunityAttackMoveSoAColumns& columns, const AIOpportunityAttackMoveStateSoAKernelInput& input) noexcept
{
    using namespace opportunity_attack_move_detail;
    if (!aligned(columns, input))
        return false;
    for (const size_t slot : executionSlotRange(input.executionSlots, input.states.size()))
    {
        if (!scheduled(input, slot))
            continue;
        const auto policy = opportunityAttackMovePolicyFor(input.states[slot]);
        if (!policy.valid)
            continue;
        const AIStateRequestId request = columns.requestAt(slot);
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
        if (!input.subjects[slot] || !request.isValid() || input.sourceOrderRevisions[slot] == 0)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        if (!available(input.pathRequests[slot]))
        {
            input.results[slot] = AIStateStepResult::blocked();
            continue;
        }
        if (!initializeMovement(input, policy, slot, request))
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        AIOpportunityAttackMoveStatePayload orchestration{request};
        orchestration.sourceOrderRevision = input.sourceOrderRevisions[slot];
        orchestration.state = input.states[slot];
        orchestration.active = true;
        orchestration.phase = AIOpportunityAttackMovePhase::Scanning;
        orchestration.phaseRevision = 1;
        orchestration.retriesRemaining =
            policy.movement == AIOpportunityAttackMoveMovement::MoveTo ? AI_OPPORTUNITY_ATTACK_MOVE_RETRIES : 0;
        columns.store(slot, orchestration);
        input.results[slot] = AIStateStepResult::continueState();
    }
    return true;
}

[[nodiscard]] inline bool updateOpportunityAttackMoveSoA(
    AIOpportunityAttackMoveSoAColumns& columns, const AIOpportunityAttackMoveStateSoAKernelInput& input) noexcept
{
    using namespace opportunity_attack_move_detail;
    if (!aligned(columns, input))
        return false;
    for (const size_t slot : executionSlotRange(input.executionSlots, input.states.size()))
    {
        if (!scheduled(input, slot) || !fact(columns.active[slot]) || columns.state[slot] != input.states[slot])
            continue;
        const auto policy = opportunityAttackMovePolicyFor(columns.state[slot]);
        if (!policy.valid)
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
        switch (columns.phaseAt(slot))
        {
        case AIOpportunityAttackMovePhase::Scanning:
            input.results[slot] = updateScanning(columns, input, policy, slot);
            break;
        case AIOpportunityAttackMovePhase::Moving:
            input.results[slot] = updateMoving(columns, input, policy, slot);
            break;
        case AIOpportunityAttackMovePhase::Engaging:
            input.results[slot] = updateEngaging(columns, input, slot);
            break;
        case AIOpportunityAttackMovePhase::Resuming:
            input.results[slot] = updateResuming(columns, input, policy, slot);
            break;
        case AIOpportunityAttackMovePhase::Inactive:
            input.results[slot] = AIStateStepResult::unsupported();
            break;
        }
    }
    return true;
}

[[nodiscard]] inline bool canExitOpportunityAttackMoveSoA(
    const AIOpportunityAttackMoveSoAColumns& columns, const AIOpportunityAttackMoveStateSoAKernelInput& input) noexcept
{
    using namespace opportunity_attack_move_detail;
    if (!aligned(columns, input))
        return false;
    for (const size_t slot : executionSlotRange(input.executionSlots, input.states.size()))
    {
        if (!scheduled(input, slot) || !fact(columns.active[slot]))
            continue;
        const auto policy = opportunityAttackMovePolicyFor(columns.state[slot]);
        if (!policy.valid)
            continue;
        const bool pathPending = policy.movement == AIOpportunityAttackMoveMovement::MoveTo
                                     ? input.moveToColumns->load(slot).pathRequestIssued
                                     : input.waypointColumns->load(slot).pathRequestIssued;
        if ((pathPending && !available(input.pathRequests[slot])) || !available(input.movementCommands[slot]) ||
            (fact(columns.queryPending[slot]) && !available(input.queryCommands[slot])) ||
            (fact(columns.childActive[slot]) && !available(input.childCommands[slot])) ||
            (policy.movement == AIOpportunityAttackMoveMovement::Waypoint &&
             input.waypointColumns->load(slot).completionPending && !available(input.completions[slot])))
            return false;
    }
    return true;
}

[[nodiscard]] inline bool exitOpportunityAttackMoveSoA(AIOpportunityAttackMoveSoAColumns& columns,
                                                       const AIOpportunityAttackMoveStateSoAKernelInput& input) noexcept
{
    using namespace opportunity_attack_move_detail;
    if (!canExitOpportunityAttackMoveSoA(columns, input))
        return false;
    for (const size_t slot : executionSlotRange(input.executionSlots, input.states.size()))
    {
        if (!scheduled(input, slot) || !fact(columns.active[slot]))
            continue;
        const auto policy = opportunityAttackMovePolicyFor(columns.state[slot]);
        if (!policy.valid)
            continue;
        AIMoveToStatePayload move = input.moveToColumns->load(slot);
        AIWaypointPathStatePayload waypoint = input.waypointColumns->load(slot);
        const PathCorrelation movement = policy.movement == AIOpportunityAttackMoveMovement::MoveTo
                                             ? detail::moveStateSoACorrelation(input.subjects[slot], move)
                                             : detail::waypointCorrelation(input.subjects[slot], waypoint);
        const bool pathPending = policy.movement == AIOpportunityAttackMoveMovement::MoveTo
                                     ? move.pathRequestIssued
                                     : waypoint.pathRequestIssued;
        if (pathPending)
            static_cast<void>(emitPath(input, policy, slot, PathRequestKind::Cancel, move, waypoint));
        static_cast<void>(input.movementCommands[slot].push(
            {.correlation = movement,
             .kind = MovementCommandKind::EndMovement,
             .path = policy.movement == AIOpportunityAttackMoveMovement::MoveTo ? move.path : waypoint.path,
             .clearGoal = policy.movement == AIOpportunityAttackMoveMovement::MoveTo ? move.adjustDestinations
                                                                                     : waypoint.adjustDestinations,
             .preserveUltraAccurateFinalPosition = true,
             .confirmedTick = input.confirmedTick}));
        if (fact(columns.queryPending[slot]))
        {
            const auto operation = static_cast<AIOpportunityAttackMoveOperation>(columns.scanOperation[slot]);
            static_cast<void>(input.queryCommands[slot].push(
                {.correlation = correlation(columns, input, slot, operation, columns.queryRevision[slot]),
                 .kind = AIOpportunityAttackMoveQueryCommandKind::Cancel,
                 .confirmedTick = input.confirmedTick}));
        }
        if (fact(columns.childActive[slot]))
        {
            const auto operation = static_cast<AIOpportunityAttackMoveOperation>(columns.childOperation[slot]);
            static_cast<void>(input.childCommands[slot].push(
                {.correlation = correlation(columns, input, slot, operation, columns.childRevision[slot]),
                 .kind = AIOpportunityAttackMoveChildCommandKind::Cancel,
                 .target = columns.childTarget[slot],
                 .confirmedTick = input.confirmedTick}));
        }
        if (policy.movement == AIOpportunityAttackMoveMovement::Waypoint && waypoint.completionPending)
        {
            static_cast<void>(input.completions[slot].push({.subject = input.subjects[slot],
                                                            .stateRequest = waypoint.request,
                                                            .terminal = waypoint.completionTerminal,
                                                            .confirmedTick = input.confirmedTick}));
            waypoint.completionPending = false;
        }
        input.moveToColumns->store(slot, move);
        input.waypointColumns->store(slot, waypoint);
        resetSlot(columns, slot);
    }
    return true;
}

} // namespace engine::ai
