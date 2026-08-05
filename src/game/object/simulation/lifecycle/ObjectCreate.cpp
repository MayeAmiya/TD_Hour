#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/lifecycle/ObjectCreate.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <utility>

namespace engine {

container::Vector<ObjectId> collectReadySupplyAnchors(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectSupplyAnchorKind kind) {
    container::Vector<ObjectId> result;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const ObjectSupplyAnchorComponent>(registry);
    result.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectSupplyAnchorComponent& supply =
            view.template get<const ObjectSupplyAnchorComponent>(entity);
        const bool ready = kind == ObjectSupplyAnchorKind::Center
            ? supply.supplyCenterReady : supply.supplyWarehouseReady;
        if (!ready) continue;
        const ObjectId object =
            view.template get<const ObjectIdentityComponent>(entity).id;
        if (!object || !lifecycle.entityFromId(object)) continue;
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, entity);
        if (health && health->effectivelyDead) continue;
        result.push_back(object);
    }
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace engine
