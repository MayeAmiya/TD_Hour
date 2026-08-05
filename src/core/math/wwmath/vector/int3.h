#pragma once

#include <cstdint>

namespace math {

struct int3
{
    int32_t x{};
    int32_t y{};
    int32_t z{};

    int3() noexcept = default;
    int3(int32_t x, int32_t y, int32_t z) noexcept
        : x(x)
        , y(y)
        , z(z)
    {
    }
};

} // namespace math
