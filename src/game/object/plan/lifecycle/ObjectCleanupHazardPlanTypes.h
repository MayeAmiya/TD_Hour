#pragma once

#include "core/container/container_types.h"
#include "game/object/definition/CombatProfile.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

namespace game {

struct ThingTemplate;

// Immutable projection of CleanupHazardUpdate.  The authored module is an
// autonomous combat controller, so its weapon choice and scan policy remain
// separate from mutable order/weapon state on each live object.
struct ObjectCleanupHazardRule final {
    uint32_t authoredOrder = 0;
    WeaponSlot weaponSlot = WeaponSlot::Primary;
    uint32_t scanRateMilliseconds = 0;
    math::q32_32 scanRange{};
};

struct ObjectCleanupHazardPlan final {
    container::Vector<ObjectCleanupHazardRule> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectCleanupHazardPlan>
compileObjectCleanupHazardPlan(const ThingTemplate& templateData);

} // namespace game
