#pragma once

#include "collision_contracts.h"

#include "../vector/float4.h"
#include "../geometry/segment.h"
#include "../geometry/triangle.h"
#include "../geometry/aabb.h"
#include "../geometry/obb.h"
#include "../geometry/sphere.h"
#include "../geometry/plane.h"
#include "../geometry/axis_plane.h"
#include "core/math/wwmath/base/wwmath_core.h"

#include <DirectXMath.h>
#include <cmath>

namespace math {
namespace overlap {

// Basic plane-side classification
inline plane_side plane_point(const vec4& plane_eq, vec3 pt) noexcept
{
    DirectX::XMVECTOR p = DirectX::XMPlaneDotCoord(plane_eq, pt);
    float d = DirectX::XMVectorGetX(p);
    if (d > EPSILON)  return plane_side::front;
    if (d < -EPSILON) return plane_side::back;
    return plane_side::on;
}

inline plane_side plane_point(const plane& pl, vec3 pt) noexcept
{
    return plane_point(pl.to_vec4(), pt);
}

inline plane_side plane_segment(const vec4& plane_eq, const segment& seg) noexcept
{
    plane_side s0 = plane_point(plane_eq, seg.p0);
    plane_side s1 = plane_point(plane_eq, seg.p1);
    if (s0 == s1) { return s0; }
    if (s0 == plane_side::on) { return s1; }
    if (s1 == plane_side::on) { return s0; }
    return plane_side::both;
}

inline plane_side plane_triangle(const vec4& plane_eq, const triangle& tri) noexcept
{
    plane_side s0 = plane_point(plane_eq, tri.v[0]);
    plane_side s1 = plane_point(plane_eq, tri.v[1]);
    plane_side s2 = plane_point(plane_eq, tri.v[2]);
    if (s0 == s1 && s1 == s2) { return s0; }
    // `front | back`, not `front & back`: the AND is 0x01 & 0x02 == 0, and
    // is_set(x, 0) is always false, so the negation was unconditionally true.
    // A triangle straddling the plane with one vertex exactly on it therefore
    // returned front|back|on (0x07 — not any enumerator) instead of
    // plane_side::both, and callers comparing == both missed the straddle.
    if ((s0 == plane_side::on || s1 == plane_side::on || s2 == plane_side::on) &&
        !is_set(s0 | s1 | s2, plane_side::front | plane_side::back))
    {
        // all on or on+one side
        return s0 | s1 | s2;
    }
    return plane_side::both;
}

inline plane_side plane_sphere(const vec4& plane_eq, const sphere& s) noexcept
{
    float d = DirectX::XMVectorGetX(DirectX::XMPlaneDotCoord(plane_eq, s.center()));
    float r = s.radius();
    if (d > r)  return plane_side::front;
    if (d < -r) return plane_side::back;
    return plane_side::both;
}

inline plane_side plane_aabb(const vec4& plane_eq, const aabb& b) noexcept
{
    vec3 c = b.center();
    vec3 e = b.extents();
    float d = DirectX::XMVectorGetX(DirectX::XMPlaneDotCoord(plane_eq, c));
    float proj = e.x() * std::abs(DirectX::XMVectorGetX(DirectX::XMPlaneDotNormal(plane_eq, DirectX::g_XMIdentityR0)))
               + e.y() * std::abs(DirectX::XMVectorGetX(DirectX::XMPlaneDotNormal(plane_eq, DirectX::g_XMIdentityR1)))
               + e.z() * std::abs(DirectX::XMVectorGetX(DirectX::XMPlaneDotNormal(plane_eq, DirectX::g_XMIdentityR2)));
    if (d > proj)  return plane_side::front;
    if (d < -proj) return plane_side::back;
    return plane_side::both;
}

inline plane_side plane_obb(const vec4& plane_eq, const obb& b) noexcept
{
    // Transform plane normal into OBB local space, compute projected extent
    vec3 c = b.center();
    vec3 e = b.extents();

    vec3 n {
        DirectX::XMVectorGetX(DirectX::XMPlaneDotNormal(plane_eq, DirectX::g_XMIdentityR0)),
        DirectX::XMVectorGetX(DirectX::XMPlaneDotNormal(plane_eq, DirectX::g_XMIdentityR1)),
        DirectX::XMVectorGetX(DirectX::XMPlaneDotNormal(plane_eq, DirectX::g_XMIdentityR2))
    };

    // Rotate normal into OBB local space via inverse orientation
    DirectX::XMVECTOR local_n = DirectX::XMVector3Rotate(n, DirectX::XMQuaternionInverse(b.orientation()));
    DirectX::XMFLOAT3 ln;
    DirectX::XMStoreFloat3(&ln, local_n);

    float proj = e.x() * std::abs(ln.x) + e.y() * std::abs(ln.y) + e.z() * std::abs(ln.z);
    float d = DirectX::XMVectorGetX(DirectX::XMPlaneDotCoord(plane_eq, c));

    if (d > proj)  return plane_side::front;
    if (d < -proj) return plane_side::back;
    return plane_side::both;
}

// Axis-aligned plane side tests
inline plane_side axis_plane_point(const axis_plane& ap, vec3 pt) noexcept
{
    const float* arr = static_cast<const float*>(pt);
    float d = arr[ap.normal] - ap.dist;
    if (d > EPSILON)  return plane_side::front;
    if (d < -EPSILON) return plane_side::back;
    return plane_side::on;
}

inline plane_side axis_plane_segment(const axis_plane& ap, const segment& seg) noexcept
{
    plane_side s0 = axis_plane_point(ap, seg.p0);
    plane_side s1 = axis_plane_point(ap, seg.p1);
    if (s0 == s1) { return s0; }
    if (s0 == plane_side::on) { return s1; }
    if (s1 == plane_side::on) { return s0; }
    return plane_side::both;
}

inline plane_side axis_plane_triangle(const axis_plane& ap, const triangle& tri) noexcept
{
    plane_side s0 = axis_plane_point(ap, tri.v[0]);
    plane_side s1 = axis_plane_point(ap, tri.v[1]);
    plane_side s2 = axis_plane_point(ap, tri.v[2]);
    if (s0 == s1 && s1 == s2) { return s0; }
    return plane_side::both;
}

inline plane_side axis_plane_sphere(const axis_plane& ap, const sphere& s) noexcept
{
    const float* arr = static_cast<const float*>(s.center());
    float d = arr[ap.normal] - ap.dist;
    float r = s.radius();
    if (d > r)  return plane_side::front;
    if (d < -r) return plane_side::back;
    return plane_side::both;
}

inline plane_side axis_plane_aabb(const axis_plane& ap, const aabb& b) noexcept
{
    const float* c = static_cast<const float*>(b.center());
    const float* e = static_cast<const float*>(b.extents());
    float d = c[ap.normal] - ap.dist;
    if (d > e[ap.normal])  return plane_side::front;
    if (d < -e[ap.normal]) return plane_side::back;
    return plane_side::both;
}

inline plane_side axis_plane_obb(const axis_plane& ap, const obb& b) noexcept
{
    const float* c = static_cast<const float*>(b.center());
    float d = c[ap.normal] - ap.dist;

    // Project OBB extent onto axis
    DirectX::XMVECTOR axis_v = DirectX::g_XMZero;
    axis_v = DirectX::XMVectorSetByIndex(axis_v, 1.0f, ap.normal);
    axis_v = DirectX::XMVector3Rotate(axis_v, DirectX::XMQuaternionInverse(b.orientation()));
    DirectX::XMFLOAT3 local_axis;
    DirectX::XMStoreFloat3(&local_axis, axis_v);

    const float* e = static_cast<const float*>(b.extents());
    float proj = e[0] * std::abs(local_axis.x)
               + e[1] * std::abs(local_axis.y)
               + e[2] * std::abs(local_axis.z);

    if (d > proj)  return plane_side::front;
    if (d < -proj) return plane_side::back;
    return plane_side::both;
}

} // namespace overlap
} // namespace math
