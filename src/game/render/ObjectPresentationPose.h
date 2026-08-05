#pragma once

#include "core/ecs/registry.h"
#include "core/math/wwmath/vector/float3.h"

namespace engine {

struct TransformComponent;

// One renderer-neutral projection of an object's current presentation pose.
// Simulation owns the source components; extraction, FX attachment and other
// presentation producers must consume this value instead of independently
// rebuilding a slightly different object transform.
struct ObjectPresentationPose final {
    math::vec3 position;
    float rollRadians = 0.0f;
    float pitchRadians = 0.0f;
    float yawRadians = 0.0f;
};

[[nodiscard]] ObjectPresentationPose projectObjectPresentationPose(
    const ecs::registry& registry,
    ecs::entity entity,
    const TransformComponent& transform) noexcept;

} // namespace engine
