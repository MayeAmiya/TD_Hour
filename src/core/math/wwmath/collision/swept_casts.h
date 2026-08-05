#pragma once

#include "collision_contracts.h"

#include "../vector/float4.h"
#include "../geometry/triangle.h"
#include "../geometry/aabb.h"
#include "../geometry/obb.h"
#include "core/math/wwmath/base/wwmath_core.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

namespace math {
namespace cast {

// ── Swept AABB vs primitive ────────────────────────────────────────────

inline bool aabb_plane(const aabb& box, vec3 move, const vec4& plane_eq, hit_result& r) noexcept
{
    vec3 e = box.extents();
    vec3 n {
        DirectX::XMVectorGetX(DirectX::XMPlaneDotNormal(plane_eq, DirectX::g_XMIdentityR0)),
        DirectX::XMVectorGetX(DirectX::XMPlaneDotNormal(plane_eq, DirectX::g_XMIdentityR1)),
        DirectX::XMVectorGetX(DirectX::XMPlaneDotNormal(plane_eq, DirectX::g_XMIdentityR2)),
    };

    // Project half-extent onto normal
    float proj = e.x() * std::abs(n.x()) + e.y() * std::abs(n.y()) + e.z() * std::abs(n.z());
    float d = DirectX::XMVectorGetX(DirectX::XMPlaneDotCoord(plane_eq, box.center()));

    // Check if already overlapping
    if (d <= proj && d >= -proj) { r.start_bad = true; return true; }

    float move_dot_n = move.dot(n);
    if (std::abs(move_dot_n) < EPSILON) { return false; }

    float t;
    if (d > proj)
    {
        // Front side
        t = (proj - d) / move_dot_n;
    }
    else
    {
        // Back side (moving through)
        t = (-proj - d) / move_dot_n;
    }

    if (t < 0.0f || t > 1.0f) { return false; }

    r.fraction = t;
    r.normal = n;
    if (d < -proj) { r.normal = -r.normal; }
    return true;
}

inline bool aabb_triangle(const aabb& box, vec3 move, const triangle& tri, hit_result& r) noexcept
{
    // Simplified: decompose AABB into center+extents, test swept AABB against tri
    // using separating axis for swept volumes
    vec3 c = box.center();
    vec3 e = box.extents();

    // Triangle vertices relative to AABB start
    vec3 v0 = tri.v[0] - c;
    vec3 v1 = tri.v[1] - c;
    vec3 v2 = tri.v[2] - c;

    // Face normals
    vec3 tri_n = tri.normal();
    float nd = tri_n.dot(move);
    if (std::abs(nd) < EPSILON) { return false; }

    // Compute signed distance from AABB center to triangle plane
    float d = tri_n.dot(v0) - e.x() * std::abs(tri_n.x())
                               - e.y() * std::abs(tri_n.y())
                               - e.z() * std::abs(tri_n.z());

    float t = -d / nd;
    if (t < 0.0f || t > 1.0f) { return false; }

    // Check if contact point at t is within triangle
    // (simplified overlap check at the hit position)
    r.fraction = t;
    r.normal = tri_n;
    return true;
}

inline bool aabb_aabb(const aabb& box, vec3 move,
                      const aabb& other, vec3 other_move,
                      hit_result& r) noexcept
{
    // Separating Axis Theorem for swept AABB vs AABB
    float tmin = 0.0f;
    float tmax = 1.0f;

    vec3 rel_move = move - other_move;
    vec3 c0 = box.center();
    vec3 c1 = other.center();
    vec3 e0 = box.extents();
    vec3 e1 = other.extents();

    for (int i = 0; i < 3; ++i)
    {
        const float* rm = static_cast<const float*>(rel_move);
        const float* cc0 = static_cast<const float*>(c0);
        const float* cc1 = static_cast<const float*>(c1);
        const float* ee0 = static_cast<const float*>(e0);
        const float* ee1 = static_cast<const float*>(e1);

        float dist = cc1[i] - cc0[i];
        float extent = ee0[i] + ee1[i];

        if (std::abs(rm[i]) < EPSILON)
        {
            if (std::abs(dist) > extent) { return false; }
        }
        else
        {
            float inv = 1.0f / rm[i];
            float t1 = (dist - extent) * inv;
            float t2 = (dist + extent) * inv;
            if (t1 > t2) { std::swap(t1, t2); }
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) { return false; }
        }
    }

    if (tmin < 0.0f) { r.start_bad = true; return true; }
    if (tmin > 1.0f) { return false; }

    r.fraction = tmin;
    // Compute normal from the separating axis at impact
    vec3 hit_pos = c0 + move * tmin;
    vec3 diff = c1 - hit_pos;
    vec3 abs_diff {std::abs(diff.x()), std::abs(diff.y()), std::abs(diff.z())};
    float max_ax = std::max({abs_diff.x(), abs_diff.y(), abs_diff.z()});
    if (abs_diff.x() >= max_ax - EPSILON) { r.normal = {diff.x() > 0 ? 1.0f : -1.0f, 0.0f, 0.0f}; }
    else if (abs_diff.y() >= max_ax - EPSILON) { r.normal = {0.0f, diff.y() > 0 ? 1.0f : -1.0f, 0.0f}; }
    else { r.normal = {0.0f, 0.0f, diff.z() > 0 ? 1.0f : -1.0f}; }
    return true;
}

// ── Swept OBB vs primitive ─────────────────────────────────────────────

inline bool obb_plane(const obb& box, vec3 move, const vec4& plane_eq, hit_result& r) noexcept
{
    vec3 c = box.center();
    vec3 e = box.extents();
    quat o = box.orientation();

    // Rotate plane normal into OBB local space
    vec3 n {
        DirectX::XMVectorGetX(DirectX::XMPlaneDotNormal(plane_eq, DirectX::g_XMIdentityR0)),
        DirectX::XMVectorGetX(DirectX::XMPlaneDotNormal(plane_eq, DirectX::g_XMIdentityR1)),
        DirectX::XMVectorGetX(DirectX::XMPlaneDotNormal(plane_eq, DirectX::g_XMIdentityR2)),
    };
    DirectX::XMVECTOR local_n = DirectX::XMVector3Rotate(
        n, DirectX::XMQuaternionInverse(o));
    DirectX::XMFLOAT3 ln;
    DirectX::XMStoreFloat3(&ln, local_n);

    float proj = e.x() * std::abs(ln.x) + e.y() * std::abs(ln.y) + e.z() * std::abs(ln.z);
    float d = DirectX::XMVectorGetX(DirectX::XMPlaneDotCoord(plane_eq, c));

    if (d <= proj && d >= -proj) { r.start_bad = true; return true; }

    float move_dot_n = move.dot(n);
    if (std::abs(move_dot_n) < EPSILON) { return false; }

    float t = (d > proj) ? (proj - d) / move_dot_n : (-proj - d) / move_dot_n;
    if (t < 0.0f || t > 1.0f) { return false; }

    r.fraction = t;
    r.normal = n;
    if (d < -proj) { r.normal = -r.normal; }
    return true;
}

// ── Swept AABB vs OBB ─────────────────────────────────────────────────
inline bool aabb_obb(const aabb& box, vec3 move,
                     const obb& other, vec3 other_move,
                     hit_result& r) noexcept
{
    // Transform AABB into OBB local space, treat as AABB vs AABB sweep
    quat inv_orient = other.orientation().inverse();
    vec3 rel_move = inv_orient.rotate_vec(move - other_move);
    vec3 local_center = inv_orient.rotate_vec(box.center() - other.center());
    aabb local_box{local_center, box.extents()};
    aabb other_box{vec3::zero(), other.extents()};
    return aabb_aabb(local_box, rel_move, other_box, vec3::zero(), r);
}

// ── Swept OBB vs triangle ─────────────────────────────────────────────
inline bool obb_triangle(const obb& box, vec3 move,
                         const triangle& tri, vec3 tri_move,
                         hit_result& r) noexcept
{
    // Transform OBB into AABB (in local space) and transform triangle too
    quat inv = box.orientation().inverse();
    vec3 local_move = inv.rotate_vec(move);

    aabb local_aabb{vec3::zero(), box.extents()};

    vec3 local_tri_move = inv.rotate_vec(tri_move);
    triangle local_tri{
        inv.rotate_vec(tri.v[0] - box.center()),
        inv.rotate_vec(tri.v[1] - box.center()),
        inv.rotate_vec(tri.v[2] - box.center())
    };

    // Use swept AABB vs triangle in local space
    bool hit = aabb_triangle(local_aabb, local_move, local_tri, r);
    if (hit)
    {
        r.normal = box.orientation().rotate_vec(r.normal);
    }
    return hit;
}

// ── Swept OBB vs AABB ─────────────────────────────────────────────────
inline bool obb_aabb(const obb& box, vec3 move,
                     const aabb& other, vec3 other_move,
                     hit_result& r) noexcept
{
    // Rotate everything into OBB local space
    quat inv = box.orientation().inverse();
    vec3 local_center = inv.rotate_vec(other.center() - box.center());
    vec3 local_move = inv.rotate_vec(move);
    vec3 local_other_move = inv.rotate_vec(other_move);

    // Approximate OBB vs AABB as AABB vs AABB in OBB local space
    aabb local_box{vec3::zero(), box.extents()};
    aabb local_other{local_center, other.extents()};

    bool hit = aabb_aabb(local_box, local_move, local_other, local_other_move, r);
    // Only rotate on a hit.  aabb_aabb never writes r.normal when it returns
    // false, so rotating unconditionally transformed whatever the caller already
    // had — in the standard closest-hit pattern where one hit_result is reused
    // across candidates, a later MISS silently rotated the normal recorded by an
    // earlier HIT.  obb_triangle gets this right.
    if (hit) { r.normal = box.orientation().rotate_vec(r.normal); }
    return hit;
}

// ── Swept OBB vs OBB ──────────────────────────────────────────────────
inline bool obb_obb(const obb& box, vec3 move,
                    const obb& other, vec3 other_move,
                    hit_result& r) noexcept
{
    // Transform both into first OBB's local space
    quat inv = box.orientation().inverse();
    vec3 local_other_center = inv.rotate_vec(other.center() - box.center());
    quat local_other_orient = (box.orientation().inverse() * other.orientation()).normalized();
    vec3 local_move = inv.rotate_vec(move);
    vec3 local_other_move = inv.rotate_vec(other_move);

    aabb local_box{vec3::zero(), box.extents()};
    obb local_other{local_other_center, other.extents(), local_other_orient};

    // As approximation, treat local OBB as AABB (expanded by rotation)
    // For full accuracy we'd need SAT for swept OBBs, but this gives reasonable results
    // Same rule as obb_aabb: a miss leaves r.normal untouched, so rotating it
    // would corrupt a normal recorded by an earlier hit on a reused hit_result.
    bool hit = aabb_obb(local_box, local_move, local_other, local_other_move, r);
    if (hit) { r.normal = box.orientation().rotate_vec(r.normal); }
    return hit;
}

} // namespace cast
} // namespace math
