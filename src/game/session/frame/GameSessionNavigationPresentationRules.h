#pragma once

#include "core/ecs/registry.h"

namespace engine::session_navigation {

[[nodiscard]] bool blocksGround(
    const ecs::registry& registry,
    ecs::entity entity) noexcept;

[[nodiscard]] bool blocksAircraft(
    const ecs::registry& registry,
    ecs::entity entity) noexcept;

[[nodiscard]] bool isRubbleBlocker(
    const ecs::registry& registry,
    ecs::entity entity) noexcept;

} // namespace engine::session_navigation
