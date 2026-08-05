#pragma once

#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"

namespace engine {

// TransformComponent remains the compatibility/render projection, while a
// Projectile or free Physics body may own the fixed-point position. Systems
// that intentionally perform an Object::setPosition-equivalent write must go
// through this narrow boundary so a later publisher cannot resurrect stale
// coordinates from another authority component.
[[nodiscard]] LogicFixedVec3 readAuthoritativeObjectPosition(
    const ecs::registry& registry, ecs::entity entity,
    const TransformComponent& transform) noexcept;

[[nodiscard]] math::q32_32 readAuthoritativeObjectYaw(
    const ecs::registry& registry, ecs::entity entity,
    const TransformComponent& transform) noexcept;

void writeAuthoritativeObjectPosition(ecs::registry& registry, ecs::entity entity,
                                      const LogicFixedVec3& position) noexcept;

void writeAuthoritativeObjectYaw(ecs::registry& registry, ecs::entity entity,
                                 math::q32_32 yawRadians) noexcept;

void writeAuthoritativeObjectTransform(ecs::registry& registry,
                                       ecs::entity entity,
                                       const LogicFixedVec3& position,
                                       math::q32_32 yawRadians) noexcept;

} // namespace engine
