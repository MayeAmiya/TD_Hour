#include "game/object/ai/states/combat/AIAttackStateSoAKernels.h"

namespace engine::ai
{

namespace attack_detail
{

bool hasAlignedInputSpans(size_t count, const AIAttackStateSoAKernelInput& input) noexcept
{
    return (input.scheduled.empty() || input.scheduled.size() == count) && input.subjects.size() == count &&
           input.effectivelyDead.size() == count && input.constructionComplete.size() == count &&
           input.hasAmmo.size() == count && input.attackMoodAllowed.size() == count &&
           input.exitConditionSatisfied.size() == count && input.sourceOrderRevisions.size() == count &&
           input.weaponRevisions.size() == count && input.goalObjects.size() == count &&
           input.goalPositions.size() == count && input.hasGoalPositions.size() == count &&
           input.subjectPositions.size() == count && input.mobile.size() == count &&
           input.groundMovement.size() == count && input.projectile.size() == count &&
           input.combatFeedback.size() == count && input.pathFeedback.size() == count &&
           input.movementFeedback.size() == count && input.combatCommands.size() == count &&
           input.pathRequests.size() == count && input.movementCommands.size() == count &&
           input.results.size() == count;
}

namespace
{

void resetColumns(AIAttackSoAColumns& columns, size_t slot) noexcept
{
    columns.phase[slot] = AIAttackPhase::Inactive;
    columns.phaseRevision[slot] = 0;
    columns.weaponRevision[slot] = 0;
    columns.sourceOrderRevision[slot] = 0;
    columns.pathGeneration[slot] = 0;
    columns.pathHandle[slot] = 0;
    columns.trackedTarget[slot] = INVALID_OBJECT_ID;
    columns.targetXRaw[slot] = 0;
    columns.targetYRaw[slot] = 0;
    columns.targetZRaw[slot] = 0;
    columns.arrivalRadiusRaw[slot] = 0;
    columns.minimumArrivalRadiusRaw[slot] = 0;
    columns.pathRequestIssued[slot] = 0;
    columns.movementActive[slot] = 0;
    columns.aimingActive[slot] = 0;
    columns.firingActive[slot] = 0;
    columns.fireCommandIssued[slot] = 0;
    columns.contactWeapon[slot] = 0;
}

} // namespace

bool hasAlignedSpans(const AIAttackSoAColumns& columns, const AIAttackStateSoAKernelInput& input) noexcept
{
    const size_t count = input.activeStates.size();
    return hasAlignedInputSpans(count, input) && columns.requestTick.size() == count &&
           columns.requestSequence.size() == count && columns.phase.size() == count &&
           columns.phaseRevision.size() == count && columns.weaponRevision.size() == count &&
           columns.sourceOrderRevision.size() == count && columns.pathGeneration.size() == count &&
           columns.pathHandle.size() == count && columns.trackedTarget.size() == count &&
           columns.targetXRaw.size() == count && columns.targetYRaw.size() == count &&
           columns.targetZRaw.size() == count && columns.arrivalRadiusRaw.size() == count &&
           columns.minimumArrivalRadiusRaw.size() == count &&
           columns.pathRequestIssued.size() == count && columns.movementActive.size() == count &&
           columns.aimingActive.size() == count && columns.firingActive.size() == count &&
           columns.fireCommandIssued.size() == count && columns.contactWeapon.size() == count;
}

} // namespace attack_detail

bool enterAttackStateSoA(AIAttackSoAColumns columns, const AIAttackStateSoAKernelInput& input) noexcept
{
    using namespace attack_detail;
    if (!hasAlignedSpans(columns, input))
        return false;

    for (size_t slot = 0; slot < input.activeStates.size(); ++slot)
    {
        if (!scheduled(input, slot))
            continue;
        const AIAttackPolicy policy = attackPolicyFor(input.activeStates[slot]);
        if (!policy.valid)
            continue;
        const bool validGoal =
            policy.attacksObject ? input.goalObjects[slot].isValid() : fact(input.hasGoalPositions[slot]);
        const AIStateRequestId request{columns.requestTick[slot], columns.requestSequence[slot]};
        if (!fact(input.effectivelyDead[slot]) && fact(input.attackMoodAllowed[slot]) &&
            !fact(input.exitConditionSatisfied[slot]) && fact(input.constructionComplete[slot]) &&
            (fact(input.hasAmmo[slot]) || fact(input.projectile[slot])) &&
            input.subjects[slot] && request.isValid() && input.sourceOrderRevisions[slot] != 0 &&
            input.weaponRevisions[slot] != 0 && validGoal && !hasCapacity(input.combatCommands[slot], 1))
        {
            return false;
        }
    }

    for (size_t slot = 0; slot < input.activeStates.size(); ++slot)
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
        if (!fact(input.constructionComplete[slot]) ||
            (!fact(input.hasAmmo[slot]) && !fact(input.projectile[slot])))
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        const bool validGoal =
            policy.attacksObject ? input.goalObjects[slot].isValid() : fact(input.hasGoalPositions[slot]);
        const AIStateRequestId request{columns.requestTick[slot], columns.requestSequence[slot]};
        if (!input.subjects[slot] || !request.isValid() || input.sourceOrderRevisions[slot] == 0 ||
            input.weaponRevisions[slot] == 0)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        if (!validGoal)
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }

        resetColumns(columns, slot);
        columns.phase[slot] = AIAttackPhase::Aim;
        columns.phaseRevision[slot] = 1;
        columns.weaponRevision[slot] = input.weaponRevisions[slot];
        columns.sourceOrderRevision[slot] = input.sourceOrderRevisions[slot];
        columns.pathGeneration[slot] = 1;
        columns.trackedTarget[slot] = policy.attacksObject ? input.goalObjects[slot] : INVALID_OBJECT_ID;
        setTargetPosition(columns, slot, input.goalPositions[slot]);
        emitCombat(input,
                   slot,
                   combatCorrelation(columns, input, slot),
                   AIAttackCommandKind::BeginAim,
                   policy,
                   columns.trackedTarget[slot],
                   targetPosition(columns, slot));
        columns.aimingActive[slot] = 1;
        input.results[slot] = AIStateStepResult::continueState();
    }
    return true;
}

bool canExitAttackStateSoA(const AIAttackSoAColumns& columns, const AIAttackStateSoAKernelInput& input) noexcept
{
    using namespace attack_detail;
    if (!hasAlignedSpans(columns, input))
        return false;
    for (size_t slot = 0; slot < input.activeStates.size(); ++slot)
    {
        if (!scheduled(input, slot) || !attackPolicyFor(input.activeStates[slot]).valid ||
            columns.phase[slot] == AIAttackPhase::Inactive)
        {
            continue;
        }
        if (!combatCorrelation(columns, input, slot).isValid() || !pathCorrelation(columns, input, slot).isValid() ||
            !hasCapacity(input, slot, cleanupNeeds(columns, slot)))
        {
            return false;
        }
    }
    return true;
}

bool exitAttackStateSoA(AIAttackSoAColumns columns, const AIAttackStateSoAKernelInput& input) noexcept
{
    using namespace attack_detail;
    if (!canExitAttackStateSoA(columns, input))
        return false;
    for (size_t slot = 0; slot < input.activeStates.size(); ++slot)
    {
        const AIAttackPolicy policy = attackPolicyFor(input.activeStates[slot]);
        if (!scheduled(input, slot) || !policy.valid || columns.phase[slot] == AIAttackPhase::Inactive)
            continue;
        emitCleanup(columns, input, slot, policy);
        resetColumns(columns, slot);
    }
    return true;
}

} // namespace engine::ai
