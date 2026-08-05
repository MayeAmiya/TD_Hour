#pragma once

#include "core/container/container_types.h"

#include "game/object/definition/CombatProfile.h"
#include "game/object/creation/ObjectCreationListRuntime.h"
#include "game/object/plan/combat/ObjectFireWeaponBehaviorPlanTypes.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
namespace game {

struct ThingTemplate;

struct ObjectFlammableParameters final {
    uint32_t authoredOrder = 0;
    uint32_t burnedDelayMilliseconds = 0;
    uint32_t aflameDurationMilliseconds = 0;
    uint32_t aflameDamageDelayMilliseconds = 0;
    int32_t aflameDamageAmount = 0;
    container::String burningSoundName;
    math::q32_32 flameDamageLimit{20};
    uint32_t flameDamageExpirationMilliseconds = 2000;
};

struct ObjectFlammablePlan final {
    container::Vector<ObjectFlammableParameters> rules;
    container::Vector<container::String> diagnostics;
};

struct ObjectFireSpreadParameters final {
    uint32_t authoredOrder = 0;
    container::String embersObjectCreationList;
    uint32_t minimumSpreadDelayMilliseconds = 0;
    uint32_t maximumSpreadDelayMilliseconds = 0;
    math::q32_32 spreadTryRange{};
};

struct ObjectFireSpreadPlan final {
    container::Vector<ObjectFireSpreadParameters> rules;
    container::Vector<container::String> diagnostics;
};

struct ObjectFireOclAfterCooldownParameters final {
    uint32_t authoredOrder = 0;
    WeaponSlot weaponSlot = WeaponSlot::Primary;
    container::String objectCreationList;
    uint32_t minimumShotsRequired = 1;
    uint32_t oclLifetimePerSecondMilliseconds = 1000;
    uint32_t oclLifetimeMaximumMilliseconds = 0;
    bool hasAuthoredLifetimeMaximum = false;
    ObjectUpgradeMuxRecipe upgradeMux;
};

struct ObjectFireOclAfterCooldownPlan final {
    container::Vector<ObjectFireOclAfterCooldownParameters> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectFlammablePlan>
compileObjectFlammablePlan(const ThingTemplate& templateData);
[[nodiscard]] container::SharedPtr<const ObjectFireSpreadPlan>
compileObjectFireSpreadPlan(const ThingTemplate& templateData);
[[nodiscard]] container::SharedPtr<const ObjectFireOclAfterCooldownPlan>
compileObjectFireOclAfterCooldownPlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog = nullptr);

} // namespace game
