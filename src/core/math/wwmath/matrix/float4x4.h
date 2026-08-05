#pragma once

#include "../vector/float3.h"
#include "../vector/float4.h"
#include <DirectXMath.h>

namespace math {

class float4x4
{
public:
    DirectX::XMFLOAT4X4 m{};

    float4x4() noexcept
    {
        DirectX::XMStoreFloat4x4(&m, DirectX::XMMatrixIdentity());
    }
    explicit float4x4(DirectX::FXMMATRIX mat) noexcept
    {
        DirectX::XMStoreFloat4x4(&m, mat);
    }

    DirectX::XMMATRIX load() const noexcept { return DirectX::XMLoadFloat4x4(&m); }
    void store(DirectX::FXMMATRIX mat) noexcept { DirectX::XMStoreFloat4x4(&m, mat); }
    operator DirectX::XMMATRIX() const noexcept { return load(); }
    operator const float*() const noexcept { return &m._11; }

    vec4 r0() const noexcept { return vec4{load().r[0]}; }
    vec4 r1() const noexcept { return vec4{load().r[1]}; }
    vec4 r2() const noexcept { return vec4{load().r[2]}; }
    vec4 r3() const noexcept { return vec4{load().r[3]}; }

    float4x4 operator*(const float4x4& r) const noexcept
    {
        return float4x4{DirectX::XMMatrixMultiply(load(), r)};
    }
    float4x4& operator*=(const float4x4& r) noexcept
    {
        store(DirectX::XMMatrixMultiply(load(), r));
        return *this;
    }

    [[nodiscard]] float4x4 transpose() const noexcept
    {
        return float4x4{DirectX::XMMatrixTranspose(load())};
    }
    [[nodiscard]] float4x4 inverse() const noexcept
    {
        return float4x4{DirectX::XMMatrixInverse(nullptr, load())};
    }

    vec3 transform_point(vec3 pt) const noexcept
    {
        return vec3{DirectX::XMVector3Transform(pt, load())};
    }
    vec3 transform_dir(vec3 dir) const noexcept
    {
        return vec3{DirectX::XMVector3TransformNormal(dir, load())};
    }
    vec4 transform_vec4(vec4 v) const noexcept
    {
        return vec4{DirectX::XMVector4Transform(v, load())};
    }

    static float4x4 identity() noexcept { return float4x4{}; }
    static float4x4 translation(vec3 t) noexcept { return float4x4{DirectX::XMMatrixTranslation(t.x(), t.y(), t.z())}; }
    static float4x4 rotation_x(float angle) noexcept { return float4x4{DirectX::XMMatrixRotationX(angle)}; }
    static float4x4 rotation_y(float angle) noexcept { return float4x4{DirectX::XMMatrixRotationY(angle)}; }
    static float4x4 rotation_z(float angle) noexcept { return float4x4{DirectX::XMMatrixRotationZ(angle)}; }
    static float4x4 scale(vec3 s) noexcept { return float4x4{DirectX::XMMatrixScaling(s.x(), s.y(), s.z())}; }
    static float4x4 perspective_fov(float fov_y, float aspect, float zn, float zf) noexcept
    {
        return float4x4{DirectX::XMMatrixPerspectiveFovLH(fov_y, aspect, zn, zf)};
    }
    static float4x4 perspective_fov_rh(float fov_y, float aspect, float zn, float zf) noexcept
    {
        return float4x4{DirectX::XMMatrixPerspectiveFovRH(fov_y, aspect, zn, zf)};
    }
    static float4x4 orthographic(float w, float h, float zn, float zf) noexcept
    {
        return float4x4{DirectX::XMMatrixOrthographicLH(w, h, zn, zf)};
    }
    static float4x4 look_at(vec3 eye, vec3 target, vec3 up = vec3::up()) noexcept
    {
        return float4x4{DirectX::XMMatrixLookAtLH(eye, target, up)};
    }
    static float4x4 look_at_rh(vec3 eye, vec3 target, vec3 up = vec3::up()) noexcept
    {
        return float4x4{DirectX::XMMatrixLookAtRH(eye, target, up)};
    }
};

} // namespace math
