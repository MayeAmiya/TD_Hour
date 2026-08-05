#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

namespace game {

struct ThingTemplate;

struct ObjectOverchargeParameters final {
    uint32_t authoredOrder = 0;
    math::q32_32 healthPercentToDrainPerSecond{};
    math::q32_32 notAllowedWhenHealthBelowPercent{};
};

struct ObjectOverchargePlan final {
    container::Vector<ObjectOverchargeParameters> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectOverchargePlan>
compileObjectOverchargePlan(const ThingTemplate& templateData);

} // namespace game

