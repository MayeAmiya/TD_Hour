#include "game/session/frame/GameSessionTransitionFxProjection.h"

#include "math/fixed/q32_32_trig.h"

namespace engine::session_fx {

LogicFixedVec3 transitionWorldPositionFixed(
    const ObjectTransitionDamageFxEvent& event) noexcept {
    if (event.location.kind == game::ObjectTransitionDamageFxLocationKind::Bone) {
        return event.primary.position;
    }
    LogicFixedVec3 value = event.location.localPosition;
    const math::q32_32_sincos x =
        math::fixed_sincos(-event.primary.rollRadians);
    value = {
        .x = value.x,
        .y = value.y * x.cosine - value.z * x.sine,
        .z = value.y * x.sine + value.z * x.cosine,
    };
    const math::q32_32_sincos y =
        math::fixed_sincos(event.primary.pitchRadians);
    value = {
        .x = value.x * y.cosine + value.z * y.sine,
        .y = value.y,
        .z = -value.x * y.sine + value.z * y.cosine,
    };
    const math::q32_32_sincos z =
        math::fixed_sincos(event.primary.yawRadians);
    value = {
        .x = value.x * z.cosine - value.y * z.sine,
        .y = value.x * z.sine + value.y * z.cosine,
        .z = value.z,
    };
    return {
        .x = event.primary.position.x + value.x,
        .y = event.primary.position.y + value.y,
        .z = event.primary.position.z + value.z,
    };
}

math::vec3 transitionWorldPosition(
    const ObjectTransitionDamageFxEvent& event) noexcept {
    const LogicFixedVec3 fixed = transitionWorldPositionFixed(event);
    return {
        fixed.x.to_float(),
        fixed.y.to_float(),
        fixed.z.to_float(),
    };
}

} // namespace engine::session_fx
