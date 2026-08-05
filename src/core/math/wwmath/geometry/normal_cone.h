#pragma once

#include "../vector/float3.h"
#include "core/math/wwmath/base/wwmath_core.h"

namespace math {

class normal_cone
{
public:
    vec3 direction{0, 0, 1};
    float angle = 1.0f;  // cos(half angle), -1 = full sphere

    normal_cone() noexcept = default;
    normal_cone(vec3 dir, float ang) noexcept
        : direction(dir)
        , angle(ang)
    {
    }

    [[nodiscard]] bool is_complete_sphere() const noexcept
    {
        return angle <= -1.0f;
    }

    void merge(const vec3& normal) noexcept
    {
        float dot = direction.dot(normal);
        if (dot < angle)
        {
            if (dot < -angle)
            {
                // Normal is outside the cone, expand
                float t = angle + dot;
                float s = 1.0f - angle * dot;
                float new_angle = angle;
                if (s > EPSILON)
                {
                    new_angle = dot + (t * angle) / s;
                }
                if (new_angle < -1.0f) { new_angle = -1.0f; }
                angle = new_angle;
            }
            else
            {
                angle = dot;
            }
        }
    }

    void merge(const normal_cone& other) noexcept
    {
        if (other.is_complete_sphere()) { return; }
        if (is_complete_sphere())
        {
            direction = other.direction;
            angle = other.angle;
            return;
        }
        merge(other.direction);
    }

    [[nodiscard]] float smallest_dot(const vec3& dir) const noexcept
    {
        return direction.dot(dir) * angle - std::sqrt(
            (1.0f - direction.dot(dir) * direction.dot(dir)) *
            (1.0f - angle * angle));
    }
};

} // namespace math
