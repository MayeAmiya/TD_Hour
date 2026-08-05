#pragma once

#include "core/ecs/registry.h"

#include <cstdint>

namespace engine::session_animation {

void updateConfirmedClocks(
    ecs::registry& registry,
    float fixedDeltaSeconds,
    uint64_t confirmedTick) noexcept;

} // namespace engine::session_animation
