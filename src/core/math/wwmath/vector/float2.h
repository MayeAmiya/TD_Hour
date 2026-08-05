#pragma once

#include <DirectXMath.h>

namespace math {

class vec2
{
public:
    DirectX::XMFLOAT2 v{};

    vec2() noexcept = default;
    vec2(float x, float y) noexcept
        : v{x, y}
    {
    }
    explicit vec2(DirectX::FXMVECTOR vec) noexcept
    {
        DirectX::XMStoreFloat2(&v, vec);
    }
    explicit vec2(const DirectX::XMFLOAT2& f) noexcept
        : v(f)
    {
    }

    float x() const noexcept { return v.x; }
    float y() const noexcept { return v.y; }
    float& operator[](size_t index) noexcept { return (&v.x)[index]; }
    float operator[](size_t index) const noexcept { return (&v.x)[index]; }

    DirectX::XMVECTOR load() const noexcept { return DirectX::XMLoadFloat2(&v); }
    void store(DirectX::FXMVECTOR vec) noexcept { DirectX::XMStoreFloat2(&v, vec); }
    operator DirectX::XMVECTOR() const noexcept { return load(); }
    operator const float*() const noexcept { return &v.x; }

    vec2& operator+=(vec2 r) noexcept
    {
        store(DirectX::XMVectorAdd(load(), r));
        return *this;
    }
    vec2& operator-=(vec2 r) noexcept
    {
        store(DirectX::XMVectorSubtract(load(), r));
        return *this;
    }
    vec2& operator*=(float s) noexcept
    {
        store(DirectX::XMVectorScale(load(), s));
        return *this;
    }
    vec2 operator+(vec2 r) const noexcept
    {
        return vec2{DirectX::XMVectorAdd(load(), r)};
    }
    vec2 operator-(vec2 r) const noexcept
    {
        return vec2{DirectX::XMVectorSubtract(load(), r)};
    }
    vec2 operator*(float s) const noexcept
    {
        return vec2{DirectX::XMVectorScale(load(), s)};
    }
    vec2 operator/(float s) const noexcept
    {
        return vec2{DirectX::XMVectorScale(load(), 1.0f / s)};
    }
    vec2 operator-() const noexcept
    {
        return vec2{DirectX::XMVectorNegate(load())};
    }

    [[nodiscard]] float dot(vec2 r) const noexcept
    {
        return DirectX::XMVectorGetX(DirectX::XMVector2Dot(load(), r));
    }
    [[nodiscard]] float length() const noexcept
    {
        return DirectX::XMVectorGetX(DirectX::XMVector2Length(load()));
    }
    [[nodiscard]] float length_sq() const noexcept
    {
        return DirectX::XMVectorGetX(DirectX::XMVector2LengthSq(load()));
    }
    void normalize() noexcept
    {
        store(DirectX::XMVector2Normalize(load()));
    }
    [[nodiscard]] vec2 normalized() const noexcept
    {
        return vec2{DirectX::XMVector2Normalize(load())};
    }
    [[nodiscard]] float distance(vec2 r) const noexcept
    {
        return (*this - r).length();
    }
    [[nodiscard]] float distance_sq(vec2 r) const noexcept
    {
        return (*this - r).length_sq();
    }

    static vec2 lerp(vec2 a, vec2 b, float t) noexcept
    {
        return vec2{DirectX::XMVectorLerp(a, b, t)};
    }

    static vec2 zero()    noexcept { return {0, 0}; }
    static vec2 one()     noexcept { return {1, 1}; }
    static vec2 up()      noexcept { return {0, 1}; }
    static vec2 down()    noexcept { return {0, -1}; }
    static vec2 right()   noexcept { return {1, 0}; }
    static vec2 left()    noexcept { return {-1, 0}; }
};

} // namespace math
