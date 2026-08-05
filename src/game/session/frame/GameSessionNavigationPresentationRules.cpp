#include "game/session/frame/GameSessionNavigationPresentationRules.h"

#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/definition/ObjectArchetype.h"

namespace engine::session_navigation {
namespace {

[[nodiscard]] bool hasKind(
    const ObjectKindOfComponent* kinds,
    game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

} // namespace

bool blocksGround(
    const ecs::registry& registry,
    ecs::entity entity) noexcept {
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(registry, entity);
    if (hasKind(kinds, game::ObjectKindOf::Mine) ||
        hasKind(kinds, game::ObjectKindOf::Projectile) ||
        hasKind(kinds, game::ObjectKindOf::BridgeTower)) {
        return false;
    }
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (type && type->archetype) {
        if (type->archetype->templateData.fenceWidthFixed > math::q32_32{} &&
            !hasKind(kinds, game::ObjectKindOf::DefensiveWall)) {
            return true;
        }
        const game::ObjectBodyKind body =
            type->archetype->templateData.body.kind;
        if (body == game::ObjectBodyKind::Structure ||
            body == game::ObjectBodyKind::HiveStructure) {
            return true;
        }
    }
    return hasKind(kinds, game::ObjectKindOf::Structure);
}

bool blocksAircraft(
    const ecs::registry& registry,
    ecs::entity entity) noexcept {
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(registry, entity);
    return hasKind(kinds, game::ObjectKindOf::AircraftPathAround);
}

bool isRubbleBlocker(
    const ecs::registry& registry,
    ecs::entity entity) noexcept {
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    return health && health->damageState == ObjectBodyDamageState::Rubble &&
        blocksGround(registry, entity);
}

} // namespace engine::session_navigation
