#pragma once

#include "core/container/container_types.h"
#include "game/object/definition/ObjectKindOf.h"
#include "game/object/plan/combat/ObjectTurretPlanTypes.h"

#include <cstdint>

namespace game {
struct ThingTemplate;
}

namespace engine {

struct ObjectPointDefenseLaserRulePlan final {
    container::String weaponTemplate;
    game::ObjectKindOfMask primaryTargetKindMask{};
    game::ObjectKindOfMask secondaryTargetKindMask{};
    uint32_t scanRateMilliseconds = 0;
    math::q32_32 scanRange{};
    math::q32_32 predictTargetVelocityFactor{};
    uint32_t authoredOrder = 0;
};

struct ObjectCombatInitializationPlan final {
    container::Vector<ObjectPointDefenseLaserRulePlan> pointDefenseRules;
    container::Array<ObjectTurretRecipe, 2> turrets;
    bool turretsLinked = false;
};

[[nodiscard]] container::SharedPtr<const ObjectCombatInitializationPlan>
compileObjectCombatInitializationPlan(
    const game::ThingTemplate& templateData);

} // namespace engine
