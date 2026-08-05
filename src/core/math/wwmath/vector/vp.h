#pragma once

#include "float2.h"
#include "float3.h"
#include "float4.h"
#include "../matrix/transform.h"
#include "../matrix/float4x4.h"
#include <DirectXMath.h>
#include <cstring>
#include <cmath>

namespace math {

class vector_processor
{
public:
    static void transform(vec3* dst, const vec3* src, const transform& matrix, int count) noexcept
    {
        DirectX::XMMATRIX m = matrix.load();
        for (int i = 0; i < count; ++i)
            dst[i] = vec3{DirectX::XMVector3Transform(src[i], m)};
    }

    static void transform(vec4* dst, const vec3* src, const float4x4& matrix, int count) noexcept
    {
        DirectX::XMMATRIX m = matrix.load();
        for (int i = 0; i < count; ++i)
            dst[i] = vec4{DirectX::XMVector3Transform(src[i], m)};
    }

    static void copy(uint32_t* dst, const uint32_t* src, int count) noexcept
    {
        std::memcpy(dst, src, count * sizeof(uint32_t));
    }
    static void copy(vec2* dst, const vec2* src, int count) noexcept
    {
        std::memcpy(dst, src, count * sizeof(vec2));
    }
    static void copy(vec3* dst, const vec3* src, int count) noexcept
    {
        std::memcpy(dst, src, count * sizeof(vec3));
    }
    static void copy(vec4* dst, const vec4* src, int count) noexcept
    {
        std::memcpy(dst, src, count * sizeof(vec4));
    }

    static void copy(vec4* dst, const vec3* src, const float* srca, int count) noexcept
    {
        for (int i = 0; i < count; ++i)
            dst[i] = vec4{src[i].x(), src[i].y(), src[i].z(), srca[i]};
    }

    static void copy(vec4* dst, const vec3* src, float srca, int count) noexcept
    {
        for (int i = 0; i < count; ++i)
            DirectX::XMStoreFloat4(&dst[i].v,
                DirectX::XMVectorSetW(DirectX::XMLoadFloat3(&src[i].v), srca));
    }

    static void copy(vec4* dst, const vec3& src, const float* srca, int count) noexcept
    {
        DirectX::XMVECTOR s = src;
        for (int i = 0; i < count; ++i)
            DirectX::XMStoreFloat4(&dst[i].v, DirectX::XMVectorSetW(s, srca[i]));
    }

    static void copy_indexed(uint32_t* dst, const uint32_t* src, const uint32_t* index, int count) noexcept
    {
        for (int i = 0; i < count; ++i) dst[i] = src[index[i]];
    }
    static void copy_indexed(vec2* dst, const vec2* src, const uint32_t* index, int count) noexcept
    {
        for (int i = 0; i < count; ++i) dst[i] = src[index[i]];
    }
    static void copy_indexed(vec3* dst, const vec3* src, const uint32_t* index, int count) noexcept
    {
        for (int i = 0; i < count; ++i) dst[i] = src[index[i]];
    }
    static void copy_indexed(vec4* dst, const vec4* src, const uint32_t* index, int count) noexcept
    {
        for (int i = 0; i < count; ++i) dst[i] = src[index[i]];
    }
    static void copy_indexed(uint8_t* dst, const uint8_t* src, const uint32_t* index, int count) noexcept
    {
        for (int i = 0; i < count; ++i) dst[i] = src[index[i]];
    }
    static void copy_indexed(float* dst, float* src, const uint32_t* index, int count) noexcept
    {
        for (int i = 0; i < count; ++i) dst[i] = src[index[i]];
    }

    static void clamp(vec4* dst, const vec4* src, float min_val, float max_val, int count) noexcept
    {
        DirectX::XMVECTOR mn = DirectX::XMVectorReplicate(min_val);
        DirectX::XMVECTOR mx = DirectX::XMVectorReplicate(max_val);
        for (int i = 0; i < count; ++i)
            dst[i] = vec4{DirectX::XMVectorClamp(src[i], mn, mx)};
    }

    static void clear(vec3* dst, int count) noexcept
    {
        std::memset(dst, 0, count * sizeof(vec3));
    }

    static void normalize(vec3* dst, int count) noexcept
    {
        for (int i = 0; i < count; ++i) dst[i].normalize();
    }

    static void min_max(vec3* src, vec3& min_out, vec3& max_out, int count) noexcept
    {
        if (count <= 0) { return; }
        DirectX::XMVECTOR mn = src[0];
        DirectX::XMVECTOR mx = src[0];
        for (int i = 1; i < count; ++i)
        {
            mn = DirectX::XMVectorMin(mn, src[i]);
            mx = DirectX::XMVectorMax(mx, src[i]);
        }
        min_out = vec3{mn};
        max_out = vec3{mx};
    }

    static void mul_add(float* dest, float multiplier, float add, int count) noexcept
    {
        DirectX::XMVECTOR m = DirectX::XMVectorReplicate(multiplier);
        DirectX::XMVECTOR a = DirectX::XMVectorReplicate(add);
        for (int i = 0; i < count; ++i)
            dest[i] = dest[i] * multiplier + add;
    }

    static void dot_product(float* dst, const vec3& a, const vec3* b, int count) noexcept
    {
        DirectX::XMVECTOR va = a;
        for (int i = 0; i < count; ++i)
            dst[i] = DirectX::XMVectorGetX(DirectX::XMVector3Dot(va, b[i]));
    }

    static void clamp_min(float* dst, float* src, float min_val, int count) noexcept
    {
        for (int i = 0; i < count; ++i)
            dst[i] = src[i] < min_val ? min_val : src[i];
    }

    static void power(float* dst, float* src, float pow_val, int count) noexcept
    {
        for (int i = 0; i < count; ++i)
            dst[i] = std::pow(src[i], pow_val);
    }
};

} // namespace math
