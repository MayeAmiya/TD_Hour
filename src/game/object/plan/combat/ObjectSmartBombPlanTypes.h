#pragma once

#include "core/container/container_types.h"
#include "math/fixed/q32_32.h"

namespace game {
struct ThingTemplate;
namespace terrain { class TerrainLogic; }

struct ObjectSmartBombRule final {
    uint32_t authoredOrder = 0;
    math::q32_32 courseCorrectionScalar{0.99};
};

struct ObjectSmartBombPlan final {
    container::Vector<ObjectSmartBombRule> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectSmartBombPlan>
compileObjectSmartBombPlan(const ThingTemplate& templateData);
} // namespace game
