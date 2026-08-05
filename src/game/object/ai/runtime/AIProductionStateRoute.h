#pragma once

#include "core/container/container_types.h"
#include "game/object/ai/runtime/AIStateTypes.h"

#include <cstddef>
#include <cstdint>

namespace engine::ai
{

// This is the production admission policy, not the SoA implementation table.
// A state can have a complete staging kernel while remaining deliberately
// unavailable to content until its unique gameplay owner is connected.
enum class AIProductionAdmission : uint8_t
{
    None,
    Internal,
    MoveOrder,
    AttackOrder,
    TacticalOrder,
    SpecializedSystem,
};

enum class AIProductionRouteStatus : uint8_t
{
    InternalOnly,
    Enabled,
    LinkedPartial,
    SpecializedOwner,
    ExplicitUnsupported,
    StagingOnly,
};

struct AIProductionStateRoute final
{
    AIStateId state = AIStateId::Invalid;
    AIProductionAdmission admission = AIProductionAdmission::None;
    AIProductionRouteStatus status = AIProductionRouteStatus::StagingOnly;
};

inline constexpr container::Array<
    AIProductionStateRoute, static_cast<size_t>(AIStateId::Count)>
    AI_PRODUCTION_STATE_ROUTES{{
        {AIStateId::Idle, AIProductionAdmission::Internal, AIProductionRouteStatus::InternalOnly},
        {AIStateId::MoveTo, AIProductionAdmission::MoveOrder, AIProductionRouteStatus::Enabled},
        {AIStateId::FollowWaypointPathAsTeam, AIProductionAdmission::MoveOrder, AIProductionRouteStatus::Enabled},
        {AIStateId::FollowWaypointPathAsIndividuals, AIProductionAdmission::MoveOrder, AIProductionRouteStatus::Enabled},
        {AIStateId::FollowWaypointPathAsTeamExact, AIProductionAdmission::MoveOrder, AIProductionRouteStatus::Enabled},
        {AIStateId::FollowWaypointPathAsIndividualsExact, AIProductionAdmission::MoveOrder, AIProductionRouteStatus::Enabled},
        {AIStateId::FollowPath, AIProductionAdmission::MoveOrder, AIProductionRouteStatus::Enabled},
        {AIStateId::FollowExitProductionPath, AIProductionAdmission::MoveOrder, AIProductionRouteStatus::Enabled},
        {AIStateId::Wait, AIProductionAdmission::Internal, AIProductionRouteStatus::InternalOnly},
        {AIStateId::AttackPosition, AIProductionAdmission::AttackOrder, AIProductionRouteStatus::Enabled},
        {AIStateId::AttackObject, AIProductionAdmission::AttackOrder, AIProductionRouteStatus::Enabled},
        {AIStateId::ForceAttackObject, AIProductionAdmission::AttackOrder, AIProductionRouteStatus::Enabled},
        {AIStateId::AttackAndFollowObject, AIProductionAdmission::Internal, AIProductionRouteStatus::InternalOnly},
        {AIStateId::Dead, AIProductionAdmission::Internal, AIProductionRouteStatus::InternalOnly},
        {AIStateId::Dock, AIProductionAdmission::SpecializedSystem, AIProductionRouteStatus::SpecializedOwner},
        {AIStateId::Enter, AIProductionAdmission::SpecializedSystem, AIProductionRouteStatus::SpecializedOwner},
        {AIStateId::Guard, AIProductionAdmission::TacticalOrder, AIProductionRouteStatus::Enabled},
        {AIStateId::Hunt, AIProductionAdmission::TacticalOrder, AIProductionRouteStatus::Enabled},
        {AIStateId::Wander, AIProductionAdmission::MoveOrder, AIProductionRouteStatus::Enabled},
        {AIStateId::Panic, AIProductionAdmission::MoveOrder, AIProductionRouteStatus::Enabled},
        {AIStateId::AttackSquad, AIProductionAdmission::TacticalOrder, AIProductionRouteStatus::Enabled},
        {AIStateId::GuardTunnelNetwork, AIProductionAdmission::TacticalOrder, AIProductionRouteStatus::Enabled},
        {AIStateId::GetRepaired, AIProductionAdmission::SpecializedSystem, AIProductionRouteStatus::SpecializedOwner},
        {AIStateId::MoveOutOfTheWay, AIProductionAdmission::SpecializedSystem, AIProductionRouteStatus::SpecializedOwner},
        {AIStateId::MoveAndTighten, AIProductionAdmission::MoveOrder, AIProductionRouteStatus::Enabled},
        {AIStateId::MoveAndEvacuate, AIProductionAdmission::SpecializedSystem, AIProductionRouteStatus::SpecializedOwner},
        {AIStateId::MoveAndEvacuateAndExit, AIProductionAdmission::SpecializedSystem, AIProductionRouteStatus::SpecializedOwner},
        {AIStateId::MoveAndDelete, AIProductionAdmission::Internal, AIProductionRouteStatus::InternalOnly},
        {AIStateId::AttackArea, AIProductionAdmission::TacticalOrder, AIProductionRouteStatus::Enabled},
        {AIStateId::HackInternet, AIProductionAdmission::SpecializedSystem, AIProductionRouteStatus::SpecializedOwner},
        {AIStateId::AttackMoveTo, AIProductionAdmission::MoveOrder, AIProductionRouteStatus::Enabled},
        {AIStateId::AttackFollowWaypointPathAsIndividuals, AIProductionAdmission::MoveOrder, AIProductionRouteStatus::Enabled},
        {AIStateId::AttackFollowWaypointPathAsTeam, AIProductionAdmission::MoveOrder, AIProductionRouteStatus::Enabled},
        {AIStateId::FaceObject, AIProductionAdmission::SpecializedSystem, AIProductionRouteStatus::SpecializedOwner},
        {AIStateId::FacePosition, AIProductionAdmission::SpecializedSystem, AIProductionRouteStatus::SpecializedOwner},
        {AIStateId::RappelInto, AIProductionAdmission::SpecializedSystem, AIProductionRouteStatus::SpecializedOwner},
        {AIStateId::CombatDrop, AIProductionAdmission::SpecializedSystem, AIProductionRouteStatus::SpecializedOwner},
        {AIStateId::Exit, AIProductionAdmission::SpecializedSystem, AIProductionRouteStatus::SpecializedOwner},
        {AIStateId::PickUpCrate, AIProductionAdmission::Internal, AIProductionRouteStatus::InternalOnly},
        {AIStateId::MoveAwayFromRepulsors, AIProductionAdmission::Internal, AIProductionRouteStatus::InternalOnly},
        {AIStateId::WanderInPlace, AIProductionAdmission::MoveOrder, AIProductionRouteStatus::Enabled},
        {AIStateId::Busy, AIProductionAdmission::Internal, AIProductionRouteStatus::InternalOnly},
        {AIStateId::ExitInstantly, AIProductionAdmission::SpecializedSystem, AIProductionRouteStatus::SpecializedOwner},
        {AIStateId::GuardRetaliate, AIProductionAdmission::TacticalOrder, AIProductionRouteStatus::Enabled},
    }};

[[nodiscard]] consteval bool validateProductionStateRoutes()
{
    for (size_t index = 0; index < AI_PRODUCTION_STATE_ROUTES.size(); ++index)
    {
        const AIProductionStateRoute& route = AI_PRODUCTION_STATE_ROUTES[index];
        if (static_cast<size_t>(route.state) != index)
            return false;

        switch (route.status)
        {
        case AIProductionRouteStatus::InternalOnly:
            if (route.admission != AIProductionAdmission::Internal)
                return false;
            break;
        case AIProductionRouteStatus::Enabled:
        case AIProductionRouteStatus::LinkedPartial:
            if (route.admission != AIProductionAdmission::MoveOrder &&
                route.admission != AIProductionAdmission::AttackOrder &&
                route.admission != AIProductionAdmission::TacticalOrder)
                return false;
            break;
        case AIProductionRouteStatus::SpecializedOwner:
            if (route.admission != AIProductionAdmission::SpecializedSystem)
                return false;
            break;
        case AIProductionRouteStatus::ExplicitUnsupported:
        case AIProductionRouteStatus::StagingOnly:
            if (route.admission != AIProductionAdmission::None)
                return false;
            break;
        }
    }
    return true;
}

static_assert(validateProductionStateRoutes());

[[nodiscard]] constexpr const AIProductionStateRoute* productionStateRouteFor(
    AIStateId state) noexcept
{
    if (!isValidState(state))
        return nullptr;
    return &AI_PRODUCTION_STATE_ROUTES[static_cast<size_t>(state)];
}

[[nodiscard]] constexpr bool acceptsProductionAdmission(
    AIStateId state, AIProductionAdmission admission) noexcept
{
    const AIProductionStateRoute* route = productionStateRouteFor(state);
    return route && route->admission == admission &&
           (route->status == AIProductionRouteStatus::Enabled ||
            route->status == AIProductionRouteStatus::LinkedPartial);
}

static_assert(acceptsProductionAdmission(
    AIStateId::WanderInPlace, AIProductionAdmission::MoveOrder));

} // namespace engine::ai
