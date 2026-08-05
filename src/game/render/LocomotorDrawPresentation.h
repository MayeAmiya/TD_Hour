#pragma once

#include <cstdint>

#include "core/ecs/registry.h"
#include "core/math/fixed/q32_32.h"

namespace engine {

class ObjectLifecycle;

// Advances only Drawable-equivalent attitude state. It never writes
// Transform, Physics, navigation or any other authoritative component.
void updateLocomotorDrawPresentation(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    math::q32_32 logicDeltaSeconds,
    uint64_t confirmedTick) noexcept;

} // namespace engine
