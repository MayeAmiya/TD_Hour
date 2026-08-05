#include "game/object/ai/runtime/AIStateSoAParity.h"

#include <variant>

namespace engine::ai
{

AIStateSoABridgeResult writeAIStateDataToSoASlot(const AIStateData& oracle,
                                                 AIStateFamilySoAStorage& storage,
                                                 size_t slot) noexcept
{
    AIStateSoABridgeResult result;
    if (slot >= storage.size())
        result.add(AIStateSoABridgeError::SlotOutOfRange);
    if (!detail::payloadTagMatches(oracle))
        result.add(AIStateSoABridgeError::PayloadTagMismatch);
    if (!result.succeeded())
        return result;

    if (!storage.restorePayloadMetadata(slot, oracle.payloadState, oracle.activationSequence))
    {
        result.add(AIStateSoABridgeError::PayloadMetadataRejected);
        return result;
    }

    storage.parameters()[slot] = oracle.parameters;

    switch (oracle.payloadState)
    {
    case AIStateId::Idle:
        storage.idle()[slot] = std::get<AIIdleStatePayload>(oracle.activePayload);
        break;
    case AIStateId::Wait:
        storage.wait()[slot] = std::get<AIWaitStatePayload>(oracle.activePayload);
        break;
    case AIStateId::Busy:
        storage.busy()[slot] = std::get<AIBusyStatePayload>(oracle.activePayload);
        break;
    case AIStateId::Dead:
        storage.dead()[slot] = std::get<AIDeadStatePayload>(oracle.activePayload);
        break;
    case AIStateId::FaceObject:
    case AIStateId::FacePosition:
        storage.face().store(slot, std::get<AIFaceStatePayload>(oracle.activePayload));
        break;
    case AIStateId::MoveTo:
        storage.moveTo().store(slot, std::get<AIMoveToStatePayload>(oracle.activePayload));
        break;
    case AIStateId::PickUpCrate:
    {
        const AIPickUpCrateStatePayload& payload =
            std::get<AIPickUpCrateStatePayload>(oracle.activePayload);
        storage.moveTo().store(slot, payload.movement);
        storage.pickUpCrateDelayUpdates()[slot] = payload.delayUpdatesRemaining;
        break;
    }
    case AIStateId::MoveAndEvacuate:
    case AIStateId::MoveAndEvacuateAndExit:
    case AIStateId::MoveAndDelete:
    {
        const AIMoveEvacuateStatePayload& payload =
            std::get<AIMoveEvacuateStatePayload>(oracle.activePayload);
        storage.moveTo().store(slot, payload.movement);
        storage.moveEvacuate().setOrigin(slot, payload.origin);
        storage.moveEvacuate().setAppendDeleteGoal(slot, payload.appendDeleteGoal);
        break;
    }
    case AIStateId::Enter:
    case AIStateId::Exit:
    case AIStateId::ExitInstantly:
    {
        const AIContainmentStatePayload& payload =
            std::get<AIContainmentStatePayload>(oracle.activePayload);
        storage.containmentRequestTick()[slot] = payload.request.issuedTick;
        storage.containmentRequestSequence()[slot] = payload.request.sequence;
        storage.containmentTrackedGoal()[slot] = payload.trackedGoal;
        storage.containmentEntryToClear()[slot] = payload.entryToClear;
        storage.containmentPhase()[slot] = payload.phase;
        break;
    }
    case AIStateId::HackInternet:
    {
        const AIHackInternetStatePayload& payload =
            std::get<AIHackInternetStatePayload>(oracle.activePayload);
        auto& columns = storage.hackInternet();
        columns.activate(slot, payload.request);
        columns.setProfile(slot, payload.profile, payload.profileRevision);
        columns.setSourceRevision(slot, payload.sourceOrderRevision);
        columns.setPhaseEndTick(slot, payload.phaseEndTick);
        columns.setNextPayoutTick(slot, payload.nextPayoutTick);
        columns.setDeferredOrderRevision(slot, payload.deferredOrderRevision);
        columns.setPhase(slot, payload.phase);
        break;
    }
    case AIStateId::Guard:
    case AIStateId::GuardRetaliate:
    case AIStateId::GuardTunnelNetwork:
        storage.guard().store(slot, std::get<AIGuardStatePayload>(oracle.activePayload));
        break;
    case AIStateId::Hunt:
    case AIStateId::AttackSquad:
    case AIStateId::AttackArea:
        storage.tacticalAttack().store(
            slot, std::get<AITacticalAttackStatePayload>(oracle.activePayload));
        break;
    case AIStateId::AttackMoveTo:
    case AIStateId::AttackFollowWaypointPathAsIndividuals:
    case AIStateId::AttackFollowWaypointPathAsTeam:
        storage.opportunityAttackMove().store(
            slot, std::get<AIOpportunityAttackMoveStatePayload>(oracle.activePayload));
        break;
    case AIStateId::AttackPosition:
    case AIStateId::AttackObject:
    case AIStateId::ForceAttackObject:
    case AIStateId::AttackAndFollowObject:
        storage.attack().store(slot, std::get<AIAttackStatePayload>(oracle.activePayload));
        break;
    case AIStateId::Dock:
    case AIStateId::GetRepaired:
        storage.dock().store(slot, std::get<AIDockStatePayload>(oracle.activePayload));
        break;
    case AIStateId::RappelInto:
    case AIStateId::CombatDrop:
        storage.insertion().store(slot, std::get<AIInsertionStatePayload>(oracle.activePayload));
        break;
    case AIStateId::FollowPath:
    case AIStateId::FollowExitProductionPath:
        storage.followPath().store(slot, std::get<AIFollowPathStatePayload>(oracle.activePayload));
        break;
    case AIStateId::FollowWaypointPathAsTeam:
    case AIStateId::FollowWaypointPathAsIndividuals:
    case AIStateId::FollowWaypointPathAsTeamExact:
    case AIStateId::FollowWaypointPathAsIndividualsExact:
        storage.waypointPath().store(
            slot, std::get<AIWaypointPathStatePayload>(oracle.activePayload));
        break;
    case AIStateId::Wander:
    case AIStateId::Panic:
    {
        const AIWanderPanicStatePayload& payload =
            std::get<AIWanderPanicStatePayload>(oracle.activePayload);
        storage.waypointPath().store(slot, payload.movement);
        storage.approachPath().store(slot, payload.scan);
        break;
    }
    case AIStateId::MoveOutOfTheWay:
        storage.moveOutOfWay().store(
            slot, std::get<AIMoveOutOfWayStatePayload>(oracle.activePayload));
        break;
    case AIStateId::MoveAndTighten:
    case AIStateId::MoveAwayFromRepulsors:
    case AIStateId::WanderInPlace:
        storage.approachPath().store(
            slot, std::get<AIApproachPathStatePayload>(oracle.activePayload));
        break;
    default:
        break;
    }
    return result;
}

AIStateSoABridgeResult rebuildAIStateDataFromSoASlot(const AIStateFamilySoAStorage& storage,
                                                     size_t slot,
                                                     AIStateData& output) noexcept
{
    AIStateSoABridgeResult result;
    if (slot >= storage.size())
    {
        result.add(AIStateSoABridgeError::SlotOutOfRange);
        return result;
    }

    AIStateData rebuilt;
    rebuilt.parameters = storage.parameters()[slot];
    rebuilt.payloadState = storage.payloadStates()[slot];
    rebuilt.activationSequence = storage.activationSequences()[slot];
    switch (rebuilt.payloadState)
    {
    case AIStateId::Idle:
        rebuilt.activePayload = storage.idle()[slot];
        break;
    case AIStateId::Wait:
        rebuilt.activePayload = storage.wait()[slot];
        break;
    case AIStateId::Busy:
        rebuilt.activePayload = storage.busy()[slot];
        break;
    case AIStateId::Dead:
        rebuilt.activePayload = storage.dead()[slot];
        break;
    case AIStateId::FaceObject:
    case AIStateId::FacePosition:
        rebuilt.activePayload = storage.face().load(slot);
        break;
    case AIStateId::MoveTo:
        rebuilt.activePayload = storage.moveTo().load(slot);
        break;
    case AIStateId::PickUpCrate:
        rebuilt.activePayload = AIPickUpCrateStatePayload{
            storage.moveTo().load(slot), storage.pickUpCrateDelayUpdates()[slot]};
        break;
    case AIStateId::MoveAndEvacuate:
    case AIStateId::MoveAndEvacuateAndExit:
    case AIStateId::MoveAndDelete:
        rebuilt.activePayload = AIMoveEvacuateStatePayload{
            storage.moveTo().load(slot),
            storage.moveEvacuate().origin(slot),
            storage.moveEvacuate().appendDeleteGoal(slot)};
        break;
    case AIStateId::Enter:
    case AIStateId::Exit:
    case AIStateId::ExitInstantly:
    {
        AIContainmentStatePayload payload{
            AIStateRequestId{storage.containmentRequestTick()[slot],
                             storage.containmentRequestSequence()[slot]}};
        payload.trackedGoal = storage.containmentTrackedGoal()[slot];
        payload.entryToClear = storage.containmentEntryToClear()[slot];
        payload.phase = storage.containmentPhase()[slot];
        rebuilt.activePayload = payload;
        break;
    }
    case AIStateId::HackInternet:
    {
        const auto& columns = storage.hackInternet();
        AIHackInternetStatePayload payload{columns.request(slot)};
        payload.profile = columns.profile(slot);
        payload.sourceOrderRevision = columns.sourceRevision(slot);
        payload.profileRevision = columns.profileRevision(slot);
        payload.phaseEndTick = columns.phaseEndTick(slot);
        payload.nextPayoutTick = columns.nextPayoutTick(slot);
        payload.deferredOrderRevision = columns.deferredOrderRevision(slot);
        payload.phase = columns.phase(slot);
        rebuilt.activePayload = payload;
        break;
    }
    case AIStateId::Guard:
    case AIStateId::GuardRetaliate:
    case AIStateId::GuardTunnelNetwork:
        rebuilt.activePayload = storage.guard().load(slot);
        break;
    case AIStateId::Hunt:
    case AIStateId::AttackSquad:
    case AIStateId::AttackArea:
        rebuilt.activePayload = storage.tacticalAttack().load(slot);
        break;
    case AIStateId::AttackMoveTo:
    case AIStateId::AttackFollowWaypointPathAsIndividuals:
    case AIStateId::AttackFollowWaypointPathAsTeam:
        rebuilt.activePayload = storage.opportunityAttackMove().load(slot);
        break;
    case AIStateId::FollowPath:
    case AIStateId::FollowExitProductionPath:
        rebuilt.activePayload = storage.followPath().load(slot);
        break;
    case AIStateId::FollowWaypointPathAsTeam:
    case AIStateId::FollowWaypointPathAsIndividuals:
    case AIStateId::FollowWaypointPathAsTeamExact:
    case AIStateId::FollowWaypointPathAsIndividualsExact:
        rebuilt.activePayload = storage.waypointPath().load(slot);
        break;
    case AIStateId::Wander:
    case AIStateId::Panic:
        rebuilt.activePayload = AIWanderPanicStatePayload{
            storage.waypointPath().load(slot), storage.approachPath().load(slot)};
        break;
    case AIStateId::MoveOutOfTheWay:
        rebuilt.activePayload = storage.moveOutOfWay().load(slot);
        break;
    case AIStateId::MoveAndTighten:
    case AIStateId::MoveAwayFromRepulsors:
    case AIStateId::WanderInPlace:
        rebuilt.activePayload = storage.approachPath().load(slot);
        break;
    case AIStateId::AttackPosition:
    case AIStateId::AttackObject:
    case AIStateId::ForceAttackObject:
    case AIStateId::AttackAndFollowObject:
        rebuilt.activePayload = storage.attack().load(slot);
        break;
    case AIStateId::Dock:
    case AIStateId::GetRepaired:
        rebuilt.activePayload = storage.dock().load(slot);
        break;
    case AIStateId::RappelInto:
    case AIStateId::CombatDrop:
        rebuilt.activePayload = storage.insertion().load(slot);
        break;
    default:
        rebuilt.activePayload.emplace<std::monostate>();
        break;
    }
    output = rebuilt;
    return result;
}

} // namespace engine::ai
