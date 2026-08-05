#pragma once

#include "core/ecs/ObjectId.h"
#include "core/ecs/registry.h"

namespace engine {

class ObjectLifecycle;

namespace session_command_policy {

// Shared confirmed-command admission policy. It reads the frozen archetype
// attached to a live object and exposes no ECS storage to callers.
[[nodiscard]] bool objectForbidsPlayerCommands(
    const ecs::registry& registry,
    const ObjectLifecycle& objects,
    ObjectId object) noexcept;

} // namespace session_command_policy
} // namespace engine
