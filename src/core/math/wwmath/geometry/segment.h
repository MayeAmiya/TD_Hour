#pragma once

#include "../vector/float3.h"
#include "core/math/wwmath/base/wwmath_core.h"

namespace math {

struct segment
{
    vec3 p0{};
    vec3 p1{};

    segment() noexcept = default;
    segment(vec3 a, vec3 b) noexcept
        : p0(a)
        , p1(b)
    {
    }

    [[nodiscard]] float length() const noexcept
    {
        return p0.distance(p1);
    }

    [[nodiscard]] vec3 closest_point(vec3 pt) const noexcept
    {
        vec3 dir = p1 - p0;
        float len_sq = dir.length_sq();
        if (len_sq < EPSILON)
        {
            return p0;
        }
        float t = clamp((pt - p0).dot(dir) / len_sq, 0.0f, 1.0f);
        return p0 + dir * t;
    }

    [[nodiscard]] vec3 point_at(float t) const noexcept
    {
        return lerp(p0, p1, t);
    }

    bool intersect(const segment& other, vec3& pt, float& f1, float& f2) const noexcept
    {
        vec3 d1 = p1 - p0;
        vec3 d2 = other.p1 - other.p0;
        vec3 r = p0 - other.p0;

        float a = d1.dot(d1);
        float b = d1.dot(d2);
        float c = d2.dot(d2);
        float d = d1.dot(r);
        float e = d2.dot(r);

        float det = a * c - b * b;
        if (det < EPSILON)
        {
            return false;
        }

        f1 = (b * e - c * d) / det;
        f2 = (a * e - b * d) / det;

        if (f1 < 0.0f || f1 > 1.0f || f2 < 0.0f || f2 > 1.0f)
        {
            return false;
        }

        pt = p0 + d1 * f1;
        return true;
    }
};

} // namespace math
