#pragma once

#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/simulation/structure/ObjectAirfield.h"

namespace engine::object_repair_rules {

[[nodiscard]] inline bool isRepairDockTarget(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ObjectEconomyComponent* economy =
        ecs::try_get<ObjectEconomyComponent>(registry, entity);
    return economy && economy->plan && !economy->repairDocks.empty() &&
        economy->repairDocks.size() == economy->plan->repairDocks.size();
}

[[nodiscard]] inline bool isDamagedRepairDockActor(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(registry, entity);
    const auto hasKind = [kinds](game::ObjectKindOf sought) noexcept {
        return kinds && game::objectHasKind(kinds->mask, sought);
    };
    return health && !health->effectivelyDead &&
        hasKind(game::ObjectKindOf::Vehicle) &&
        !hasKind(game::ObjectKindOf::Aircraft) &&
        health->maximumFixed > math::q32_32{} &&
        health->currentFixed < health->maximumFixed;
}

[[nodiscard]] inline bool isAircraftRepairAirfieldTarget(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ObjectAirfieldComponent* airfield =
        ecs::try_get<ObjectAirfieldComponent>(registry, entity);
    return airfield && airfield->plan &&
        (!airfield->parkingPlaces.empty() ||
         !airfield->flightDecks.empty());
}

[[nodiscard]] inline bool isDamagedAircraftRepairActor(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(registry, entity);
    return health && !health->effectivelyDead && kinds &&
        game::objectHasKind(kinds->mask, game::ObjectKindOf::Vehicle) &&
        game::objectHasKind(kinds->mask, game::ObjectKindOf::Aircraft) &&
        health->maximumFixed > math::q32_32{} &&
        health->currentFixed < health->maximumFixed;
}

} // namespace engine::object_repair_rules
