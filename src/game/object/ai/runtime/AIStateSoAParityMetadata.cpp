#include "game/object/ai/runtime/AIStateSoAParity.h"

#include <variant>

namespace engine::ai::detail
{

bool payloadTagMatches(const AIStateData& data) noexcept
{
    switch (data.payloadState)
    {
    case AIStateId::Idle:
        return std::holds_alternative<AIIdleStatePayload>(data.activePayload);
    case AIStateId::Wait:
        return std::holds_alternative<AIWaitStatePayload>(data.activePayload);
    case AIStateId::Busy:
        return std::holds_alternative<AIBusyStatePayload>(data.activePayload);
    case AIStateId::Dead:
        return std::holds_alternative<AIDeadStatePayload>(data.activePayload);
    case AIStateId::FaceObject:
    case AIStateId::FacePosition:
        return std::holds_alternative<AIFaceStatePayload>(data.activePayload);
    case AIStateId::MoveTo:
        return std::holds_alternative<AIMoveToStatePayload>(data.activePayload);
    case AIStateId::PickUpCrate:
        return std::holds_alternative<AIPickUpCrateStatePayload>(data.activePayload);
    case AIStateId::MoveAndEvacuate:
    case AIStateId::MoveAndEvacuateAndExit:
    case AIStateId::MoveAndDelete:
        return std::holds_alternative<AIMoveEvacuateStatePayload>(data.activePayload);
    case AIStateId::Enter:
    case AIStateId::Exit:
    case AIStateId::ExitInstantly:
        return std::holds_alternative<AIContainmentStatePayload>(data.activePayload);
    case AIStateId::HackInternet:
        return std::holds_alternative<AIHackInternetStatePayload>(data.activePayload);
    case AIStateId::Guard:
    case AIStateId::GuardRetaliate:
    case AIStateId::GuardTunnelNetwork:
        return std::holds_alternative<AIGuardStatePayload>(data.activePayload);
    case AIStateId::Hunt:
    case AIStateId::AttackSquad:
    case AIStateId::AttackArea:
        return std::holds_alternative<AITacticalAttackStatePayload>(data.activePayload);
    case AIStateId::AttackMoveTo:
    case AIStateId::AttackFollowWaypointPathAsIndividuals:
    case AIStateId::AttackFollowWaypointPathAsTeam:
        return std::holds_alternative<AIOpportunityAttackMoveStatePayload>(data.activePayload);
    case AIStateId::AttackPosition:
    case AIStateId::AttackObject:
    case AIStateId::ForceAttackObject:
    case AIStateId::AttackAndFollowObject:
        return std::holds_alternative<AIAttackStatePayload>(data.activePayload);
    case AIStateId::Dock:
    case AIStateId::GetRepaired:
        return std::holds_alternative<AIDockStatePayload>(data.activePayload);
    case AIStateId::RappelInto:
    case AIStateId::CombatDrop:
        return std::holds_alternative<AIInsertionStatePayload>(data.activePayload);
    case AIStateId::FollowPath:
    case AIStateId::FollowExitProductionPath:
        return std::holds_alternative<AIFollowPathStatePayload>(data.activePayload);
    case AIStateId::FollowWaypointPathAsTeam:
    case AIStateId::FollowWaypointPathAsIndividuals:
    case AIStateId::FollowWaypointPathAsTeamExact:
    case AIStateId::FollowWaypointPathAsIndividualsExact:
        return std::holds_alternative<AIWaypointPathStatePayload>(data.activePayload);
    case AIStateId::Wander:
    case AIStateId::Panic:
        return std::holds_alternative<AIWanderPanicStatePayload>(data.activePayload);
    case AIStateId::MoveOutOfTheWay:
        return std::holds_alternative<AIMoveOutOfWayStatePayload>(data.activePayload);
    case AIStateId::MoveAndTighten:
    case AIStateId::MoveAwayFromRepulsors:
    case AIStateId::WanderInPlace:
        return std::holds_alternative<AIApproachPathStatePayload>(data.activePayload);
    default:
        return std::holds_alternative<std::monostate>(data.activePayload);
    }
}

} // namespace engine::ai::detail
