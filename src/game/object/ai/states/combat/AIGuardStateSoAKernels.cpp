#include "game/object/ai/states/combat/AIGuardStateSoAKernels.h"

namespace engine::ai
{

namespace guard_detail
{

bool hasAlignedSpans(const AIGuardSoAColumns& columns, const AIGuardStateSoAKernelInput& input) noexcept
{
    const size_t count = columns.size();
    return columns.isAligned() && (input.scheduled.empty() || input.scheduled.size() == count) &&
           input.subjects.size() == count && input.sourceOrderRevisions.size() == count &&
           input.effectivelyDead.size() == count && input.mobile.size() == count &&
           input.allWeaponsOutOfAmmo.size() == count && input.projectile.size() == count &&
           input.jetAI.size() == count && input.enterGuard.size() == count &&
           input.guardWithoutPursuit.size() == count && input.flyingOnly.size() == count &&
           input.tracksAnchor.size() == count && input.contained.size() == count &&
           input.currentAnchor.size() == count && input.initialNemesis.size() == count &&
           input.priorityNemesis.size() == count && input.aggressor.size() == count && input.crate.size() == count &&
           input.cratePosition.size() == count && input.cratePositionValid.size() == count &&
           input.nearestTunnel.size() == count && input.guardRangeRaw.size() == count &&
           input.visionRangeRaw.size() == count && input.guardAreas.size() == count &&
           input.guardAreaRevisions.size() == count && input.initialScanJitter.size() == count &&
           input.feedback.size() == count && input.tacticalCommands.size() == count &&
           input.interactionCommands.size() == count && input.results.size() == count;
}

AIStateStepResult processTask(AIGuardSoAColumns& columns,
                              AIStateId state,
                              const AIGuardStateSoAKernelInput& input,
                              size_t slot,
                              const AIGuardFeedback& feedback) noexcept
{
    if (feedback.kind == AIGuardFeedbackKind::Progress)
    {
        const AIGuardPolicy policy = guardPolicyFor(state);
        if (columns.phaseAt(slot) == AIGuardPhase::Outer && !policy.tunnelNetwork && feedback.targetWithinInnerRange)
        {
            columns.chaseDeadlineTick[slot] = saturatingAdd(input.confirmedTick, input.chaseDurationTicks);
        }
        return AIStateStepResult::continueState();
    }
    if (feedback.kind == AIGuardFeedbackKind::Unsupported)
    {
        clearTask(columns, slot);
        return AIStateStepResult::unsupported();
    }

    const AIGuardPolicy policy = guardPolicyFor(state);
    const AIGuardPhase phase = columns.phaseAt(slot);
    clearTask(columns, slot);
    switch (phase)
    {
    case AIGuardPhase::Return:
        enterPhase(columns,
                   input,
                   slot,
                   feedback.kind == AIGuardFeedbackKind::Succeeded ? AIGuardPhase::Idle : AIGuardPhase::Inner);
        break;
    case AIGuardPhase::Idle:
        enterPhase(columns,
                   input,
                   slot,
                   feedback.kind == AIGuardFeedbackKind::Succeeded ? AIGuardPhase::Inner : AIGuardPhase::Return);
        break;
    case AIGuardPhase::Inner:
        enterPhase(columns, input, slot, AIGuardPhase::Outer);
        break;
    case AIGuardPhase::Outer:
        enterPhase(columns, input, slot, AIGuardPhase::GetCrate);
        break;
    case AIGuardPhase::GetCrate:
        enterPhase(columns, input, slot, AIGuardPhase::Return);
        break;
    case AIGuardPhase::AttackAggressor:
        enterPhase(columns, input, slot, policy.aggressorCompletionPhase);
        break;
    case AIGuardPhase::Inactive:
        return AIStateStepResult::unsupported();
    }
    return AIStateStepResult::continueState();
}

} // namespace guard_detail

bool enterGuardSoA(AIGuardSoAColumns& columns,
                   AIStateId state,
                   const AIGuardStateSoAKernelInput& input) noexcept
{
    using namespace guard_detail;
    const AIGuardPolicy policy = guardPolicyFor(state);
    if (!policy.valid || !hasAlignedSpans(columns, input))
        return false;

    for (size_t slot = 0; slot < columns.size(); ++slot)
    {
        if (!scheduled(input, slot))
            continue;
        if (fact(input.effectivelyDead[slot]))
        {
            input.results[slot] = AIStateStepResult::transitionTo(AIStateId::Dead);
            continue;
        }
        const AIStateRequestId request = columns.requestAt(slot);
        if (!input.subjects[slot] || !request.isValid() || input.sourceOrderRevisions[slot] == 0)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        columns.sourceOrderRevision[slot] = input.sourceOrderRevisions[slot];
        columns.state[slot] = state;
        columns.active[slot] = 1;
        columns.nextOperationRevision[slot] = 1;
        columns.taskOperation[slot] = static_cast<uint8_t>(AIGuardOperation::None);
        columns.taskDomain[slot] = static_cast<uint8_t>(AIGuardOperationDomain::None);
        columns.taskRevision[slot] = 0;
        columns.scanPending[slot] = 0;
        columns.scanRevision[slot] = 0;
        columns.chaseDeadlineTick[slot] = 0;
        columns.nemesis[slot] = input.initialNemesis[slot];
        columns.anchorX[slot] = input.currentAnchor[slot].xRaw;
        columns.anchorY[slot] = input.currentAnchor[slot].yRaw;
        columns.anchorZ[slot] = input.currentAnchor[slot].zRaw;
        enterPhase(columns, input, slot, policy.initialPhase);

        if (!beginCurrentPhase(columns, state, input, slot))
        {
            columns.active[slot] = 0;
            columns.state[slot] = AIStateId::Invalid;
            setPhase(columns, slot, AIGuardPhase::Inactive);
            input.results[slot] =
                fact(input.mobile[slot]) ? AIStateStepResult::blocked() : AIStateStepResult::failure();
            continue;
        }
        input.results[slot] = AIStateStepResult::continueState();
    }
    return true;
}

bool canExitGuardSoA(const AIGuardSoAColumns& columns,
                     AIStateId state,
                     const AIGuardStateSoAKernelInput& input) noexcept
{
    using namespace guard_detail;
    if (!guardPolicyFor(state).valid || !hasAlignedSpans(columns, input))
        return false;
    for (size_t slot = 0; slot < columns.size(); ++slot)
    {
        if (!scheduled(input, slot) || !fact(columns.active[slot]))
            continue;
        if (columns.state[slot] != state)
            continue;
        const AIGuardOperationDomain domain = columns.taskDomainAt(slot);
        const size_t tacticalCount = static_cast<size_t>(fact(columns.scanPending[slot])) +
                                     static_cast<size_t>(domain == AIGuardOperationDomain::Tactical);
        const size_t interactionCount = 1 + static_cast<size_t>(domain == AIGuardOperationDomain::Interaction);
        if (!input.tacticalCommands[slot].hasCapacity(tacticalCount) ||
            !input.interactionCommands[slot].hasCapacity(interactionCount))
        {
            return false;
        }
    }
    return true;
}

bool exitGuardSoA(AIGuardSoAColumns& columns,
                  AIStateId state,
                  const AIGuardStateSoAKernelInput& input) noexcept
{
    using namespace guard_detail;
    if (!canExitGuardSoA(columns, state, input))
        return false;

    for (size_t slot = 0; slot < columns.size(); ++slot)
    {
        if (!scheduled(input, slot) || !fact(columns.active[slot]) || columns.state[slot] != state)
            continue;
        static_cast<void>(cancelOutstanding(columns, state, input, slot));
        const uint32_t revision = allocateRevision(columns, slot);
        static_cast<void>(input.interactionCommands[slot].push({
            .correlation =
                correlation(columns, state, input.subjects[slot], slot, AIGuardOperation::EndGuard, revision),
            .kind = AIGuardInteractionCommandKind::EndGuard,
            .target = {},
            .urgent = false,
            .clearTeamTarget = clearsTeamTargetOnExit(
                state, columns.phaseAt(slot)),
            .confirmedTick = input.confirmedTick,
        }));
        columns.active[slot] = 0;
        columns.state[slot] = AIStateId::Invalid;
        columns.scanPending[slot] = 0;
        columns.scanRevision[slot] = 0;
        setPhase(columns, slot, AIGuardPhase::Inactive);
    }
    return true;
}

} // namespace engine::ai
