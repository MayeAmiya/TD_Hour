#pragma once

#include "core/container/container_types.h"


#include <cstdint>
namespace game {

struct ThingTemplate;

struct ObjectFireWeaponUpdateParameters final {
    uint32_t authoredOrder = 0;
    container::String weapon;
    uint32_t initialDelayMilliseconds = 0;
    uint32_t exclusiveWeaponDelayMilliseconds = 0;
};

struct ObjectFireWeaponUpdatePlan final {
    container::Vector<ObjectFireWeaponUpdateParameters> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectFireWeaponUpdatePlan>
compileObjectFireWeaponUpdatePlan(const ThingTemplate& templateData);

} // namespace game
