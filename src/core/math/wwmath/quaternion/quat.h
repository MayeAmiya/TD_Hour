#pragma once

#include "../vector/float3.h"
#include <DirectXMath.h>

namespace math {

class quat
{
public:
    DirectX::XMFLOAT4 q{0, 0, 0, 1};

    quat() noexcept = default;
    quat(float x, float y, float z, float w) noexcept
        : q{x, y, z, w}
    {
    }
    explicit quat(DirectX::FXMVECTOR vec) noexcept
    {
        DirectX::XMStoreFloat4(&q, vec);
    }
    explicit quat(const DirectX::XMFLOAT4& value) noexcept
        : q(value)
    {
    }

    float x() const noexcept { return q.x; }
    float y() const noexcept { return q.y; }
    float z() const noexcept { return q.z; }
    float w() const noexcept { return q.w; }

    DirectX::XMVECTOR load() const noexcept { return DirectX::XMLoadFloat4(&q); }
    void store(DirectX::FXMVECTOR v) noexcept { DirectX::XMStoreFloat4(&q, v); }
    operator DirectX::XMVECTOR() const noexcept { return load(); }

    static quat identity() noexcept { return {0, 0, 0, 1}; }

    static quat from_axis_angle(vec3 axis, float angle) noexcept
    {
        return quat{DirectX::XMQuaternionRotationAxis(axis, angle)};
    }

    static quat from_euler(float yaw, float pitch, float roll) noexcept
    {
        return quat{DirectX::XMQuaternionRotationRollPitchYaw(pitch, yaw, roll)};
    }

    static quat from_matrix(const class transform& tm) noexcept;

    quat operator*(quat r) const noexcept
    {
        return quat{DirectX::XMQuaternionMultiply(load(), r)};
    }
    quat& operator*=(quat r) noexcept
    {
        store(DirectX::XMQuaternionMultiply(load(), r));
        return *this;
    }

    vec3 rotate_vec(vec3 v) const noexcept
    {
        return vec3{DirectX::XMVector3Rotate(v, load())};
    }

    [[nodiscard]] float length() const noexcept
    {
        return DirectX::XMVectorGetX(DirectX::XMQuaternionLength(load()));
    }
    void normalize() noexcept
    {
        store(DirectX::XMQuaternionNormalize(load()));
    }
    [[nodiscard]] quat normalized() const noexcept
    {
        return quat{DirectX::XMQuaternionNormalize(load())};
    }
    [[nodiscard]] quat conjugate() const noexcept
    {
        return quat{DirectX::XMQuaternionConjugate(load())};
    }
    [[nodiscard]] quat inverse() const noexcept
    {
        return quat{DirectX::XMQuaternionInverse(load())};
    }

    static quat slerp(quat a, quat b, float t) noexcept
    {
        return quat{DirectX::XMQuaternionSlerp(a, b, t)};
    }
};

} // namespace math
