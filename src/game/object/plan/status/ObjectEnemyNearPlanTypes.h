#pragma once

#include "core/container/container_types.h"

#include <cstdint>

namespace game {

struct ThingTemplate;
namespace terrain {
struct MapVisibilitySnapshot;
}

struct ObjectEnemyNearRule final {
    uint32_t authoredOrder = 0;
    uint32_t scanDelayMilliseconds = 0;
};

struct ObjectEnemyNearPlan final {
    container::Vector<ObjectEnemyNearRule> rules;
};

[[nodiscard]] container::SharedPtr<const ObjectEnemyNearPlan>
compileObjectEnemyNearPlan(const ThingTemplate& templateData);

} // namespace game
