#pragma once

#include "core/container/container_types.h"

#include "core/ecs/ObjectId.h"

#include <cstdint>
#include <limits>
namespace game {

struct ThingTemplate;

// BaseRegenerateUpdate has no module-local fields: all behavior comes from
// the frozen GameData BaseRegen globals. Retain every final recipe occurrence
// nevertheless, because a modded final recipe can legally carry more than one
// Update/Damage callback and RefCode would run each instance.
struct ObjectBaseRegenerateRule final {
    uint32_t authoredOrder = 0;
};

struct ObjectBaseRegeneratePlan final {
    container::Vector<ObjectBaseRegenerateRule> rules;
};

[[nodiscard]] container::SharedPtr<const ObjectBaseRegeneratePlan>
compileObjectBaseRegeneratePlan(const ThingTemplate& templateData);

} // namespace game

