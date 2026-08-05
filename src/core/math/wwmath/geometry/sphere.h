#pragma once

#include "../vector/float3.h"
#include <DirectXCollision.h>

namespace math {

class aabb;
class obb;

class sphere
{
public:
    DirectX::BoundingSphere sph{};

    sphere() noexcept = default;
    sphere(vec3 center, float radius) noexcept
        : sph{center.v, radius}
    {
    }
    explicit sphere(const DirectX::BoundingSphere& s) noexcept
        : sph(s)
    {
    }

    vec3  center() const noexcept { return vec3{sph.Center}; }
    float radius() const noexcept { return sph.Radius; }

    [[nodiscard]] bool intersects(const aabb& other) const noexcept;
    [[nodiscard]] bool intersects(const obb& other) const noexcept;
    [[nodiscard]] bool intersects(const sphere& other) const noexcept
    {
        return sph.Intersects(other.sph);
    }
    [[nodiscard]] bool intersects(vec3 v0, vec3 v1, vec3 v2) const noexcept
    {
        return sph.Intersects(v0.load(), v1.load(), v2.load());
    }
    [[nodiscard]] bool intersects(vec3 origin, vec3 dir, float& dist) const noexcept
    {
        return sph.Intersects(origin.load(), dir.load(), dist);
    }

    [[nodiscard]] bool contains(vec3 pt) const noexcept
    {
        return sph.Contains(pt.load()) == DirectX::CONTAINS;
    }

    static sphere merge(const sphere& a, const sphere& b) noexcept
    {
        sphere result;
        DirectX::BoundingSphere::CreateMerged(result.sph, a.sph, b.sph);
        return result;
    }
    static sphere from_points(const vec3* pts, int count) noexcept
    {
        sphere result;
        DirectX::BoundingSphere::CreateFromPoints(result.sph, count, reinterpret_cast<const DirectX::XMFLOAT3*>(pts), sizeof(vec3));
        return result;
    }
    static sphere from_aabb(const aabb& b) noexcept;
};

} // namespace math
