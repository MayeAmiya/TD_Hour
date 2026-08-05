#pragma once

#include "../vector/float3.h"
#include "../vector/float4.h"
#include <DirectXMath.h>

namespace math {

class sphere;
class aabb;

class plane
{
public:
    vec3 n{};
    float d{};  // N·P = D

    plane() noexcept = default;
    plane(vec3 normal, float dist) noexcept
        : n(normal)
        , d(dist)
    {
    }
    plane(vec3 p0, vec3 p1, vec3 p2) noexcept
    {
        vec3 e1 = p1 - p0;
        vec3 e2 = p2 - p0;
        // Guard the degenerate (collinear / duplicated vertex) case: an unguarded
        // normalize yields a QNaN normal and a NaN d, silently poisoning every
        // downstream distance()/in_front() test.  triangle::compute_normal and
        // plane::normalize() both guard the identical computation; only this
        // constructor did not.
        const vec3 cross = e1.cross(e2);
        const float len = cross.length();
        if (len > EPSILON)
        {
            n = cross / len;
            d = n.dot(p0);
        }
        else
        {
            n = vec3{0.0f, 0.0f, 0.0f};
            d = 0.0f;
        }
    }
    explicit plane(const vec4& v) noexcept
        : n(v.x(), v.y(), v.z())
        , d(v.w())
    {
    }

    vec4 to_vec4() const noexcept
    {
        return vec4{n.x(), n.y(), n.z(), d};
    }

    DirectX::XMVECTOR to_dx() const noexcept
    {
        return DirectX::XMVectorSet(n.x(), n.y(), n.z(), d);
    }

    [[nodiscard]] float distance(vec3 pt) const noexcept
    {
        return n.dot(pt) - d;
    }
    [[nodiscard]] bool in_front(vec3 pt) const noexcept
    {
        return distance(pt) > 0.0f;
    }
    [[nodiscard]] bool in_front(const sphere& s) const noexcept;
    [[nodiscard]] bool in_front(const aabb& b) const noexcept;

    void normalize() noexcept
    {
        float len = n.length();
        if (len > 0.0f)
        {
            n = n / len;
            d /= len;
        }
    }
};

} // namespace math
