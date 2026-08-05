#pragma once

#include "core/container/container_types.h"


#include <cstdint>
#include <optional>
namespace game {

struct ThingTemplate;

// Immutable final-recipe projection of FloatUpdate. The original module owns
// only an Enabled switch; water surface and transform authority stay outside
// template data.
struct ObjectFloatRule final {
    uint32_t authoredOrder = 0;
    bool startsEnabled = false;
};

struct ObjectFloatPlan final {
    container::Vector<ObjectFloatRule> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectFloatPlan>
compileObjectFloatPlan(const ThingTemplate& templateData);

} // namespace game

namespace game::terrain {
class TerrainLogic;
}
