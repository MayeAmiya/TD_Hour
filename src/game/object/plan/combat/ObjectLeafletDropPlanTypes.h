#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

namespace game {

struct ThingTemplate;

struct ObjectLeafletDropParameters final {
    uint32_t authoredOrder = 0;
    uint32_t delayMilliseconds = 0;
    uint32_t disabledDurationMilliseconds = 0;
    math::q32_32 radius{60};
    container::String particleSystem;
    bool delayAuthored = false;
};

struct ObjectLeafletDropPlan final {
    container::Vector<ObjectLeafletDropParameters> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectLeafletDropPlan>
compileObjectLeafletDropPlan(const ThingTemplate& templateData);

} // namespace game
