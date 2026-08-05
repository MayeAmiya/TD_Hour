#include "game/base/SimulationRandom.h"

#include <limits>

namespace engine {
namespace {

void addWithCarry(uint32_t& sum, uint32_t left, uint32_t right, uint32_t& carry) noexcept {
    sum = left + right + carry;
    // Preserve the source recurrence rather than substituting an unrelated
    // standard engine: its carry test intentionally compares against both
    // operands after unsigned wraparound.
    carry = (sum < left || sum < right) ? 1u : 0u;
}

} // namespace

void SimulationRandom::seed(uint32_t value) noexcept {
    uint32_t accumulator = value;
    accumulator += 0xf22d0e56u;
    m_state[0] = accumulator;
    accumulator += 0x883126e9u - 0xf22d0e56u;
    m_state[1] = accumulator;
    accumulator += 0xc624dd2fu - 0x883126e9u;
    m_state[2] = accumulator;
    accumulator += 0x0702c49cu - 0xc624dd2fu;
    m_state[3] = accumulator;
    accumulator += 0x9e353f7du - 0x0702c49cu;
    m_state[4] = accumulator;
    accumulator += 0x6fdf3b64u - 0x9e353f7du;
    m_state[5] = accumulator;
}

uint32_t SimulationRandom::nextUInt32() noexcept {
    uint32_t value = 0;
    uint32_t carry = 0;
    addWithCarry(value, m_state[5], m_state[4], carry);
    m_state[4] = value;
    addWithCarry(value, value, m_state[3], carry);
    m_state[3] = value;
    addWithCarry(value, value, m_state[2], carry);
    m_state[2] = value;
    addWithCarry(value, value, m_state[1], carry);
    m_state[1] = value;
    addWithCarry(value, value, m_state[0], carry);
    m_state[0] = value;

    if (!++m_state[5]) {
        if (!++m_state[4]) {
            if (!++m_state[3]) {
                if (!++m_state[2]) {
                    if (!++m_state[1]) {
                        ++m_state[0];
                        ++value;
                    }
                }
            }
        }
    }
    return value;
}

int32_t SimulationRandom::integerInclusive(int32_t lo, int32_t hi) noexcept {
    if (lo >= hi) return hi;
    const uint64_t width = static_cast<uint64_t>(static_cast<int64_t>(hi) -
                                                  static_cast<int64_t>(lo)) + 1u;
    const uint64_t offset = static_cast<uint64_t>(nextUInt32()) % width;
    return static_cast<int32_t>(static_cast<int64_t>(lo) + static_cast<int64_t>(offset));
}

float SimulationRandom::realInclusive(float lo, float hi) noexcept {
    if (lo >= hi) return hi;
    constexpr float inverseMax = 1.0f / static_cast<float>(std::numeric_limits<uint32_t>::max());
    return static_cast<float>(nextUInt32()) * inverseMax * (hi - lo) + lo;
}

math::q32_32 SimulationRandom::fixedInclusive(
    math::q32_32 lo, math::q32_32 hi) noexcept {
    if (lo >= hi) return hi;
    const math::q32_32 unit = math::q32_32::from_fraction(
        static_cast<int64_t>(nextUInt32()),
        static_cast<int64_t>(std::numeric_limits<uint32_t>::max()));
    return lo + (hi - lo) * unit;
}

} // namespace engine
