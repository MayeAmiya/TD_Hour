#pragma once

#include <DirectXMath.h>

namespace math {

class vec4
{
public:
    DirectX::XMFLOAT4 v{};

    vec4() noexcept = default;
    vec4(float x, float y, float z, float w) noexcept
        : v{x, y, z, w}
    {
    }
    explicit vec4(DirectX::FXMVECTOR vec) noexcept
    {
        DirectX::XMStoreFloat4(&v, vec);
    }
    explicit vec4(const DirectX::XMFLOAT4& f) noexcept
        : v(f)
    {
    }

    float x() const noexcept { return v.x; }
    float y() const noexcept { return v.y; }
    float z() const noexcept { return v.z; }
    float w() const noexcept { return v.w; }
    float& operator[](size_t index) noexcept { return (&v.x)[index]; }
    float operator[](size_t index) const noexcept { return (&v.x)[index]; }

    DirectX::XMVECTOR load() const noexcept { return DirectX::XMLoadFloat4(&v); }
    void store(DirectX::FXMVECTOR vec) noexcept { DirectX::XMStoreFloat4(&v, vec); }
    operator DirectX::XMVECTOR() const noexcept { return load(); }
    operator const float*() const noexcept { return &v.x; }

    vec4& operator+=(vec4 r) noexcept
    {
        store(DirectX::XMVectorAdd(load(), r));
        return *this;
    }
    vec4& operator-=(vec4 r) noexcept
    {
        store(DirectX::XMVectorSubtract(load(), r));
        return *this;
    }
    vec4& operator*=(float s) noexcept
    {
        store(DirectX::XMVectorScale(load(), s));
        return *this;
    }
    vec4 operator+(vec4 r) const noexcept
    {
        return vec4{DirectX::XMVectorAdd(load(), r)};
    }
    vec4 operator-(vec4 r) const noexcept
    {
        return vec4{DirectX::XMVectorSubtract(load(), r)};
    }
    vec4 operator*(float s) const noexcept
    {
        return vec4{DirectX::XMVectorScale(load(), s)};
    }
    vec4 operator/(float s) const noexcept
    {
        return vec4{DirectX::XMVectorScale(load(), 1.0f / s)};
    }
    vec4 operator-() const noexcept
    {
        return vec4{DirectX::XMVectorNegate(load())};
    }

    [[nodiscard]] float dot(vec4 r) const noexcept
    {
        return DirectX::XMVectorGetX(DirectX::XMVector4Dot(load(), r));
    }
    [[nodiscard]] float length() const noexcept
    {
        return DirectX::XMVectorGetX(DirectX::XMVector4Length(load()));
    }
    [[nodiscard]] float length_sq() const noexcept
    {
        return DirectX::XMVectorGetX(DirectX::XMVector4LengthSq(load()));
    }
    void normalize() noexcept
    {
        store(DirectX::XMVector4Normalize(load()));
    }
    [[nodiscard]] vec4 normalized() const noexcept
    {
        return vec4{DirectX::XMVector4Normalize(load())};
    }

    static vec4 zero() noexcept { return {0, 0, 0, 0}; }
    static vec4 one()  noexcept { return {1, 1, 1, 1}; }
};

} // namespace math
