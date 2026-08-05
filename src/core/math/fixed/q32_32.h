#pragma once

#include "container/container_types.h"

#include <cstdint>
#include <cmath>
#include <limits>
#include <type_traits>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include <vectorclass.h>

namespace math {

class q32_32
{
    int64_t m_value_;

    static constexpr int FRAC_BITS = 32;
    static constexpr int64_t ONE = 1LL << FRAC_BITS;

    [[nodiscard]] static constexpr uint64_t unsigned_magnitude(
        int64_t value) noexcept
    {
        return value < 0
            ? static_cast<uint64_t>(-(value + 1)) + uint64_t{1}
            : static_cast<uint64_t>(value);
    }

    [[nodiscard]] static constexpr int64_t from_double_raw(double value) noexcept
    {
        const double scaled = value * static_cast<double>(ONE);
        if (!(scaled == scaled)) return 0;
        if (scaled >= static_cast<double>(std::numeric_limits<int64_t>::max()))
            return std::numeric_limits<int64_t>::max();
        if (scaled <= static_cast<double>(std::numeric_limits<int64_t>::min()))
            return std::numeric_limits<int64_t>::min();
        return static_cast<int64_t>(scaled);
    }

    [[nodiscard]] static constexpr int64_t add_raw(int64_t left,
                                                    int64_t right) noexcept
    {
        if (right > 0 && left > std::numeric_limits<int64_t>::max() - right)
            return std::numeric_limits<int64_t>::max();
        if (right < 0 && left < std::numeric_limits<int64_t>::min() - right)
            return std::numeric_limits<int64_t>::min();
        return left + right;
    }

    [[nodiscard]] static constexpr int64_t subtract_raw(
        int64_t left, int64_t right) noexcept
    {
        if (right < 0 && left > std::numeric_limits<int64_t>::max() + right)
            return std::numeric_limits<int64_t>::max();
        if (right > 0 && left < std::numeric_limits<int64_t>::min() + right)
            return std::numeric_limits<int64_t>::min();
        return left - right;
    }

    [[nodiscard]] static constexpr int64_t negate_raw(int64_t value) noexcept
    {
        return value == std::numeric_limits<int64_t>::min()
            ? std::numeric_limits<int64_t>::max()
            : -value;
    }

    [[nodiscard]] static constexpr uint64_t divide_unsigned_128_by_64(
        uint64_t high, uint64_t low, uint64_t divisor,
        bool& overflow) noexcept
    {
        if (high >= divisor) {
            overflow = true;
            return std::numeric_limits<uint64_t>::max();
        }
        uint64_t quotient = 0;
        uint64_t remainder = high;
        for (int bit = 63; bit >= 0; --bit) {
            const bool carry =
                (remainder & (uint64_t{1} << 63u)) != 0;
            remainder = (remainder << 1u) | ((low >> bit) & 1u);
            if (carry || remainder >= divisor) {
                remainder -= divisor;
                quotient |= uint64_t{1} << bit;
            }
        }
        return quotient;
    }

    [[nodiscard]] static constexpr int64_t apply_divide_sign(
        uint64_t quotient, bool negative, bool overflow) noexcept
    {
        const uint64_t negativeLimit = uint64_t{1} << 63u;
        const uint64_t limit = negative
            ? negativeLimit
            : static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        if (overflow || quotient > limit) {
            return negative ? std::numeric_limits<int64_t>::min()
                            : std::numeric_limits<int64_t>::max();
        }
        if (!negative) return static_cast<int64_t>(quotient);
        if (quotient == negativeLimit)
            return std::numeric_limits<int64_t>::min();
        return -static_cast<int64_t>(quotient);
    }

public:
    constexpr q32_32() noexcept : m_value_(0) {}
    constexpr explicit q32_32(int32_t i) noexcept
        : m_value_(static_cast<int64_t>(i) * ONE) {}
    constexpr explicit q32_32(float f) noexcept
        : m_value_(from_double_raw(static_cast<double>(f))) {}
    constexpr explicit q32_32(double d) noexcept : m_value_(from_double_raw(d)) {}

    static constexpr q32_32 from_raw(int64_t v) noexcept { q32_32 r; r.m_value_ = v; return r; }
    static constexpr q32_32 from_fraction(int64_t num, int64_t den) noexcept { return from_raw(div_128(num, den)); }

    // 128-bit multiply: (a * b) >> 32
    static int64_t mul_128(int64_t a, int64_t b) noexcept
    {
#if defined(_MSC_VER) && defined(_M_X64)
        int64_t high = 0;
        const uint64_t low = static_cast<uint64_t>(_mul128(a, b, &high));
        const uint64_t shiftedLow =
            (low >> FRAC_BITS) | (static_cast<uint64_t>(high) << FRAC_BITS);
        const int64_t shiftedHigh = high >> FRAC_BITS;
        if (shiftedHigh == 0 &&
            shiftedLow <= static_cast<uint64_t>(
                std::numeric_limits<int64_t>::max())) {
            return static_cast<int64_t>(shiftedLow);
        }
        if (shiftedHigh == -1 &&
            shiftedLow >= (uint64_t{1} << 63u)) {
            return static_cast<int64_t>(shiftedLow);
        }
        return (a < 0) != (b < 0)
            ? std::numeric_limits<int64_t>::min()
            : std::numeric_limits<int64_t>::max();
#else
        const __int128 product = static_cast<__int128>(a) * static_cast<__int128>(b);
        const __int128 scaled = product >> FRAC_BITS;
        if (scaled > std::numeric_limits<int64_t>::max())
            return std::numeric_limits<int64_t>::max();
        if (scaled < std::numeric_limits<int64_t>::min())
            return std::numeric_limits<int64_t>::min();
        return static_cast<int64_t>(scaled);
#endif
    }

    // 128-bit divide: (a << 32) / b
    static constexpr int64_t div_128(int64_t a, int64_t b) noexcept
    {
        if (b == 0) {
            if (a == 0) return 0;
            return a > 0 ? std::numeric_limits<int64_t>::max()
                         : std::numeric_limits<int64_t>::min();
        }
        const bool negative = (a < 0) != (b < 0);
        const uint64_t numerator = unsigned_magnitude(a);
        const uint64_t divisor = unsigned_magnitude(b);
        const uint64_t high = numerator >> FRAC_BITS;
        const uint64_t low = numerator << FRAC_BITS;
        bool overflow = false;
        uint64_t quotient = 0;
#if defined(_MSC_VER) && defined(_M_X64)
        if (!std::is_constant_evaluated() && high < divisor) {
            uint64_t remainder = 0;
            quotient = _udiv128(high, low, divisor, &remainder);
        } else {
            quotient = divide_unsigned_128_by_64(
                high, low, divisor, overflow);
        }
#else
        quotient = divide_unsigned_128_by_64(
            high, low, divisor, overflow);
#endif
        return apply_divide_sign(quotient, negative, overflow);
    }

    q32_32 operator+(q32_32 r) const noexcept { return from_raw(add_raw(m_value_, r.m_value_)); }
    q32_32 operator-(q32_32 r) const noexcept { return from_raw(subtract_raw(m_value_, r.m_value_)); }
    q32_32 operator*(q32_32 r) const noexcept { return from_raw(mul_128(m_value_, r.m_value_)); }
    q32_32 operator/(q32_32 r) const noexcept { return from_raw(div_128(m_value_, r.m_value_)); }

    q32_32& operator+=(q32_32 r) noexcept { m_value_ = add_raw(m_value_, r.m_value_); return *this; }
    q32_32& operator-=(q32_32 r) noexcept { m_value_ = subtract_raw(m_value_, r.m_value_); return *this; }
    q32_32& operator*=(q32_32 r) noexcept { m_value_ = mul_128(m_value_, r.m_value_); return *this; }
    q32_32& operator/=(q32_32 r) noexcept { m_value_ = div_128(m_value_, r.m_value_); return *this; }

    q32_32 operator-() const noexcept { return from_raw(negate_raw(m_value_)); }

    bool operator==(q32_32 r) const noexcept { return m_value_ == r.m_value_; }
    bool operator!=(q32_32 r) const noexcept { return m_value_ != r.m_value_; }
    bool operator<(q32_32 r) const noexcept { return m_value_ < r.m_value_; }
    bool operator>(q32_32 r) const noexcept { return m_value_ > r.m_value_; }
    bool operator<=(q32_32 r) const noexcept { return m_value_ <= r.m_value_; }
    bool operator>=(q32_32 r) const noexcept { return m_value_ >= r.m_value_; }

    [[nodiscard]] int32_t to_int() const noexcept { return static_cast<int32_t>(m_value_ >> FRAC_BITS); }
    [[nodiscard]] float to_float() const noexcept { return static_cast<float>(static_cast<double>(m_value_) / static_cast<double>(ONE)); }
    [[nodiscard]] double to_double() const noexcept { return static_cast<double>(m_value_) / static_cast<double>(ONE); }
    [[nodiscard]] int64_t raw() const noexcept { return m_value_; }

    static q32_32 abs(q32_32 v) noexcept { return v.m_value_ < 0 ? from_raw(negate_raw(v.m_value_)) : v; }
    static q32_32 min(q32_32 a, q32_32 b) noexcept { return a < b ? a : b; }
    static q32_32 max(q32_32 a, q32_32 b) noexcept { return a > b ? a : b; }
    static q32_32 lerp(q32_32 a, q32_32 b, q32_32 t) noexcept { return a + (b - a) * t; }
    static q32_32 clamp(q32_32 v, q32_32 lo, q32_32 hi) noexcept { return min(max(v, lo), hi); }

    static q32_32 sqrt(q32_32 v) noexcept
    {
        if (v.m_value_ <= 0) return from_raw(0);
        int64_t x = v.m_value_;
        int64_t guess = x >> 1;
        if (guess == 0) guess = 1;
        for (int i = 0; i < 32; ++i)
        {
            guess = (guess + div_128(x, guess)) >> 1;
        }
        return from_raw(guess);
    }

    static q32_32 from_degrees(double deg) noexcept
    {
        return from_raw(static_cast<int64_t>(deg * ONE * 3.14159265358979323846 / 180.0));
    }

    double to_degrees() const noexcept { return to_double() * 180.0 / 3.14159265358979323846; }

    container::String to_string() const
    {
        char buf[64];
        double val = to_double();
        std::snprintf(buf, sizeof(buf), "%.6f", val);
        return buf;
    }
};

inline q32_32 operator""_q(unsigned long long v) { return q32_32(static_cast<int32_t>(v)); }
inline q32_32 operator""_qf(long double v) { return q32_32(static_cast<float>(v)); }

// ── SIMD batch types ────────────────────────────────────────────────────

// 2× q32_32 packed in Vec2q (2×int64, SSE2)
using q32_32x2 = Vec2q;

inline q32_32x2 q32_32x2_from_raw(Vec2q v) noexcept { return v; }
inline q32_32x2 q32_32x2_load(const int64_t* raw) noexcept { return Vec2q().load(raw); }
inline void q32_32x2_store(int64_t* raw, q32_32x2 v) noexcept { v.store(raw); }

inline q32_32x2 q32_32x2_mul(q32_32x2 a, q32_32x2 b) noexcept
{
    alignas(16) int64_t av[2];
    alignas(16) int64_t bv[2];
    a.store(av);
    b.store(bv);
    return Vec2q(q32_32::mul_128(av[0], bv[0]), q32_32::mul_128(av[1], bv[1]));
}

inline q32_32x2 q32_32x2_add(q32_32x2 a, q32_32x2 b) noexcept { return a + b; }
inline q32_32x2 q32_32x2_sub(q32_32x2 a, q32_32x2 b) noexcept { return a - b; }

#if MAX_VECTOR_SIZE >= 256
// 4× q32_32 packed in Vec4q (4×int64, AVX2)
using q32_32x4 = Vec4q;

inline q32_32x4 q32_32x4_from_raw(Vec4q v) noexcept { return v; }
inline q32_32x4 q32_32x4_load(const int64_t* raw) noexcept { return Vec4q().load(raw); }
inline void q32_32x4_store(int64_t* raw, q32_32x4 v) noexcept { v.store(raw); }

inline q32_32x4 q32_32x4_mul(q32_32x4 a, q32_32x4 b) noexcept
{
    alignas(32) int64_t av[4];
    alignas(32) int64_t bv[4];
    a.store(av);
    b.store(bv);
    return Vec4q(q32_32::mul_128(av[0], bv[0]), q32_32::mul_128(av[1], bv[1]),
                 q32_32::mul_128(av[2], bv[2]), q32_32::mul_128(av[3], bv[3]));
}

inline q32_32x4 q32_32x4_add(q32_32x4 a, q32_32x4 b) noexcept { return a + b; }
inline q32_32x4 q32_32x4_sub(q32_32x4 a, q32_32x4 b) noexcept { return a - b; }
#endif

} // namespace math
