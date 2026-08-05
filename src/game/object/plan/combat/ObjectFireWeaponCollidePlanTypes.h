#pragma once

#include "core/container/container_types.h"

#include "game/object/contracts/ObjectDeathReaction.h"

#include <cstdint>
namespace game {

struct ThingTemplate;

struct ObjectFireWeaponCollideRule final {
    uint32_t authoredOrder = 0;
    container::String collideWeapon;
    ObjectStatusMask requiredStatus = 0;
    ObjectStatusMask forbiddenStatus = 0;
    bool fireOnce = false;
};

struct ObjectFireWeaponCollidePlan final {
    container::Vector<ObjectFireWeaponCollideRule> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectFireWeaponCollidePlan>
compileObjectFireWeaponCollidePlan(const ThingTemplate& templateData);

} // namespace game
