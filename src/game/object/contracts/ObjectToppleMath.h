#pragma once

#include "core/math/fixed/q32_32.h"

namespace game {

inline constexpr math::q32_32 kTopplePi =
    math::q32_32::from_raw(13'493'037'705ll);
inline constexpr math::q32_32 kToppleHalfPi =
    math::q32_32::from_raw(6'746'518'852ll);
inline constexpr math::q32_32 kToppleTwoPi =
    math::q32_32::from_raw(26'986'075'409ll);

[[nodiscard]] inline math::q32_32 normalizeToppleAngle(
    math::q32_32 angle) noexcept {
    int64_t raw = angle.raw() % kToppleTwoPi.raw();
    if (raw > kTopplePi.raw()) raw -= kToppleTwoPi.raw();
    if (raw < -kTopplePi.raw()) raw += kToppleTwoPi.raw();
    return math::q32_32::from_raw(raw);
}

[[nodiscard]] inline math::q32_32 shortestToppleAngleDelta(
    math::q32_32 from, math::q32_32 to) noexcept {
    return normalizeToppleAngle(to - from);
}

[[nodiscard]] inline math::q32_32 angleClosestTo(
    math::q32_32 first, math::q32_32 second,
    math::q32_32 desired) noexcept {
    first = normalizeToppleAngle(first);
    second = normalizeToppleAngle(second);
    return math::q32_32::abs(shortestToppleAngleDelta(desired, first)) <
            math::q32_32::abs(shortestToppleAngleDelta(desired, second))
        ? first : second;
}

} // namespace game
