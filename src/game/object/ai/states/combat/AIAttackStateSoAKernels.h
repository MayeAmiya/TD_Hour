#pragma once

#include <cstddef>
#include <cstdint>

#include "core/container/container_types.h"
#include "game/object/ai/contracts/AIAttackServices.h"
#include "game/object/ai/runtime/AIStateStep.h"

namespace engine::ai
{

struct AIAttackStateSoAKernelInput final
{
    uint64_t confirmedTick = 0;
    int64_t contactExtraDistanceRaw = 0;
    container::Span<const uint8_t> scheduled{};
    AIExecutionSlotRange executionSlots{};
    container::Span<const AIStateId> activeStates;
    container::Span<const ObjectId> subjects;
    container::Span<const uint8_t> effectivelyDead;
    container::Span<const uint8_t> constructionComplete;
    container::Span<const uint8_t> hasAmmo;
    container::Span<const uint8_t> attackMoodAllowed;
    container::Span<const uint8_t> exitConditionSatisfied;
    container::Span<const uint64_t> sourceOrderRevisions;
    container::Span<const uint64_t> weaponRevisions;
    container::Span<const ObjectId> goalObjects;
    container::Span<const AIFixedPosition> goalPositions;
    container::Span<const uint8_t> hasGoalPositions;
    container::Span<const AIFixedPosition> subjectPositions;
    container::Span<const uint8_t> mobile;
    // A current locomotor does not imply ground navigation. A selected
    // AIR-surface locomotor uses the original engine's QuickPath branch for
    // pursuit instead of entering the terrain pathfinder queue.
    container::Span<const uint8_t> groundMovement;
    container::Span<const uint8_t> projectile;
    container::Span<const AIAttackFeedbackBuffer> combatFeedback;
    container::Span<const PathFeedback> pathFeedback;
    container::Span<const MovementFeedback> movementFeedback;
    container::Span<AIAttackCommandBuffer> combatCommands;
    container::Span<PathRequestBuffer> pathRequests;
    container::Span<MovementCommandBuffer> movementCommands;
    container::Span<AIStateStepResult> results;
};

// Allocation-free scratch for a wrapper-owned Attack child. The authoritative
// Attack payload remains AIAttackSoAColumns; these columns only project a
// synthetic child state/goal into the existing kernel for one executor call.
// In particular, wrapper order parameters and destination columns are never
// borrowed as mutable child state.
struct AIAttackChildSoAScratch final
{
    container::Span<uint8_t> scheduled;
    container::Span<AIStateId> states;
    container::Span<ObjectId> goalObjects;
    container::Span<AIFixedPosition> goalPositions;
    container::Span<uint8_t> hasGoalPositions;
    container::Span<AIStateStepResult> results;

    [[nodiscard]] bool aligned(size_t count) const noexcept
    {
        return scheduled.size() == count && states.size() == count && goalObjects.size() == count &&
               goalPositions.size() == count && hasGoalPositions.size() == count && results.size() == count;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return scheduled.empty() && states.empty() && goalObjects.empty() && goalPositions.empty() &&
               hasGoalPositions.empty() && results.empty();
    }

    [[nodiscard]] AIAttackStateSoAKernelInput bind(
        const AIAttackStateSoAKernelInput& base,
        AIExecutionSlotRange execution = {}) const noexcept
    {
        AIAttackStateSoAKernelInput child = base;
        child.executionSlots = execution.specified() ? execution : base.executionSlots;
        child.scheduled = scheduled;
        child.activeStates = states;
        child.goalObjects = goalObjects;
        child.goalPositions = goalPositions;
        child.hasGoalPositions = hasGoalPositions;
        child.results = results;
        return child;
    }

};

namespace attack_detail
{

[[nodiscard]] constexpr bool fact(uint8_t value) noexcept
{
    return value != 0;
}

[[nodiscard]] inline bool scheduled(const AIAttackStateSoAKernelInput& input, size_t slot) noexcept
{
    return input.scheduled.empty() || input.scheduled[slot] != 0;
}

[[nodiscard]] bool hasAlignedInputSpans(size_t count,
                                        const AIAttackStateSoAKernelInput& input) noexcept;

[[nodiscard]] bool hasAlignedSpans(const AIAttackSoAColumns& columns,
                                   const AIAttackStateSoAKernelInput& input) noexcept;

[[nodiscard]] constexpr uint32_t nextRevision(uint32_t revision) noexcept
{
    ++revision;
    return revision == 0 ? 1 : revision;
}

[[nodiscard]] inline AIFixedPosition targetPosition(const AIAttackSoAColumns& columns, size_t slot) noexcept
{
    return {columns.targetXRaw[slot], columns.targetYRaw[slot], columns.targetZRaw[slot]};
}

inline void setTargetPosition(AIAttackSoAColumns& columns, size_t slot, const AIFixedPosition& position) noexcept
{
    columns.targetXRaw[slot] = position.xRaw;
    columns.targetYRaw[slot] = position.yRaw;
    columns.targetZRaw[slot] = position.zRaw;
}

[[nodiscard]] inline AIAttackCorrelation combatCorrelation(const AIAttackSoAColumns& columns,
                                                           const AIAttackStateSoAKernelInput& input,
                                                           size_t slot) noexcept
{
    return {
        .subject = input.subjects[slot],
        .stateRequest = {columns.requestTick[slot], columns.requestSequence[slot]},
        .state = input.activeStates[slot],
        .phase = columns.phase[slot],
        .weaponRevision = columns.weaponRevision[slot],
        .phaseRevision = columns.phaseRevision[slot],
    };
}

[[nodiscard]] inline PathCorrelation pathCorrelation(const AIAttackSoAColumns& columns,
                                                     const AIAttackStateSoAKernelInput& input,
                                                     size_t slot) noexcept
{
    return {
        .subject = input.subjects[slot],
        .stateRequest = {columns.requestTick[slot], columns.requestSequence[slot]},
        .generation = columns.pathGeneration[slot],
        .sourceOrderRevision = columns.sourceOrderRevision[slot],
    };
}

[[nodiscard]] inline const AIAttackFeedback* relevantFeedback(const AIAttackFeedbackBuffer& buffer,
                                                              const AIAttackCorrelation& expected,
                                                              ObjectId expectedTarget,
                                                              bool attacksObject,
                                                              uint64_t confirmedTick) noexcept
{
    const size_t count = buffer.count < buffer.values.size() ? buffer.count : buffer.values.size();
    const AIAttackFeedback* relevant = nullptr;
    for (size_t index = 0; index < count; ++index)
    {
        const AIAttackFeedback& candidate = buffer.values[index];
        if (candidate.correlation == expected && (!attacksObject || candidate.target == expectedTarget) &&
            candidate.confirmedTick <= confirmedTick &&
            (!relevant || candidate.confirmedTick >= relevant->confirmedTick))
        {
            relevant = &candidate;
        }
    }
    return relevant;
}

template <typename Buffer>
[[nodiscard]] inline bool hasCapacity(const Buffer& buffer, size_t additional) noexcept
{
    return buffer.count <= buffer.values.size() && additional <= buffer.values.size() - buffer.count;
}

struct OutputNeeds final
{
    size_t combat = 0;
    size_t path = 0;
    size_t movement = 0;
};

[[nodiscard]] inline OutputNeeds cleanupNeeds(const AIAttackSoAColumns& columns, size_t slot) noexcept
{
    const bool pathActive =
        fact(columns.pathRequestIssued[slot]) || fact(columns.movementActive[slot]) || columns.pathHandle[slot] != 0;
    return {
        .combat = static_cast<size_t>(fact(columns.aimingActive[slot])) +
                  static_cast<size_t>(fact(columns.firingActive[slot])),
        .path = static_cast<size_t>(fact(columns.pathRequestIssued[slot])),
        .movement = static_cast<size_t>(pathActive),
    };
}

[[nodiscard]] inline bool hasCapacity(const AIAttackStateSoAKernelInput& input, size_t slot, OutputNeeds needs) noexcept
{
    return hasCapacity(input.combatCommands[slot], needs.combat) && hasCapacity(input.pathRequests[slot], needs.path) &&
           hasCapacity(input.movementCommands[slot], needs.movement);
}

inline void emitCombat(const AIAttackStateSoAKernelInput& input,
                       size_t slot,
                       const AIAttackCorrelation& correlation,
                       AIAttackCommandKind kind,
                       const AIAttackPolicy& policy,
                       ObjectId target,
                       const AIFixedPosition& position) noexcept
{
    static_cast<void>(input.combatCommands[slot].push({
        .correlation = correlation,
        .kind = kind,
        .target = target,
        .targetPosition = position,
        .attacksObject = policy.attacksObject,
        .forceAttack = policy.forceAttack,
        .confirmedTick = input.confirmedTick,
    }));
}

inline void emitPath(const AIAttackSoAColumns& columns,
                     const AIAttackStateSoAKernelInput& input,
                     size_t slot,
                     const AIAttackPolicy& policy,
                     PathRequestKind kind,
                     AIAttackPhase phase) noexcept
{
    const bool contact = fact(columns.contactWeapon[slot]);
    const bool quickPath = !fact(input.groundMovement[slot]);
    // AIUpdateInterface::doPathfind() clears m_isApproachPath for an
    // AIR-surface locomotor before falling through to computePath(). Match
    // that transition at the value boundary: DirectLine deliberately accepts
    // only ordinary New/Patch requests.
    const PathRequestKind requestKind =
        quickPath && kind == PathRequestKind::Approach
            ? PathRequestKind::New
            : kind;
    static_cast<void>(input.pathRequests[slot].push({
        .correlation = pathCorrelation(columns, input, slot),
        .start = input.subjectPositions[slot],
        .originalGoal = targetPosition(columns, slot),
        .adjustDestinations = phase == AIAttackPhase::Chase || !contact,
        .ignoredObstacle =
            phase == AIAttackPhase::Approach && contact ? columns.trackedTarget[slot] : INVALID_OBJECT_ID,
        .surfaceMask = 0,
        .arrivalRadiusRaw = columns.arrivalRadiusRaw[slot],
        .minimumArrivalRadiusRaw =
            columns.minimumArrivalRadiusRaw[slot],
        .kind = requestKind,
        .currentPath = PathHandle{columns.pathHandle[slot]},
        .safePathRepulsor = INVALID_OBJECT_ID,
        .traversalMode = quickPath ? AIPathTraversalMode::DirectLine
                                   : AIPathTraversalMode::Navmesh,
        .waypointStart = {},
        .waypointGraphRevision = 0,
        .waypointHopLimit = 0,
        .polylineOffset = {},
        .extraDistanceRaw = phase == AIAttackPhase::Approach && contact ? input.contactExtraDistanceRaw : 0,
        .pathThroughUnits = false,
        .preciseFinalZ = fact(input.projectile[slot]),
        .attackTarget = policy.attacksObject
            ? columns.trackedTarget[slot] : INVALID_OBJECT_ID,
        .attackContactWeapon = contact,
    }));
}

inline void emitCleanup(AIAttackSoAColumns& columns,
                        const AIAttackStateSoAKernelInput& input,
                        size_t slot,
                        const AIAttackPolicy& policy) noexcept
{
    const AIAttackCorrelation combat = combatCorrelation(columns, input, slot);
    const AIFixedPosition position = targetPosition(columns, slot);
    if (fact(columns.aimingActive[slot]))
        emitCombat(input, slot, combat, AIAttackCommandKind::EndAim, policy, columns.trackedTarget[slot], position);
    if (fact(columns.firingActive[slot]))
        emitCombat(input, slot, combat, AIAttackCommandKind::EndFire, policy, columns.trackedTarget[slot], position);
    if (fact(columns.pathRequestIssued[slot]))
    {
        emitPath(columns, input, slot, policy,
                 PathRequestKind::Cancel, columns.phase[slot]);
    }
    if (fact(columns.pathRequestIssued[slot]) || fact(columns.movementActive[slot]) || columns.pathHandle[slot] != 0)
    {
        static_cast<void>(input.movementCommands[slot].push({
            .correlation = pathCorrelation(columns, input, slot),
            .kind = MovementCommandKind::EndMovement,
            .path = PathHandle{columns.pathHandle[slot]},
            .clearGoal = false,
            .preserveUltraAccurateFinalPosition = true,
            .allowPathThroughUnits = false,
            .confirmedTick = input.confirmedTick,
        }));
    }
    columns.pathRequestIssued[slot] = 0;
    columns.movementActive[slot] = 0;
    columns.pathHandle[slot] = 0;
    columns.aimingActive[slot] = 0;
    columns.firingActive[slot] = 0;
    columns.fireCommandIssued[slot] = 0;
}

inline void setPhase(AIAttackSoAColumns& columns, size_t slot, AIAttackPhase phase) noexcept
{
    columns.phase[slot] = phase;
    columns.phaseRevision[slot] = nextRevision(columns.phaseRevision[slot]);
}

[[nodiscard]] inline bool transitionToAim(AIAttackSoAColumns& columns,
                                          const AIAttackStateSoAKernelInput& input,
                                          size_t slot,
                                          const AIAttackPolicy& policy) noexcept
{
    OutputNeeds needs = cleanupNeeds(columns, slot);
    ++needs.combat;
    if (!hasCapacity(input, slot, needs))
        return false;
    emitCleanup(columns, input, slot, policy);
    setPhase(columns, slot, AIAttackPhase::Aim);
    emitCombat(input,
               slot,
               combatCorrelation(columns, input, slot),
               AIAttackCommandKind::BeginAim,
               policy,
               columns.trackedTarget[slot],
               targetPosition(columns, slot));
    columns.aimingActive[slot] = 1;
    return true;
}

[[nodiscard]] inline bool transitionToFire(AIAttackSoAColumns& columns,
                                           const AIAttackStateSoAKernelInput& input,
                                           size_t slot,
                                           const AIAttackPolicy& policy) noexcept
{
    OutputNeeds needs = cleanupNeeds(columns, slot);
    ++needs.combat;
    if (!hasCapacity(input, slot, needs))
        return false;
    emitCleanup(columns, input, slot, policy);
    setPhase(columns, slot, AIAttackPhase::Fire);
    emitCombat(input,
               slot,
               combatCorrelation(columns, input, slot),
               AIAttackCommandKind::BeginFire,
               policy,
               columns.trackedTarget[slot],
               targetPosition(columns, slot));
    columns.firingActive[slot] = 1;
    return true;
}

[[nodiscard]] inline bool transitionToPath(AIAttackSoAColumns& columns,
                                           const AIAttackStateSoAKernelInput& input,
                                           size_t slot,
                                           const AIAttackPolicy& policy,
                                           AIAttackPhase phase,
                                           PathRequestKind kind) noexcept
{
    OutputNeeds needs = cleanupNeeds(columns, slot);
    ++needs.path;
    if (!hasCapacity(input, slot, needs))
        return false;
    emitCleanup(columns, input, slot, policy);
    setPhase(columns, slot, phase);
    columns.pathGeneration[slot] = nextRevision(columns.pathGeneration[slot]);
    emitPath(columns, input, slot, policy, kind, phase);
    columns.pathRequestIssued[slot] = 1;
    return true;
}

[[nodiscard]] inline bool targetFailed(const AIAttackFeedback& feedback, const AIAttackPolicy& policy) noexcept
{
    return policy.attacksObject && !feedback.targetValid;
}

} // namespace attack_detail

[[nodiscard]] bool enterAttackStateSoA(AIAttackSoAColumns columns,
                                       const AIAttackStateSoAKernelInput& input) noexcept;

[[nodiscard]] inline bool updateAttackStateSoA(AIAttackSoAColumns columns,
                                               const AIAttackStateSoAKernelInput& input) noexcept
{
    using namespace attack_detail;
    if (!hasAlignedSpans(columns, input))
        return false;

    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot))
            continue;
        const AIAttackPolicy policy = attackPolicyFor(input.activeStates[slot]);
        if (!policy.valid)
            continue;
        if (fact(input.effectivelyDead[slot]))
        {
            input.results[slot] = AIStateStepResult::transitionTo(AIStateId::Dead);
            continue;
        }
        if (!fact(input.attackMoodAllowed[slot]) || fact(input.exitConditionSatisfied[slot]))
        {
            input.results[slot] = AIStateStepResult::success();
            continue;
        }
        if (!fact(input.hasAmmo[slot]) && !fact(input.projectile[slot]))
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        if (columns.phase[slot] == AIAttackPhase::Inactive || !combatCorrelation(columns, input, slot).isValid())
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        // A selected-weapon change is a correlated re-entry into Aim. Cleanup
        // and BeginAim are one slot transaction; all old feedback is stale.
        if (input.weaponRevisions[slot] != columns.weaponRevision[slot])
        {
            if (input.weaponRevisions[slot] == 0)
            {
                input.results[slot] = AIStateStepResult::failure();
                continue;
            }
            OutputNeeds needs = cleanupNeeds(columns, slot);
            ++needs.combat;
            if (!hasCapacity(input, slot, needs))
            {
                input.results[slot] = AIStateStepResult::unsupported();
                continue;
            }
            emitCleanup(columns, input, slot, policy);
            columns.weaponRevision[slot] = input.weaponRevisions[slot];
            setPhase(columns, slot, AIAttackPhase::Aim);
            emitCombat(input,
                       slot,
                       combatCorrelation(columns, input, slot),
                       AIAttackCommandKind::BeginAim,
                       policy,
                       columns.trackedTarget[slot],
                       targetPosition(columns, slot));
            columns.aimingActive[slot] = 1;
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }

        const AIAttackCorrelation combat = combatCorrelation(columns, input, slot);
        const AIAttackFeedback* feedback = relevantFeedback(
            input.combatFeedback[slot], combat, columns.trackedTarget[slot], policy.attacksObject, input.confirmedTick);

        if (columns.phase[slot] == AIAttackPhase::Aim)
        {
            if (!feedback)
            {
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
            if (feedback->kind == AIAttackFeedbackKind::Unsupported)
            {
                input.results[slot] = AIStateStepResult::unsupported();
                continue;
            }
            if (feedback->kind != AIAttackFeedbackKind::Snapshot)
            {
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
            if (feedback->shotLimitReached)
            {
                input.results[slot] = AIStateStepResult::success();
                continue;
            }
            if (policy.attacksObject && feedback->targetEffectivelyDead)
            {
                input.results[slot] = AIStateStepResult::success();
                continue;
            }
            if (targetFailed(*feedback, policy) || !feedback->hasWeapon || !feedback->canPossiblyAttack)
            {
                input.results[slot] = AIStateStepResult::failure();
                continue;
            }
            if (policy.attacksObject)
            {
                columns.trackedTarget[slot] = feedback->target;
                setTargetPosition(columns, slot, feedback->targetPosition);
            }
            columns.contactWeapon[slot] = feedback->contactWeapon ? uint8_t{1} : uint8_t{0};
            columns.arrivalRadiusRaw[slot] = feedback->attackArrivalRadiusRaw;
            columns.minimumArrivalRadiusRaw[slot] =
                feedback->attackMinimumArrivalRadiusRaw;
            if (!feedback->inRange || feedback->viewBlocked || feedback->wantToSquishTarget)
            {
                if (!fact(input.mobile[slot]) || (policy.attacksObject && !feedback->chaseAllowed))
                {
                    input.results[slot] = AIStateStepResult::failure();
                    continue;
                }
                const AIAttackPhase next =
                    policy.attacksObject && feedback->canPursue ? AIAttackPhase::Chase : AIAttackPhase::Approach;
                input.results[slot] =
                    transitionToPath(columns,
                                     input,
                                     slot,
                                     policy,
                                     next,
                                     next == AIAttackPhase::Approach ? PathRequestKind::Approach : PathRequestKind::New)
                        ? AIStateStepResult::continueState()
                        : AIStateStepResult::unsupported();
                continue;
            }
            if (feedback->aimReady && !feedback->aimTemporarilyPrevented)
            {
                input.results[slot] = transitionToFire(columns, input, slot, policy)
                                          ? AIStateStepResult::continueState()
                                          : AIStateStepResult::unsupported();
                continue;
            }
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }

        if (columns.phase[slot] == AIAttackPhase::Fire)
        {
            if (!feedback)
            {
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
            if (feedback->kind == AIAttackFeedbackKind::Unsupported)
            {
                input.results[slot] = AIStateStepResult::unsupported();
                continue;
            }
            if (feedback->kind == AIAttackFeedbackKind::FireCompleted && fact(columns.fireCommandIssued[slot]))
            {
                if (feedback->shotLimitReached)
                {
                    input.results[slot] = AIStateStepResult::success();
                    continue;
                }
                if (policy.attacksObject && feedback->targetEffectivelyDead)
                {
                    input.results[slot] = AIStateStepResult::success();
                    continue;
                }
                if (targetFailed(*feedback, policy) || !feedback->hasWeapon ||
                    !feedback->attackAllowed)
                {
                    input.results[slot] = AIStateStepResult::failure();
                    continue;
                }

                // RefCode's turret-owned FIRE succeeds into AIM in the same
                // state-machine update. On the next logic frame an already
                // aligned turret may immediately enter FIRE again. Our
                // detached Combat feedback arrives one frame later, so an
                // unconditional FireCompleted -> BeginAim handoff inserted an
                // extra empty frame into every shot cycle. That stretched a
                // 40 ms continuous weapon to three ticks and let TurretAI's
                // authored three-frame sweep window expire before the next
                // shot. Collapse only that transport latency when the
                // completed-shot snapshot still proves the Aim state would
                // succeed; all other cases re-enter Aim normally.
                const bool aimStillComplete = feedback->inRange &&
                    !feedback->viewBlocked &&
                    !feedback->wantToSquishTarget && feedback->aimReady &&
                    !feedback->aimTemporarilyPrevented;
                const bool transitioned = aimStillComplete
                    ? transitionToFire(columns, input, slot, policy)
                    : transitionToAim(columns, input, slot, policy);
                input.results[slot] = transitioned
                    ? AIStateStepResult::continueState()
                    : AIStateStepResult::unsupported();
                continue;
            }
            if (feedback->kind != AIAttackFeedbackKind::Snapshot)
            {
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
            if (feedback->shotLimitReached)
            {
                input.results[slot] = AIStateStepResult::success();
                continue;
            }
            if (policy.attacksObject && feedback->targetEffectivelyDead)
            {
                input.results[slot] = AIStateStepResult::success();
                continue;
            }
            if (targetFailed(*feedback, policy) || !feedback->hasWeapon)
            {
                input.results[slot] = AIStateStepResult::failure();
                continue;
            }
            if (!feedback->attackAllowed)
            {
                input.results[slot] = AIStateStepResult::failure();
                continue;
            }
            if (feedback->weaponPreAttack || fact(columns.fireCommandIssued[slot]))
            {
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
            // A direct post-shot Fire re-entry is based on the previous
            // completed-shot snapshot. Revalidate the new confirmed target
            // facts before granting another Fire command; a moving target or
            // newly blocked line returns through Aim, whose normal path branch
            // owns chase/approach admission. Preserve an already-armed
            // preattack above: its existing Fire owner must mature or cancel
            // it at the normal Combat boundary.
            if (!feedback->inRange || feedback->viewBlocked ||
                feedback->wantToSquishTarget || !feedback->aimReady ||
                feedback->aimTemporarilyPrevented)
            {
                input.results[slot] = transitionToAim(columns, input, slot, policy)
                    ? AIStateStepResult::continueState()
                    : AIStateStepResult::unsupported();
                continue;
            }
            if (!feedback->weaponReady || !feedback->weaponSlotAllowed)
            {
                input.results[slot] = transitionToAim(columns, input, slot, policy) ? AIStateStepResult::continueState()
                                                                                    : AIStateStepResult::unsupported();
                continue;
            }
            if (!hasCapacity(input.combatCommands[slot], 1))
            {
                input.results[slot] = AIStateStepResult::unsupported();
                continue;
            }
            emitCombat(input,
                       slot,
                       combat,
                       AIAttackCommandKind::Fire,
                       policy,
                       columns.trackedTarget[slot],
                       targetPosition(columns, slot));
            columns.fireCommandIssued[slot] = 1;
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }

        // Chase and Approach consume path/movement feedback independently of
        // combat snapshots, but a matching in-range snapshot wins immediately.
        if (feedback && feedback->kind == AIAttackFeedbackKind::Unsupported)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        if (feedback && feedback->kind == AIAttackFeedbackKind::Snapshot)
        {
            if (feedback->shotLimitReached)
            {
                input.results[slot] = AIStateStepResult::success();
                continue;
            }
            if (policy.attacksObject && feedback->targetEffectivelyDead)
            {
                input.results[slot] = AIStateStepResult::success();
                continue;
            }
            if (targetFailed(*feedback, policy) || !feedback->hasWeapon || !feedback->canPossiblyAttack)
            {
                input.results[slot] = AIStateStepResult::failure();
                continue;
            }
            if (policy.attacksObject)
            {
                columns.trackedTarget[slot] = feedback->target;
                setTargetPosition(columns, slot, feedback->targetPosition);
            }
            columns.contactWeapon[slot] = feedback->contactWeapon ? uint8_t{1} : uint8_t{0};
            columns.arrivalRadiusRaw[slot] = feedback->attackArrivalRadiusRaw;
            columns.minimumArrivalRadiusRaw[slot] =
                feedback->attackMinimumArrivalRadiusRaw;
            if (feedback->inRange && !feedback->viewBlocked)
            {
                input.results[slot] = transitionToAim(columns, input, slot, policy) ? AIStateStepResult::continueState()
                                                                                    : AIStateStepResult::unsupported();
                continue;
            }
        }

        const PathCorrelation expectedPath = pathCorrelation(columns, input, slot);
        if (fact(columns.pathRequestIssued[slot]) && input.pathFeedback[slot].correlation == expectedPath)
        {
            const PathFeedback& path = input.pathFeedback[slot];
            if (path.status == PathFeedbackStatus::Ready)
            {
                if (!path.path || !hasCapacity(input.movementCommands[slot], 1))
                {
                    input.results[slot] = AIStateStepResult::unsupported();
                    continue;
                }
                static_cast<void>(input.movementCommands[slot].push({
                    .correlation = expectedPath,
                    .kind = MovementCommandKind::InstallPath,
                    .path = path.path,
                    .ignoredObstacle =
                        columns.phase[slot] == AIAttackPhase::Approach &&
                                fact(columns.contactWeapon[slot])
                            ? columns.trackedTarget[slot]
                            : INVALID_OBJECT_ID,
                    .extraDistanceRaw =
                        columns.phase[slot] == AIAttackPhase::Approach &&
                                fact(columns.contactWeapon[slot])
                            ? input.contactExtraDistanceRaw
                            : 0,
                    .clearGoal = false,
                    .preserveUltraAccurateFinalPosition = false,
                    .allowPathThroughUnits = false,
                    .confirmedTick = input.confirmedTick,
                }));
                columns.pathHandle[slot] = path.path.value;
                columns.pathRequestIssued[slot] = 0;
                columns.movementActive[slot] = 1;
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
            if (path.status == PathFeedbackStatus::NoPath || path.status == PathFeedbackStatus::Cancelled)
            {
                if (columns.phase[slot] == AIAttackPhase::Chase)
                {
                    input.results[slot] =
                        transitionToPath(
                            columns, input, slot, policy, AIAttackPhase::Approach, PathRequestKind::Approach)
                            ? AIStateStepResult::continueState()
                            : AIStateStepResult::unsupported();
                }
                else
                {
                    input.results[slot] = AIStateStepResult::failure();
                }
                continue;
            }
            if (path.status == PathFeedbackStatus::Unsupported)
            {
                input.results[slot] = AIStateStepResult::unsupported();
                continue;
            }
        }

        if (fact(columns.movementActive[slot]) && input.movementFeedback[slot].correlation == expectedPath)
        {
            const MovementFeedbackStatus status = input.movementFeedback[slot].status;
            if (status == MovementFeedbackStatus::Completed)
            {
                if (columns.phase[slot] == AIAttackPhase::Chase)
                {
                    input.results[slot] =
                        transitionToPath(
                            columns, input, slot, policy, AIAttackPhase::Approach, PathRequestKind::Approach)
                            ? AIStateStepResult::continueState()
                            : AIStateStepResult::unsupported();
                }
                else if (policy.follow && feedback && feedback->kind == AIAttackFeedbackKind::Snapshot &&
                         feedback->targetMobile)
                {
                    input.results[slot] =
                        transitionToPath(columns, input, slot, policy, AIAttackPhase::Approach, PathRequestKind::Patch)
                            ? AIStateStepResult::continueState()
                            : AIStateStepResult::unsupported();
                }
                else
                {
                    input.results[slot] = transitionToAim(columns, input, slot, policy)
                                              ? AIStateStepResult::continueState()
                                              : AIStateStepResult::unsupported();
                }
                continue;
            }
            if (status == MovementFeedbackStatus::Blocked || status == MovementFeedbackStatus::Stuck)
            {
                const bool leavingChase = columns.phase[slot] == AIAttackPhase::Chase;
                input.results[slot] =
                    transitionToPath(columns,
                                     input,
                                     slot,
                                     policy,
                                     AIAttackPhase::Approach,
                                     leavingChase ? PathRequestKind::Approach : PathRequestKind::Patch)
                        ? AIStateStepResult::continueState()
                        : AIStateStepResult::unsupported();
                continue;
            }
            if (status == MovementFeedbackStatus::Cancelled)
            {
                input.results[slot] = AIStateStepResult::failure();
                continue;
            }
            if (status == MovementFeedbackStatus::Unsupported)
            {
                input.results[slot] = AIStateStepResult::unsupported();
                continue;
            }
        }
        input.results[slot] = AIStateStepResult::continueState();
    }
    return true;
}

[[nodiscard]] bool canExitAttackStateSoA(const AIAttackSoAColumns& columns,
                                         const AIAttackStateSoAKernelInput& input) noexcept;

// Exit is one transaction across every scheduled slot. No command, path
// cancellation, movement stop, or column reset occurs unless all sinks can
// accept their complete cleanup set.
[[nodiscard]] bool exitAttackStateSoA(AIAttackSoAColumns columns,
                                      const AIAttackStateSoAKernelInput& input) noexcept;

} // namespace engine::ai
