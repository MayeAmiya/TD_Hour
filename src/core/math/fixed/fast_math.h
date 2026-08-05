#pragma once

#include <cmath>
#include <cfloat>
#include <algorithm>
#include <limits>
#include "vectorclass.h"

namespace math {

inline constexpr int ARC_TABLE_SIZE = 1024;
inline constexpr int SIN_TABLE_SIZE = 1024;

extern float g_fast_acos_table[ARC_TABLE_SIZE];
extern float g_fast_asin_table[ARC_TABLE_SIZE];
extern float g_fast_sin_table[SIN_TABLE_SIZE];
extern float g_fast_inv_sin_table[SIN_TABLE_SIZE];

struct fast_math
{
    static void init() noexcept
    {
        for (int a = 0; a < ARC_TABLE_SIZE; ++a)
        {
            float cv = static_cast<float>(a - ARC_TABLE_SIZE / 2) * (1.0f / static_cast<float>(ARC_TABLE_SIZE / 2));
            g_fast_acos_table[a] = std::acosf(cv);
            g_fast_asin_table[a] = std::asinf(cv);
        }
        for (int a = 0; a < SIN_TABLE_SIZE; ++a)
        {
            float cv = static_cast<float>(a) * 2.0f * 3.14159265f / static_cast<float>(SIN_TABLE_SIZE);
            g_fast_sin_table[a] = std::sinf(cv);
            g_fast_inv_sin_table[a] = (a > 0) ? (1.0f / g_fast_sin_table[a]) : FLT_MAX;
        }
    }

    static void shutdown() noexcept {}

    // ── Scalar trig ────────────────────────────────────────────────────
    static float sin(float val) noexcept
    {
        if (!std::isfinite(val))
            return std::numeric_limits<float>::quiet_NaN();
        constexpr float kSafeLookupAngle =
            (static_cast<float>(std::numeric_limits<int>::max()) - 2.0f) *
            (2.0f * 3.14159265f) / static_cast<float>(SIN_TABLE_SIZE);
        if (std::fabs(val) > kSafeLookupAngle)
            val = std::remainder(val, 2.0f * 3.14159265f);
        val *= static_cast<float>(SIN_TABLE_SIZE) / (2.0f * 3.14159265f);

        int idx0 = static_cast<int>(val > 0.0f ? val : val - 1.0f);
        int idx1 = idx0 + 1;
        float frac = val - static_cast<float>(idx0);

        idx0 = static_cast<unsigned>(idx0) & (SIN_TABLE_SIZE - 1);
        idx1 = static_cast<unsigned>(idx1) & (SIN_TABLE_SIZE - 1);

        return (1.0f - frac) * g_fast_sin_table[idx0] + frac * g_fast_sin_table[idx1];
    }

    static float cos(float val) noexcept
    {
        if (!std::isfinite(val))
            return std::numeric_limits<float>::quiet_NaN();
        val += 3.14159265f * 0.5f;
        constexpr float kSafeLookupAngle =
            (static_cast<float>(std::numeric_limits<int>::max()) - 2.0f) *
            (2.0f * 3.14159265f) / static_cast<float>(SIN_TABLE_SIZE);
        if (std::fabs(val) > kSafeLookupAngle)
            val = std::remainder(val, 2.0f * 3.14159265f);
        val *= static_cast<float>(SIN_TABLE_SIZE) / (2.0f * 3.14159265f);

        int idx0 = static_cast<int>(val > 0.0f ? val : val - 1.0f);
        int idx1 = idx0 + 1;
        float frac = val - static_cast<float>(idx0);

        idx0 = static_cast<unsigned>(idx0) & (SIN_TABLE_SIZE - 1);
        idx1 = static_cast<unsigned>(idx1) & (SIN_TABLE_SIZE - 1);

        return (1.0f - frac) * g_fast_sin_table[idx0] + frac * g_fast_sin_table[idx1];
    }

    static float acos(float val) noexcept
    {
        if (!std::isfinite(val))
            return std::numeric_limits<float>::quiet_NaN();
        val = std::clamp(val, -1.0f, 1.0f);
        if (std::fabsf(val) > 0.975f) { return std::acosf(val); }
        val *= static_cast<float>(ARC_TABLE_SIZE / 2);
        int idx0 = static_cast<int>(val > 0.0f ? val : val - 1.0f);
        int idx1 = idx0 + 1;
        float frac = val - static_cast<float>(idx0);
        idx0 += ARC_TABLE_SIZE / 2;
        idx1 += ARC_TABLE_SIZE / 2;
        return (1.0f - frac) * g_fast_acos_table[idx0] + frac * g_fast_acos_table[idx1];
    }

    static float asin(float val) noexcept
    {
        if (!std::isfinite(val))
            return std::numeric_limits<float>::quiet_NaN();
        val = std::clamp(val, -1.0f, 1.0f);
        if (std::fabsf(val) > 0.975f) { return std::asinf(val); }
        val *= static_cast<float>(ARC_TABLE_SIZE / 2);
        int idx0 = static_cast<int>(val > 0.0f ? val : val - 1.0f);
        int idx1 = idx0 + 1;
        float frac = val - static_cast<float>(idx0);
        idx0 += ARC_TABLE_SIZE / 2;
        idx1 += ARC_TABLE_SIZE / 2;
        return (1.0f - frac) * g_fast_asin_table[idx0] + frac * g_fast_asin_table[idx1];
    }

    // ── SIMD batch trig (4-wide, Vec4f) ────────────────────────────────
    static Vec4f sin(Vec4f val) noexcept
    {
        // Keep the batch API on the exact scalar safety contract. There are
        // currently no production batch callers; a future vectorized version
        // must retain finite checks and endpoint-safe table addressing.
        alignas(16) float values[4];
        val.store(values);
        for (float& value : values) value = sin(value);
        return Vec4f().load(values);
    }

    static Vec4f cos(Vec4f val) noexcept
    {
        return sin(val + 3.14159265f * 0.5f);
    }

    static Vec4f acos(Vec4f val) noexcept
    {
        alignas(16) float values[4];
        val.store(values);
        for (float& value : values) value = acos(value);
        return Vec4f().load(values);
    }

    static Vec4f asin(Vec4f val) noexcept
    {
        alignas(16) float values[4];
        val.store(values);
        for (float& value : values) value = asin(value);
        return Vec4f().load(values);
    }
};

} // namespace math
