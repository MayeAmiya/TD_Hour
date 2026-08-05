#pragma once

#include "../vector/float3.h"
#include "../quaternion/quat.h"
#include <DirectXCollision.h>

namespace math {

class aabb;
class sphere;

class obb
{
public:
    DirectX::BoundingOrientedBox box{};

    obb() noexcept = default;
    obb(vec3 center, vec3 extents, quat orientation) noexcept
        : box{center.v, extents.v, orientation.q}
    {
    }
    explicit obb(const DirectX::BoundingOrientedBox& b) noexcept
        : box(b)
    {
    }

    vec3 center()       const noexcept { return vec3{box.Center}; }
    vec3 extents()      const noexcept { return vec3{box.Extents}; }
    quat orientation()  const noexcept { return quat{box.Orientation}; }

    [[nodiscard]] bool intersects(const aabb& other) const noexcept;
    [[nodiscard]] bool intersects(const obb& other) const noexcept
    {
        return box.Intersects(other.box);
    }
    [[nodiscard]] bool intersects(const sphere& s) const noexcept;
    [[nodiscard]] bool intersects(vec3 v0, vec3 v1, vec3 v2) const noexcept
    {
        return box.Intersects(v0.load(), v1.load(), v2.load());
    }
    [[nodiscard]] bool intersects(vec3 origin, vec3 dir, float& dist) const noexcept
    {
        return box.Intersects(origin.load(), dir.load(), dist);
    }

    [[nodiscard]] float volume() const noexcept
    {
        return 8.0f * box.Extents.x * box.Extents.y * box.Extents.z;
    }
    [[nodiscard]] float project_to_axis(vec3 axis) const noexcept
    {
        return DirectX::XMVectorGetX(
            DirectX::XMVector3Dot(
                DirectX::XMVectorAbs(axis),
                DirectX::XMLoadFloat3(&box.Extents)));
    }
};

} // namespace math
