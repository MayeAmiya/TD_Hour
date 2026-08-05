#pragma once

#include "q32_32.h"

namespace math {

struct q32_32_sincos final {
    q32_32 sine{};
    q32_32 cosine{int32_t{1}};
};

// Deterministic fixed polynomial with explicit range reduction. This is for
// lockstep transforms, not renderer animation sampling.
[[nodiscard]] q32_32_sincos fixed_sincos(q32_32 radians) noexcept;
[[nodiscard]] q32_32 fixed_sin(q32_32 radians) noexcept;
[[nodiscard]] q32_32 fixed_cos(q32_32 radians) noexcept;
// Deterministic vectoring CORDIC. Returns an angle in [-pi, pi].
[[nodiscard]] q32_32 fixed_atan2(q32_32 y, q32_32 x) noexcept;

} // namespace math
