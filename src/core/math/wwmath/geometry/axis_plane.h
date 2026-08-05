#pragma once

#include "../vector/float3.h"

namespace math {

struct axis_plane
{
    enum axis : int
    {
        X = 0,
        Y = 1,
        Z = 2,
    };

    axis normal = X;
    float dist = 0.0f;

    axis_plane() noexcept = default;
    axis_plane(axis a, float d) noexcept
        : normal(a)
        , dist(d)
    {
    }

    void get_normal(vec3& out) const noexcept
    {
        out = vec3::zero();
        float* arr = const_cast<float*>(static_cast<const float*>(out));
        arr[normal] = 1.0f;
    }

    [[nodiscard]] vec3 get_normal() const noexcept
    {
        vec3 result;
        get_normal(result);
        return result;
    }

    [[nodiscard]] float distance(vec3 pt) const noexcept
    {
        const float* arr = static_cast<const float*>(pt);
        return arr[normal] - dist;
    }
};

} // namespace math
