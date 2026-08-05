#include "game/object/ai/runtime/AIStateSoAParity.h"

#include <variant>

namespace engine::ai
{
namespace
{

template <typename T>
constexpr void compareField(AIStateParityResult& result,
                            const T& oracle,
                            const T& candidate,
                            AIStateParityMismatch mismatch) noexcept
{
    if (oracle != candidate)
        result.add(mismatch);
}

void comparePosition(AIStateParityResult& result,
                     const AIFixedPosition& oracle,
                     const AIFixedPosition& candidate,
                     AIStateParityMismatch x,
                     AIStateParityMismatch y,
                     AIStateParityMismatch z) noexcept
{
    compareField(result, oracle.xRaw, candidate.xRaw, x);
    compareField(result, oracle.yRaw, candidate.yRaw, y);
    compareField(result, oracle.zRaw, candidate.zRaw, z);
}

} // namespace

AIStateParityResult compareAIStateMachineRuntime(const AIStateMachineRuntime& oracle,
                                                 const AIStateMachineRuntime& candidate) noexcept
{
    AIStateParityResult result;
    compareField(result, oracle.currentState, candidate.currentState,
                 AIStateParityMismatch::RuntimeCurrentState);
    compareField(result, oracle.previousState, candidate.previousState,
                 AIStateParityMismatch::RuntimePreviousState);
    compareField(result, oracle.defaultState, candidate.defaultState,
                 AIStateParityMismatch::RuntimeDefaultState);
    compareField(result, oracle.temporaryResumeState, candidate.temporaryResumeState,
                 AIStateParityMismatch::RuntimeTemporaryResumeState);
    compareField(result, oracle.enteredTick, candidate.enteredTick,
                 AIStateParityMismatch::RuntimeEnteredTick);
    compareField(result, oracle.wakeTick, candidate.wakeTick,
                 AIStateParityMismatch::RuntimeWakeTick);
    compareField(result, oracle.temporaryEndTickExclusive, candidate.temporaryEndTickExclusive,
                 AIStateParityMismatch::RuntimeTemporaryEndTickExclusive);
    compareField(result, oracle.revision, candidate.revision,
                 AIStateParityMismatch::RuntimeRevision);
    compareField(result, oracle.transitionBudgetTick, candidate.transitionBudgetTick,
                 AIStateParityMismatch::RuntimeTransitionBudgetTick);
    compareField(result, oracle.substateDomain, candidate.substateDomain,
                 AIStateParityMismatch::RuntimeSubstateDomain);
    compareField(result, oracle.substate, candidate.substate,
                 AIStateParityMismatch::RuntimeSubstate);
    compareField(result, oracle.lock, candidate.lock,
                 AIStateParityMismatch::RuntimeLock);
    compareField(result, oracle.lastWakeReason, candidate.lastWakeReason,
                 AIStateParityMismatch::RuntimeLastWakeReason);
    compareField(result, oracle.lastTransitionReason, candidate.lastTransitionReason,
                 AIStateParityMismatch::RuntimeLastTransitionReason);
    compareField(result, oracle.transitionsThisTick, candidate.transitionsThisTick,
                 AIStateParityMismatch::RuntimeTransitionsThisTick);
    compareField(result, oracle.initialized, candidate.initialized,
                 AIStateParityMismatch::RuntimeInitialized);
    compareField(result, oracle.temporaryActive, candidate.temporaryActive,
                 AIStateParityMismatch::RuntimeTemporaryActive);
    compareField(result, oracle.transitionLimitExceeded, candidate.transitionLimitExceeded,
                 AIStateParityMismatch::RuntimeTransitionLimitExceeded);
    return result;
}

AIStateParityResult compareAIStateDataToSoASlot(const AIStateData& oracle,
                                                const AIStateFamilySoAStorage& storage,
                                                size_t slot) noexcept
{
    AIStateParityResult result;
    if (slot >= storage.size())
    {
        result.add(AIStateParityMismatch::SlotOutOfRange);
        return result;
    }

    const AIStateParameters& candidate = storage.parameters()[slot];
    compareField(result, oracle.parameters.waitEndTick, candidate.waitEndTick,
                 AIStateParityMismatch::ParameterWaitEndTick);
    compareField(result, oracle.parameters.goalObject, candidate.goalObject,
                 AIStateParityMismatch::ParameterGoalObject);
    comparePosition(result, oracle.parameters.goalPosition, candidate.goalPosition,
                    AIStateParityMismatch::ParameterGoalPositionX,
                    AIStateParityMismatch::ParameterGoalPositionY,
                    AIStateParityMismatch::ParameterGoalPositionZ);
    compareField(result, oracle.parameters.ignoredObstacle, candidate.ignoredObstacle,
                 AIStateParityMismatch::ParameterIgnoredObstacle);
    compareField(result, oracle.parameters.sourceOrderRevision, candidate.sourceOrderRevision,
                 AIStateParityMismatch::ParameterSourceOrderRevision);
    compareField(result, oracle.parameters.pathSurfaceMask, candidate.pathSurfaceMask,
                 AIStateParityMismatch::ParameterPathSurfaceMask);
    compareField(result, oracle.parameters.arrivalRadiusRaw, candidate.arrivalRadiusRaw,
                 AIStateParityMismatch::ParameterArrivalRadiusRaw);
    compareField(result, oracle.parameters.hasGoalPosition, candidate.hasGoalPosition,
                 AIStateParityMismatch::ParameterHasGoalPosition);
    compareField(result, oracle.parameters.adjustDestinations, candidate.adjustDestinations,
                 AIStateParityMismatch::ParameterAdjustDestinations);
    compareField(result, oracle.parameters.pathSequence, candidate.pathSequence,
                 AIStateParityMismatch::ParameterPathSequence);
    compareField(result, oracle.parameters.pathSequenceRevision, candidate.pathSequenceRevision,
                 AIStateParityMismatch::ParameterPathSequenceRevision);
    compareField(result, oracle.parameters.waypoint, candidate.waypoint,
                 AIStateParityMismatch::ParameterWaypoint);
    compareField(result, oracle.parameters.waypointGraphRevision, candidate.waypointGraphRevision,
                 AIStateParityMismatch::ParameterWaypointGraphRevision);
    compareField(result, oracle.parameters.waypointTeam, candidate.waypointTeam,
                 AIStateParityMismatch::ParameterWaypointTeam);
    comparePosition(result, oracle.parameters.waypointGroupOffset,
                    candidate.waypointGroupOffset,
                    AIStateParityMismatch::ParameterWaypointGroupOffsetX,
                    AIStateParityMismatch::ParameterWaypointGroupOffsetY,
                    AIStateParityMismatch::ParameterWaypointGroupOffsetZ);
    compareField(result, oracle.parameters.waypointGroupSpeedRaw,
                 candidate.waypointGroupSpeedRaw,
                 AIStateParityMismatch::ParameterWaypointGroupSpeed);
    compareField(result, oracle.parameters.existingPath, candidate.existingPath,
                 AIStateParityMismatch::ParameterExistingPath);
    compareField(result, oracle.payloadState, storage.payloadStates()[slot],
                 AIStateParityMismatch::PayloadState);
    compareField(result, oracle.activationSequence, storage.activationSequences()[slot],
                 AIStateParityMismatch::ActivationSequence);

    if (!detail::payloadTagMatches(oracle))
    {
        result.add(AIStateParityMismatch::PayloadTag);
        return result;
    }

    switch (oracle.payloadState)
    {
    case AIStateId::Idle:
        compareField(result,
                     std::get<AIIdleStatePayload>(oracle.activePayload).nextTargetScanTick,
                     storage.idle()[slot].nextTargetScanTick,
                     AIStateParityMismatch::IdleNextTargetScanTick);
        break;
    case AIStateId::Wait:
        compareField(result,
                     std::get<AIWaitStatePayload>(oracle.activePayload).endTick,
                     storage.wait()[slot].endTick,
                     AIStateParityMismatch::WaitEndTick);
        break;
    case AIStateId::Busy:
        compareField(result,
                     std::get<AIBusyStatePayload>(oracle.activePayload).enteredTick,
                     storage.busy()[slot].enteredTick,
                     AIStateParityMismatch::BusyEnteredTick);
        break;
    case AIStateId::Dead:
        compareField(result,
                     std::get<AIDeadStatePayload>(oracle.activePayload).enteredTick,
                     storage.dead()[slot].enteredTick,
                     AIStateParityMismatch::DeadEnteredTick);
        break;
    case AIStateId::FaceObject:
    case AIStateId::FacePosition:
    {
        const AIFaceStatePayload& payload = std::get<AIFaceStatePayload>(oracle.activePayload);
        const AIFaceStatePayload soa = storage.face().load(slot);
        compareField(result, payload.request.issuedTick, soa.request.issuedTick,
                     AIStateParityMismatch::FaceRequestIssuedTick);
        compareField(result, payload.request.sequence, soa.request.sequence,
                     AIStateParityMismatch::FaceRequestSequence);
        compareField(result, payload.commandIssued, soa.commandIssued,
                     AIStateParityMismatch::FaceCommandIssued);
        compareField(result, payload.canTurnInPlace, soa.canTurnInPlace,
                     AIStateParityMismatch::FaceCanTurnInPlace);
        break;
    }
    case AIStateId::MoveTo:
    {
        const AIMoveToStatePayload& payload = std::get<AIMoveToStatePayload>(oracle.activePayload);
        const AIMoveToStatePayload soa = storage.moveTo().load(slot);
        compareField(result, payload.request.issuedTick, soa.request.issuedTick,
                     AIStateParityMismatch::MoveToRequestIssuedTick);
        compareField(result, payload.request.sequence, soa.request.sequence,
                     AIStateParityMismatch::MoveToRequestSequence);
        comparePosition(result, payload.resolvedGoal, soa.resolvedGoal,
                        AIStateParityMismatch::MoveToResolvedGoalX,
                        AIStateParityMismatch::MoveToResolvedGoalY,
                        AIStateParityMismatch::MoveToResolvedGoalZ);
        comparePosition(result, payload.adjustedGoal, soa.adjustedGoal,
                        AIStateParityMismatch::MoveToAdjustedGoalX,
                        AIStateParityMismatch::MoveToAdjustedGoalY,
                        AIStateParityMismatch::MoveToAdjustedGoalZ);
        compareField(result, payload.path, soa.path, AIStateParityMismatch::MoveToPath);
        compareField(result, payload.sourceOrderRevision, soa.sourceOrderRevision,
                     AIStateParityMismatch::MoveToSourceOrderRevision);
        compareField(result, payload.generation, soa.generation,
                     AIStateParityMismatch::MoveToGeneration);
        compareField(result, payload.adjustedLayer, soa.adjustedLayer,
                     AIStateParityMismatch::MoveToAdjustedLayer);
        compareField(result, payload.phase, soa.phase, AIStateParityMismatch::MoveToPhase);
        compareField(result, payload.pathRequestIssued, soa.pathRequestIssued,
                     AIStateParityMismatch::MoveToPathRequestIssued);
        compareField(result, payload.adjustDestinations, soa.adjustDestinations,
                     AIStateParityMismatch::MoveToAdjustDestinations);
        break;
    }
    case AIStateId::PickUpCrate:
    {
        const AIPickUpCrateStatePayload& payload =
            std::get<AIPickUpCrateStatePayload>(oracle.activePayload);
        const AIPickUpCrateStatePayload soa{
            storage.moveTo().load(slot), storage.pickUpCrateDelayUpdates()[slot]};
        compareField(result, payload, soa, AIStateParityMismatch::PickUpCratePayload);
        break;
    }
    case AIStateId::MoveAndEvacuate:
    case AIStateId::MoveAndEvacuateAndExit:
    case AIStateId::MoveAndDelete:
    {
        const AIMoveEvacuateStatePayload& payload =
            std::get<AIMoveEvacuateStatePayload>(oracle.activePayload);
        const AIMoveEvacuateStatePayload soa{
            storage.moveTo().load(slot),
            storage.moveEvacuate().origin(slot),
            storage.moveEvacuate().appendDeleteGoal(slot)};
        compareField(result, payload, soa, AIStateParityMismatch::MoveEvacuatePayload);
        break;
    }
    case AIStateId::Enter:
    case AIStateId::Exit:
    case AIStateId::ExitInstantly:
    {
        const AIContainmentStatePayload& payload =
            std::get<AIContainmentStatePayload>(oracle.activePayload);
        AIContainmentStatePayload soa{
            AIStateRequestId{storage.containmentRequestTick()[slot],
                             storage.containmentRequestSequence()[slot]}};
        soa.trackedGoal = storage.containmentTrackedGoal()[slot];
        soa.entryToClear = storage.containmentEntryToClear()[slot];
        soa.phase = storage.containmentPhase()[slot];
        compareField(result, payload, soa, AIStateParityMismatch::ContainmentPayload);
        break;
    }
    case AIStateId::HackInternet:
    {
        const AIHackInternetStatePayload& payload =
            std::get<AIHackInternetStatePayload>(oracle.activePayload);
        const auto& columns = storage.hackInternet();
        AIHackInternetStatePayload soa{columns.request(slot)};
        soa.profile = columns.profile(slot);
        soa.sourceOrderRevision = columns.sourceRevision(slot);
        soa.profileRevision = columns.profileRevision(slot);
        soa.phaseEndTick = columns.phaseEndTick(slot);
        soa.nextPayoutTick = columns.nextPayoutTick(slot);
        soa.deferredOrderRevision = columns.deferredOrderRevision(slot);
        soa.phase = columns.phase(slot);
        compareField(result, payload, soa, AIStateParityMismatch::HackInternetPayload);
        break;
    }
    case AIStateId::Guard:
    case AIStateId::GuardRetaliate:
    case AIStateId::GuardTunnelNetwork:
        compareField(result, std::get<AIGuardStatePayload>(oracle.activePayload),
                     storage.guard().load(slot), AIStateParityMismatch::GuardPayload);
        break;
    case AIStateId::Hunt:
    case AIStateId::AttackSquad:
    case AIStateId::AttackArea:
        compareField(result, std::get<AITacticalAttackStatePayload>(oracle.activePayload),
                     storage.tacticalAttack().load(slot),
                     AIStateParityMismatch::TacticalAttackPayload);
        break;
    case AIStateId::AttackMoveTo:
    case AIStateId::AttackFollowWaypointPathAsIndividuals:
    case AIStateId::AttackFollowWaypointPathAsTeam:
        compareField(result,
                     std::get<AIOpportunityAttackMoveStatePayload>(oracle.activePayload),
                     storage.opportunityAttackMove().load(slot),
                     AIStateParityMismatch::OpportunityAttackMovePayload);
        break;
    case AIStateId::AttackPosition:
    case AIStateId::AttackObject:
    case AIStateId::ForceAttackObject:
    case AIStateId::AttackAndFollowObject:
        compareField(result, std::get<AIAttackStatePayload>(oracle.activePayload),
                     storage.attack().load(slot), AIStateParityMismatch::AttackPayload);
        break;
    case AIStateId::Dock:
    case AIStateId::GetRepaired:
        compareField(result, std::get<AIDockStatePayload>(oracle.activePayload),
                     storage.dock().load(slot), AIStateParityMismatch::DockPayload);
        break;
    case AIStateId::RappelInto:
    case AIStateId::CombatDrop:
        compareField(result, std::get<AIInsertionStatePayload>(oracle.activePayload),
                     storage.insertion().load(slot), AIStateParityMismatch::InsertionPayload);
        break;
    case AIStateId::FollowPath:
    case AIStateId::FollowExitProductionPath:
        compareField(result, std::get<AIFollowPathStatePayload>(oracle.activePayload),
                     storage.followPath().load(slot), AIStateParityMismatch::FollowPathPayload);
        break;
    case AIStateId::FollowWaypointPathAsTeam:
    case AIStateId::FollowWaypointPathAsIndividuals:
    case AIStateId::FollowWaypointPathAsTeamExact:
    case AIStateId::FollowWaypointPathAsIndividualsExact:
        compareField(result, std::get<AIWaypointPathStatePayload>(oracle.activePayload),
                     storage.waypointPath().load(slot),
                     AIStateParityMismatch::WaypointPathPayload);
        break;
    case AIStateId::Wander:
    case AIStateId::Panic:
    {
        const AIWanderPanicStatePayload& payload =
            std::get<AIWanderPanicStatePayload>(oracle.activePayload);
        const AIWanderPanicStatePayload soa{
            storage.waypointPath().load(slot), storage.approachPath().load(slot)};
        compareField(result, payload, soa, AIStateParityMismatch::WanderPanicPayload);
        break;
    }
    case AIStateId::MoveOutOfTheWay:
        compareField(result, std::get<AIMoveOutOfWayStatePayload>(oracle.activePayload),
                     storage.moveOutOfWay().load(slot),
                     AIStateParityMismatch::MoveOutOfWayPayload);
        break;
    case AIStateId::MoveAndTighten:
    case AIStateId::MoveAwayFromRepulsors:
    case AIStateId::WanderInPlace:
        compareField(result, std::get<AIApproachPathStatePayload>(oracle.activePayload),
                     storage.approachPath().load(slot),
                     AIStateParityMismatch::ApproachPathPayload);
        break;
    default:
        break;
    }
    return result;
}

AIStateParityResult compareAIStateExecutorParity(const AIStateMachineRuntime& oracleRuntime,
                                                 const AIStateData& oracleData,
                                                 const AIStateFamilySoAStorage& storage,
                                                 size_t slot) noexcept
{
    if (slot >= storage.size())
    {
        AIStateParityResult result;
        result.add(AIStateParityMismatch::SlotOutOfRange);
        return result;
    }
    AIStateParityResult result = compareAIStateMachineRuntime(oracleRuntime, storage.runtimes()[slot]);
    result.merge(compareAIStateDataToSoASlot(oracleData, storage, slot));
    return result;
}

} // namespace engine::ai
