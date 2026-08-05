#include "game/object/ai/runtime/AIStateFamilySoAStorage.h"

namespace engine::ai
{

void AIFaceSoAColumns::activate(size_t slot, AIStateRequestId request) noexcept
{
    store(slot, AIFaceStatePayload{request});
}

AIFaceStatePayload AIFaceSoAColumns::load(size_t slot) const noexcept
{
    AIFaceStatePayload value{AIStateRequestId{m_requestIssuedTick[slot], m_requestSequence[slot]}};
    value.commandIssued = m_commandIssued[slot] != 0;
    value.canTurnInPlace = m_canTurnInPlace[slot] != 0;
    return value;
}

void AIFaceSoAColumns::store(size_t slot, const AIFaceStatePayload& value) noexcept
{
    m_requestIssuedTick[slot] = value.request.issuedTick;
    m_requestSequence[slot] = value.request.sequence;
    m_commandIssued[slot] = value.commandIssued ? uint8_t{1} : uint8_t{0};
    m_canTurnInPlace[slot] = value.canTurnInPlace ? uint8_t{1} : uint8_t{0};
}

void AIMoveToSoAColumns::activate(size_t slot, AIStateRequestId request) noexcept
{
    store(slot, AIMoveToStatePayload{request});
}

AIMoveToStatePayload AIMoveToSoAColumns::load(size_t slot) const noexcept
{
    AIMoveToStatePayload value{AIStateRequestId{m_requestIssuedTick[slot], m_requestSequence[slot]}};
    value.resolvedGoal = {m_resolvedGoalX[slot], m_resolvedGoalY[slot], m_resolvedGoalZ[slot]};
    value.adjustedGoal = {m_adjustedGoalX[slot], m_adjustedGoalY[slot], m_adjustedGoalZ[slot]};
    value.path = PathHandle{m_path[slot]};
    value.sourceOrderRevision = m_sourceOrderRevision[slot];
    value.generation = m_generation[slot];
    value.adjustedLayer = m_adjustedLayer[slot];
    value.phase = static_cast<AIMoveToPhase>(m_phase[slot]);
    value.pathRequestIssued = m_pathRequestIssued[slot] != 0;
    value.adjustDestinations = m_adjustDestinations[slot] != 0;
    return value;
}

void AIMoveToSoAColumns::store(size_t slot, const AIMoveToStatePayload& value) noexcept
{
    m_requestIssuedTick[slot] = value.request.issuedTick;
    m_requestSequence[slot] = value.request.sequence;
    m_resolvedGoalX[slot] = value.resolvedGoal.xRaw;
    m_resolvedGoalY[slot] = value.resolvedGoal.yRaw;
    m_resolvedGoalZ[slot] = value.resolvedGoal.zRaw;
    m_adjustedGoalX[slot] = value.adjustedGoal.xRaw;
    m_adjustedGoalY[slot] = value.adjustedGoal.yRaw;
    m_adjustedGoalZ[slot] = value.adjustedGoal.zRaw;
    m_path[slot] = value.path.value;
    m_sourceOrderRevision[slot] = value.sourceOrderRevision;
    m_generation[slot] = value.generation;
    m_adjustedLayer[slot] = value.adjustedLayer;
    m_phase[slot] = static_cast<uint8_t>(value.phase);
    m_pathRequestIssued[slot] = value.pathRequestIssued ? uint8_t{1} : uint8_t{0};
    m_adjustDestinations[slot] = value.adjustDestinations ? uint8_t{1} : uint8_t{0};
}

void AIFollowPathSoAColumns::activate(size_t slot, AIStateRequestId request) noexcept
{
    store(slot, AIFollowPathStatePayload{request});
}

AIFollowPathStatePayload AIFollowPathSoAColumns::load(size_t slot) const noexcept
{
    AIFollowPathStatePayload value;
    value.request = {m_requestIssuedTick[slot], m_requestSequence[slot]};
    value.sequence = AIPathSequenceHandle{m_sequence[slot]};
    value.path = PathHandle{m_path[slot]};
    value.ignoredObstacle = m_ignoredObstacle[slot];
    value.segmentGoal = {m_segmentGoalX[slot], m_segmentGoalY[slot], m_segmentGoalZ[slot]};
    value.sequenceRevision = m_sequenceRevision[slot];
    value.sourceOrderRevision = m_sourceOrderRevision[slot];
    value.extraDistanceRaw = m_extraDistanceRaw[slot];
    value.index = m_index[slot];
    value.generation = m_generation[slot];
    value.retriesRemaining = m_retriesRemaining[slot];
    value.phase = static_cast<AIMoveToPhase>(m_phase[slot]);
    value.pathRequestIssued = m_pathRequestIssued[slot] != 0;
    value.finalSegment = m_finalSegment[slot] != 0;
    value.adjustDestinations = m_adjustDestinations[slot] != 0;
    value.exitProduction = m_exitProduction[slot] != 0;
    value.allowThroughUnits = m_allowThroughUnits[slot] != 0;
    value.preciseFinalZ = m_preciseFinalZ[slot] != 0;
    return value;
}

void AIFollowPathSoAColumns::store(size_t slot, const AIFollowPathStatePayload& value) noexcept
{
    m_requestIssuedTick[slot] = value.request.issuedTick;
    m_requestSequence[slot] = value.request.sequence;
    m_sequence[slot] = value.sequence.value;
    m_path[slot] = value.path.value;
    m_ignoredObstacle[slot] = value.ignoredObstacle;
    m_segmentGoalX[slot] = value.segmentGoal.xRaw;
    m_segmentGoalY[slot] = value.segmentGoal.yRaw;
    m_segmentGoalZ[slot] = value.segmentGoal.zRaw;
    m_sequenceRevision[slot] = value.sequenceRevision;
    m_sourceOrderRevision[slot] = value.sourceOrderRevision;
    m_extraDistanceRaw[slot] = value.extraDistanceRaw;
    m_index[slot] = value.index;
    m_generation[slot] = value.generation;
    m_retriesRemaining[slot] = value.retriesRemaining;
    m_phase[slot] = static_cast<uint8_t>(value.phase);
    m_pathRequestIssued[slot] = value.pathRequestIssued ? uint8_t{1} : uint8_t{0};
    m_finalSegment[slot] = value.finalSegment ? uint8_t{1} : uint8_t{0};
    m_adjustDestinations[slot] = value.adjustDestinations ? uint8_t{1} : uint8_t{0};
    m_exitProduction[slot] = value.exitProduction ? uint8_t{1} : uint8_t{0};
    m_allowThroughUnits[slot] = value.allowThroughUnits ? uint8_t{1} : uint8_t{0};
    m_preciseFinalZ[slot] = value.preciseFinalZ ? uint8_t{1} : uint8_t{0};
}

void AIWaypointPathSoAColumns::activate(size_t slot, AIStateRequestId request) noexcept
{
    store(slot, AIWaypointPathStatePayload{request});
}

AIWaypointPathStatePayload AIWaypointPathSoAColumns::load(size_t slot) const noexcept
{
    AIWaypointPathStatePayload value;
    value.request = {m_requestTick[slot], m_requestSequence[slot]};
    value.current = AIWaypointHandle{m_current[slot]};
    value.prior = AIWaypointHandle{m_prior[slot]};
    value.completionTerminal = AIWaypointHandle{m_completionTerminal[slot]};
    value.team = AITeamHandle{m_team[slot]};
    value.path = PathHandle{m_path[slot]};
    value.goal = {m_goalX[slot], m_goalY[slot], m_goalZ[slot]};
    value.groupOffset = {m_offsetX[slot], m_offsetY[slot], m_offsetZ[slot]};
    value.graphRevision = m_graphRevision[slot];
    value.sourceOrderRevision = m_sourceRevision[slot];
    value.teamRevision = m_teamRevision[slot];
    value.extraDistanceRaw = m_extraDistance[slot];
    value.generation = m_generation[slot];
    value.phase = static_cast<AIMoveToPhase>(m_phase[slot]);
    value.waypointHopLimit = m_waypointHopLimit[slot];
    value.pathRequestIssued = m_pathRequestIssued[slot] != 0;
    value.moveAsTeam = m_moveAsTeam[slot] != 0;
    value.exactPolyline = m_exact[slot] != 0;
    value.adjustDestinations = m_adjust[slot] != 0;
    value.preciseFinalZ = m_preciseFinalZ[slot] != 0;
    value.awaitingTeamProgress = m_awaitingTeamProgress[slot] != 0;
    value.completionPending = m_completionPending[slot] != 0;
    return value;
}

void AIWaypointPathSoAColumns::store(size_t slot, const AIWaypointPathStatePayload& value) noexcept
{
    m_requestTick[slot] = value.request.issuedTick;
    m_requestSequence[slot] = value.request.sequence;
    m_current[slot] = value.current.value;
    m_prior[slot] = value.prior.value;
    m_completionTerminal[slot] = value.completionTerminal.value;
    m_team[slot] = value.team.value;
    m_path[slot] = value.path.value;
    m_goalX[slot] = value.goal.xRaw;
    m_goalY[slot] = value.goal.yRaw;
    m_goalZ[slot] = value.goal.zRaw;
    m_offsetX[slot] = value.groupOffset.xRaw;
    m_offsetY[slot] = value.groupOffset.yRaw;
    m_offsetZ[slot] = value.groupOffset.zRaw;
    m_graphRevision[slot] = value.graphRevision;
    m_sourceRevision[slot] = value.sourceOrderRevision;
    m_teamRevision[slot] = value.teamRevision;
    m_extraDistance[slot] = value.extraDistanceRaw;
    m_generation[slot] = value.generation;
    m_phase[slot] = static_cast<uint8_t>(value.phase);
    m_waypointHopLimit[slot] = value.waypointHopLimit;
    m_pathRequestIssued[slot] = value.pathRequestIssued ? uint8_t{1} : uint8_t{0};
    m_moveAsTeam[slot] = value.moveAsTeam ? uint8_t{1} : uint8_t{0};
    m_exact[slot] = value.exactPolyline ? uint8_t{1} : uint8_t{0};
    m_adjust[slot] = value.adjustDestinations ? uint8_t{1} : uint8_t{0};
    m_preciseFinalZ[slot] = value.preciseFinalZ ? uint8_t{1} : uint8_t{0};
    m_awaitingTeamProgress[slot] = value.awaitingTeamProgress ? uint8_t{1} : uint8_t{0};
    m_completionPending[slot] = value.completionPending ? uint8_t{1} : uint8_t{0};
}

void AIMoveOutOfWaySoAColumns::activate(size_t slot, AIStateRequestId request) noexcept
{
    store(slot, AIMoveOutOfWayStatePayload{request});
}

AIMoveOutOfWayStatePayload AIMoveOutOfWaySoAColumns::load(size_t slot) const noexcept
{
    AIMoveOutOfWayStatePayload value;
    value.request = {m_requestTick[slot], m_requestSequence[slot]};
    value.path = PathHandle{m_path[slot]};
    value.goal = {m_goalX[slot], m_goalY[slot], m_goalZ[slot]};
    value.sourceOrderRevision = m_sourceRevision[slot];
    value.deadlineTick = m_deadlineTick[slot];
    value.generation = m_generation[slot];
    value.phase = static_cast<AIMoveToPhase>(m_phase[slot]);
    value.pathRequestIssued = m_pathRequestIssued[slot] != 0;
    value.allowPathThroughUnits = m_allowThrough[slot] != 0;
    return value;
}

void AIMoveOutOfWaySoAColumns::store(size_t slot, const AIMoveOutOfWayStatePayload& value) noexcept
{
    m_requestTick[slot] = value.request.issuedTick;
    m_requestSequence[slot] = value.request.sequence;
    m_path[slot] = value.path.value;
    m_goalX[slot] = value.goal.xRaw;
    m_goalY[slot] = value.goal.yRaw;
    m_goalZ[slot] = value.goal.zRaw;
    m_sourceRevision[slot] = value.sourceOrderRevision;
    m_deadlineTick[slot] = value.deadlineTick;
    m_generation[slot] = value.generation;
    m_phase[slot] = static_cast<uint8_t>(value.phase);
    m_pathRequestIssued[slot] = value.pathRequestIssued ? uint8_t{1} : uint8_t{0};
    m_allowThrough[slot] = value.allowPathThroughUnits ? uint8_t{1} : uint8_t{0};
}

void AIApproachPathSoAColumns::activate(size_t slot, AIStateRequestId request) noexcept
{
    store(slot, AIApproachPathStatePayload{request});
}

AIApproachPathStatePayload AIApproachPathSoAColumns::load(size_t slot) const noexcept
{
    AIApproachPathStatePayload value;
    value.request = {m_requestTick[slot], m_requestSequence[slot]};
    value.path = PathHandle{m_path[slot]};
    value.goal = {m_goalX[slot], m_goalY[slot], m_goalZ[slot]};
    value.origin = {m_originX[slot], m_originY[slot], m_originZ[slot]};
    value.repulsor = m_repulsor[slot];
    value.repulsor2 = m_repulsor2[slot];
    value.sourceOrderRevision = m_sourceRevision[slot];
    value.generation = m_generation[slot];
    value.nextRepulsorScanTick = m_nextScanTick[slot];
    value.repathsRemaining = m_repaths[slot];
    value.phase = static_cast<AIMoveToPhase>(m_phase[slot]);
    value.pathRequestIssued = m_pathRequestIssued[slot] != 0;
    value.adjustDestinations = m_adjust[slot] != 0;
    return value;
}

void AIApproachPathSoAColumns::store(size_t slot, const AIApproachPathStatePayload& value) noexcept
{
    m_requestTick[slot] = value.request.issuedTick;
    m_requestSequence[slot] = value.request.sequence;
    m_path[slot] = value.path.value;
    m_goalX[slot] = value.goal.xRaw;
    m_goalY[slot] = value.goal.yRaw;
    m_goalZ[slot] = value.goal.zRaw;
    m_originX[slot] = value.origin.xRaw;
    m_originY[slot] = value.origin.yRaw;
    m_originZ[slot] = value.origin.zRaw;
    m_repulsor[slot] = value.repulsor;
    m_repulsor2[slot] = value.repulsor2;
    m_sourceRevision[slot] = value.sourceOrderRevision;
    m_nextScanTick[slot] = value.nextRepulsorScanTick;
    m_generation[slot] = value.generation;
    m_repaths[slot] = value.repathsRemaining;
    m_phase[slot] = static_cast<uint8_t>(value.phase);
    m_pathRequestIssued[slot] = value.pathRequestIssued ? 1 : 0;
    m_adjust[slot] = value.adjustDestinations ? 1 : 0;
}

void AIHackInternetStateSoAColumns::activate(size_t slot, AIStateRequestId request) noexcept
{
    m_requestTick[slot] = request.issuedTick;
    m_requestSequence[slot] = request.sequence;
    m_sourceRevision[slot] = 0;
    m_profile[slot] = 0;
    m_profileRevision[slot] = 0;
    m_phaseEndTick[slot] = 0;
    m_nextPayoutTick[slot] = 0;
    m_deferredOrderRevision[slot] = 0;
    m_phase[slot] = static_cast<uint8_t>(AIHackInternetPhase::Unpacking);
}

void AIAttackStateSoAStorage::activate(size_t slot, AIStateRequestId request) noexcept
{
    AIAttackStatePayload value{request};
    store(slot, value);
}

AIAttackStatePayload AIAttackStateSoAStorage::load(size_t slot) const noexcept
{
    AIAttackStatePayload value{{m_requestTick[slot], m_requestSequence[slot]}};
    value.phase = m_phase[slot];
    value.phaseRevision = m_phaseRevision[slot];
    value.weaponRevision = m_weaponRevision[slot];
    value.sourceOrderRevision = m_sourceRevision[slot];
    value.pathGeneration = m_pathGeneration[slot];
    value.path = {m_path[slot]};
    value.trackedTarget = m_trackedTarget[slot];
    value.targetPosition = {m_targetX[slot], m_targetY[slot], m_targetZ[slot]};
    value.arrivalRadiusRaw = m_arrivalRadius[slot];
    value.minimumArrivalRadiusRaw = m_minimumArrivalRadius[slot];
    value.pathRequestIssued = m_pathIssued[slot] != 0;
    value.movementActive = m_movementActive[slot] != 0;
    value.aimingActive = m_aimingActive[slot] != 0;
    value.firingActive = m_firingActive[slot] != 0;
    value.fireCommandIssued = m_fireIssued[slot] != 0;
    value.contactWeapon = m_contactWeapon[slot] != 0;
    return value;
}

void AIAttackStateSoAStorage::store(size_t slot, const AIAttackStatePayload& value) noexcept
{
    m_requestTick[slot] = value.request.issuedTick;
    m_requestSequence[slot] = value.request.sequence;
    m_phase[slot] = value.phase;
    m_phaseRevision[slot] = value.phaseRevision;
    m_weaponRevision[slot] = value.weaponRevision;
    m_sourceRevision[slot] = value.sourceOrderRevision;
    m_pathGeneration[slot] = value.pathGeneration;
    m_path[slot] = value.path.value;
    m_trackedTarget[slot] = value.trackedTarget;
    m_targetX[slot] = value.targetPosition.xRaw;
    m_targetY[slot] = value.targetPosition.yRaw;
    m_targetZ[slot] = value.targetPosition.zRaw;
    m_arrivalRadius[slot] = value.arrivalRadiusRaw;
    m_minimumArrivalRadius[slot] = value.minimumArrivalRadiusRaw;
    m_pathIssued[slot] = value.pathRequestIssued ? 1 : 0;
    m_movementActive[slot] = value.movementActive ? 1 : 0;
    m_aimingActive[slot] = value.aimingActive ? 1 : 0;
    m_firingActive[slot] = value.firingActive ? 1 : 0;
    m_fireIssued[slot] = value.fireCommandIssued ? 1 : 0;
    m_contactWeapon[slot] = value.contactWeapon ? 1 : 0;
}

void AIDockStateSoAStorage::activate(size_t slot, AIStateRequestId request, AIDockPurpose purpose) noexcept
{
    AIDockStatePayload payload;
    payload.token.stateRequest = request;
    payload.token.purpose = purpose;
    store(slot, payload);
}

AIDockStatePayload AIDockStateSoAStorage::load(size_t slot) const noexcept
{
    AIDockStatePayload payload;
    payload.token = {m_subject[slot], m_dock[slot], {m_tick[slot], m_sequence[slot]}, m_purpose[slot]};
    payload.phase = m_phase[slot];
    payload.phaseRevision = m_phaseRevision[slot];
    payload.exchangeSequence = m_exchange[slot];
    payload.pendingRequest = m_pending[slot];
    payload.approachPosition = m_approach[slot];
    payload.clearanceEnterTick = m_clearanceTick[slot];
    payload.nextActionTick = m_nextAction[slot];
    payload.actionDelayTicks = m_actionDelay[slot];
    payload.drone = m_drone[slot];
    payload.movementActive = m_movement[slot] != 0;
    return payload;
}

void AIDockStateSoAStorage::store(size_t slot, const AIDockStatePayload& payload) noexcept
{
    m_subject[slot] = payload.token.subject;
    m_dock[slot] = payload.token.dock;
    m_tick[slot] = payload.token.stateRequest.issuedTick;
    m_sequence[slot] = payload.token.stateRequest.sequence;
    m_purpose[slot] = payload.token.purpose;
    m_phase[slot] = payload.phase;
    m_phaseRevision[slot] = payload.phaseRevision;
    m_exchange[slot] = payload.exchangeSequence;
    m_pending[slot] = payload.pendingRequest;
    m_approach[slot] = payload.approachPosition;
    m_clearanceTick[slot] = payload.clearanceEnterTick;
    m_nextAction[slot] = payload.nextActionTick;
    m_actionDelay[slot] = payload.actionDelayTicks;
    m_drone[slot] = payload.drone;
    m_movement[slot] = payload.movementActive ? 1 : 0;
}

void AIInsertionStateSoAStorage::activate(size_t slot, AIStateRequestId request) noexcept
{
    AIInsertionStatePayload payload;
    payload.request = request;
    store(slot, payload);
}

AIInsertionStatePayload AIInsertionStateSoAStorage::load(size_t slot) const noexcept
{
    AIInsertionStatePayload payload;
    payload.request = {m_tick[slot], m_sequence[slot]};
    payload.rappelTarget = m_target[slot];
    payload.rappelTargetIsBuilding = m_building[slot] != 0;
    payload.rappelDestinationZRaw = m_destinationZ[slot];
    payload.rappelSpeedRaw = m_speed[slot];
    payload.rappelPhase = m_rappelPhase[slot];
    payload.combatDropOperation = m_operation[slot];
    payload.combatDropNextEventSequence = m_eventSequence[slot];
    payload.combatDropPhase = m_dropPhase[slot];
    payload.combatDropPath = m_dropPath[slot];
    payload.combatDropSourceOrderRevision = m_dropSourceRevision[slot];
    payload.combatDropPathGeneration = m_dropGeneration[slot];
    payload.combatDropOldPreferredHeightRaw = m_dropOldPreferredHeight[slot];
    payload.combatDropPathRequestIssued = m_dropPathIssued[slot] != 0;
    payload.combatDropApproachConfigured = m_dropApproachConfigured[slot] != 0;
    return payload;
}

void AIInsertionStateSoAStorage::store(size_t slot, const AIInsertionStatePayload& payload) noexcept
{
    m_tick[slot] = payload.request.issuedTick;
    m_sequence[slot] = payload.request.sequence;
    m_target[slot] = payload.rappelTarget;
    m_building[slot] = payload.rappelTargetIsBuilding ? 1 : 0;
    m_destinationZ[slot] = payload.rappelDestinationZRaw;
    m_speed[slot] = payload.rappelSpeedRaw;
    m_rappelPhase[slot] = payload.rappelPhase;
    m_operation[slot] = payload.combatDropOperation;
    m_eventSequence[slot] = payload.combatDropNextEventSequence;
    m_dropPhase[slot] = payload.combatDropPhase;
    m_dropPath[slot] = payload.combatDropPath;
    m_dropSourceRevision[slot] = payload.combatDropSourceOrderRevision;
    m_dropGeneration[slot] = payload.combatDropPathGeneration;
    m_dropOldPreferredHeight[slot] = payload.combatDropOldPreferredHeightRaw;
    m_dropPathIssued[slot] = payload.combatDropPathRequestIssued ? 1 : 0;
    m_dropApproachConfigured[slot] = payload.combatDropApproachConfigured ? 1 : 0;
}

void AIStateFamilySoAStorage::activate(size_t slot, AIStateId state, uint64_t confirmedTick) noexcept
{
    const AIStateId previousPayloadState = m_payloadStates[slot];
    if (isValidState(previousPayloadState))
        --m_activeStateCounts[static_cast<size_t>(previousPayloadState)];
    if (isValidState(state))
        ++m_activeStateCounts[static_cast<size_t>(state)];

    uint32_t& sequence = m_activationSequences[slot];
    ++sequence;
    if (sequence == 0)
        ++sequence;
    m_payloadStates[slot] = state;

    switch (state)
    {
    case AIStateId::Idle:
        m_idle[slot] = AIIdleStatePayload{confirmedTick};
        break;
    case AIStateId::Wait:
        m_wait[slot] = AIWaitStatePayload{m_parameters[slot].waitEndTick};
        break;
    case AIStateId::Busy:
        m_busy[slot] = AIBusyStatePayload{confirmedTick};
        break;
    case AIStateId::Dead:
        m_dead[slot] = AIDeadStatePayload{confirmedTick};
        break;
    case AIStateId::FaceObject:
    case AIStateId::FacePosition:
        m_face.activate(slot, AIStateRequestId{confirmedTick, sequence});
        break;
    case AIStateId::MoveTo:
        m_moveTo.activate(slot, AIStateRequestId{confirmedTick, sequence});
        break;
    case AIStateId::PickUpCrate:
        m_moveTo.activate(slot, AIStateRequestId{confirmedTick, sequence});
        m_pickUpCrateDelayUpdates[slot] = 0;
        break;
    case AIStateId::Enter:
    case AIStateId::Exit:
    case AIStateId::ExitInstantly:
        m_containmentRequestTick[slot] = confirmedTick;
        m_containmentRequestSequence[slot] = sequence;
        m_containmentTrackedGoal[slot] = INVALID_OBJECT_ID;
        m_containmentEntryToClear[slot] = INVALID_OBJECT_ID;
        m_containmentPhase[slot] = AIContainmentPhase::Inactive;
        break;
    case AIStateId::HackInternet:
        m_hackInternet.activate(slot, AIStateRequestId{confirmedTick, sequence});
        break;
    case AIStateId::Guard:
    case AIStateId::GuardRetaliate:
    case AIStateId::GuardTunnelNetwork:
        m_guard.activate(slot, AIStateRequestId{confirmedTick, sequence});
        break;
    case AIStateId::Hunt:
    case AIStateId::AttackSquad:
    case AIStateId::AttackArea:
        m_tacticalAttack.activate(slot, AIStateRequestId{confirmedTick, sequence});
        break;
    case AIStateId::AttackMoveTo:
        m_opportunityAttackMove.activate(slot, AIStateRequestId{confirmedTick, sequence});
        m_moveTo.activate(slot, AIStateRequestId{confirmedTick, sequence});
        break;
    case AIStateId::AttackFollowWaypointPathAsIndividuals:
    case AIStateId::AttackFollowWaypointPathAsTeam:
        m_opportunityAttackMove.activate(slot, AIStateRequestId{confirmedTick, sequence});
    {
        AIWaypointPathStatePayload payload{AIStateRequestId{confirmedTick, sequence}};
        payload.current = m_parameters[slot].waypoint;
        payload.graphRevision = m_parameters[slot].waypointGraphRevision;
        payload.team = m_parameters[slot].waypointTeam;
        payload.sourceOrderRevision = m_parameters[slot].sourceOrderRevision;
        m_waypointPath.store(slot, payload);
        break;
    }
    case AIStateId::AttackPosition:
    case AIStateId::AttackObject:
    case AIStateId::ForceAttackObject:
    case AIStateId::AttackAndFollowObject:
        m_attack.activate(slot, AIStateRequestId{confirmedTick, sequence});
        break;
    case AIStateId::Dock:
    case AIStateId::GetRepaired:
        m_dock.activate(slot, AIStateRequestId{confirmedTick, sequence},
                        state == AIStateId::GetRepaired ? AIDockPurpose::Repair : AIDockPurpose::Dock);
        break;
    case AIStateId::RappelInto:
    case AIStateId::CombatDrop:
        m_insertion.activate(slot, {confirmedTick, sequence});
        break;
    case AIStateId::MoveAndEvacuate:
    case AIStateId::MoveAndEvacuateAndExit:
    case AIStateId::MoveAndDelete:
        m_moveTo.activate(slot, AIStateRequestId{confirmedTick, sequence});
        m_moveEvacuate.setOrigin(slot, {});
        m_moveEvacuate.setAppendDeleteGoal(slot, false);
        break;
    case AIStateId::FollowPath:
    case AIStateId::FollowExitProductionPath:
        m_followPath.activate(slot, AIStateRequestId{confirmedTick, sequence});
        break;
    case AIStateId::FollowWaypointPathAsTeam:
    case AIStateId::FollowWaypointPathAsIndividuals:
    case AIStateId::FollowWaypointPathAsTeamExact:
    case AIStateId::FollowWaypointPathAsIndividualsExact:
        m_waypointPath.activate(slot, AIStateRequestId{confirmedTick, sequence});
        break;
    case AIStateId::Wander:
    case AIStateId::Panic:
        m_waypointPath.activate(slot, AIStateRequestId{confirmedTick, sequence});
        m_approachPath.activate(slot, AIStateRequestId{confirmedTick, sequence});
        break;
    case AIStateId::MoveOutOfTheWay:
        m_moveOutOfWay.activate(slot, AIStateRequestId{confirmedTick, sequence});
        break;
    case AIStateId::MoveAndTighten:
    case AIStateId::MoveAwayFromRepulsors:
    case AIStateId::WanderInPlace:
        m_approachPath.activate(slot, AIStateRequestId{confirmedTick, sequence});
        break;
    default:
        break;
    }
}

} // namespace engine::ai
