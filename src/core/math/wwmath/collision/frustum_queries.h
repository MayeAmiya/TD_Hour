#pragma once

#include "collision_contracts.h"

#include "../geometry/aabb.h"
#include "../geometry/obb.h"
#include "../geometry/frustum.h"

#include <DirectXMath.h>
#include <cmath>
#include <cstdint>

namespace math {
namespace overlap {

// Frustum queries
inline plane_side frustum_aabb(const frustum& f, const aabb& b, uint32_t* planes_passed = nullptr) noexcept
{
    DirectX::ContainmentType ct = f.fr.Contains(b.box);
    if (ct == DirectX::CONTAINS)
    {
        if (planes_passed) { *planes_passed = 0x3F; }
        return plane_side::front;
    }
    if (ct == DirectX::DISJOINT) { return plane_side::back; }

    if (planes_passed)
    {
        *planes_passed = 0;
        DirectX::XMVECTOR raw_planes[6];
        f.fr.GetPlanes(&raw_planes[0], &raw_planes[1], &raw_planes[2],
                       &raw_planes[3], &raw_planes[4], &raw_planes[5]);
        DirectX::XMFLOAT4 planes[6];
        for (int i = 0; i < 6; ++i) { DirectX::XMStoreFloat4(&planes[i], raw_planes[i]); }
        vec3 c = b.center();
        vec3 e = b.extents();
        for (int i = 0; i < 6; ++i)
        {
            float d = planes[i].x * c.x() + planes[i].y * c.y() + planes[i].z * c.z() - planes[i].w;
            float proj = e.x() * std::abs(planes[i].x) + e.y() * std::abs(planes[i].y) + e.z() * std::abs(planes[i].z);
            if (d > proj) { *planes_passed |= (1u << i); }
        }
    }
    return plane_side::both;
}

inline plane_side frustum_obb(const frustum& f, const obb& b, uint32_t* planes_passed = nullptr) noexcept
{
    DirectX::XMVECTOR raw_planes[6];
    f.fr.GetPlanes(&raw_planes[0], &raw_planes[1], &raw_planes[2],
                   &raw_planes[3], &raw_planes[4], &raw_planes[5]);
    DirectX::XMFLOAT4 planes[6];
    for (int i = 0; i < 6; ++i) { DirectX::XMStoreFloat4(&planes[i], raw_planes[i]); }
    vec3 c = b.center();
    quat o = b.orientation();
    vec3 e = b.extents();

    plane_side result = plane_side::front;
    uint32_t passed = 0;

    for (int i = 0; i < 6; ++i)
    {
        DirectX::XMVECTOR n_dx = DirectX::XMVectorSet(planes[i].x, planes[i].y, planes[i].z, 0);
        DirectX::XMVECTOR local_n = DirectX::XMVector3Rotate(n_dx, DirectX::XMQuaternionInverse(o));
        float plane_d = planes[i].x * c.x() + planes[i].y * c.y() + planes[i].z * c.z() - planes[i].w;
        float proj = e.x() * std::abs(DirectX::XMVectorGetX(local_n))
                   + e.y() * std::abs(DirectX::XMVectorGetY(local_n))
                   + e.z() * std::abs(DirectX::XMVectorGetZ(local_n));

        if (plane_d > proj)
        {
            passed |= (1u << i);
            continue;
        }
        if (plane_d < -proj) { return plane_side::back; }
        result = plane_side::both;
    }

    if (planes_passed) { *planes_passed = passed; }
    return result;
}

} // namespace overlap
} // namespace math
