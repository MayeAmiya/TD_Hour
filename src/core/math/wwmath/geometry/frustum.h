#pragma once

#include "../vector/float3.h"
#include "../quaternion/quat.h"
#include <DirectXCollision.h>
#include <cmath>

namespace math {

class aabb;
class obb;
class sphere;
class float4x4;

class frustum
{
public:
    DirectX::BoundingFrustum fr{};

    frustum() noexcept = default;
    explicit frustum(const DirectX::BoundingFrustum& f) noexcept
        : fr(f)
    {
    }

    void set(float fov_y, float aspect, float zn, float zf) noexcept
    {
        const float slope_y = std::tan(fov_y * 0.5f);
        fr.Origin = {0.0f, 0.0f, 0.0f};
        fr.Orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        fr.RightSlope = slope_y * aspect;
        fr.LeftSlope = -fr.RightSlope;
        fr.TopSlope = slope_y;
        fr.BottomSlope = -slope_y;
        fr.Near = zn;
        fr.Far = zf;
    }

    void transform(const class transform& tm) noexcept;
    void transform(const float4x4& m) noexcept;

    vec3 origin() const noexcept { return vec3{fr.Origin}; }
    quat orientation() const noexcept { return quat{fr.Orientation}; }
    float right_slope()   const noexcept { return fr.RightSlope; }
    float left_slope()    const noexcept { return fr.LeftSlope; }
    float top_slope()     const noexcept { return fr.TopSlope; }
    float bottom_slope()  const noexcept { return fr.BottomSlope; }
    float near_plane()    const noexcept { return fr.Near; }
    float far_plane()     const noexcept { return fr.Far; }

    [[nodiscard]] bool contains(vec3 pt) const noexcept
    {
        return fr.Contains(pt.load()) == DirectX::CONTAINS;
    }
    [[nodiscard]] bool contains(const aabb& b) const noexcept;
    [[nodiscard]] bool contains(const sphere& s) const noexcept;
    [[nodiscard]] bool intersects(const aabb& b) const noexcept;
    [[nodiscard]] bool intersects(const obb& b) const noexcept;
    [[nodiscard]] bool intersects(const sphere& s) const noexcept;
};

} // namespace math
