#pragma once

#include "core/ecs/ObjectId.h"
#include "core/ecs/registry.h"
#include "core/math/wwmath/vector/float3.h"
#include "game/fx/runtime/GameFxEvents.h"

#include <optional>

namespace engine {

class ObjectLifecycle;
struct FxInvocationAnchorSnapshot;
struct ObjectTransitionDamageFxAnchor;

namespace session_fx {

[[nodiscard]] std::optional<game::FxInvocationAnchor> snapshotAnchor(
    const ecs::registry& registry,
    const ObjectLifecycle& lifecycle,
    ObjectId object) noexcept;

[[nodiscard]] game::FxInvocationAnchor snapshotAnchor(
    const FxInvocationAnchorSnapshot& source,
    ObjectId object = INVALID_OBJECT_ID) noexcept;

[[nodiscard]] game::FxInvocationAnchor snapshotAnchor(
    const ObjectTransitionDamageFxAnchor& source,
    ObjectId object = INVALID_OBJECT_ID) noexcept;

[[nodiscard]] game::FxInvocationAnchor worldAnchor(
    math::vec3 position,
    ObjectId object = INVALID_OBJECT_ID) noexcept;

} // namespace session_fx
} // namespace engine
