#pragma once

#include <cstdint>
#include "core/container/container_types.h"

namespace engine::ai
{

// Values 0..43 deliberately match GeneralsMD's AIStateType. This gives
// migration tools, diagnostics and future save conversion one unambiguous
// compatibility mapping. Append new states after Count; never reorder these.
enum class AIStateId : uint16_t
{
    Idle = 0,
    MoveTo,
    FollowWaypointPathAsTeam,
    FollowWaypointPathAsIndividuals,
    FollowWaypointPathAsTeamExact,
    FollowWaypointPathAsIndividualsExact,
    FollowPath,
    FollowExitProductionPath,
    Wait,
    AttackPosition,
    AttackObject,
    ForceAttackObject,
    AttackAndFollowObject,
    Dead,
    Dock,
    Enter,
    Guard,
    Hunt,
    Wander,
    Panic,
    AttackSquad,
    GuardTunnelNetwork,
    GetRepaired,
    MoveOutOfTheWay,
    MoveAndTighten,
    MoveAndEvacuate,
    MoveAndEvacuateAndExit,
    MoveAndDelete,
    AttackArea,
    HackInternet,
    AttackMoveTo,
    AttackFollowWaypointPathAsIndividuals,
    AttackFollowWaypointPathAsTeam,
    FaceObject,
    FacePosition,
    RappelInto,
    CombatDrop,
    Exit,
    PickUpCrate,
    MoveAwayFromRepulsors,
    WanderInPlace,
    Busy,
    ExitInstantly,
    GuardRetaliate,

    Count,
    Invalid = 0xffff,
};

static_assert(static_cast<uint16_t>(AIStateId::Count) == 44);

enum class AIStateOutcome : uint8_t
{
    Continue,
    Success,
    Failure,
    Sleep,
    Blocked,
};

enum class AIStateTransitionReason : uint8_t
{
    Initialize,
    Explicit,
    Condition,
    Success,
    Failure,
    Temporary,
    TemporaryCompleted,
    TemporaryExpired,
    Reset,
};

// External transitions originate in player/script order ingestion. Internal
// transitions originate in the active behavior. Terminal authority is
// reserved for death/destruction and snapshot repair paths.
enum class AIStateTransitionAuthority : uint8_t
{
    External,
    Internal,
    Terminal,
};

enum class AIStateMachineLock : uint8_t
{
    Unlocked,
    ExternalTransitionsBlocked,
    Terminal,
};

enum class AIStateExitReason : uint8_t
{
    Transition,
    Reenter,
    Reset,
    Interrupted,
    TemporaryCompleted,
    TemporaryExpired,
    Terminal,
};

enum class AIWakeReason : uint8_t
{
    None,
    Deadline,
    ExternalCommand,
    ServiceResult,
    TargetInvalidated,
    Damage,
    MovementEvent,
    SnapshotRepair,
};

enum class AISubstateDomain : uint8_t
{
    None,
    MoveSequence,
    Attack,
    Guard,
    Dock,
    Special,
};

enum class AIStateFamily : uint8_t
{
    Core,
    Move,
    MoveSequence,
    Attack,
    AttackMove,
    Guard,
    Dock,
    Containment,
    Special,
};

enum class AIContainmentPhase : uint8_t
{
    Inactive,
    EnterActive,
    ExitActive,
    ExitCommandIssued,
    ExitInstantlyCommandIssued,
};

struct AIBehaviorProfileHandle final
{
    uint64_t value = 0;
    [[nodiscard]] constexpr bool isValid() const noexcept { return value != 0; }
    explicit constexpr operator bool() const noexcept { return isValid(); }
    constexpr bool operator==(const AIBehaviorProfileHandle&) const noexcept = default;
};

enum class AIHackInternetPhase : uint8_t
{
    Unpacking,
    Hacking,
    Packing,
};

using AISubstateId = uint8_t;
inline constexpr AISubstateId INVALID_AI_SUBSTATE = 0xff;

[[nodiscard]] constexpr bool isValidState(AIStateId state) noexcept
{
    return state < AIStateId::Count;
}

[[nodiscard]] constexpr container::StringView stateName(AIStateId state) noexcept
{
    switch (state)
    {
    case AIStateId::Idle:
        return "Idle";
    case AIStateId::MoveTo:
        return "MoveTo";
    case AIStateId::FollowWaypointPathAsTeam:
        return "FollowWaypointPathAsTeam";
    case AIStateId::FollowWaypointPathAsIndividuals:
        return "FollowWaypointPathAsIndividuals";
    case AIStateId::FollowWaypointPathAsTeamExact:
        return "FollowWaypointPathAsTeamExact";
    case AIStateId::FollowWaypointPathAsIndividualsExact:
        return "FollowWaypointPathAsIndividualsExact";
    case AIStateId::FollowPath:
        return "FollowPath";
    case AIStateId::FollowExitProductionPath:
        return "FollowExitProductionPath";
    case AIStateId::Wait:
        return "Wait";
    case AIStateId::AttackPosition:
        return "AttackPosition";
    case AIStateId::AttackObject:
        return "AttackObject";
    case AIStateId::ForceAttackObject:
        return "ForceAttackObject";
    case AIStateId::AttackAndFollowObject:
        return "AttackAndFollowObject";
    case AIStateId::Dead:
        return "Dead";
    case AIStateId::Dock:
        return "Dock";
    case AIStateId::Enter:
        return "Enter";
    case AIStateId::Guard:
        return "Guard";
    case AIStateId::Hunt:
        return "Hunt";
    case AIStateId::Wander:
        return "Wander";
    case AIStateId::Panic:
        return "Panic";
    case AIStateId::AttackSquad:
        return "AttackSquad";
    case AIStateId::GuardTunnelNetwork:
        return "GuardTunnelNetwork";
    case AIStateId::GetRepaired:
        return "GetRepaired";
    case AIStateId::MoveOutOfTheWay:
        return "MoveOutOfTheWay";
    case AIStateId::MoveAndTighten:
        return "MoveAndTighten";
    case AIStateId::MoveAndEvacuate:
        return "MoveAndEvacuate";
    case AIStateId::MoveAndEvacuateAndExit:
        return "MoveAndEvacuateAndExit";
    case AIStateId::MoveAndDelete:
        return "MoveAndDelete";
    case AIStateId::AttackArea:
        return "AttackArea";
    case AIStateId::HackInternet:
        return "HackInternet";
    case AIStateId::AttackMoveTo:
        return "AttackMoveTo";
    case AIStateId::AttackFollowWaypointPathAsIndividuals:
        return "AttackFollowWaypointPathAsIndividuals";
    case AIStateId::AttackFollowWaypointPathAsTeam:
        return "AttackFollowWaypointPathAsTeam";
    case AIStateId::FaceObject:
        return "FaceObject";
    case AIStateId::FacePosition:
        return "FacePosition";
    case AIStateId::RappelInto:
        return "RappelInto";
    case AIStateId::CombatDrop:
        return "CombatDrop";
    case AIStateId::Exit:
        return "Exit";
    case AIStateId::PickUpCrate:
        return "PickUpCrate";
    case AIStateId::MoveAwayFromRepulsors:
        return "MoveAwayFromRepulsors";
    case AIStateId::WanderInPlace:
        return "WanderInPlace";
    case AIStateId::Busy:
        return "Busy";
    case AIStateId::ExitInstantly:
        return "ExitInstantly";
    case AIStateId::GuardRetaliate:
        return "GuardRetaliate";
    case AIStateId::Count:
    case AIStateId::Invalid:
        break;
    }
    return "Invalid";
}

} // namespace engine::ai
