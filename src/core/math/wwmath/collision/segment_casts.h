#pragma once

#include "collision_contracts.h"

#include "../vector/float4.h"
#include "../geometry/segment.h"
#include "../geometry/triangle.h"
#include "../geometry/aabb.h"
#include "../geometry/obb.h"
#include "../geometry/sphere.h"
#include "core/math/wwmath/base/wwmath_core.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

namespace math {
namespace cast {

// Helper: clamp fraction to [0,1]
inline void clamp_fraction(hit_result& r) noexcept
{
    if (r.fraction > 1.0f) { r.fraction = 1.0f; }
    if (r.fraction < 0.0f) { r.fraction = 0.0f; }
}

// ── Segment vs primitive ───────────────────────────────────────────────

inline bool segment_plane(const segment& seg, const vec4& plane_eq, hit_result& r) noexcept
{
    vec3 dir = seg.p1 - seg.p0;
    float nd = DirectX::XMVectorGetX(DirectX::XMPlaneDotNormal(plane_eq, dir));
    if (std::abs(nd) < EPSILON) { return false; }

    float t = -DirectX::XMVectorGetX(DirectX::XMPlaneDotCoord(plane_eq, seg.p0)) / nd;
    if (t < 0.0f || t > 1.0f) { return false; }

    r.fraction = t;
    r.normal = vec3{DirectX::XMPlaneDotNormal(plane_eq, DirectX::g_XMOne)};
    return true;
}

inline bool segment_triangle(const segment& seg, const triangle& tri, hit_result& r) noexcept
{
    vec3 dir = seg.p1 - seg.p0;
    float len = dir.length();
    if (len < EPSILON) { return false; }

    vec3 d = dir / len;
    vec3 e1 = tri.v[1] - tri.v[0];
    vec3 e2 = tri.v[2] - tri.v[0];
    vec3 p = d.cross(e2);
    float det = e1.dot(p);

    vec3 s;
    if (det > 0.0f)
    {
        s = seg.p0 - tri.v[0];
    }
    else
    {
        s = tri.v[0] - seg.p0;
        det = -det;
    }

    if (det < EPSILON) { return false; }

    float u = s.dot(p);
    if (u < 0.0f || u > det) { return false; }

    vec3 q = s.cross(e1);
    float v = d.dot(q);
    if (v < 0.0f || u + v > det) { return false; }

    float t = e2.dot(q) / det;
    if (t < 0.0f || t > 1.0f) { return false; }

    r.fraction = t;
    r.normal = tri.normal();
    return true;
}

inline bool segment_sphere(const segment& seg, const sphere& s, hit_result& r) noexcept
{
    vec3 dir = seg.p1 - seg.p0;
    vec3 oc = seg.p0 - s.center();

    float a = dir.dot(dir);
    // Degenerate (zero-length) segment: every sibling in this file guards this
    // (segment_triangle checks len < EPSILON, segment_aabb/obb test each axis).
    // Without it a == b == disc == 0, the disc < 0 early-out does not fire, t
    // becomes -0/0 = NaN, and because every NaN comparison is false the range
    // tests below both pass — the function returned true with a NaN fraction and
    // a NaN normal, poisoning any closest-hit comparison that consumed it.
    if (a < EPSILON) { return false; }

    float b = 2.0f * oc.dot(dir);
    float c = oc.dot(oc) - s.radius() * s.radius();

    float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f) { return false; }

    float sqrt_disc = std::sqrt(disc);
    float t = (-b - sqrt_disc) / (2.0f * a);
    if (t < 0.0f || t > 1.0f)
    {
        t = (-b + sqrt_disc) / (2.0f * a);
        if (t < 0.0f || t > 1.0f) { return false; }
    }

    r.fraction = t;
    vec3 hit_pt = seg.point_at(t);
    r.normal = (hit_pt - s.center()).normalized();
    return true;
}

inline bool segment_aabb(const segment& seg, const aabb& b, hit_result& r) noexcept
{
    vec3 dir = seg.p1 - seg.p0;
    vec3 min_pt = b.min();
    vec3 max_pt = b.max();

    float tmin = 0.0f;
    float tmax = 1.0f;

    for (int i = 0; i < 3; ++i)
    {
        const float* d = static_cast<const float*>(dir);
        const float* p0 = static_cast<const float*>(seg.p0);
        const float* mn = static_cast<const float*>(min_pt);
        const float* mx = static_cast<const float*>(max_pt);

        if (std::abs(d[i]) < EPSILON)
        {
            if (p0[i] < mn[i] || p0[i] > mx[i]) { return false; }
        }
        else
        {
            float inv_d = 1.0f / d[i];
            float t1 = (mn[i] - p0[i]) * inv_d;
            float t2 = (mx[i] - p0[i]) * inv_d;
            if (t1 > t2) { std::swap(t1, t2); }
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) { return false; }
        }
    }

    if (tmin < 0.0f || tmin > 1.0f) { return false; }

    r.fraction = tmin;
    // Compute normal from the face that was hit
    vec3 hit_pt = seg.point_at(tmin);
    vec3 center = b.center();
    vec3 diff = hit_pt - center;
    vec3 abs_diff {std::abs(diff.x()), std::abs(diff.y()), std::abs(diff.z())};
    float max_ax = std::max({abs_diff.x(), abs_diff.y(), abs_diff.z()});
    if (abs_diff.x() >= max_ax - EPSILON) { r.normal = {diff.x() > 0 ? 1.0f : -1.0f, 0.0f, 0.0f}; }
    else if (abs_diff.y() >= max_ax - EPSILON) { r.normal = {0.0f, diff.y() > 0 ? 1.0f : -1.0f, 0.0f}; }
    else { r.normal = {0.0f, 0.0f, diff.z() > 0 ? 1.0f : -1.0f}; }
    return true;
}

inline bool segment_obb(const segment& seg, const obb& b, hit_result& r) noexcept
{
    // Transform segment into OBB local space
    vec3 dir = seg.p1 - seg.p0;
    vec3 p = seg.p0 - b.center();
    quat inv_orient = b.orientation().inverse();
    vec3 local_p = inv_orient.rotate_vec(p);
    vec3 local_dir = inv_orient.rotate_vec(dir);
    vec3 e = b.extents();

    float tmin = 0.0f;
    float tmax = 1.0f;

    for (int i = 0; i < 3; ++i)
    {
        const float* pd = static_cast<const float*>(local_dir);
        const float* pp = static_cast<const float*>(local_p);
        const float* pe = static_cast<const float*>(e);

        if (std::abs(pd[i]) < EPSILON)
        {
            if (pp[i] < -pe[i] || pp[i] > pe[i]) { return false; }
        }
        else
        {
            float inv_d = 1.0f / pd[i];
            float t1 = (-pe[i] - pp[i]) * inv_d;
            float t2 = (pe[i] - pp[i]) * inv_d;
            if (t1 > t2) { std::swap(t1, t2); }
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) { return false; }
        }
    }

    if (tmin < 0.0f || tmin > 1.0f) { return false; }

    r.fraction = tmin;
    // Normal in local space, rotate back to world
    vec3 local_hit = local_p + local_dir * tmin;
    vec3 abs_hit {std::abs(local_hit.x()), std::abs(local_hit.y()), std::abs(local_hit.z())};
    vec3 local_n;
    float max_ax = std::max({abs_hit.x(), abs_hit.y(), abs_hit.z()});
    if (abs_hit.x() >= max_ax - EPSILON) { local_n = {local_hit.x() > 0 ? 1.0f : -1.0f, 0.0f, 0.0f}; }
    else if (abs_hit.y() >= max_ax - EPSILON) { local_n = {0.0f, local_hit.y() > 0 ? 1.0f : -1.0f, 0.0f}; }
    else { local_n = {0.0f, 0.0f, local_hit.z() > 0 ? 1.0f : -1.0f}; }
    r.normal = b.orientation().rotate_vec(local_n);
    return true;
}

} // namespace cast
} // namespace math
