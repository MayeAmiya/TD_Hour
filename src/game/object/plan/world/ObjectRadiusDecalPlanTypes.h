#pragma once

#include "core/container/container_types.h"

#include <cstdint>

namespace game {

struct ThingTemplate;

struct ObjectRadiusDecalRule final {
    uint32_t authoredOrder = 0;
};

struct ObjectRadiusDecalPlan final {
    container::Vector<ObjectRadiusDecalRule> rules;
};

[[nodiscard]] container::SharedPtr<const ObjectRadiusDecalPlan>
compileObjectRadiusDecalPlan(const ThingTemplate& templateData);

} // namespace game
