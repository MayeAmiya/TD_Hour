#pragma once

#include "core/ecs/ObjectId.h"
#include "core/ecs/registry.h"

namespace engine
{

class ObjectLifecycle;
class ObjectOwnershipIndex;

namespace session_query
{

// Read-only containment-network query shared by player commands and object
// AI. It receives the exact world indexes it reads and exposes no Session or
// mutable registry capability.
[[nodiscard]] bool canExitPassengerThrough(const ecs::registry& registry,
                                           const ObjectLifecycle& objects,
                                           const ObjectOwnershipIndex& ownership,
                                           ObjectId container,
                                           ObjectId passenger) noexcept;

} // namespace session_query
} // namespace engine
