#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <limits>

namespace game {

struct ThingTemplate;

struct ObjectAnimationSteeringRule final {
    uint32_t authoredOrder = 0;
    uint32_t minimumTransitionMilliseconds = 0;
};

struct ObjectAnimationSteeringPlan final {
    container::Vector<ObjectAnimationSteeringRule> rules;
};

[[nodiscard]] container::SharedPtr<const ObjectAnimationSteeringPlan>
compileObjectAnimationSteeringPlan(const ThingTemplate& templateData);

} // namespace game

