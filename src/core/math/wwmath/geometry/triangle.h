#pragma once

#include "../vector/float3.h"
#include "core/math/wwmath/base/wwmath_core.h"

namespace math {

struct triangle
{
    vec3 v[3]{};
    vec3 normal_{};

    triangle() noexcept = default;
    triangle(vec3 v0, vec3 v1, vec3 v2) noexcept
        : v{v0, v1, v2}
    {
        compute_normal();
    }

    void compute_normal() noexcept
    {
        vec3 e1 = v[1] - v[0];
        vec3 e2 = v[2] - v[0];
        normal_ = e1.cross(e2);
        float len = normal_.length();
        if (len > EPSILON)
        {
            normal_ = normal_ / len;
        }
    }

    [[nodiscard]] vec3 normal() const noexcept { return normal_; }
    [[nodiscard]] vec3 center() const noexcept
    {
        return (v[0] + v[1] + v[2]) * (1.0f / 3.0f);
    }

    [[nodiscard]] bool contains_point(vec3 pt) const noexcept
    {
        vec3 e0 = v[1] - v[0];
        vec3 e1 = v[2] - v[0];
        vec3 p = pt - v[0];

        float d00 = e0.dot(e0);
        float d01 = e0.dot(e1);
        float d11 = e1.dot(e1);
        float d20 = p.dot(e0);
        float d21 = p.dot(e1);

        float denom = d00 * d11 - d01 * d01;
        if (denom < EPSILON) { return false; }

        float u = (d11 * d20 - d01 * d21) / denom;
        float v = (d00 * d21 - d01 * d20) / denom;

        return u >= 0.0f && v >= 0.0f && (u + v) <= 1.0f;
    }

    [[nodiscard]] float area() const noexcept
    {
        vec3 e1 = v[1] - v[0];
        vec3 e2 = v[2] - v[0];
        return e1.cross(e2).length() * 0.5f;
    }
};

} // namespace math
