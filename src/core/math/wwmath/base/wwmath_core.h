#pragma once

#include <cstdint>
#include <cmath>
#include <limits>
#include <numbers>
#include <random>
#include <type_traits>
#include "fixed/fast_math.h"

namespace math {

// ── Constants ──────────────────────────────────────────────────────────
inline constexpr float PI       = std::numbers::pi_v<float>;
inline constexpr float TWO_PI   = 2.0f * std::numbers::pi_v<float>;
inline constexpr float HALF_PI  = std::numbers::pi_v<float> / 2.0f;
inline constexpr float E        = std::numbers::e_v<float>;
inline constexpr float EPSILON  = 0.0001f;
inline constexpr float SQRT2    = std::numbers::sqrt2_v<float>;
inline constexpr float SQRT3    = 1.73205080756887729352f;
inline constexpr float INV_SQRT2 = 1.0f / SQRT2;
inline constexpr float INV_SQRT3 = 1.0f / SQRT3;
inline constexpr float DEG_TO_RAD = PI / 180.0f;
inline constexpr float RAD_TO_DEG = 180.0f / PI;

// ── Angle conversion ───────────────────────────────────────────────────
[[nodiscard]] inline constexpr float deg_to_rad(float d) noexcept { return d * DEG_TO_RAD; }
[[nodiscard]] inline constexpr float rad_to_deg(float r) noexcept { return r * RAD_TO_DEG; }

// ── Clamp / Lerp / Wrap / Sign ─────────────────────────────────────────
template <typename T>
[[nodiscard]] inline constexpr T clamp(T v, T mn, T mx) noexcept
{
    return v < mn ? mn : (v > mx ? mx : v);
}

template <typename T>
[[nodiscard]] inline constexpr T lerp(T a, T b, float t) noexcept
{
    return static_cast<T>(a + (b - a) * t);
}

[[nodiscard]] inline constexpr float inverse_lerp(float a, float b, float v) noexcept
{
    return a == b ? 0.0f : (v - a) / (b - a);
}

[[nodiscard]] inline constexpr double inverse_lerp(double a, double b, double v) noexcept
{
    return a == b ? 0.0 : (v - a) / (b - a);
}

template <typename T>
[[nodiscard]] inline constexpr T wrap(T v, T mn, T mx) noexcept
{
    T range = mx - mn;
    if (range <= T{}) { return mn; }
    if constexpr (std::is_floating_point_v<T>) {
        // Constant-time reduction.  The add/subtract-in-a-loop form used here
        // previously shares the hazard documented on normalize_angle: once
        // ulp(v) exceeds `range` the loop makes no progress and never exits.
        if (!std::isfinite(v)) { return mn; }
        T offset = std::fmod(v - mn, range);
        if (offset < T{}) { offset += range; }
        T result = mn + offset;
        // fmod is exact, but mn + offset can round up to mx at the boundary.
        if (result >= mx) { result = mn; }
        return result;
    } else {
        // Integer arithmetic always makes progress, so the loop terminates.
        T result = v;
        while (result < mn) result += range;
        while (result >= mx) result -= range;
        return result;
    }
}

template <typename T>
[[nodiscard]] inline constexpr int sign(T v) noexcept
{
    return (T{} < v) - (v < T{});
}

// ── Float conversions ──────────────────────────────────────────────────
[[nodiscard]] inline int float_to_int_chop(float f) noexcept
{
    return static_cast<int>(f);
}

[[nodiscard]] inline int float_to_int_floor(float f) noexcept
{
    return static_cast<int>(f > 0.0f ? f : f - 0.999999f);
}

[[nodiscard]] inline bool is_valid_float(float x) noexcept
{
    return std::isfinite(x);
}

[[nodiscard]] inline bool is_valid_double(double x) noexcept { return std::isfinite(x); }

[[nodiscard]] inline float abs(float v) noexcept { return std::abs(v); }
[[nodiscard]] inline float sqrt(float v) noexcept { return std::sqrt(v); }
[[nodiscard]] inline float inv_sqrt(float v) noexcept { return 1.0f / std::sqrt(v); }
[[nodiscard]] inline float atan(float v) noexcept { return std::atan(v); }
[[nodiscard]] inline float atan2(float y, float x) noexcept { return std::atan2(y, x); }
[[nodiscard]] inline float ceil(float v) noexcept { return std::ceil(v); }
[[nodiscard]] inline float floor(float v) noexcept { return std::floor(v); }
[[nodiscard]] inline float round(float v) noexcept { return std::round(v); }
[[nodiscard]] inline constexpr float min(float a, float b) noexcept { return a < b ? a : b; }
[[nodiscard]] inline constexpr float max(float a, float b) noexcept { return a > b ? a : b; }

// ── Power of 2 ─────────────────────────────────────────────────────────
[[nodiscard]] inline constexpr bool is_power_of_2(uint32_t v) noexcept
{
    return v && (v & (v - 1)) == 0;
}

[[nodiscard]] inline uint32_t find_pot(uint32_t v) noexcept
{
    if (v == 0) { return 0; }
    uint32_t r = 1;
    while (r < v) { r <<= 1; }
    return r;
}

[[nodiscard]] inline constexpr uint32_t find_pot_log2(uint32_t v) noexcept
{
    if (v == 0) { return 0; }
    uint32_t result = 0;
    --v;
    while (v != 0) { v >>= 1; ++result; }
    return result;
}

// ── Angle normalization ────────────────────────────────────────────────
// Reduces to [-PI, PI) in constant time.  The subtract-in-a-loop form this
// replaced never terminated for |a| >= 2^27: there ulp(a) exceeds TWO_PI, so
// `a -= TWO_PI` rounds straight back to `a` and the loop spins forever.  That
// was reachable from unvalidated content — a map object angle flows here from
// MapObjectImport, and a vehicle rotation flows here every presentation frame
// — and an `isfinite` check upstream does not help, because a hostile value
// only has to be large, not infinite.
[[nodiscard]] inline float normalize_angle(float a) noexcept
{
    if (!std::isfinite(a)) { return 0.0f; }
    // std::remainder rounds the quotient to nearest, yielding [-PI, PI].
    float r = std::remainder(a, TWO_PI);
    if (r >= PI)  { r -= TWO_PI; }
    if (r < -PI)  { r += TWO_PI; }
    return r;
}

// ── Unit float <-> byte conversion ─────────────────────────────────────
[[nodiscard]] inline uint8_t unit_float_to_byte(float f) noexcept
{
    int v = static_cast<int>(f * 255.0f + 0.5f);
    if (v < 0)   { v = 0; }
    if (v > 255) { v = 255; }
    return static_cast<uint8_t>(v);
}

[[nodiscard]] inline float byte_to_unit_float(uint8_t b) noexcept
{
    return b * (1.0f / 255.0f);
}

[[nodiscard]] inline float random_float(float min_value = 0.0f, float max_value = 1.0f)
{
    thread_local std::mt19937 generator{std::random_device{}()};
    return std::uniform_real_distribution<float>{min_value, max_value}(generator);
}

// ── Fast trig (delegates to fast_math) ─────────────────────────────────
class fast_trig
{
public:
    static void init() noexcept { fast_math::init(); }
    static void shutdown() noexcept { fast_math::shutdown(); }
    static float sin(float a) noexcept { return fast_math::sin(a); }
    static float cos(float a) noexcept { return fast_math::cos(a); }
    static float acos(float v) noexcept { return fast_math::acos(v); }
    static float asin(float v) noexcept { return fast_math::asin(v); }
};

} // namespace math
