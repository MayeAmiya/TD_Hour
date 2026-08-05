#pragma once

#include "math/fixed/q32_32.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace engine {

// Deterministic position value shared by authored plans, simulation
// components, and value-only event contracts.  It intentionally contains no
// ECS entity, registry, renderer, or mutable content-store reference.
struct LogicFixedVec3 final {
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};

    [[nodiscard]] static math::q32_32 scalarFromFloat(float value) noexcept {
        if (!std::isfinite(value)) return {};
        constexpr float minimum =
            static_cast<float>(std::numeric_limits<int32_t>::min());
        constexpr float maximum =
            static_cast<float>(std::numeric_limits<int32_t>::max());
        if (value <= minimum) {
            return math::q32_32::from_raw(
                std::numeric_limits<int64_t>::min());
        }
        if (value >= maximum) {
            return math::q32_32::from_raw(
                std::numeric_limits<int64_t>::max());
        }
        return math::q32_32{value};
    }

    [[nodiscard]] static LogicFixedVec3 fromFloats(
        float valueX, float valueY, float valueZ) noexcept {
        return {scalarFromFloat(valueX), scalarFromFloat(valueY),
                scalarFromFloat(valueZ)};
    }
};

} // namespace engine
