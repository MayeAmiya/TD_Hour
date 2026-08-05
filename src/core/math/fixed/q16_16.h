#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>

// VCL vector types
#include <vectorclass.h>

namespace math {

class q16_16
{
    int32_t m_value_;

    static constexpr int FRAC_BITS = 16;
    static constexpr int32_t ONE = 1 << FRAC_BITS;

    [[nodiscard]] static constexpr int32_t saturate_raw(int64_t value) noexcept
    {
        return value > std::numeric_limits<int32_t>::max()
            ? std::numeric_limits<int32_t>::max()
            : value < std::numeric_limits<int32_t>::min()
                ? std::numeric_limits<int32_t>::min()
                : static_cast<int32_t>(value);
    }

    [[nodiscard]] static constexpr int32_t from_float_raw(float value) noexcept
    {
        const double scaled = static_cast<double>(value) *
            static_cast<double>(ONE);
        if (!(scaled == scaled)) return 0;
        if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
            return std::numeric_limits<int32_t>::max();
        if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
            return std::numeric_limits<int32_t>::min();
        return static_cast<int32_t>(scaled);
    }

    [[nodiscard]] static constexpr int32_t add_raw(int32_t left,
                                                    int32_t right) noexcept
    {
        return saturate_raw(static_cast<int64_t>(left) + right);
    }

    [[nodiscard]] static constexpr int32_t subtract_raw(
        int32_t left, int32_t right) noexcept
    {
        return saturate_raw(static_cast<int64_t>(left) - right);
    }

    [[nodiscard]] static constexpr int32_t multiply_raw(
        int32_t left, int32_t right) noexcept
    {
        const int64_t product = static_cast<int64_t>(left) * right;
        // Match MSVC's arithmetic right-shift result without relying on a
        // signed-negative shift: negative values round toward -infinity.
        const int64_t scaled = product >= 0
            ? product / ONE
            : -((-product + ONE - 1) / ONE);
        return saturate_raw(scaled);
    }

    [[nodiscard]] static constexpr int32_t divide_raw(
        int32_t numerator, int32_t denominator) noexcept
    {
        if (denominator == 0) {
            if (numerator == 0) return 0;
            return numerator > 0 ? std::numeric_limits<int32_t>::max()
                                 : std::numeric_limits<int32_t>::min();
        }
        const int64_t scaled = static_cast<int64_t>(numerator) * ONE;
        return saturate_raw(scaled / denominator);
    }

    [[nodiscard]] static constexpr int32_t negate_raw(int32_t value) noexcept
    {
        return value == std::numeric_limits<int32_t>::min()
            ? std::numeric_limits<int32_t>::max()
            : -value;
    }

public:
    constexpr q16_16() noexcept : m_value_(0) {}
    constexpr explicit q16_16(int16_t i) noexcept
        : m_value_(static_cast<int32_t>(i) * ONE) {}
    constexpr explicit q16_16(float f) noexcept : m_value_(from_float_raw(f)) {}

    static constexpr q16_16 from_raw(int32_t v) noexcept { q16_16 r; r.m_value_ = v; return r; }

    constexpr q16_16 operator+(q16_16 r) const noexcept { return from_raw(add_raw(m_value_, r.m_value_)); }
    constexpr q16_16 operator-(q16_16 r) const noexcept { return from_raw(subtract_raw(m_value_, r.m_value_)); }
    constexpr q16_16 operator*(q16_16 r) const noexcept { return from_raw(multiply_raw(m_value_, r.m_value_)); }
    constexpr q16_16 operator/(q16_16 r) const noexcept { return from_raw(divide_raw(m_value_, r.m_value_)); }

    q16_16& operator+=(q16_16 r) noexcept { m_value_ = add_raw(m_value_, r.m_value_); return *this; }
    q16_16& operator-=(q16_16 r) noexcept { m_value_ = subtract_raw(m_value_, r.m_value_); return *this; }
    q16_16& operator*=(q16_16 r) noexcept { m_value_ = multiply_raw(m_value_, r.m_value_); return *this; }
    q16_16& operator/=(q16_16 r) noexcept { m_value_ = divide_raw(m_value_, r.m_value_); return *this; }

    constexpr q16_16 operator-() const noexcept { return from_raw(negate_raw(m_value_)); }

    constexpr bool operator==(q16_16 r) const noexcept { return m_value_ == r.m_value_; }
    constexpr bool operator!=(q16_16 r) const noexcept { return m_value_ != r.m_value_; }
    constexpr bool operator<(q16_16 r) const noexcept { return m_value_ < r.m_value_; }
    constexpr bool operator>(q16_16 r) const noexcept { return m_value_ > r.m_value_; }
    constexpr bool operator<=(q16_16 r) const noexcept { return m_value_ <= r.m_value_; }
    constexpr bool operator>=(q16_16 r) const noexcept { return m_value_ >= r.m_value_; }

    [[nodiscard]] int16_t to_int() const noexcept { return static_cast<int16_t>(m_value_ >> FRAC_BITS); }
    [[nodiscard]] float to_float() const noexcept { return static_cast<float>(m_value_) / static_cast<float>(ONE); }
    [[nodiscard]] int32_t raw() const noexcept { return m_value_; }

    static constexpr q16_16 abs(q16_16 v) noexcept { return v.m_value_ < 0 ? from_raw(negate_raw(v.m_value_)) : v; }
    static q16_16 min(q16_16 a, q16_16 b) noexcept { return a < b ? a : b; }
    static q16_16 max(q16_16 a, q16_16 b) noexcept { return a > b ? a : b; }
    static q16_16 lerp(q16_16 a, q16_16 b, q16_16 t) noexcept { return a + (b - a) * t; }
    static q16_16 clamp(q16_16 v, q16_16 lo, q16_16 hi) noexcept { return min(max(v, lo), hi); }
};

// ── SIMD batch types ────────────────────────────────────────────────────

// 4× q16_16 packed in Vec4i (4×int32). Each lane holds one q16_16's raw int32.
using q16_16x4 = Vec4i;

inline q16_16x4 q16_16x4_from_raw(Vec4i v) noexcept { return v; }
inline q16_16x4 q16_16x4_load(const q16_16* ptr) noexcept { return Vec4i(ptr->raw()); } // single
inline q16_16x4 q16_16x4_load(const int32_t* raw) noexcept { return Vec4i().load(raw); }
inline void     q16_16x4_store(int32_t* raw, q16_16x4 v) noexcept { v.store(raw); }

inline q16_16x4 q16_16x4_mul(q16_16x4 a, q16_16x4 b) noexcept
{
    // Widen before multiplication. Vec4i operator* keeps only the low 32
    // bits, which is not a q16.16 product and diverges badly near the range
    // limits. Shift the signed 64-bit products arithmetically (the scalar
    // contract rounds negative values toward -infinity), then saturate each
    // lane to int32 before packing it back.
    const Vec2q productLow =
        (extend_low(a) * extend_low(b)) >> 16;
    const Vec2q productHigh =
        (extend_high(a) * extend_high(b)) >> 16;
    alignas(16) int64_t scaled[4];
    productLow.store(scaled);
    productHigh.store(scaled + 2);
    alignas(16) int32_t packed[4];
    for (size_t lane = 0; lane < 4; ++lane) {
        packed[lane] = scaled[lane] > std::numeric_limits<int32_t>::max()
            ? std::numeric_limits<int32_t>::max()
            : scaled[lane] < std::numeric_limits<int32_t>::min()
                ? std::numeric_limits<int32_t>::min()
                : static_cast<int32_t>(scaled[lane]);
    }
    return Vec4i().load(packed);
}

inline q16_16x4 q16_16x4_add(q16_16x4 a, q16_16x4 b) noexcept { return a + b; }
inline q16_16x4 q16_16x4_sub(q16_16x4 a, q16_16x4 b) noexcept { return a - b; }

#if MAX_VECTOR_SIZE >= 256
// 8× q16_16 packed in Vec8s (8×int16). Each lane holds one q16_16 as int16.
// Note: q16_16 uses int32 internally, so Vec8s would require narrowing.
// For 8-wide, use Vec8i (8×int32, AVX2) instead.
using q16_16x8 = Vec8i;

inline q16_16x8 q16_16x8_mul(q16_16x8 a, q16_16x8 b) noexcept {
    return q16_16x8{
        q16_16x4_mul(a.get_low(), b.get_low()),
        q16_16x4_mul(a.get_high(), b.get_high())};
}
inline q16_16x8 q16_16x8_add(q16_16x8 a, q16_16x8 b) noexcept { return a + b; }
inline q16_16x8 q16_16x8_sub(q16_16x8 a, q16_16x8 b) noexcept { return a - b; }
#endif

} // namespace math
