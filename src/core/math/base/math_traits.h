#pragma once

#include <type_traits>
#include <cmath>
#include <cstdint>

namespace math {

// ── Forward declarations for fixed-point types ──────────────────────────
class q16_16;
class q32_32;

// ── Primary template (default: floating point types) ─────────────────────
template<typename T>
struct math_traits
{
    using scalar_type = T;

    static constexpr bool is_fixed_point    = false;
    static constexpr bool is_integral       = std::is_integral_v<T>;
    static constexpr bool is_floating_point = std::is_floating_point_v<T>;

    static constexpr T zero()    { return T(0); }
    static constexpr T one()     { return T(1); }
    static constexpr T two()     { return T(2); }
    static constexpr T half()    { return T(0.5); }
    static constexpr T epsilon() { return T(1e-6); }

    static constexpr T pi()      { return T(3.14159265358979323846); }
    static constexpr T two_pi()  { return T(6.28318530717958647692); }
    static constexpr T half_pi() { return T(1.57079632679489661923); }
    static constexpr T deg_to_rad() { return pi() / T(180); }
    static constexpr T rad_to_deg() { return T(180) / pi(); }

    static T sqrt(T v)   { return std::sqrt(v); }
    static T abs(T v)    { return std::abs(v); }
    static T sin(T v)    { return std::sin(v); }
    static T cos(T v)    { return std::cos(v); }
    static T tan(T v)    { return std::tan(v); }
    static T asin(T v)   { return std::asin(v); }
    static T acos(T v)   { return std::acos(v); }
    static T atan(T v)   { return std::atan(v); }
    static T atan2(T y, T x) { return std::atan2(y, x); }

    static T floor(T v)  { return std::floor(v); }
    static T ceil(T v)   { return std::ceil(v); }
    static T round(T v)  { return std::round(v); }

    static T min(T a, T b) { return a < b ? a : b; }
    static T max(T a, T b) { return a > b ? a : b; }
    static T clamp(T v, T lo, T hi) { return min(max(v, lo), hi); }
    static T lerp(T a, T b, T t)    { return a + (b - a) * t; }
    static void swap(T& a, T& b)    { T t = a; a = b; b = t; }
    static T fmod(T a, T b)         { return std::fmod(a, b); }

    static T sign(T v)    { return (v > T(0)) ? T(1) : (v < T(0)) ? T(-1) : T(0); }
    static T inv_sqrt(T v) { return T(1) / sqrt(v); }
    static int float_to_int_chop(T f)  { return static_cast<int>(f); }
    static int float_to_int_floor(T f) { return static_cast<int>(std::floor(f)); }

    static bool approx_equal(T a, T b, T eps = epsilon()) { return abs(a - b) <= eps; }
    static bool is_zero(T v, T eps = epsilon()) { return abs(v) <= eps; }
};

// ── float specialization ────────────────────────────────────────────────
template<>
struct math_traits<float>
{
    using scalar_type = float;

    static constexpr bool is_fixed_point    = false;
    static constexpr bool is_integral       = false;
    static constexpr bool is_floating_point = true;

    static constexpr float zero()    { return 0.0f; }
    static constexpr float one()     { return 1.0f; }
    static constexpr float two()     { return 2.0f; }
    static constexpr float half()    { return 0.5f; }
    static constexpr float epsilon() { return 1e-6f; }

    static constexpr float pi()      { return 3.14159265f; }
    static constexpr float two_pi()  { return 6.28318530f; }
    static constexpr float half_pi() { return 1.57079632f; }
    static constexpr float deg_to_rad() { return pi() / 180.0f; }
    static constexpr float rad_to_deg() { return 180.0f / pi(); }

    static float sqrt(float v)  { return std::sqrtf(v); }
    static float abs(float v)   { return std::fabsf(v); }
    static float sin(float v)   { return std::sinf(v); }
    static float cos(float v)   { return std::cosf(v); }
    static float tan(float v)   { return std::tanf(v); }
    static float asin(float v)  { return std::asinf(v); }
    static float acos(float v)  { return std::acosf(v); }
    static float atan(float v)  { return std::atanf(v); }
    static float atan2(float y, float x) { return std::atan2f(y, x); }

    static float floor(float v) { return std::floorf(v); }
    static float ceil(float v)  { return std::ceilf(v); }
    static float round(float v) { return std::roundf(v); }

    static float min(float a, float b) { return a < b ? a : b; }
    static float max(float a, float b) { return a > b ? a : b; }
    static float clamp(float v, float lo, float hi) { return min(max(v, lo), hi); }
    static float lerp(float a, float b, float t)    { return a + (b - a) * t; }
    static void swap(float& a, float& b)        { float t = a; a = b; b = t; }
    static float fmod(float a, float b)         { return std::fmodf(a, b); }

    static float sign(float v)  { return (v > 0.0f) ? 1.0f : (v < 0.0f) ? -1.0f : 0.0f; }
    static float inv_sqrt(float v) { return 1.0f / std::sqrtf(v); }

    // These two were hand-rolled bit-twiddling versions with two defects the
    // generic template and the `double` specialization do not have:
    //   * shift-count UB on completely ordinary inputs — for |f| < 1 the exponent
    //     is negative, so `1 << (31 - exponent)` shifts by >= 32 (f = 0.5f gives
    //     `1 << 32`), and `>> (31 - exponent)` is a negative shift for |f| >= 2^31;
    //   * type punning via `*reinterpret_cast<const int*>(&f)`, a strict-aliasing
    //     violation.
    // The standard forms below are what both siblings already use and what these
    // were emulating.
    static int float_to_int_chop(float f) noexcept
    {
        return static_cast<int>(f);
    }

    static int float_to_int_floor(float f) noexcept
    {
        return static_cast<int>(std::floor(f));
    }

    static bool approx_equal(float a, float b, float eps = epsilon()) { return abs(a - b) <= eps; }
    static bool is_zero(float v, float eps = epsilon()) { return abs(v) <= eps; }
};

// ── double specialization ───────────────────────────────────────────────
template<>
struct math_traits<double>
{
    using scalar_type = double;

    static constexpr bool is_fixed_point    = false;
    static constexpr bool is_integral       = false;
    static constexpr bool is_floating_point = true;

    static constexpr double zero()    { return 0.0; }
    static constexpr double one()     { return 1.0; }
    static constexpr double two()     { return 2.0; }
    static constexpr double half()    { return 0.5; }
    static constexpr double epsilon() { return 1e-10; }

    static constexpr double pi()      { return 3.14159265358979323846; }
    static constexpr double two_pi()  { return 6.28318530717958647692; }
    static constexpr double half_pi() { return 1.57079632679489661923; }
    static constexpr double deg_to_rad() { return pi() / 180.0; }
    static constexpr double rad_to_deg() { return 180.0 / pi(); }

    static double sqrt(double v)  { return std::sqrt(v); }
    static double abs(double v)   { return std::abs(v); }
    static double sin(double v)   { return std::sin(v); }
    static double cos(double v)   { return std::cos(v); }
    static double tan(double v)   { return std::tan(v); }
    static double asin(double v)  { return std::asin(v); }
    static double acos(double v)  { return std::acos(v); }
    static double atan(double v)  { return std::atan(v); }
    static double atan2(double y, double x) { return std::atan2(y, x); }

    static double floor(double v) { return std::floor(v); }
    static double ceil(double v)  { return std::ceil(v); }
    static double round(double v) { return std::round(v); }

    static double min(double a, double b) { return a < b ? a : b; }
    static double max(double a, double b) { return a > b ? a : b; }
    static double clamp(double v, double lo, double hi) { return min(max(v, lo), hi); }
    static double lerp(double a, double b, double t)    { return a + (b - a) * t; }
    static void swap(double& a, double& b)          { double t = a; a = b; b = t; }
    static double fmod(double a, double b)           { return std::fmod(a, b); }

    static double sign(double v)    { return (v > 0.0) ? 1.0 : (v < 0.0) ? -1.0 : 0.0; }
    static double inv_sqrt(double v) { return 1.0 / std::sqrt(v); }
    static int float_to_int_chop(double f)  { return static_cast<int>(f); }
    static int float_to_int_floor(double f) { return static_cast<int>(std::floor(f)); }

    static bool approx_equal(double a, double b, double eps = epsilon()) { return abs(a - b) <= eps; }
    static bool is_zero(double v, double eps = epsilon()) { return abs(v) <= eps; }
};

// ── int specialization ──────────────────────────────────────────────────
template<>
struct math_traits<int>
{
    using scalar_type = int;

    static constexpr bool is_fixed_point    = false;
    static constexpr bool is_integral       = true;
    static constexpr bool is_floating_point = false;

    static constexpr int zero()    { return 0; }
    static constexpr int one()     { return 1; }
    static constexpr int two()     { return 2; }
    static constexpr int half()    { return 0; }
    static constexpr int epsilon() { return 0; }

    static constexpr int pi()      { return 3; }
    static constexpr int two_pi()  { return 6; }
    static constexpr int half_pi() { return 1; }
    static constexpr int deg_to_rad() { return 0; }
    static constexpr int rad_to_deg() { return 0; }

    static int sqrt(int v)   { return static_cast<int>(std::sqrt(static_cast<double>(v))); }
    static int abs(int v)    { return v < 0 ? -v : v; }
    static int sin(int)      { return 0; }
    static int cos(int)      { return 1; }
    static int tan(int)      { return 0; }
    static int asin(int)     { return 0; }
    static int acos(int)     { return 0; }
    static int atan(int)     { return 0; }
    static int atan2(int, int) { return 0; }

    static int floor(int v) { return v; }
    static int ceil(int v)  { return v; }
    static int round(int v) { return v; }

    static int min(int a, int b) { return a < b ? a : b; }
    static int max(int a, int b) { return a > b ? a : b; }
    static int clamp(int v, int lo, int hi) { return min(max(v, lo), hi); }
    static int lerp(int a, int b, int t)    { return a + (b - a) * t; }
    static void swap(int& a, int& b)    { int t = a; a = b; b = t; }
    static int fmod(int a, int b)       { return a % b; }

    static int sign(int v) { return (v > 0) ? 1 : (v < 0) ? -1 : 0; }
    static int inv_sqrt(int) { return 0; }
    static int float_to_int_chop(int f)  { return f; }
    static int float_to_int_floor(int f) { return f; }

    static bool approx_equal(int a, int b, int eps = epsilon()) { return abs(a - b) <= eps; }
    static bool is_zero(int v, int eps = epsilon()) { return abs(v) <= eps; }
};

// ── q16_16 specialization ──────────────────────────────────────────────
template<>
struct math_traits<q16_16>
{
    using scalar_type = q16_16;

    static constexpr bool is_fixed_point    = true;
    static constexpr bool is_integral       = false;
    static constexpr bool is_floating_point = false;

    static q16_16 zero()    { return q16_16(int16_t(0)); }
    static q16_16 one()     { return q16_16(int16_t(1)); }
    static q16_16 two()     { return q16_16(int16_t(2)); }
    static q16_16 half()    { return q16_16(0.5f); }
    static q16_16 epsilon() { return q16_16::from_raw(1); }

    static q16_16 pi()      { return q16_16(3.14159265f); }
    static q16_16 two_pi()  { return q16_16(6.28318530f); }
    static q16_16 half_pi() { return q16_16(1.57079632f); }
    static q16_16 deg_to_rad() { return q16_16(0.0174532925f); }
    static q16_16 rad_to_deg() { return q16_16(57.2957795f); }

    static q16_16 sqrt(q16_16 v)   { return q16_16::from_raw(static_cast<int32_t>(std::sqrtf(v.to_float()) * 65536.0f)); }
    static q16_16 abs(q16_16 v)    { return q16_16::abs(v); }
    static q16_16 min(q16_16 a, q16_16 b) { return q16_16::min(a, b); }
    static q16_16 max(q16_16 a, q16_16 b) { return q16_16::max(a, b); }
    static q16_16 clamp(q16_16 v, q16_16 lo, q16_16 hi) { return q16_16::clamp(v, lo, hi); }
    static q16_16 lerp(q16_16 a, q16_16 b, q16_16 t)    { return q16_16::lerp(a, b, t); }
    static void swap(q16_16& a, q16_16& b) { q16_16 t = a; a = b; b = t; }

    static q16_16 sign(q16_16 v) {
        return (v.raw() > 0) ? q16_16(int16_t(1)) : (v.raw() < 0) ? -q16_16(int16_t(1)) : q16_16(int16_t(0));
    }

    static int float_to_int_chop(q16_16 f)  { return f.to_int(); }
    static int float_to_int_floor(q16_16 f) { int i = f.to_int(); return (f.raw() >= 0) ? i : i - 1; }

    static bool approx_equal(q16_16 a, q16_16 b, q16_16 eps = q16_16::from_raw(16)) {
        return q16_16::abs(a - b).raw() <= eps.raw();
    }
    static bool is_zero(q16_16 v, q16_16 eps = q16_16::from_raw(16)) {
        return q16_16::abs(v).raw() <= eps.raw();
    }
};

// ── q32_32 specialization ──────────────────────────────────────────────
template<>
struct math_traits<q32_32>
{
    using scalar_type = q32_32;

    static constexpr bool is_fixed_point    = true;
    static constexpr bool is_integral       = false;
    static constexpr bool is_floating_point = false;

    static q32_32 zero()    { return q32_32(int32_t(0)); }
    static q32_32 one()     { return q32_32(int32_t(1)); }
    static q32_32 two()     { return q32_32(int32_t(2)); }
    static q32_32 half()    { return q32_32(0.5f); }
    static q32_32 epsilon() { return q32_32::from_raw(1); }

    static q32_32 pi()      { return q32_32(3.14159265358979323846); }
    static q32_32 two_pi()  { return q32_32(6.28318530717958647692); }
    static q32_32 half_pi() { return q32_32(1.57079632679489661923); }
    static q32_32 deg_to_rad() { return q32_32(0.01745329251994329576); }
    static q32_32 rad_to_deg() { return q32_32(57.2957795130823208768); }

    static q32_32 sqrt(q32_32 v)   { return q32_32::sqrt(v); }
    static q32_32 abs(q32_32 v)    { return q32_32::abs(v); }
    static q32_32 min(q32_32 a, q32_32 b) { return q32_32::min(a, b); }
    static q32_32 max(q32_32 a, q32_32 b) { return q32_32::max(a, b); }
    static q32_32 clamp(q32_32 v, q32_32 lo, q32_32 hi) { return q32_32::clamp(v, lo, hi); }
    static q32_32 lerp(q32_32 a, q32_32 b, q32_32 t)    { return q32_32::lerp(a, b, t); }
    static void swap(q32_32& a, q32_32& b) { q32_32 t = a; a = b; b = t; }

    static q32_32 sign(q32_32 v) {
        return (v.raw() > 0) ? q32_32(int32_t(1)) : (v.raw() < 0) ? -q32_32(int32_t(1)) : q32_32(int32_t(0));
    }

    static int float_to_int_chop(q32_32 f)  { return f.to_int(); }
    static int float_to_int_floor(q32_32 f) { int i = f.to_int(); return (f.raw() >= 0) ? i : i - 1; }

    static bool approx_equal(q32_32 a, q32_32 b, q32_32 eps = q32_32::from_raw(16)) {
        return q32_32::abs(a - b).raw() <= eps.raw();
    }
    static bool is_zero(q32_32 v, q32_32 eps = q32_32::from_raw(16)) {
        return q32_32::abs(v).raw() <= eps.raw();
    }
};

} // namespace math
