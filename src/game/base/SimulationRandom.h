#pragma once

#include "core/container/container_types.h"
#include "core/math/fixed/q32_32.h"
#include <cstdint>

namespace engine {

// Session-owned deterministic simulation RNG. It intentionally has no
// random_device, thread-local state, renderer dependency, or wall-clock
// seed. The six-word recurrence is compatible with the original logic RNG's
// stream shape while making ownership explicit: future gameplay systems share
// one GameSession instance instead of reaching for a global singleton.
class SimulationRandom final {
public:
    void seed(uint32_t value) noexcept;

    [[nodiscard]] uint32_t nextUInt32() noexcept;
    // Both bounds are inclusive, matching the legacy GameLogicRandomValue
    // contract. An inverted/equal interval deterministically returns hi.
    [[nodiscard]] int32_t integerInclusive(int32_t lo, int32_t hi) noexcept;
    // The upper bound is reachable when nextUInt32() returns UINT32_MAX,
    // matching the original real-valued helper.
    [[nodiscard]] float realInclusive(float lo, float hi) noexcept;
    // Fixed-point counterpart for authoritative geometry. It consumes the
    // same single stream value as realInclusive() for a non-empty interval.
    [[nodiscard]] math::q32_32 fixedInclusive(
        math::q32_32 lo, math::q32_32 hi) noexcept;

private:
    container::Array<uint32_t, 6> m_state{};
};

} // namespace engine
