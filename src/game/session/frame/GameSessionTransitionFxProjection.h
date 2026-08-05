#pragma once

#include "core/math/wwmath/vector/float3.h"
#include "game/object/simulation/combat/ObjectTransitionDamageFx.h"

namespace engine::session_fx {

[[nodiscard]] LogicFixedVec3 transitionWorldPositionFixed(
    const ObjectTransitionDamageFxEvent& event) noexcept;

[[nodiscard]] math::vec3 transitionWorldPosition(
    const ObjectTransitionDamageFxEvent& event) noexcept;

} // namespace engine::session_fx
