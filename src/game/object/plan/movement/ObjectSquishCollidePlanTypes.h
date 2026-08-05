#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>

namespace game {
struct ThingTemplate;
namespace terrain {
class TerrainLogic;
}

struct ObjectSquishCollideRule final {
    uint32_t authoredOrder = 0;
};

struct ObjectSquishCollidePlan final {
    container::Vector<ObjectSquishCollideRule> rules;
    bool hasHijackerUpdate = false;
};

[[nodiscard]] container::SharedPtr<const ObjectSquishCollidePlan>
compileObjectSquishCollidePlan(const ThingTemplate& templateData);

}

