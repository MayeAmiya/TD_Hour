#pragma once

#include "core/container/container_types.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <optional>

namespace game {

struct ThingTemplate;
namespace terrain { class TerrainLogic; }

// Immutable, pointer-free projection of StickyBombUpdateModuleData. The bone
// name is preserved for Mod/content parity even though RefCode's runtime never
// consumes it after parsing.
struct ObjectStickyBombRule final {
    uint32_t authoredOrder = 0;
    container::String attachToTargetBone;
    math::q32_32 offsetZ{int32_t{10}};
    container::String geometryBasedDamageWeapon;
    container::String geometryBasedDamageFx;
};

struct ObjectStickyBombPlan final {
    container::Vector<ObjectStickyBombRule> rules;
    container::String createdSound;
    container::String pingSound;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectStickyBombPlan>
compileObjectStickyBombPlan(const ThingTemplate& templateData);

} // namespace game
