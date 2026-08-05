#pragma once

#include <DirectXMath.h>

namespace math {

class vec3
{
public:
    DirectX::XMFLOAT3 v{};

    vec3() noexcept = default;
    vec3(float x, float y, float z) noexcept
        : v{x, y, z}
    {
    }
    explicit vec3(DirectX::FXMVECTOR vec) noexcept
    {
        DirectX::XMStoreFloat3(&v, vec);
    }
    explicit vec3(const DirectX::XMFLOAT3& f) noexcept
        : v(f)
    {
    }

    float x() const noexcept { return v.x; }
    float y() const noexcept { return v.y; }
    float z() const noexcept { return v.z; }
    float& operator[](size_t index) noexcept { return (&v.x)[index]; }
    float operator[](size_t index) const noexcept { return (&v.x)[index]; }

    DirectX::XMVECTOR load() const noexcept { return DirectX::XMLoadFloat3(&v); }
    void store(DirectX::FXMVECTOR vec) noexcept { DirectX::XMStoreFloat3(&v, vec); }
    operator DirectX::XMVECTOR() const noexcept { return load(); }
    operator const float*() const noexcept { return &v.x; }

    vec3& operator+=(vec3 r) noexcept
    {
        store(DirectX::XMVectorAdd(load(), r));
        return *this;
    }
    vec3& operator-=(vec3 r) noexcept
    {
        store(DirectX::XMVectorSubtract(load(), r));
        return *this;
    }
    vec3& operator*=(float s) noexcept
    {
        store(DirectX::XMVectorScale(load(), s));
        return *this;
    }
    vec3& operator*=(vec3 r) noexcept
    {
        store(DirectX::XMVectorMultiply(load(), r));
        return *this;
    }
    vec3 operator+(vec3 r) const noexcept
    {
        return vec3{DirectX::XMVectorAdd(load(), r)};
    }
    vec3 operator-(vec3 r) const noexcept
    {
        return vec3{DirectX::XMVectorSubtract(load(), r)};
    }
    vec3 operator*(float s) const noexcept
    {
        return vec3{DirectX::XMVectorScale(load(), s)};
    }
    vec3 operator*(vec3 r) const noexcept
    {
        return vec3{DirectX::XMVectorMultiply(load(), r)};
    }
    vec3 operator/(float s) const noexcept
    {
        return vec3{DirectX::XMVectorScale(load(), 1.0f / s)};
    }
    vec3 operator-() const noexcept
    {
        return vec3{DirectX::XMVectorNegate(load())};
    }

    bool operator==(vec3 r) const noexcept
    {
        return DirectX::XMVector3Equal(load(), r);
    }
    bool operator!=(vec3 r) const noexcept
    {
        return DirectX::XMVector3NotEqual(load(), r);
    }

    [[nodiscard]] float dot(vec3 r) const noexcept
    {
        return DirectX::XMVectorGetX(DirectX::XMVector3Dot(load(), r));
    }
    [[nodiscard]] vec3 cross(vec3 r) const noexcept
    {
        return vec3{DirectX::XMVector3Cross(load(), r)};
    }
    [[nodiscard]] float length() const noexcept
    {
        return DirectX::XMVectorGetX(DirectX::XMVector3Length(load()));
    }
    [[nodiscard]] float length_sq() const noexcept
    {
        return DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(load()));
    }
    void normalize() noexcept
    {
        store(DirectX::XMVector3Normalize(load()));
    }
    [[nodiscard]] vec3 normalized() const noexcept
    {
        return vec3{DirectX::XMVector3Normalize(load())};
    }
    [[nodiscard]] float distance(vec3 r) const noexcept
    {
        return (*this - r).length();
    }
    [[nodiscard]] float distance_sq(vec3 r) const noexcept
    {
        return (*this - r).length_sq();
    }

    static vec3 lerp(vec3 a, vec3 b, float t) noexcept
    {
        return vec3{DirectX::XMVectorLerp(a, b, t)};
    }
    static vec3 min(vec3 a, vec3 b) noexcept
    {
        return vec3{DirectX::XMVectorMin(a, b)};
    }
    static vec3 max(vec3 a, vec3 b) noexcept
    {
        return vec3{DirectX::XMVectorMax(a, b)};
    }
    static vec3 reflect(vec3 v, vec3 n) noexcept
    {
        return vec3{DirectX::XMVector3Reflect(v, n)};
    }

    static vec3 zero()    noexcept { return {0, 0, 0}; }
    static vec3 one()     noexcept { return {1, 1, 1}; }
    static vec3 up()      noexcept { return {0, 1, 0}; }
    static vec3 down()    noexcept { return {0, -1, 0}; }
    static vec3 right()   noexcept { return {1, 0, 0}; }
    static vec3 left()    noexcept { return {-1, 0, 0}; }
    static vec3 forward() noexcept { return {0, 0, 1}; }
    static vec3 back()    noexcept { return {0, 0, -1}; }
};

} // namespace math
