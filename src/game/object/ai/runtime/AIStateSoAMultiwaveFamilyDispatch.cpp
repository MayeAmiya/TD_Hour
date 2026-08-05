#include "game/object/ai/runtime/AIStateSoAMultiwaveFamilyDispatch.h"

#include <algorithm>
#include <limits>

namespace engine::ai::detail
{

[[nodiscard]] bool dispatchSoAUpdates(AIStateFamilySoAStorage& storage,
                                             const AIStateSoAMultiwaveInput& input,
                                             container::Span<const uint8_t> mask,
                                             container::Span<AIStateStepResult> results) noexcept
{
    if (!storage.prepareExecutionSlots(mask))
        return false;
    AICoreStateSoAKernelInput core = input.core;
    core.confirmedTick = input.confirmedTick;
    core.scheduled = mask;
    core.results = results;
    AIFacingStateSoAKernelInput facing = input.facing;
    facing.confirmedTick = input.confirmedTick;
    facing.scheduled = mask;
    facing.results = results;
    AIMoveStateSoAKernelInput move = input.move;
    move.confirmedTick = input.confirmedTick;
    move.scheduled = mask;
    move.results = results;
    AIFollowPathStateSoAKernelInput followPath = input.followPath;
    followPath.confirmedTick = input.confirmedTick;
    followPath.scheduled = mask;
    followPath.results = results;
    AIWaypointStateSoAKernelInput waypoint = input.waypoint;
    waypoint.confirmedTick = input.confirmedTick;
    waypoint.scheduled = mask;
    waypoint.results = results;
    AIMoveOutOfWayStateSoAKernelInput moveOut = input.moveOutOfWay;
    moveOut.confirmedTick = input.confirmedTick; moveOut.scheduled = mask; moveOut.results = results;
    AIMoveAndTightenStateSoAKernelInput tighten = input.tighten;
    tighten.confirmedTick=input.confirmedTick;tighten.scheduled=mask;tighten.results=results;
    AIRepulsorStateSoAKernelInput repulsor = input.repulsor;
    repulsor.confirmedTick = input.confirmedTick; repulsor.scheduled = mask; repulsor.results = results;
    AIWanderPanicStateSoAKernelInput wanderPanic = input.wanderPanic;
    wanderPanic.confirmedTick = input.confirmedTick; wanderPanic.scheduled = mask; wanderPanic.results = results;
    AIPickUpCrateStateSoAKernelInput pickUpCrate = input.pickUpCrate;
    pickUpCrate.confirmedTick = input.confirmedTick; pickUpCrate.scheduled = mask; pickUpCrate.results = results;
    AIMoveEvacuateStateSoAKernelInput moveEvacuate = input.moveEvacuate;
    moveEvacuate.confirmedTick = input.confirmedTick; moveEvacuate.scheduled = mask; moveEvacuate.results = results;
    AIContainmentStateSoAKernelInput containment = input.containment;
    containment.confirmedTick = input.confirmedTick; containment.scheduled = mask;
    containment.executionSlots = storage.executionSlots();
    containment.activeStates = storage.payloadStates(); containment.subjects = storage.subjects();
    containment.results = results;
    AIHackInternetStateSoAKernelInput hackInternet = input.hackInternet;
    hackInternet.confirmedTick = input.confirmedTick; hackInternet.scheduled = mask; hackInternet.results = results;
    AIAttackStateSoAKernelInput attack = input.attack;
    attack.confirmedTick=input.confirmedTick;attack.scheduled=mask;attack.activeStates=storage.payloadStates();
    attack.executionSlots = storage.executionSlots();
    attack.subjects=storage.subjects();attack.results=results;
    AIDockStateSoAKernelInput dock=input.dock;dock.confirmedTick=input.confirmedTick;dock.scheduled=mask;
    dock.executionSlots = storage.executionSlots();
    dock.activeStates=storage.payloadStates();dock.subjects=storage.subjects();dock.results=results;
    AIInsertionStateSoAKernelInput insertion=input.insertion;insertion.confirmedTick=input.confirmedTick;
    insertion.scheduled=mask;insertion.activeStates=storage.payloadStates();insertion.subjects=storage.subjects();
    insertion.executionSlots = storage.executionSlots();
    insertion.results=results;
    AIGuardStateSoAKernelInput guard=input.guard;guard.confirmedTick=input.confirmedTick;guard.scheduled=mask;
    guard.executionSlots = storage.executionSlots();
    guard.subjects=storage.subjects();guard.results=results;
    AITacticalAttackStateSoAKernelInput tactical=input.tacticalAttack;tactical.confirmedTick=input.confirmedTick;
    tactical.scheduled=mask;tactical.subjects=storage.subjects();tactical.results=results;
    tactical.executionSlots = storage.executionSlots();
    AIOpportunityAttackMoveStateSoAKernelInput opportunity=input.opportunityAttackMove;
    opportunity.confirmedTick=input.confirmedTick;opportunity.scheduled=mask;opportunity.states=storage.payloadStates();
    opportunity.executionSlots = storage.executionSlots();
    opportunity.subjects=storage.subjects();opportunity.parameters=storage.parameters();
    opportunity.moveToColumns=&storage.moveTo();opportunity.waypointColumns=&storage.waypointPath();
    opportunity.results=results;
    AIMoveStateSoAKernelInput opportunityMove = move;
    opportunityMove.activeState = AIStateId::AttackMoveTo;
    opportunityMove.results = opportunity.movementResults;
    AIWaypointStateSoAKernelInput opportunityWaypoint = waypoint;
    opportunityWaypoint.results = opportunity.movementResults;
    const bool anyTacticalAttack =
        storage.activeStateCount(AIStateId::Hunt) != 0 ||
        storage.activeStateCount(AIStateId::AttackSquad) != 0 ||
        storage.activeStateCount(AIStateId::AttackArea) != 0;

    if ((storage.activeStateCount(AIStateId::Idle) != 0 && !updateIdleSoA(storage, core)) ||
        (storage.activeStateCount(AIStateId::Wait) != 0 && !updateWaitSoA(storage, core)) ||
        (storage.activeStateCount(AIStateId::Busy) != 0 && !updateBusySoA(storage, core)) ||
        (storage.activeStateCount(AIStateId::Dead) != 0 && !updateDeadSoA(storage, core)) ||
        (storage.activeStateCount(AIStateId::FaceObject) != 0 && !updateFaceObjectSoA(storage, facing)) ||
        (storage.activeStateCount(AIStateId::FacePosition) != 0 && !updateFacePositionSoA(storage, facing)) ||
        (storage.activeStateCount(AIStateId::MoveTo) != 0 && !updateMoveToSoA(storage, move)) ||
        (storage.activeStateCount(AIStateId::FollowPath) != 0 &&
         !updateFollowPathSoA(storage, AIStateId::FollowPath, followPath)) ||
        (storage.activeStateCount(AIStateId::FollowExitProductionPath) != 0 &&
         !updateFollowPathSoA(storage, AIStateId::FollowExitProductionPath, followPath)) ||
        (storage.activeStateCount(AIStateId::FollowWaypointPathAsTeam) != 0 &&
         !updateWaypointPathSoA(storage, AIStateId::FollowWaypointPathAsTeam, waypoint)) ||
        (storage.activeStateCount(AIStateId::FollowWaypointPathAsIndividuals) != 0 &&
         !updateWaypointPathSoA(storage, AIStateId::FollowWaypointPathAsIndividuals, waypoint)) ||
        (storage.activeStateCount(AIStateId::FollowWaypointPathAsTeamExact) != 0 &&
         !updateWaypointPathSoA(storage, AIStateId::FollowWaypointPathAsTeamExact, waypoint)) ||
        (storage.activeStateCount(AIStateId::FollowWaypointPathAsIndividualsExact) != 0 &&
         !updateWaypointPathSoA(storage, AIStateId::FollowWaypointPathAsIndividualsExact, waypoint)) ||
        (storage.activeStateCount(AIStateId::MoveOutOfTheWay) != 0 && !updateMoveOutOfWaySoA(storage, moveOut)) ||
        (storage.activeStateCount(AIStateId::MoveAndTighten) != 0 && !updateMoveAndTightenSoA(storage,tighten)) ||
        (storage.activeStateCount(AIStateId::MoveAwayFromRepulsors) != 0 &&
         !updateMoveAwayFromRepulsorsSoA(storage, repulsor)) ||
        (storage.activeStateCount(AIStateId::WanderInPlace) != 0 &&
         !updateWanderInPlaceSoA(storage, repulsor)) ||
        (storage.activeStateCount(AIStateId::Wander) != 0 &&
         !updateWanderPanicSoA(storage, AIStateId::Wander, wanderPanic)) ||
        (storage.activeStateCount(AIStateId::Panic) != 0 &&
         !updateWanderPanicSoA(storage, AIStateId::Panic, wanderPanic)) ||
        (storage.activeStateCount(AIStateId::PickUpCrate) != 0 &&
         !updatePickUpCrateSoA(storage, pickUpCrate)) ||
        ((storage.activeStateCount(AIStateId::MoveAndEvacuate) != 0 ||
          storage.activeStateCount(AIStateId::MoveAndEvacuateAndExit) != 0 ||
          storage.activeStateCount(AIStateId::MoveAndDelete) != 0) &&
         !updateMoveEvacuateStateSoA(storage, moveEvacuate)) ||
        (storage.activeStateCount(AIStateId::Enter) != 0 &&
         !updateContainmentEnterStateSoA(containmentColumns(storage), containment)) ||
        (storage.activeStateCount(AIStateId::Exit) != 0 &&
         !updateContainmentExitStateSoA(containmentColumns(storage), containment)) ||
        (storage.activeStateCount(AIStateId::ExitInstantly) != 0 &&
         !updateContainmentExitInstantlyStateSoA(containmentColumns(storage), containment)) ||
        (storage.activeStateCount(AIStateId::HackInternet) != 0 &&
         !updateHackInternetSoA(storage, hackInternet)) ||
        ((storage.activeStateCount(AIStateId::AttackPosition)!=0||storage.activeStateCount(AIStateId::AttackObject)!=0||
          storage.activeStateCount(AIStateId::ForceAttackObject)!=0||storage.activeStateCount(AIStateId::AttackAndFollowObject)!=0)&&
         !updateAttackStateSoA(storage.attack().view(),attack)) ||
        ((storage.activeStateCount(AIStateId::Dock)!=0||storage.activeStateCount(AIStateId::GetRepaired)!=0)&&
         !updateDockStateSoA(storage.dock().view(),dock)) ||
        (storage.activeStateCount(AIStateId::RappelInto)!=0&&
         !updateRappelIntoStateSoA(storage.insertion().view(),insertion)) ||
        (storage.activeStateCount(AIStateId::CombatDrop)!=0&&
         !updateCombatDropStateSoA(storage.insertion().view(),insertion)) ||
        ((storage.activeStateCount(AIStateId::Guard)!=0 ||
          storage.activeStateCount(AIStateId::GuardRetaliate)!=0 ||
          storage.activeStateCount(AIStateId::GuardTunnelNetwork)!=0) &&
         (!updateGuardMoveChildren(storage, input, guard, mask) ||
          !updateGuardAttackObjectChildren(storage, input, guard, mask) ||
          !updateGuardPickUpCrateChildren(storage, input, guard, mask) ||
          (storage.activeStateCount(AIStateId::Guard)!=0 &&
           !updateGuardSoA(storage.guard(), AIStateId::Guard, guard)) ||
          (storage.activeStateCount(AIStateId::GuardRetaliate)!=0 &&
           !updateGuardSoA(storage.guard(), AIStateId::GuardRetaliate, guard)) ||
          (storage.activeStateCount(AIStateId::GuardTunnelNetwork)!=0 &&
           !updateGuardSoA(storage.guard(), AIStateId::GuardTunnelNetwork, guard)) ||
          !beginGuardMoveChildren(storage, input, guard, mask) ||
          !beginGuardAttackObjectChildren(storage, input, guard, mask) ||
          !beginGuardPickUpCrateChildren(storage, input, guard, mask))) ||
        (anyTacticalAttack &&
         (!updateTacticalAttackObjectChildren(
              storage, input, tactical, mask) ||
          !updateTacticalPickUpCrateChildren(
              storage, input, tactical, mask))) ||
        (storage.activeStateCount(AIStateId::Hunt)!=0 &&
         !updateTacticalAttackStateSoA(storage.tacticalAttack(), AIStateId::Hunt, tactical)) ||
        (storage.activeStateCount(AIStateId::AttackSquad)!=0 &&
         !updateTacticalAttackStateSoA(storage.tacticalAttack(), AIStateId::AttackSquad, tactical)) ||
        (storage.activeStateCount(AIStateId::AttackArea)!=0 &&
         !updateTacticalAttackStateSoA(storage.tacticalAttack(), AIStateId::AttackArea, tactical)) ||
        (anyTacticalAttack &&
         (!beginTacticalAttackObjectChildren(
              storage, input, tactical, mask) ||
          !beginTacticalPickUpCrateChildren(
              storage, input, tactical, mask))) ||
        (storage.activeStateCount(AIStateId::AttackMoveTo)!=0 &&
         !updateMoveToSoA(storage, opportunityMove)) ||
        (storage.activeStateCount(AIStateId::AttackFollowWaypointPathAsIndividuals)!=0 &&
         !updateWaypointPathSoA(storage,
                                AIStateId::AttackFollowWaypointPathAsIndividuals,
                                opportunityWaypoint)) ||
        (storage.activeStateCount(AIStateId::AttackFollowWaypointPathAsTeam)!=0 &&
         !updateWaypointPathSoA(storage,
                                AIStateId::AttackFollowWaypointPathAsTeam,
                                opportunityWaypoint)) ||
        ((storage.activeStateCount(AIStateId::AttackMoveTo)!=0||
          storage.activeStateCount(AIStateId::AttackFollowWaypointPathAsIndividuals)!=0||
          storage.activeStateCount(AIStateId::AttackFollowWaypointPathAsTeam)!=0)&&
         (!updateOpportunityAttackObjectChildren(storage, input, opportunity, mask) ||
          !updateOpportunityPickUpCrateChildren(storage, input, opportunity, mask) ||
          !updateOpportunityAttackMoveSoA(storage.opportunityAttackMove(), opportunity) ||
          !beginOpportunityAttackObjectChildren(storage, input, opportunity, mask) ||
          !beginOpportunityPickUpCrateChildren(storage, input, opportunity, mask))))
        return false;
    const auto runtimes = storage.runtimes();
    for (const size_t slot : storage.executionSlots())
    {
        if (mask[slot] != 0 && !isImplementedStateSoAState(runtimes[slot].currentState))
            results[slot] = AIStateStepResult::unsupported();
    }
    return true;
}

[[nodiscard]] bool dispatchSoAEnters(AIStateFamilySoAStorage& storage,
                                            const AIStateSoAMultiwaveInput& input,
                                            container::Span<const uint8_t> mask,
                                            container::Span<AIStateStepResult> results) noexcept
{
    if (!storage.prepareExecutionSlots(mask))
        return false;
    AICoreStateSoAKernelInput core = input.core;
    core.confirmedTick = input.confirmedTick;
    core.scheduled = mask;
    core.results = results;
    AIFacingStateSoAKernelInput facing = input.facing;
    facing.confirmedTick = input.confirmedTick;
    facing.scheduled = mask;
    facing.results = results;
    AIMoveStateSoAKernelInput move = input.move;
    move.confirmedTick = input.confirmedTick;
    move.scheduled = mask;
    move.results = results;
    AIFollowPathStateSoAKernelInput followPath = input.followPath;
    followPath.confirmedTick = input.confirmedTick;
    followPath.scheduled = mask;
    followPath.results = results;
    AIWaypointStateSoAKernelInput waypoint = input.waypoint;
    waypoint.confirmedTick = input.confirmedTick;
    waypoint.scheduled = mask;
    waypoint.results = results;
    AIMoveOutOfWayStateSoAKernelInput moveOut = input.moveOutOfWay;
    moveOut.confirmedTick = input.confirmedTick; moveOut.scheduled = mask; moveOut.results = results;
    AIMoveAndTightenStateSoAKernelInput tighten = input.tighten;
    tighten.confirmedTick=input.confirmedTick;tighten.scheduled=mask;tighten.results=results;
    AIRepulsorStateSoAKernelInput repulsor = input.repulsor;
    repulsor.confirmedTick = input.confirmedTick; repulsor.scheduled = mask; repulsor.results = results;
    AIWanderPanicStateSoAKernelInput wanderPanic = input.wanderPanic;
    wanderPanic.confirmedTick = input.confirmedTick; wanderPanic.scheduled = mask; wanderPanic.results = results;
    AIPickUpCrateStateSoAKernelInput pickUpCrate = input.pickUpCrate;
    pickUpCrate.confirmedTick = input.confirmedTick; pickUpCrate.scheduled = mask; pickUpCrate.results = results;
    AIMoveEvacuateStateSoAKernelInput moveEvacuate = input.moveEvacuate;
    moveEvacuate.confirmedTick = input.confirmedTick; moveEvacuate.scheduled = mask; moveEvacuate.results = results;
    AIContainmentStateSoAKernelInput containment = input.containment;
    containment.confirmedTick = input.confirmedTick; containment.scheduled = mask;
    containment.executionSlots = storage.executionSlots();
    containment.activeStates = storage.payloadStates(); containment.subjects = storage.subjects();
    containment.results = results;
    AIHackInternetStateSoAKernelInput hackInternet = input.hackInternet;
    hackInternet.confirmedTick = input.confirmedTick; hackInternet.scheduled = mask; hackInternet.results = results;
    AIAttackStateSoAKernelInput attack = input.attack;
    attack.confirmedTick=input.confirmedTick;attack.scheduled=mask;attack.activeStates=storage.payloadStates();
    attack.executionSlots = storage.executionSlots();
    attack.subjects=storage.subjects();attack.results=results;
    AIDockStateSoAKernelInput dock=input.dock;dock.confirmedTick=input.confirmedTick;dock.scheduled=mask;
    dock.executionSlots = storage.executionSlots();
    dock.activeStates=storage.payloadStates();dock.subjects=storage.subjects();dock.results=results;
    AIInsertionStateSoAKernelInput insertion=input.insertion;insertion.confirmedTick=input.confirmedTick;
    insertion.scheduled=mask;insertion.activeStates=storage.payloadStates();insertion.subjects=storage.subjects();
    insertion.executionSlots = storage.executionSlots();
    insertion.results=results;

    AIGuardStateSoAKernelInput guard=input.guard;guard.confirmedTick=input.confirmedTick;guard.scheduled=mask;
    guard.executionSlots = storage.executionSlots();
    guard.subjects=storage.subjects();guard.results=results;
    AITacticalAttackStateSoAKernelInput tactical=input.tacticalAttack;tactical.confirmedTick=input.confirmedTick;
    tactical.scheduled=mask;tactical.subjects=storage.subjects();tactical.results=results;
    tactical.executionSlots = storage.executionSlots();
    AIOpportunityAttackMoveStateSoAKernelInput opportunity=input.opportunityAttackMove;
    opportunity.confirmedTick=input.confirmedTick;opportunity.scheduled=mask;opportunity.states=storage.payloadStates();
    opportunity.executionSlots = storage.executionSlots();
    opportunity.subjects=storage.subjects();opportunity.parameters=storage.parameters();
    opportunity.moveToColumns=&storage.moveTo();opportunity.waypointColumns=&storage.waypointPath();
    opportunity.results=results;

    if ((storage.activeStateCount(AIStateId::Idle) != 0 && !enterIdleSoA(storage, core)) ||
        (storage.activeStateCount(AIStateId::Wait) != 0 && !enterWaitSoA(storage, core)) ||
        (storage.activeStateCount(AIStateId::Busy) != 0 && !enterBusySoA(storage, core)) ||
        (storage.activeStateCount(AIStateId::Dead) != 0 && !enterDeadSoA(storage, core)) ||
        (storage.activeStateCount(AIStateId::FaceObject) != 0 && !enterFaceObjectSoA(storage, facing)) ||
        (storage.activeStateCount(AIStateId::FacePosition) != 0 && !enterFacePositionSoA(storage, facing)) ||
        (storage.activeStateCount(AIStateId::MoveTo) != 0 && !enterMoveToSoA(storage, move)) ||
        (storage.activeStateCount(AIStateId::FollowPath) != 0 &&
         !enterFollowPathSoA(storage, AIStateId::FollowPath, followPath)) ||
        (storage.activeStateCount(AIStateId::FollowExitProductionPath) != 0 &&
         !enterFollowPathSoA(storage, AIStateId::FollowExitProductionPath, followPath)) ||
        (storage.activeStateCount(AIStateId::FollowWaypointPathAsTeam) != 0 &&
         !enterWaypointPathSoA(storage, AIStateId::FollowWaypointPathAsTeam, waypoint)) ||
        (storage.activeStateCount(AIStateId::FollowWaypointPathAsIndividuals) != 0 &&
         !enterWaypointPathSoA(storage, AIStateId::FollowWaypointPathAsIndividuals, waypoint)) ||
        (storage.activeStateCount(AIStateId::FollowWaypointPathAsTeamExact) != 0 &&
         !enterWaypointPathSoA(storage, AIStateId::FollowWaypointPathAsTeamExact, waypoint)) ||
        (storage.activeStateCount(AIStateId::FollowWaypointPathAsIndividualsExact) != 0 &&
         !enterWaypointPathSoA(storage, AIStateId::FollowWaypointPathAsIndividualsExact, waypoint)) ||
        (storage.activeStateCount(AIStateId::MoveOutOfTheWay) != 0 && !enterMoveOutOfWaySoA(storage, moveOut)) ||
        (storage.activeStateCount(AIStateId::MoveAndTighten) != 0 && !enterMoveAndTightenSoA(storage,tighten)) ||
        (storage.activeStateCount(AIStateId::MoveAwayFromRepulsors) != 0 &&
         !enterMoveAwayFromRepulsorsSoA(storage, repulsor)) ||
        (storage.activeStateCount(AIStateId::WanderInPlace) != 0 &&
         !enterWanderInPlaceSoA(storage, repulsor)) ||
        (storage.activeStateCount(AIStateId::Wander) != 0 &&
         !enterWanderPanicSoA(storage, AIStateId::Wander, wanderPanic)) ||
        (storage.activeStateCount(AIStateId::Panic) != 0 &&
         !enterWanderPanicSoA(storage, AIStateId::Panic, wanderPanic)) ||
        (storage.activeStateCount(AIStateId::PickUpCrate) != 0 &&
         !enterPickUpCrateSoA(storage, pickUpCrate)) ||
        ((storage.activeStateCount(AIStateId::MoveAndEvacuate) != 0 ||
          storage.activeStateCount(AIStateId::MoveAndEvacuateAndExit) != 0 ||
          storage.activeStateCount(AIStateId::MoveAndDelete) != 0) &&
         !enterMoveEvacuateStateSoA(storage, moveEvacuate)) ||
        (storage.activeStateCount(AIStateId::Enter) != 0 &&
         !enterContainmentEnterStateSoA(containmentColumns(storage), containment)) ||
        (storage.activeStateCount(AIStateId::Exit) != 0 &&
         !enterContainmentExitStateSoA(containmentColumns(storage), containment)) ||
        (storage.activeStateCount(AIStateId::ExitInstantly) != 0 &&
         !enterContainmentExitInstantlyStateSoA(containmentColumns(storage), containment)) ||
        (storage.activeStateCount(AIStateId::HackInternet) != 0 &&
         !enterHackInternetSoA(storage, hackInternet)) ||
        ((storage.activeStateCount(AIStateId::AttackPosition)!=0||storage.activeStateCount(AIStateId::AttackObject)!=0||
          storage.activeStateCount(AIStateId::ForceAttackObject)!=0||storage.activeStateCount(AIStateId::AttackAndFollowObject)!=0)&&
         !enterAttackStateSoA(storage.attack().view(),attack)) ||
        ((storage.activeStateCount(AIStateId::Dock)!=0||storage.activeStateCount(AIStateId::GetRepaired)!=0)&&
         !enterDockStateSoA(storage.dock().view(),dock)) ||
        (storage.activeStateCount(AIStateId::RappelInto)!=0&&
         !enterRappelIntoStateSoA(storage.insertion().view(),insertion)) ||
        (storage.activeStateCount(AIStateId::CombatDrop)!=0&&
         !enterCombatDropStateSoA(storage.insertion().view(),insertion)) ||
        (storage.activeStateCount(AIStateId::Guard)!=0 &&
         !enterGuardSoA(storage.guard(), AIStateId::Guard, guard)) ||
        (storage.activeStateCount(AIStateId::GuardRetaliate)!=0 &&
         !enterGuardSoA(storage.guard(), AIStateId::GuardRetaliate, guard)) ||
        (storage.activeStateCount(AIStateId::GuardTunnelNetwork)!=0 &&
         !enterGuardSoA(storage.guard(), AIStateId::GuardTunnelNetwork, guard)) ||
        (storage.activeStateCount(AIStateId::Hunt)!=0 &&
         !enterTacticalAttackStateSoA(storage.tacticalAttack(), AIStateId::Hunt, tactical)) ||
        (storage.activeStateCount(AIStateId::AttackSquad)!=0 &&
         !enterTacticalAttackStateSoA(storage.tacticalAttack(), AIStateId::AttackSquad, tactical)) ||
        (storage.activeStateCount(AIStateId::AttackArea)!=0 &&
         !enterTacticalAttackStateSoA(storage.tacticalAttack(), AIStateId::AttackArea, tactical)) ||
        ((storage.activeStateCount(AIStateId::AttackMoveTo)!=0||
          storage.activeStateCount(AIStateId::AttackFollowWaypointPathAsIndividuals)!=0||
          storage.activeStateCount(AIStateId::AttackFollowWaypointPathAsTeam)!=0)&&
         !enterOpportunityAttackMoveSoA(storage.opportunityAttackMove(), opportunity)))
        return false;
    const auto runtimes = storage.runtimes();
    for (const size_t slot : storage.executionSlots())
    {
        if (mask[slot] != 0 && !isImplementedStateSoAState(runtimes[slot].currentState))
            results[slot] = AIStateStepResult::unsupported();
    }
    return true;
}

[[nodiscard]] bool dispatchSoAExits(AIStateFamilySoAStorage& storage,
                                           const AIStateSoAMultiwaveInput& input,
                                           container::Span<const uint8_t> mask,
                                           container::Span<AIStateStepResult> results) noexcept
{
    if (!storage.prepareExecutionSlots(mask))
        return false;
    // States without a migrated SoA kernel currently own no staged payload
    // or output cleanup, so their exit is explicitly a no-op. Their target
    // enter is still reported Unsupported and stops that slot's chain.
    AIMoveStateSoAKernelInput move = input.move;
    move.confirmedTick = input.confirmedTick;
    move.scheduled = mask;
    AIFollowPathStateSoAKernelInput followPath = input.followPath;
    followPath.confirmedTick = input.confirmedTick;
    followPath.scheduled = mask;
    AIWaypointStateSoAKernelInput waypoint = input.waypoint;
    waypoint.confirmedTick = input.confirmedTick;
    waypoint.scheduled = mask;
    AIMoveOutOfWayStateSoAKernelInput moveOut = input.moveOutOfWay;
    moveOut.confirmedTick = input.confirmedTick; moveOut.scheduled = mask;
    AIMoveAndTightenStateSoAKernelInput tighten = input.tighten;
    tighten.confirmedTick=input.confirmedTick;tighten.scheduled=mask;
    AIRepulsorStateSoAKernelInput repulsor = input.repulsor;
    repulsor.confirmedTick = input.confirmedTick; repulsor.scheduled = mask;
    AIWanderPanicStateSoAKernelInput wanderPanic = input.wanderPanic;
    wanderPanic.confirmedTick = input.confirmedTick; wanderPanic.scheduled = mask;
    AIPickUpCrateStateSoAKernelInput pickUpCrate = input.pickUpCrate;
    pickUpCrate.confirmedTick = input.confirmedTick; pickUpCrate.scheduled = mask;
    AIMoveEvacuateStateSoAKernelInput moveEvacuate = input.moveEvacuate;
    moveEvacuate.confirmedTick = input.confirmedTick; moveEvacuate.scheduled = mask;
    AIContainmentStateSoAKernelInput containment = input.containment;
    containment.confirmedTick = input.confirmedTick; containment.scheduled = mask;
    containment.executionSlots = storage.executionSlots();
    containment.activeStates = storage.payloadStates(); containment.subjects = storage.subjects();
    AIHackInternetStateSoAKernelInput hackInternet = input.hackInternet;
    hackInternet.confirmedTick = input.confirmedTick; hackInternet.scheduled = mask;
    AIAttackStateSoAKernelInput attack=input.attack;attack.confirmedTick=input.confirmedTick;attack.scheduled=mask;
    attack.executionSlots = storage.executionSlots();
    attack.activeStates=storage.payloadStates();attack.subjects=storage.subjects();attack.results=results;
    AIDockStateSoAKernelInput dock=input.dock;dock.confirmedTick=input.confirmedTick;dock.scheduled=mask;
    dock.executionSlots = storage.executionSlots();
    dock.activeStates=storage.payloadStates();dock.subjects=storage.subjects();dock.results=results;
    AIInsertionStateSoAKernelInput insertion=input.insertion;insertion.confirmedTick=input.confirmedTick;
    insertion.scheduled=mask;insertion.activeStates=storage.payloadStates();insertion.subjects=storage.subjects();
    insertion.executionSlots = storage.executionSlots();
    insertion.results=results;
    AIGuardStateSoAKernelInput guard=input.guard;guard.confirmedTick=input.confirmedTick;guard.scheduled=mask;
    guard.executionSlots = storage.executionSlots();
    guard.subjects=storage.subjects();guard.results=results;
    AITacticalAttackStateSoAKernelInput tactical=input.tacticalAttack;tactical.confirmedTick=input.confirmedTick;
    tactical.scheduled=mask;tactical.subjects=storage.subjects();tactical.results=results;
    tactical.executionSlots = storage.executionSlots();
    AIOpportunityAttackMoveStateSoAKernelInput opportunity=input.opportunityAttackMove;
    opportunity.confirmedTick=input.confirmedTick;opportunity.scheduled=mask;opportunity.states=storage.payloadStates();
    opportunity.executionSlots = storage.executionSlots();
    opportunity.subjects=storage.subjects();opportunity.parameters=storage.parameters();
    opportunity.moveToColumns=&storage.moveTo();opportunity.waypointColumns=&storage.waypointPath();
    opportunity.results=results;
    if (!canExitMoveToSoA(storage, move) || !canExitFollowPathSoA(storage, followPath) ||
        !canExitWaypointPathSoA(storage, waypoint) || !canExitMoveOutOfWaySoA(storage, moveOut) ||
        !canExitMoveAndTightenSoA(storage,tighten) ||
        !canExitRepulsorStateSoA(storage, repulsor, AIStateId::MoveAwayFromRepulsors) ||
        !canExitRepulsorStateSoA(storage, repulsor, AIStateId::WanderInPlace) ||
        !canExitWanderPanicSoA(storage, wanderPanic) ||
        !canExitPickUpCrateSoA(storage, pickUpCrate) ||
        !canExitMoveEvacuateStateSoA(storage, moveEvacuate) ||
        !canExitContainmentStateSoA(containmentColumns(storage), containment, AIStateId::Enter) ||
        !canExitContainmentStateSoA(containmentColumns(storage), containment, AIStateId::Exit) ||
        !canExitContainmentStateSoA(containmentColumns(storage), containment, AIStateId::ExitInstantly) ||
        !canExitHackInternetSoA(storage, hackInternet) ||
        !canExitAttackStateSoA(storage.attack().view(),attack) || !canExitDockStateSoA(storage.dock().view(),dock) ||
        !canExitRappelIntoStateSoA(storage.insertion().view(),insertion) ||
        !canExitCombatDropStateSoA(storage.insertion().view(),insertion) ||
        !canExitGuardMoveChildren(storage,input,mask) ||
        !canExitGuardAttackObjectChildren(storage,input,mask) ||
        !canExitGuardPickUpCrateChildren(storage,input,guard,mask) ||
        !canExitGuardSoA(storage.guard(),AIStateId::Guard,guard) ||
        !canExitGuardSoA(storage.guard(),AIStateId::GuardRetaliate,guard) ||
        !canExitGuardSoA(storage.guard(),AIStateId::GuardTunnelNetwork,guard) ||
        !canExitTacticalAttackObjectChildren(storage,input,mask) ||
        !canExitTacticalPickUpCrateChildren(storage,input,mask) ||
        !canExitTacticalAttackStateSoA(storage.tacticalAttack(),AIStateId::Hunt,tactical) ||
        !canExitTacticalAttackStateSoA(storage.tacticalAttack(),AIStateId::AttackSquad,tactical) ||
        !canExitTacticalAttackStateSoA(storage.tacticalAttack(),AIStateId::AttackArea,tactical) ||
        !canExitOpportunityAttackObjectChildren(storage,input,mask) ||
        !canExitOpportunityPickUpCrateChildren(storage,input,mask) ||
        !canExitOpportunityAttackMoveSoA(storage.opportunityAttackMove(),opportunity))
        return false;
    return exitMoveToSoA(storage, move) && exitFollowPathSoA(storage, followPath) &&
           exitWaypointPathSoA(storage, waypoint) && exitMoveOutOfWaySoA(storage, moveOut) &&
           exitMoveAndTightenSoA(storage,tighten) &&
           exitRepulsorStateSoA(storage, repulsor, AIStateId::MoveAwayFromRepulsors) &&
           exitRepulsorStateSoA(storage, repulsor, AIStateId::WanderInPlace) &&
           exitWanderPanicSoA(storage, wanderPanic) &&
           exitPickUpCrateSoA(storage, pickUpCrate) &&
           exitMoveEvacuateStateSoA(storage, moveEvacuate) &&
           exitContainmentStateSoA(containmentColumns(storage), containment, AIStateId::Enter) &&
           exitContainmentStateSoA(containmentColumns(storage), containment, AIStateId::Exit) &&
           exitContainmentStateSoA(containmentColumns(storage), containment, AIStateId::ExitInstantly) &&
           exitHackInternetSoA(storage, hackInternet) && exitAttackStateSoA(storage.attack().view(),attack) &&
           exitDockStateSoA(storage.dock().view(),dock) &&
           exitRappelIntoStateSoA(storage.insertion().view(),insertion) &&
           exitCombatDropStateSoA(storage.insertion().view(),insertion) &&
           exitGuardMoveChildren(storage,input,mask) &&
           exitGuardAttackObjectChildren(storage,input,mask) &&
           exitGuardPickUpCrateChildren(storage,input,guard,mask) &&
           exitGuardSoA(storage.guard(),AIStateId::Guard,guard) &&
           exitGuardSoA(storage.guard(),AIStateId::GuardRetaliate,guard) &&
           exitGuardSoA(storage.guard(),AIStateId::GuardTunnelNetwork,guard) &&
           exitTacticalAttackObjectChildren(storage,input,mask) &&
           exitTacticalPickUpCrateChildren(storage,input,mask) &&
           exitTacticalAttackStateSoA(storage.tacticalAttack(),AIStateId::Hunt,tactical) &&
           exitTacticalAttackStateSoA(storage.tacticalAttack(),AIStateId::AttackSquad,tactical) &&
           exitTacticalAttackStateSoA(storage.tacticalAttack(),AIStateId::AttackArea,tactical) &&
           exitOpportunityAttackObjectChildren(storage,input,mask) &&
           exitOpportunityPickUpCrateChildren(storage,input,mask) &&
           exitOpportunityAttackMoveSoA(storage.opportunityAttackMove(),opportunity);
}

[[nodiscard]] bool hasAlignedSoAMultiwaveSpans(AIStateFamilySoAStorage& storage,
                                                       const AIStateSoAMultiwaveInput& input,
                                                       const AIStateSoAMultiwaveScratch& scratch) noexcept
{
    const size_t count = storage.size();
    if (input.scheduled.size() != count || scratch.results.size() != count ||
        scratch.actionMask.size() != count || scratch.exitMask.size() != count ||
        scratch.enterMask.size() != count)
    {
        return false;
    }

    AICoreStateSoAKernelInput core = input.core;
    core.scheduled = scratch.actionMask;
    core.results = scratch.results;
    AIFacingStateSoAKernelInput facing = input.facing;
    facing.scheduled = scratch.actionMask;
    facing.results = scratch.results;
    AIMoveStateSoAKernelInput move = input.move;
    move.scheduled = scratch.actionMask;
    move.results = scratch.results;
    AIFollowPathStateSoAKernelInput followPath = input.followPath;
    followPath.scheduled = scratch.actionMask;
    followPath.results = scratch.results;
    AIWaypointStateSoAKernelInput waypoint = input.waypoint;
    waypoint.scheduled = scratch.actionMask;
    waypoint.results = scratch.results;
    AIMoveOutOfWayStateSoAKernelInput moveOut = input.moveOutOfWay;
    moveOut.scheduled = scratch.actionMask; moveOut.results = scratch.results;
    AIMoveAndTightenStateSoAKernelInput tighten = input.tighten;
    tighten.scheduled=scratch.actionMask;tighten.results=scratch.results;
    AIRepulsorStateSoAKernelInput repulsor = input.repulsor;
    repulsor.scheduled = scratch.actionMask; repulsor.results = scratch.results;
    AIWanderPanicStateSoAKernelInput wanderPanic = input.wanderPanic;
    wanderPanic.scheduled = scratch.actionMask; wanderPanic.results = scratch.results;
    AIPickUpCrateStateSoAKernelInput pickUpCrate = input.pickUpCrate;
    pickUpCrate.scheduled = scratch.actionMask; pickUpCrate.results = scratch.results;
    AIMoveEvacuateStateSoAKernelInput moveEvacuate = input.moveEvacuate;
    moveEvacuate.scheduled = scratch.actionMask; moveEvacuate.results = scratch.results;
    AIContainmentStateSoAKernelInput containment = input.containment;
    containment.scheduled = scratch.actionMask; containment.activeStates = storage.payloadStates();
    containment.subjects = storage.subjects(); containment.results = scratch.results;
    AIHackInternetStateSoAKernelInput hackInternet = input.hackInternet;
    hackInternet.scheduled = scratch.actionMask; hackInternet.results = scratch.results;
    AIAttackStateSoAKernelInput attack=input.attack;attack.scheduled=scratch.actionMask;
    attack.activeStates=storage.payloadStates();attack.subjects=storage.subjects();attack.results=scratch.results;
    AIDockStateSoAKernelInput dock=input.dock;dock.scheduled=scratch.actionMask;
    dock.activeStates=storage.payloadStates();dock.subjects=storage.subjects();dock.results=scratch.results;
    AIInsertionStateSoAKernelInput insertion=input.insertion;insertion.scheduled=scratch.actionMask;
    insertion.activeStates=storage.payloadStates();insertion.subjects=storage.subjects();insertion.results=scratch.results;
    AIGuardStateSoAKernelInput guard=input.guard;guard.scheduled=scratch.actionMask;
    guard.subjects=storage.subjects();guard.results=scratch.results;
    AITacticalAttackStateSoAKernelInput tactical=input.tacticalAttack;tactical.scheduled=scratch.actionMask;
    tactical.subjects=storage.subjects();tactical.results=scratch.results;
    AIOpportunityAttackMoveStateSoAKernelInput opportunity=input.opportunityAttackMove;
    opportunity.scheduled=scratch.actionMask;opportunity.states=storage.payloadStates();
    opportunity.subjects=storage.subjects();opportunity.parameters=storage.parameters();
    opportunity.moveToColumns=&storage.moveTo();
    opportunity.waypointColumns=&storage.waypointPath();
    opportunity.results=scratch.results;
    return hasAlignedCoreStateSoASpans(storage, core) && hasAlignedFacingStateSoASpans(storage, facing) &&
           hasAlignedMoveStateSoASpans(storage, move) && hasAlignedFollowPathSoASpans(storage, followPath) &&
           hasAlignedWaypointSpans(storage, waypoint) && detail::hasAlignedMoveOutSpans(storage, moveOut) &&
           detail::hasAlignedTightenSpans(storage,tighten) &&
           detail::hasAlignedRepulsorStateSoASpans(storage, repulsor) &&
           wander_panic_detail::hasAlignedSpans(storage, wanderPanic) &&
           detail::hasAlignedPickUpCrateStateSoASpans(storage, pickUpCrate) &&
           detail::hasAlignedMoveEvacuateSpans(storage, moveEvacuate) &&
           containment.goalObjects.size() == count && containment.feedback.size() == count &&
           containment.commands.size() == count && hack_detail::aligned(storage, hackInternet) &&
           storage.attack().size()==count && attack_detail::hasAlignedInputSpans(count,attack) &&
           storage.dock().size()==count && dock_detail::hasAlignedInputSpans(count,dock) &&
           storage.insertion().size()==count && insertion_detail::hasAlignedInputSpans(count,insertion) &&
           guard_detail::hasAlignedSpans(storage.guard(),guard) &&
           (input.guardMoveChild.empty() || input.guardMoveChild.aligned(count)) &&
           (input.guardAttackChild.empty() || input.guardAttackChild.aligned(count)) &&
           tactical_attack_detail::hasAlignedSpans(storage.tacticalAttack(),tactical) &&
           ((input.tacticalAttackChild.empty() && input.tacticalAttackChildFeedback.empty()) ||
            (input.tacticalAttackChild.aligned(count) && input.tacticalAttackChildFeedback.size() == count)) &&
           opportunity_attack_move_detail::aligned(storage.opportunityAttackMove(),opportunity) &&
           ((input.opportunityAttackChild.empty() && input.opportunityAttackChildFeedback.empty()) ||
            (input.opportunityAttackChild.aligned(count) && input.opportunityAttackChildFeedback.size() == count));
}

} // namespace engine::ai::detail
