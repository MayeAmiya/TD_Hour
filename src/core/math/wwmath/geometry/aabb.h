#pragma once

#include "../vector/float3.h"
#include <DirectXCollision.h>

namespace math {

class obb;
class sphere;

class aabb
{
public:
    DirectX::BoundingBox box{};

    aabb() noexcept = default;
    aabb(vec3 center, vec3 extents) noexcept
        : box{center.v, extents.v}
    {
    }
    explicit aabb(const DirectX::BoundingBox& b) noexcept
        : box(b)
    {
    }

    vec3 center()  const noexcept { return vec3{box.Center}; }
    vec3 extents() const noexcept { return vec3{box.Extents}; }
    vec3 min()     const noexcept { return center() - extents(); }
    vec3 max()     const noexcept { return center() + extents(); }

    [[nodiscard]] bool intersects(const aabb& other) const noexcept
    {
        return box.Intersects(other.box);
    }
    [[nodiscard]] bool intersects(const obb& other) const noexcept;
    [[nodiscard]] bool intersects(const sphere& s) const noexcept;
    [[nodiscard]] bool intersects(vec3 v0, vec3 v1, vec3 v2) const noexcept
    {
        return box.Intersects(v0.load(), v1.load(), v2.load());
    }
    [[nodiscard]] bool intersects(vec3 origin, vec3 dir, float& dist) const noexcept
    {
        return box.Intersects(origin.load(), dir.load(), dist);
    }

    [[nodiscard]] bool contains(vec3 pt) const noexcept
    {
        return box.Contains(pt.load()) == DirectX::CONTAINS;
    }
    [[nodiscard]] bool contains(const aabb& other) const noexcept
    {
        return box.Contains(other.box) == DirectX::CONTAINS;
    }

    void add_point(vec3 pt) noexcept
    {
        DirectX::XMFLOAT3 mn, mx;
        DirectX::XMStoreFloat3(&mn, DirectX::XMVectorMin(DirectX::XMLoadFloat3(&box.Center) - DirectX::XMLoadFloat3(&box.Extents), pt));
        DirectX::XMStoreFloat3(&mx, DirectX::XMVectorMax(DirectX::XMLoadFloat3(&box.Center) + DirectX::XMLoadFloat3(&box.Extents), pt));
        box.Center  = DirectX::XMFLOAT3((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f);
        box.Extents = DirectX::XMFLOAT3((mx.x - mn.x) * 0.5f, (mx.y - mn.y) * 0.5f, (mx.z - mn.z) * 0.5f);
    }
    void add_box(const aabb& other) noexcept
    {
        DirectX::BoundingBox::CreateMerged(box, box, other.box);
    }
    void transform(const class transform& tm) noexcept;
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

    static aabb merge(const aabb& a, const aabb& b) noexcept
    {
        aabb result;
        DirectX::BoundingBox::CreateMerged(result.box, a.box, b.box);
        return result;
    }
    static aabb from_points(const vec3* pts, int count) noexcept
    {
        aabb result;
        DirectX::BoundingBox::CreateFromPoints(result.box, count, reinterpret_cast<const DirectX::XMFLOAT3*>(pts), sizeof(vec3));
        return result;
    }
};

} // namespace math
