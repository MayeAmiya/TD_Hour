#pragma once

#include "core/container/container_types.h"
#include <cstddef>

#include "game/object/ai/runtime/AIStateTypes.h"

namespace engine::ai
{

struct AIStateDescriptor final
{
    AIStateId id = AIStateId::Invalid;
    AIStateId successState = AIStateId::Invalid;
    AIStateId failureState = AIStateId::Invalid;
    AIStateFamily family = AIStateFamily::Core;
    bool registeredInGeneralsMD = false;
    bool terminal = false;
};

inline constexpr container::Array<AIStateDescriptor, static_cast<size_t>(AIStateId::Count)> AI_STATE_DESCRIPTORS{{
    {AIStateId::Idle, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Core, true, false},
    {AIStateId::MoveTo, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Move, true, false},
    {AIStateId::FollowWaypointPathAsTeam, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Move, true, false},
    {AIStateId::FollowWaypointPathAsIndividuals, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Move, true, false},
    {AIStateId::FollowWaypointPathAsTeamExact, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Move, true, false},
    {AIStateId::FollowWaypointPathAsIndividualsExact,
     AIStateId::Idle,
     AIStateId::Idle,
     AIStateFamily::Move,
     true,
     false},
    {AIStateId::FollowPath, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Move, true, false},
    {AIStateId::FollowExitProductionPath, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Move, true, false},
    {AIStateId::Wait, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Core, true, false},
    {AIStateId::AttackPosition, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Attack, true, false},
    {AIStateId::AttackObject, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Attack, true, false},
    {AIStateId::ForceAttackObject, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Attack, true, false},
    {AIStateId::AttackAndFollowObject, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Attack, true, false},
    {AIStateId::Dead, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Core, true, true},
    {AIStateId::Dock, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Dock, true, false},
    {AIStateId::Enter, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Containment, true, false},
    {AIStateId::Guard, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Guard, true, false},
    {AIStateId::Hunt, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Attack, true, false},
    {AIStateId::Wander, AIStateId::Idle, AIStateId::MoveAwayFromRepulsors, AIStateFamily::Move, true, false},
    {AIStateId::Panic, AIStateId::Idle, AIStateId::MoveAwayFromRepulsors, AIStateFamily::Move, true, false},
    {AIStateId::AttackSquad, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Attack, true, false},
    {AIStateId::GuardTunnelNetwork, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Guard, true, false},
    {AIStateId::GetRepaired, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Dock, false, false},
    {AIStateId::MoveOutOfTheWay, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Move, true, false},
    {AIStateId::MoveAndTighten, AIStateId::Idle, AIStateId::Idle, AIStateFamily::MoveSequence, true, false},
    {AIStateId::MoveAndEvacuate, AIStateId::Idle, AIStateId::Idle, AIStateFamily::MoveSequence, true, false},
    {AIStateId::MoveAndEvacuateAndExit,
     AIStateId::MoveAndDelete,
     AIStateId::MoveAndDelete,
     AIStateFamily::MoveSequence,
     true,
     false},
    {AIStateId::MoveAndDelete, AIStateId::Idle, AIStateId::Idle, AIStateFamily::MoveSequence, true, false},
    {AIStateId::AttackArea, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Attack, true, false},
    {AIStateId::HackInternet, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Special, false, false},
    {AIStateId::AttackMoveTo, AIStateId::Idle, AIStateId::Idle, AIStateFamily::AttackMove, true, false},
    {AIStateId::AttackFollowWaypointPathAsIndividuals,
     AIStateId::Idle,
     AIStateId::Idle,
     AIStateFamily::AttackMove,
     true,
     false},
    {AIStateId::AttackFollowWaypointPathAsTeam,
     AIStateId::Idle,
     AIStateId::Idle,
     AIStateFamily::AttackMove,
     true,
     false},
    {AIStateId::FaceObject, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Core, true, false},
    {AIStateId::FacePosition, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Core, true, false},
    {AIStateId::RappelInto, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Special, true, false},
    {AIStateId::CombatDrop, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Special, false, false},
    {AIStateId::Exit, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Containment, true, false},
    {AIStateId::PickUpCrate, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Move, true, false},
    {AIStateId::MoveAwayFromRepulsors,
     AIStateId::WanderInPlace,
     AIStateId::WanderInPlace,
     AIStateFamily::Move,
     true,
     false},
    {AIStateId::WanderInPlace,
     AIStateId::MoveAwayFromRepulsors,
     AIStateId::MoveAwayFromRepulsors,
     AIStateFamily::Move,
     true,
     false},
    {AIStateId::Busy, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Core, true, false},
    {AIStateId::ExitInstantly, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Containment, true, false},
    {AIStateId::GuardRetaliate, AIStateId::Idle, AIStateId::Idle, AIStateFamily::Guard, true, false},
}};

[[nodiscard]] consteval bool validateStateDescriptors()
{
    for (size_t index = 0; index < AI_STATE_DESCRIPTORS.size(); ++index)
    {
        const AIStateDescriptor& descriptor = AI_STATE_DESCRIPTORS[index];
        if (static_cast<size_t>(descriptor.id) != index || !isValidState(descriptor.successState) ||
            !isValidState(descriptor.failureState))
        {
            return false;
        }
    }
    return true;
}

static_assert(validateStateDescriptors());

[[nodiscard]] constexpr const AIStateDescriptor* descriptorFor(AIStateId state) noexcept
{
    if (!isValidState(state))
        return nullptr;
    return &AI_STATE_DESCRIPTORS[static_cast<size_t>(state)];
}

} // namespace engine::ai
