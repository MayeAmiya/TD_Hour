#pragma once

#include "core/container/container_types.h"

#include "game/base/DamageTypes.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
namespace game {

struct ThingTemplate;

// Immutable projection of one final PoisonedBehavior recipe. Durations stay
// in authored milliseconds until the session's fixed logic rate is known.
struct ObjectPoisonedParameters final {
    uint32_t authoredOrder = 0;
    uint32_t poisonDamageIntervalMilliseconds = 0;
    uint32_t poisonDurationMilliseconds = 0;
};

struct ObjectPoisonedPlan final {
    container::Vector<ObjectPoisonedParameters> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectPoisonedPlan>
compileObjectPoisonedPlan(const ThingTemplate& templateData);

} // namespace game

