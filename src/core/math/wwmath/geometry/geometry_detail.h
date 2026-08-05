#pragma once

#include "aabb.h"
#include "obb.h"
#include "sphere.h"
#include "frustum.h"
#include "../matrix/float4x4.h"
#include "../matrix/transform.h"

namespace math {

// ── aabb cross-type ────────────────────────────────────────────────────
inline bool aabb::intersects(const obb& other) const noexcept
{
    return box.Intersects(other.box);
}
inline bool aabb::intersects(const sphere& s) const noexcept
{
    return box.Intersects(s.sph);
}
inline void aabb::transform(const math::transform& tm) noexcept
{
    box.Transform(box, tm.load());
}

// ── obb cross-type ─────────────────────────────────────────────────────
inline bool obb::intersects(const aabb& other) const noexcept
{
    return box.Intersects(other.box);
}
inline bool obb::intersects(const sphere& s) const noexcept
{
    return box.Intersects(s.sph);
}

// ── sphere cross-type ──────────────────────────────────────────────────
inline bool sphere::intersects(const aabb& other) const noexcept
{
    return sph.Intersects(other.box);
}
inline bool sphere::intersects(const obb& other) const noexcept
{
    return sph.Intersects(other.box);
}
inline sphere sphere::from_aabb(const aabb& b) noexcept
{
    sphere result;
    DirectX::BoundingSphere::CreateFromBoundingBox(result.sph, b.box);
    return result;
}

// ── plane cross-type ───────────────────────────────────────────────────
inline bool plane::in_front(const sphere& s) const noexcept
{
    return distance(s.center()) > s.radius();
}
inline bool plane::in_front(const aabb& b) const noexcept
{
    vec3 extent = b.extents();
    float proj = extent.x() * std::abs(n.x())
               + extent.y() * std::abs(n.y())
               + extent.z() * std::abs(n.z());
    return distance(b.center()) > proj;
}

// ── frustum cross-type ─────────────────────────────────────────────────
inline bool frustum::contains(const aabb& b) const noexcept
{
    return fr.Contains(b.box);
}
inline bool frustum::contains(const sphere& s) const noexcept
{
    return fr.Contains(s.sph);
}
inline bool frustum::intersects(const aabb& b) const noexcept
{
    return fr.Intersects(b.box);
}
inline bool frustum::intersects(const obb& b) const noexcept
{
    return fr.Intersects(b.box);
}
inline bool frustum::intersects(const sphere& s) const noexcept
{
    return fr.Intersects(s.sph);
}
inline void frustum::transform(const math::transform& tm) noexcept
{
    fr.Transform(fr, tm.load());
}
inline void frustum::transform(const float4x4& m) noexcept
{
    fr.Transform(fr, m.load());
}

} // namespace math
