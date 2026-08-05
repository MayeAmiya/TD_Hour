#pragma once

#include "core/container/container_types.h"

#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <limits>

namespace game {

struct ThingTemplate;

// Immutable projection of one final SupplyWarehouseCripplingBehavior
// declaration. Authored durations deliberately remain milliseconds here;
// the session's fixed logic rate is applied once when the entity is spawned.
struct ObjectSupplyWarehouseCripplingParameters final {
    uint32_t authoredOrder = 0;
    uint32_t selfHealSuppressionMilliseconds = 0;
    uint32_t selfHealDelayMilliseconds = 0;
    math::q32_32 selfHealAmount{};
};

struct ObjectSupplyWarehouseCripplingPlan final {
    container::Vector<ObjectSupplyWarehouseCripplingParameters> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectSupplyWarehouseCripplingPlan>
compileObjectSupplyWarehouseCripplingPlan(const ThingTemplate& templateData);

} // namespace game

