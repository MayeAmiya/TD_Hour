#include "game/object/ai/runtime/AIStateFamilySoAStorage.h"

#include <algorithm>

namespace engine::ai
{

void AIFaceSoAColumns::reset(size_t count)
{
    m_requestIssuedTick.assign(count, 0);
    m_requestSequence.assign(count, 0);
    m_commandIssued.assign(count, 0);
    m_canTurnInPlace.assign(count, 0);
}

void AIMoveToSoAColumns::reset(size_t count)
{
    m_requestIssuedTick.assign(count, 0);
    m_requestSequence.assign(count, 0);
    m_resolvedGoalX.assign(count, 0);
    m_resolvedGoalY.assign(count, 0);
    m_resolvedGoalZ.assign(count, 0);
    m_adjustedGoalX.assign(count, 0);
    m_adjustedGoalY.assign(count, 0);
    m_adjustedGoalZ.assign(count, 0);
    m_path.assign(count, 0);
    m_sourceOrderRevision.assign(count, 0);
    m_generation.assign(count, 1);
    m_adjustedLayer.assign(count, 0);
    m_phase.assign(count, static_cast<uint8_t>(AIMoveToPhase::WaitingForPath));
    m_pathRequestIssued.assign(count, 0);
    m_adjustDestinations.assign(count, 1);
}

void AIFollowPathSoAColumns::reset(size_t count)
{
    m_requestIssuedTick.assign(count, 0);
    m_requestSequence.assign(count, 0);
    m_sequence.assign(count, 0);
    m_path.assign(count, 0);
    m_ignoredObstacle.assign(count, INVALID_OBJECT_ID);
    m_segmentGoalX.assign(count, 0);
    m_segmentGoalY.assign(count, 0);
    m_segmentGoalZ.assign(count, 0);
    m_sequenceRevision.assign(count, 0);
    m_sourceOrderRevision.assign(count, 0);
    m_extraDistanceRaw.assign(count, 0);
    m_index.assign(count, 0);
    m_generation.assign(count, 1);
    m_retriesRemaining.assign(count, 10);
    m_phase.assign(count, static_cast<uint8_t>(AIMoveToPhase::WaitingForPath));
    m_pathRequestIssued.assign(count, 0);
    m_finalSegment.assign(count, 0);
    m_adjustDestinations.assign(count, 0);
    m_exitProduction.assign(count, 0);
    m_allowThroughUnits.assign(count, 0);
    m_preciseFinalZ.assign(count, 0);
}

void AIWaypointPathSoAColumns::reset(size_t count)
{
    m_requestTick.assign(count, 0);
    m_requestSequence.assign(count, 0);
    m_current.assign(count, 0);
    m_prior.assign(count, 0);
    m_completionTerminal.assign(count, 0);
    m_team.assign(count, 0);
    m_path.assign(count, 0);
    m_goalX.assign(count, 0);
    m_goalY.assign(count, 0);
    m_goalZ.assign(count, 0);
    m_offsetX.assign(count, 0);
    m_offsetY.assign(count, 0);
    m_offsetZ.assign(count, 0);
    m_graphRevision.assign(count, 0);
    m_sourceRevision.assign(count, 0);
    m_teamRevision.assign(count, 0);
    m_extraDistance.assign(count, 0);
    m_generation.assign(count, 1);
    m_waypointHopLimit.assign(count, 0);
    m_phase.assign(count, static_cast<uint8_t>(AIMoveToPhase::WaitingForPath));
    m_pathRequestIssued.assign(count, 0);
    m_moveAsTeam.assign(count, 0);
    m_exact.assign(count, 0);
    m_adjust.assign(count, 0);
    m_preciseFinalZ.assign(count, 0);
    m_awaitingTeamProgress.assign(count, 0);
    m_completionPending.assign(count, 0);
}

void AIMoveOutOfWaySoAColumns::reset(size_t count)
{
    m_requestTick.assign(count, 0);
    m_requestSequence.assign(count, 0);
    m_path.assign(count, 0);
    m_goalX.assign(count, 0);
    m_goalY.assign(count, 0);
    m_goalZ.assign(count, 0);
    m_sourceRevision.assign(count, 0);
    m_deadlineTick.assign(count, 0);
    m_generation.assign(count, 1);
    m_phase.assign(count, static_cast<uint8_t>(AIMoveToPhase::WaitingForPath));
    m_pathRequestIssued.assign(count, 0);
    m_allowThrough.assign(count, 0);
}

void AIApproachPathSoAColumns::reset(size_t count)
{
    m_requestTick.assign(count, 0);
    m_requestSequence.assign(count, 0);
    m_path.assign(count, 0);
    m_goalX.assign(count, 0);
    m_goalY.assign(count, 0);
    m_goalZ.assign(count, 0);
    m_originX.assign(count, 0);
    m_originY.assign(count, 0);
    m_originZ.assign(count, 0);
    m_repulsor.assign(count, INVALID_OBJECT_ID);
    m_repulsor2.assign(count, INVALID_OBJECT_ID);
    m_sourceRevision.assign(count, 0);
    m_generation.assign(count, 1);
    m_repaths.assign(count, 1);
    m_nextScanTick.assign(count, 0);
    m_phase.assign(count, static_cast<uint8_t>(AIMoveToPhase::WaitingForPath));
    m_pathRequestIssued.assign(count, 0);
    m_adjust.assign(count, 0);
}

void AIMoveEvacuateStateSoAColumns::reset(size_t count)
{
    m_originX.assign(count, 0);
    m_originY.assign(count, 0);
    m_originZ.assign(count, 0);
    m_appendDeleteGoal.assign(count, 0);
}

void AIHackInternetStateSoAColumns::reset(size_t count)
{
    m_requestTick.assign(count, 0);
    m_requestSequence.assign(count, 0);
    m_sourceRevision.assign(count, 0);
    m_profile.assign(count, 0);
    m_profileRevision.assign(count, 0);
    m_phaseEndTick.assign(count, 0);
    m_nextPayoutTick.assign(count, 0);
    m_deferredOrderRevision.assign(count, 0);
    m_phase.assign(count, static_cast<uint8_t>(AIHackInternetPhase::Unpacking));
}

void AIAttackStateSoAStorage::reset(size_t count)
{
    m_requestTick.assign(count, 0);
    m_requestSequence.assign(count, 0);
    m_phase.assign(count, AIAttackPhase::Inactive);
    m_phaseRevision.assign(count, 0);
    m_weaponRevision.assign(count, 0);
    m_sourceRevision.assign(count, 0);
    m_pathGeneration.assign(count, 0);
    m_path.assign(count, 0);
    m_trackedTarget.assign(count, INVALID_OBJECT_ID);
    m_targetX.assign(count, 0);
    m_targetY.assign(count, 0);
    m_targetZ.assign(count, 0);
    m_arrivalRadius.assign(count, 0);
    m_minimumArrivalRadius.assign(count, 0);
    m_pathIssued.assign(count, 0);
    m_movementActive.assign(count, 0);
    m_aimingActive.assign(count, 0);
    m_firingActive.assign(count, 0);
    m_fireIssued.assign(count, 0);
    m_contactWeapon.assign(count, 0);
}

void AIDockStateSoAStorage::reset(size_t count)
{
    m_subject.assign(count, INVALID_OBJECT_ID);
    m_dock.assign(count, INVALID_OBJECT_ID);
    m_tick.assign(count, 0);
    m_sequence.assign(count, 0);
    m_purpose.assign(count, AIDockPurpose::Dock);
    m_phase.assign(count, AIDockPhase::Inactive);
    m_phaseRevision.assign(count, 0);
    m_exchange.assign(count, 0);
    m_pending.assign(count, AIDockRequestKind::None);
    m_approach.assign(count, -1);
    m_clearanceTick.assign(count, 0);
    m_nextAction.assign(count, 0);
    m_actionDelay.assign(count, 0);
    m_drone.assign(count, INVALID_OBJECT_ID);
    m_movement.assign(count, 0);
}

void AIInsertionStateSoAStorage::reset(size_t count)
{
    m_tick.assign(count, 0);
    m_sequence.assign(count, 0);
    m_target.assign(count, INVALID_OBJECT_ID);
    m_building.assign(count, 0);
    m_destinationZ.assign(count, 0);
    m_speed.assign(count, 0);
    m_rappelPhase.assign(count, AIRappelInsertionPhase::Inactive);
    m_operation.assign(count, {});
    m_eventSequence.assign(count, 0);
    m_dropPhase.assign(count, AICombatDropInsertionPhase::Inactive);
    m_dropPath.assign(count, {});
    m_dropSourceRevision.assign(count, 0);
    m_dropGeneration.assign(count, 1);
    m_dropOldPreferredHeight.assign(count, 0);
    m_dropPathIssued.assign(count, 0);
    m_dropApproachConfigured.assign(count, 0);
}

bool AIStateFamilySoAStorage::reset(container::Span<const ObjectId> orderedSubjects)
{
    for (size_t index = 0; index < orderedSubjects.size(); ++index)
    {
        if (!orderedSubjects[index] || (index != 0 && orderedSubjects[index - 1] >= orderedSubjects[index]))
            return false;
    }

    const size_t count = orderedSubjects.size();
    resetColumns(count);
    std::copy(orderedSubjects.begin(), orderedSubjects.end(), m_subjects.begin());
    m_activeSubjectCount = count;
    return true;
}

bool AIStateFamilySoAStorage::initializeCapacity(size_t capacity)
{
    resetColumns(capacity);
    return true;
}

bool AIStateFamilySoAStorage::prepareExecutionSlots(
    container::Span<const uint8_t> scheduled) noexcept
{
    if (!scheduled.empty() && scheduled.size() != size())
        return false;

    m_executionSlots.clear();
    m_executionBlocks.clear();
    m_executionFilter = {};
    m_executionCount = 0;
    m_executionMode = AIExecutionSlotRange::Mode::Empty;

    bool filteredDense = false;
    for (size_t slot = 0; slot < size(); ++slot)
    {
        if (!m_subjects[slot] || (!scheduled.empty() && scheduled[slot] == 0))
            continue;

        if (filteredDense)
        {
            ++m_executionCount;
            continue;
        }

        if (m_executionCount < DirectExecutionSlotCapacity)
        {
            m_directExecutionSlots[m_executionCount] = slot;
        }
        else
        {
            if (m_executionCount == DirectExecutionSlotCapacity)
            {
                m_executionSlots.assign(
                    m_directExecutionSlots.begin(), m_directExecutionSlots.end());
                for (const size_t directSlot : m_directExecutionSlots)
                {
                    if (m_executionBlocks.empty() ||
                        m_executionBlocks.back().end != directSlot)
                    {
                        m_executionBlocks.push_back({directSlot, directSlot + 1});
                    }
                    else
                    {
                        m_executionBlocks.back().end = directSlot + 1;
                    }
                }
            }
            m_executionSlots.push_back(slot);
            if (m_executionBlocks.empty() || m_executionBlocks.back().end != slot)
                m_executionBlocks.push_back({slot, slot + 1});
            else
                m_executionBlocks.back().end = slot + 1;
        }
        ++m_executionCount;

        // Once at least half of the physical lane is known active, scanning
        // the mask in-place is cheaper than writing and rereading a nearly
        // dense index lane. Remaining slots cannot make it sparse again.
        if (!scheduled.empty() && m_executionCount >= (size() + 1) / 2)
        {
            m_executionSlots.clear();
            m_executionBlocks.clear();
            m_executionFilter = scheduled;
            filteredDense = true;
        }
    }

    if (m_executionCount == 0)
        return true;

    if (filteredDense)
    {
        // Preserve the exact cardinality contract even though the dense
        // iterator consumes the caller-owned filter directly.
        m_executionMode = AIExecutionSlotRange::Mode::FilteredDense;
        return true;
    }

    if (scheduled.empty() && m_executionCount == size())
    {
        m_executionSlots.clear();
        m_executionBlocks.clear();
        m_executionMode = AIExecutionSlotRange::Mode::Full;
        return true;
    }

    // Tiny sets are consumed as direct ordered indices. For a medium set,
    // contiguous slot blocks win only when their two-word descriptors are no
    // larger than the compact index lane; fragmented sets retain compact slot
    // order. Both representations preserve ascending physical-slot order.
    if (m_executionCount <= DirectExecutionSlotCapacity)
        m_executionMode = AIExecutionSlotRange::Mode::DirectSlots;
    else if (m_executionBlocks.size() * 2 <= m_executionCount)
    {
        m_executionSlots.clear();
        m_executionMode = AIExecutionSlotRange::Mode::Blocks;
    }
    else
    {
        m_executionBlocks.clear();
        m_executionMode = AIExecutionSlotRange::Mode::Slots;
    }
    return true;
}

bool AIStateFamilySoAStorage::bindSubject(size_t slot, ObjectId subject) noexcept
{
    if (slot >= size() || !subject || m_subjects[slot] ||
        std::find(m_subjects.begin(), m_subjects.end(), subject) != m_subjects.end())
    {
        return false;
    }
    clearSlotState(slot);
    m_subjects[slot] = subject;
    ++m_activeSubjectCount;
    return true;
}

bool AIStateFamilySoAStorage::releaseSubject(size_t slot) noexcept
{
    if (slot >= size() || !m_subjects[slot])
        return false;
    clearSlotState(slot);
    m_subjects[slot] = INVALID_OBJECT_ID;
    --m_activeSubjectCount;
    return true;
}

void AIStateFamilySoAStorage::resetColumns(size_t count)
{
    m_executionSlots.clear();
    m_executionSlots.reserve(count);
    m_executionBlocks.clear();
    m_executionBlocks.reserve(count / 2 + 1);
    m_executionFilter = {};
    m_executionMode = AIExecutionSlotRange::Mode::Empty;
    m_executionCount = 0;
    m_subjects.assign(count, INVALID_OBJECT_ID);
    m_runtimes.assign(count, AIStateMachineRuntime{});
    m_parameters.assign(count, AIStateParameters{});
    m_payloadStates.assign(count, AIStateId::Invalid);
    m_activationSequences.assign(count, 0);
    m_idle.assign(count, AIIdleStatePayload{});
    m_wait.assign(count, AIWaitStatePayload{});
    m_busy.assign(count, AIBusyStatePayload{});
    m_dead.assign(count, AIDeadStatePayload{});
    m_face.reset(count);
    m_moveTo.reset(count);
    m_pickUpCrateDelayUpdates.assign(count, 0);
    m_containmentRequestTick.assign(count, 0);
    m_containmentRequestSequence.assign(count, 0);
    m_containmentTrackedGoal.assign(count, INVALID_OBJECT_ID);
    m_containmentEntryToClear.assign(count, INVALID_OBJECT_ID);
    m_containmentPhase.assign(count, AIContainmentPhase::Inactive);
    m_followPath.reset(count);
    m_waypointPath.reset(count);
    m_moveOutOfWay.reset(count);
    m_approachPath.reset(count);
    m_moveEvacuate.reset(count);
    m_hackInternet.reset(count);
    m_attack.reset(count);
    m_dock.reset(count);
    m_insertion.reset(count);
    m_guard.reset(count);
    m_tacticalAttack.resize(count);
    m_opportunityAttackMove.reset(count);
    m_activeStateCounts.fill(0);
    m_activeSubjectCount = 0;
}

void AIStateFamilySoAStorage::clearSlotState(size_t slot) noexcept
{
    const AIStateId previousPayloadState = m_payloadStates[slot];
    if (isValidState(previousPayloadState))
        --m_activeStateCounts[static_cast<size_t>(previousPayloadState)];

    m_runtimes[slot] = AIStateMachineRuntime{};
    m_parameters[slot] = AIStateParameters{};
    m_payloadStates[slot] = AIStateId::Invalid;
    m_activationSequences[slot] = 0;
    m_idle[slot] = AIIdleStatePayload{};
    m_wait[slot] = AIWaitStatePayload{};
    m_busy[slot] = AIBusyStatePayload{};
    m_dead[slot] = AIDeadStatePayload{};
    m_face.activate(slot, {});
    m_moveTo.activate(slot, {});
    m_pickUpCrateDelayUpdates[slot] = 0;
    m_containmentRequestTick[slot] = 0;
    m_containmentRequestSequence[slot] = 0;
    m_containmentTrackedGoal[slot] = INVALID_OBJECT_ID;
    m_containmentEntryToClear[slot] = INVALID_OBJECT_ID;
    m_containmentPhase[slot] = AIContainmentPhase::Inactive;
    m_followPath.activate(slot, {});
    m_waypointPath.activate(slot, {});
    m_moveOutOfWay.activate(slot, {});
    m_approachPath.activate(slot, {});
    m_moveEvacuate.setOrigin(slot, {});
    m_moveEvacuate.setAppendDeleteGoal(slot, false);
    m_hackInternet.activate(slot, {});
    m_attack.activate(slot, {});
    m_dock.activate(slot, {}, AIDockPurpose::Dock);
    m_insertion.activate(slot, {});
    m_guard.activate(slot, {});
    m_tacticalAttack.activate(slot, {});
    m_opportunityAttackMove.activate(slot, {});
}

} // namespace engine::ai
