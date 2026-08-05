#pragma once

#include "collision_contracts.h"

#include "../geometry/segment.h"
#include "../geometry/triangle.h"
#include "../geometry/aabb.h"
#include "../geometry/obb.h"
#include "../geometry/sphere.h"
#include "../geometry/frustum.h"
#include "core/math/wwmath/base/wwmath_core.h"

#include <algorithm>
#include <cmath>

namespace math {
namespace cast {

// ── Sphere overlap tests ──────────────────────────────────────────────
inline plane_side sphere_point(const sphere& s, vec3 pt) noexcept
{
    float d = s.center().distance(pt);
    float r = s.radius();
    if (d > r)          return plane_side::front;
    if (d < r - EPSILON) return plane_side::back;
    return plane_side::on;
}

inline plane_side sphere_segment(const sphere& s, const segment& seg) noexcept
{
    vec3 closest = seg.closest_point(s.center());
    return sphere_point(s, closest);
}

inline plane_side sphere_triangle(const sphere& s, const triangle& tri) noexcept
{
    vec3 cp = tri.v[0];  // closest point on triangle to sphere center
    vec3 c = s.center();
    vec3 e0 = tri.v[1] - tri.v[0];
    vec3 e1 = tri.v[2] - tri.v[0];
    vec3 p = c - tri.v[0];

    float d00 = e0.dot(e0);
    float d01 = e0.dot(e1);
    float d11 = e1.dot(e1);
    float d20 = p.dot(e0);
    float d21 = p.dot(e1);

    float denom = d00 * d11 - d01 * d01;
    if (denom < EPSILON) { return sphere_point(s, tri.v[0]); }

    float u = (d11 * d20 - d01 * d21) / denom;
    float v = (d00 * d21 - d01 * d20) / denom;

    if (u < 0) { u = 0; }
    if (v < 0) { v = 0; }
    if (u + v > 1) { float t = u + v; u /= t; v /= t; }

    cp = tri.v[0] + e0 * u + e1 * v;
    return sphere_point(s, cp);
}

inline plane_side sphere_sphere(const sphere& a, const sphere& b) noexcept
{
    float d = a.center().distance(b.center());
    float r_sum = a.radius() + b.radius();
    if (d > r_sum)           return plane_side::front;
    if (d + a.radius() < b.radius() || d + b.radius() < a.radius())
        return plane_side::back;
    return plane_side::both;
}

inline plane_side sphere_aabb(const sphere& s, const aabb& b) noexcept
{
    vec3 cp = b.center();
    vec3 c = s.center();
    vec3 e = b.extents();
    vec3 diff = c - cp;

    for (int i = 0; i < 3; ++i)
    {
        if (diff[i] > e[i]) { diff[i] = e[i]; }
        if (diff[i] < -e[i]) { diff[i] = -e[i]; }
    }

    vec3 closest = cp + diff;
    return sphere_point(s, closest);
}

inline plane_side sphere_obb(const sphere& s, const obb& b) noexcept
{
    // Transform sphere center into OBB local space
    vec3 local_c = b.orientation().inverse().rotate_vec(s.center() - b.center());
    aabb local_aabb{vec3::zero(), b.extents()};
    sphere local_s{local_c, s.radius()};
    return sphere_aabb(local_s, local_aabb);
}

// ── AABB overlap tests ────────────────────────────────────────────────
inline plane_side aabb_point(const aabb& b, vec3 pt) noexcept
{
    vec3 mn = b.min();
    vec3 mx = b.max();
    if (pt.x() < mn.x() || pt.x() > mx.x() ||
        pt.y() < mn.y() || pt.y() > mx.y() ||
        pt.z() < mn.z() || pt.z() > mx.z())
    {
        return plane_side::front;
    }
    return plane_side::back;
}

inline plane_side aabb_segment(const aabb& b, const segment& seg) noexcept
{
    vec3 mn = b.min();
    vec3 mx = b.max();
    vec3 dir = seg.p1 - seg.p0;
    float tmin = 0.0f, tmax = 1.0f;

    for (int i = 0; i < 3; ++i)
    {
        const float* d = static_cast<const float*>(dir);
        const float* p0 = static_cast<const float*>(seg.p0);
        const float* minp = static_cast<const float*>(mn);
        const float* maxp = static_cast<const float*>(mx);

        if (std::abs(d[i]) < EPSILON)
        {
            if (p0[i] < minp[i] || p0[i] > maxp[i]) return plane_side::front;
        }
        else
        {
            float inv = 1.0f / d[i];
            float t1 = (minp[i] - p0[i]) * inv;
            float t2 = (maxp[i] - p0[i]) * inv;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return plane_side::front;
        }
    }

    if (tmin < 0.0f || tmin > 1.0f) return plane_side::front;
    if (tmin <= EPSILON && tmax >= 1.0f) return plane_side::back;
    return plane_side::both;
}

inline plane_side aabb_triangle(const aabb& b, const triangle& tri) noexcept
{
    // Separating axis test: 3 AABB axes + 1 tri normal + 9 edge-edge
    vec3 c = b.center(), e = b.extents();
    vec3 v[3] = {tri.v[0] - c, tri.v[1] - c, tri.v[2] - c};
    vec3 f[3] = {v[1] - v[0], v[2] - v[1], v[0] - v[2]};

    auto test_axis = [&](vec3 axis) -> bool {
        float p = std::abs(axis.dot(v[0]));
        float r = e.x() * std::abs(axis.x()) + e.y() * std::abs(axis.y()) + e.z() * std::abs(axis.z());
        float proj_min = std::min({axis.dot(v[0]), axis.dot(v[1]), axis.dot(v[2])});
        float proj_max = std::max({axis.dot(v[0]), axis.dot(v[1]), axis.dot(v[2])});
        return proj_max < -r || proj_min > r;
    };

    // AABB face axes
    if (test_axis(vec3{1,0,0}) || test_axis(vec3{0,1,0}) || test_axis(vec3{0,0,1}))
        return plane_side::front;

    // Triangle normal
    vec3 tri_n = tri.normal();
    if (test_axis(tri_n)) return plane_side::front;

    // Edge cross products
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            vec3 axis = f[i].cross(vec3{j == 0 ? 1.0f : 0.0f,
                                        j == 1 ? 1.0f : 0.0f,
                                        j == 2 ? 1.0f : 0.0f});
            if (axis.length_sq() > EPSILON && test_axis(axis))
                return plane_side::front;
        }
    }

    return plane_side::both;
}

inline plane_side aabb_aabb(const aabb& a, const aabb& b) noexcept
{
    vec3 a_min = a.min(), a_max = a.max();
    vec3 b_min = b.min(), b_max = b.max();

    if (a_max.x() < b_min.x() || a_min.x() > b_max.x() ||
        a_max.y() < b_min.y() || a_min.y() > b_max.y() ||
        a_max.z() < b_min.z() || a_min.z() > b_max.z())
    {
        return plane_side::front;
    }
    return plane_side::both;
}

inline plane_side aabb_obb(const aabb& b, const obb& ob) noexcept
{
    return b.intersects(ob) ? plane_side::both : plane_side::front;
}

inline plane_side aabb_sphere(const aabb& b, const sphere& s) noexcept
{
    return sphere_aabb(s, b);
}

// ── OBB overlap tests ─────────────────────────────────────────────────
inline plane_side obb_point(const obb& b, vec3 pt) noexcept
{
    vec3 local_pt = b.orientation().inverse().rotate_vec(pt - b.center());
    vec3 e = b.extents();
    if (std::abs(local_pt.x()) <= e.x() &&
        std::abs(local_pt.y()) <= e.y() &&
        std::abs(local_pt.z()) <= e.z())
    {
        return plane_side::back;
    }
    return plane_side::front;
}

inline plane_side obb_segment(const obb& b, const segment& seg) noexcept
{
    // Check both endpoints; if both outside and separated by a face, it's front
    plane_side s0 = obb_point(b, seg.p0);
    plane_side s1 = obb_point(b, seg.p1);
    if (s0 == plane_side::back || s1 == plane_side::back)
    {
        if (s0 == s1) return plane_side::back;
        return plane_side::both;
    }

    // Both outside, check if segment crosses OBB
    aabb local_aabb{vec3::zero(), b.extents()};
    quat inv = b.orientation().inverse();
    segment local_seg{inv.rotate_vec(seg.p0 - b.center()), inv.rotate_vec(seg.p1 - b.center())};
    return aabb_segment(local_aabb, local_seg);
}

inline plane_side obb_triangle(const obb& b, const triangle& tri) noexcept
{
    // Transform triangle into OBB local space
    quat inv = b.orientation().inverse();
    triangle local_tri{
        inv.rotate_vec(tri.v[0] - b.center()),
        inv.rotate_vec(tri.v[1] - b.center()),
        inv.rotate_vec(tri.v[2] - b.center())
    };
    aabb local_aabb{vec3::zero(), b.extents()};
    return aabb_triangle(local_aabb, local_tri);
}

inline plane_side obb_aabb(const obb& b, const aabb& box) noexcept
{
    return aabb_obb(box, b);
}

inline plane_side obb_obb(const obb& a, const obb& b) noexcept
{
    return a.intersects(b) ? plane_side::both : plane_side::front;
}

// ── Frustum overlap tests (extras) ────────────────────────────────────
inline plane_side frustum_point(const frustum& f, vec3 pt) noexcept
{
    return f.contains(pt) ? plane_side::back : plane_side::front;
}

inline plane_side frustum_tri(const frustum& f, const triangle& tri) noexcept
{
    plane_side s0 = frustum_point(f, tri.v[0]);
    plane_side s1 = frustum_point(f, tri.v[1]);
    plane_side s2 = frustum_point(f, tri.v[2]);
    if (s0 == s1 && s1 == s2) { return s0; }
    return plane_side::both;
}

inline plane_side frustum_sphere(const frustum& f, const sphere& s) noexcept
{
    return f.contains(s) ? plane_side::back : plane_side::front;
}

} // namespace cast
} // namespace math
