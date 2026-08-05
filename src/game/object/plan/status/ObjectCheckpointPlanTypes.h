#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "core/math/fixed/q32_32.h"

#include <cstdint>

namespace game {

struct ThingTemplate;

struct ObjectCheckpointRule final {
    uint32_t authoredOrder = 0;
    uint32_t scanDelayMilliseconds = 1000;
};

struct ObjectCheckpointPlan final {
    container::Vector<ObjectCheckpointRule> rules;
};

[[nodiscard]] container::SharedPtr<const ObjectCheckpointPlan>
compileObjectCheckpointPlan(const ThingTemplate& templateData);

} // namespace game

