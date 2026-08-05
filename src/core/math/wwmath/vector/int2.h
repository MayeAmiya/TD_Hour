#pragma once

#include <cstdint>

namespace math {

struct int2
{
    int32_t x{};
    int32_t y{};

    int2() noexcept = default;
    int2(int32_t x, int32_t y) noexcept
        : x(x)
        , y(y)
    {
    }
};

} // namespace math
