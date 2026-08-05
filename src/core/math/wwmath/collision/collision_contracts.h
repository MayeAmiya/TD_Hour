#pragma once

#include "debug/debug_config.h"

#include "../vector/float3.h"

#include <cstdint>

namespace math {

// ── hit_result ─────────────────────────────────────────────────────────
struct hit_result
{
    bool     start_bad      = false;
    float    fraction       = 1.0f;
    vec3     normal         = vec3::zero();
    uint32_t surface_type   = 0;
    bool     compute_contact_point = false;
    vec3     contact_point  = vec3::zero();

    void reset() noexcept
    {
        fraction  = 1.0f;
        start_bad = false;
        normal    = vec3::zero();
    }
};

// ── plane_side ─────────────────────────────────────────────────────────
enum class plane_side : uint32_t
{
    front = 0x01,
    back  = 0x02,
    on    = 0x04,
    both  = 0x08,
};

inline plane_side operator|(plane_side a, plane_side b) noexcept
{
    return static_cast<plane_side>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline plane_side operator&(plane_side a, plane_side b) noexcept
{
    return static_cast<plane_side>(
        static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline bool is_set(plane_side v, plane_side mask) noexcept
{
    return (static_cast<uint32_t>(v) & static_cast<uint32_t>(mask)) != 0;
}

// ── CULLTYPE enum ─────────────────────────────────────────────────────
enum class cull_type : uint32_t
{
    outside       = 0,
    intersecting  = 1,
    inside        = 2,
};

// ── Collision Stats ───────────────────────────────────────────────────
#if TD_DEBUG_ENABLED
struct colmath_stats
{
    int total_collision_count      = 0;
    int total_collision_hit_count  = 0;
    int collision_ray_tri_count      = 0;
    int collision_ray_tri_hit_count  = 0;
    int collision_aabb_tri_count     = 0;
    int collision_aabb_tri_hit_count = 0;
    int collision_aabb_aabb_count      = 0;
    int collision_aabb_aabb_hit_count  = 0;
    int collision_obb_tri_count     = 0;
    int collision_obb_tri_hit_count = 0;
    int collision_obb_aabb_count      = 0;
    int collision_obb_aabb_hit_count  = 0;
    int collision_obb_obb_count     = 0;
    int collision_obb_obb_hit_count = 0;

    void reset() noexcept { *this = colmath_stats{}; }
};

inline colmath_stats g_colmath_stats;
#endif

} // namespace math
