#pragma once

#include <cstdint>
#include <vectorclass.h>

namespace math {

// ── Scalar type ─────────────────────────────────────────────────────────
using game_scalar = float;

// ── 2D / 3D coordinate types ───────────────────────────────────────────
struct game_coord_2d
{
    float x = 0.0f;
    float y = 0.0f;

    game_coord_2d() noexcept = default;
    game_coord_2d(float x_, float y_) noexcept : x(x_), y(y_) {}
};

struct icoord_2d
{
    int x = 0;
    int y = 0;

    icoord_2d() noexcept = default;
    icoord_2d(int x_, int y_) noexcept : x(x_), y(y_) {}
};

struct game_coord_3d
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    game_coord_3d() noexcept = default;
    game_coord_3d(float x_, float y_, float z_) noexcept : x(x_), y(y_), z(z_) {}
};

// ── SIMD batch types (4-wide Vec4f) ────────────────────────────────────
struct game_coord_2d_x4
{
    Vec4f x;
    Vec4f y;
};

struct game_coord_3d_x4
{
    Vec4f x;
    Vec4f y;
    Vec4f z;
};

// ── GameAngle ───────────────────────────────────────────────────────────
class game_angle
{
    int16_t m_raw_ = 0;

public:
    constexpr game_angle() noexcept : m_raw_(0) {}
    constexpr explicit game_angle(int16_t raw) noexcept : m_raw_(raw) {}
    constexpr explicit game_angle(float degrees) noexcept
        : m_raw_(static_cast<int16_t>(degrees * 182.044444f))
    {
    }

    [[nodiscard]] int16_t raw() const noexcept { return m_raw_; }
    [[nodiscard]] float to_degrees() const noexcept { return m_raw_ / 182.044444f; }
    [[nodiscard]] float to_radians() const noexcept
    {
        return to_degrees() * 3.14159265f / 180.0f;
    }

    game_angle operator+(game_angle rhs) const noexcept
    {
        return game_angle(static_cast<int16_t>(m_raw_ + rhs.m_raw_));
    }
    game_angle operator-(game_angle rhs) const noexcept
    {
        return game_angle(static_cast<int16_t>(m_raw_ - rhs.m_raw_));
    }
    game_angle& operator+=(game_angle rhs) noexcept { m_raw_ += rhs.m_raw_; return *this; }
    game_angle& operator-=(game_angle rhs) noexcept { m_raw_ -= rhs.m_raw_; return *this; }
    bool operator==(game_angle rhs) const noexcept { return m_raw_ == rhs.m_raw_; }
    bool operator!=(game_angle rhs) const noexcept { return m_raw_ != rhs.m_raw_; }
    bool operator<(game_angle rhs) const noexcept { return m_raw_ < rhs.m_raw_; }
    bool operator>(game_angle rhs) const noexcept { return m_raw_ > rhs.m_raw_; }
};

} // namespace math
