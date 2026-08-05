#pragma once

#include "../vector/float3.h"
#include <DirectXMath.h>

namespace math {

class float3x3
{
public:
    DirectX::XMFLOAT3X3 m{};

    float3x3() noexcept
    {
        DirectX::XMStoreFloat3x3(&m, DirectX::XMMatrixIdentity());
    }
    explicit float3x3(DirectX::FXMMATRIX mat) noexcept
    {
        DirectX::XMStoreFloat3x3(&m, mat);
    }

    DirectX::XMMATRIX load() const noexcept { return DirectX::XMLoadFloat3x3(&m); }
    void store(DirectX::FXMMATRIX mat) noexcept { DirectX::XMStoreFloat3x3(&m, mat); }
    operator DirectX::XMMATRIX() const noexcept { return load(); }
    operator const float*() const noexcept { return &m._11; }

    vec3 right()  const noexcept { return vec3{load().r[0]}; }
    vec3 up()     const noexcept { return vec3{load().r[1]}; }
    vec3 forward() const noexcept { return vec3{load().r[2]}; }

    float3x3 operator*(const float3x3& r) const noexcept
    {
        return float3x3{DirectX::XMMatrixMultiply(load(), r)};
    }
    float3x3& operator*=(const float3x3& r) noexcept
    {
        store(DirectX::XMMatrixMultiply(load(), r));
        return *this;
    }

    vec3 transform_dir(vec3 dir) const noexcept
    {
        return vec3{DirectX::XMVector3TransformNormal(dir, load())};
    }

    void rotate_x(float angle) noexcept
    {
        store(DirectX::XMMatrixMultiply(DirectX::XMMatrixRotationX(angle), load()));
    }
    void rotate_y(float angle) noexcept
    {
        store(DirectX::XMMatrixMultiply(DirectX::XMMatrixRotationY(angle), load()));
    }
    void rotate_z(float angle) noexcept
    {
        store(DirectX::XMMatrixMultiply(DirectX::XMMatrixRotationZ(angle), load()));
    }

    static float3x3 identity() noexcept { return float3x3{}; }
    static float3x3 rotation_x(float angle) noexcept { return float3x3{DirectX::XMMatrixRotationX(angle)}; }
    static float3x3 rotation_y(float angle) noexcept { return float3x3{DirectX::XMMatrixRotationY(angle)}; }
    static float3x3 rotation_z(float angle) noexcept { return float3x3{DirectX::XMMatrixRotationZ(angle)}; }
};

} // namespace math
