#include "ObjectStatus.h"
#include "game/object/component/ObjectDirty.h"

#include <limits>

namespace engine
{
namespace
{

[[nodiscard]] constexpr game::ObjectStatusMask statusMask(game::ObjectStatusFlag first) noexcept
{
    return game::objectStatusBit(first);
}

template <typename... Rest>
[[nodiscard]] constexpr game::ObjectStatusMask statusMask(game::ObjectStatusFlag first, Rest... rest) noexcept
{
    return game::objectStatusBit(first) | statusMask(rest...);
}

[[nodiscard]] ObjectStatusDependencyMask dependenciesFor(game::ObjectStatusMask changed) noexcept
{
    ObjectStatusDependencyMask result = 0;
    const auto add = [&result](ObjectStatusDependency dependency) { result |= objectStatusDependencyBit(dependency); };

    const game::ObjectStatusMask stealthVisibility = statusMask(
        game::ObjectStatusFlag::Stealthed, game::ObjectStatusFlag::Detected, game::ObjectStatusFlag::Disguised);
    if ((changed & stealthVisibility) != 0)
    {
        add(ObjectStatusDependency::Visibility);
        add(ObjectStatusDependency::Spatial);
        add(ObjectStatusDependency::Stealth);
    }

    if ((changed & game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction)) != 0)
    {
        // RefCode dirties the partition/shroud state and performs the
        // construction-edge mine collision query here.  Modern consumers are
        // separated, so publish all three invalidation domains explicitly.
        add(ObjectStatusDependency::Construction);
        add(ObjectStatusDependency::Spatial);
        add(ObjectStatusDependency::Visibility);
        add(ObjectStatusDependency::Collision);
    }

    if ((changed & statusMask(game::ObjectStatusFlag::NoCollisions,
                              game::ObjectStatusFlag::AirborneTarget,
                              game::ObjectStatusFlag::Parachuting,
                              game::ObjectStatusFlag::DeckHeightOffset)) != 0)
    {
        add(ObjectStatusDependency::Collision);
        add(ObjectStatusDependency::Spatial);
    }

    if ((changed & statusMask(game::ObjectStatusFlag::CanAttack,
                              game::ObjectStatusFlag::NoAttack,
                              game::ObjectStatusFlag::IsFiringWeapon,
                              game::ObjectStatusFlag::IsAttacking,
                              game::ObjectStatusFlag::IsAimingWeapon,
                              game::ObjectStatusFlag::NoAttackFromAi,
                              game::ObjectStatusFlag::IgnoringStealth,
                              game::ObjectStatusFlag::FaerieFire)) != 0)
    {
        add(ObjectStatusDependency::Combat);
    }

    if ((changed & statusMask(game::ObjectStatusFlag::Unselectable,
                              game::ObjectStatusFlag::Masked,
                              game::ObjectStatusFlag::Sold)) != 0)
    {
        add(ObjectStatusDependency::Selection);
    }

    if ((changed & game::objectStatusBit(game::ObjectStatusFlag::CanStealth)) != 0)
    {
        add(ObjectStatusDependency::Stealth);
    }

    if ((changed & game::objectStatusBit(game::ObjectStatusFlag::Repulsor)) != 0)
    {
        add(ObjectStatusDependency::Repulsor);
    }

    if ((changed & game::objectStatusBit(game::ObjectStatusFlag::Destroyed)) != 0)
    {
        add(ObjectStatusDependency::Lifecycle);
        add(ObjectStatusDependency::Spatial);
        add(ObjectStatusDependency::Visibility);
        add(ObjectStatusDependency::Collision);
        add(ObjectStatusDependency::Combat);
        add(ObjectStatusDependency::Selection);
    }

    return result;
}

} // namespace

ObjectStatusTransition ObjectStatusSystem::apply(ecs::registry& registry,
                                                 ecs::entity entity,
                                                 const ObjectStatusMutation& mutation)
{
    ObjectStatusTransition transition;
    if (entity == ecs::null || !registry.valid(entity))
        return transition;

    const game::ObjectStatusMask known = game::objectStatusKnownMask();
    const game::ObjectStatusMask setMask = mutation.setMask & known;
    const game::ObjectStatusMask clearMask = mutation.clearMask & known;
    ObjectStatusComponent* status = ecs::try_get<ObjectStatusComponent>(registry, entity);
    transition.previous = status ? (status->flags & known) : 0;
    transition.current = (transition.previous | setMask) & ~clearMask;
    transition.newlySet = transition.current & ~transition.previous;
    transition.newlyCleared = transition.previous & ~transition.current;
    if (!transition.changed())
        return transition;

    if (!status)
    {
        status = &ecs::emplace<ObjectStatusComponent>(registry, entity);
    }
    status->flags = transition.current;
    if (status->revision != std::numeric_limits<uint64_t>::max())
        ++status->revision;
    status->lastChangedTick = mutation.confirmedTick;
    status->lastSetMask = transition.newlySet;
    status->lastClearedMask = transition.newlyCleared;
    transition.dependencies = dependenciesFor(transition.newlySet | transition.newlyCleared);
    status->pendingDependencies |= transition.dependencies;
    uint8_t dirtyDomains =
        objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
        objectDirtyBit(ObjectDirtyDomain::RenderExtraction);
    if ((transition.dependencies & objectStatusDependencyBit(
            ObjectStatusDependency::Spatial)) != 0) {
        dirtyDomains |= objectDirtyBit(ObjectDirtyDomain::Spatial);
    }
    markObjectDirty(registry, entity, dirtyDomains);
    return transition;
}

ObjectStatusDependencyMask ObjectStatusSystem::acknowledgeDependencies(ObjectStatusComponent& status,
                                                                       ObjectStatusDependencyMask dependencies) noexcept
{
    const ObjectStatusDependencyMask acknowledged = status.pendingDependencies & dependencies;
    status.pendingDependencies &= ~acknowledged;
    return acknowledged;
}

} // namespace engine
