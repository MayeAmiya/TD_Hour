#pragma once

#include "float2.h"
#include <algorithm>

namespace math {

class rect
{
public:
    float left   = 0;
    float top    = 0;
    float right  = 0;
    float bottom = 0;

    rect() noexcept = default;
    rect(float l, float t, float r, float b) noexcept
        : left(l), top(t), right(r), bottom(b)
    {
    }
    rect(vec2 top_left, vec2 bottom_right) noexcept
        : left(top_left.x()), top(top_left.y())
        , right(bottom_right.x()), bottom(bottom_right.y())
    {
    }

    float width()  const noexcept { return right - left; }
    float height() const noexcept { return bottom - top; }

    vec2 center()    const noexcept { return {(left + right) * 0.5f, (top + bottom) * 0.5f}; }
    vec2 extent()    const noexcept { return {(right - left) * 0.5f, (bottom - top) * 0.5f}; }
    vec2 upper_left()  const noexcept { return {left, top}; }
    vec2 lower_right() const noexcept { return {right, bottom}; }
    vec2 upper_right() const noexcept { return {right, top}; }
    vec2 lower_left()  const noexcept { return {left, bottom}; }

    rect& scale(float k) noexcept
    {
        left   *= k;
        top    *= k;
        right  *= k;
        bottom *= k;
        return *this;
    }
    rect& scale_relative_center(float k) noexcept
    {
        vec2 c = center();
        *this -= vec2{c.x(), c.y()};
        scale(k);
        *this += vec2{c.x(), c.y()};
        return *this;
    }
    rect& operator*=(float k) noexcept { return scale(k); }
    rect& operator/=(float k) noexcept { return scale(1.0f / k); }

    rect& operator+=(vec2 o) noexcept
    {
        left   += o.x();
        top    += o.y();
        right  += o.x();
        bottom += o.y();
        return *this;
    }
    rect& operator-=(vec2 o) noexcept
    {
        left   -= o.x();
        top    -= o.y();
        right  -= o.x();
        bottom -= o.y();
        return *this;
    }

    void inflate(vec2 o) noexcept
    {
        left   -= o.x();
        top    -= o.y();
        right  += o.x();
        bottom += o.y();
    }

    rect& operator+=(const rect& r) noexcept
    {
        left   = std::min(left,   r.left);
        top    = std::min(top,    r.top);
        right  = std::max(right,  r.right);
        bottom = std::max(bottom, r.bottom);
        return *this;
    }

    bool operator==(const rect& r) const noexcept
    {
        return left == r.left && top == r.top && right == r.right && bottom == r.bottom;
    }
    bool operator!=(const rect& r) const noexcept
    {
        return !(*this == r);
    }

    bool contains(vec2 pos) const noexcept
    {
        return pos.x() >= left && pos.x() <= right
            && pos.y() >= top  && pos.y() <= bottom;
    }

    void snap_to_units(vec2 u) noexcept
    {
        left   = static_cast<int>(left   / u.x() + 0.5f) * u.x();
        top    = static_cast<int>(top    / u.y() + 0.5f) * u.y();
        right  = static_cast<int>(right  / u.x() + 0.5f) * u.x();
        bottom = static_cast<int>(bottom / u.y() + 0.5f) * u.y();
    }
};

} // namespace math
