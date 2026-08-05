#pragma once

#include "game/navigation/grid/NavigationDynamicOverlay.h"

#include "core/container/container_types.h"
#include "math/fixed/q32_32_trig.h"

#include <cstddef>
#include <cstdint>
#include <intrin.h>

#if !defined(_MSC_VER) || !defined(_M_X64)
#error NavigationFootprintRasterizer requires the project's MSVC x64 toolchain.
#endif

namespace engine::navigation
{

enum class NavigationFootprintRasterStatus : uint8_t
{
    Success = 0,
    InvalidInput,
    CapacityExceeded,
};

// The shape is part of the confirmed navigation contract.  In particular,
// Fence and Line deliberately remain distinct even though both currently use
// the same deterministic oriented-rectangle intersection: Fence is sourced
// from the ZH FenceWidth/FenceXOffset fields, while Line identifies one
// authored LINEBUILD segment and keeps that provenance available to later
// clearance/obstacle work.
enum class NavigationFootprintKind : uint8_t
{
    Circle = 0,
    OrientedBox,
    Fence,
    Line,
};

struct NavigationFootprint final
{
    NavigationFootprintKind kind = NavigationFootprintKind::Circle;
    NavigationWorldPosition center;
    int64_t halfExtentXRaw = 0;
    int64_t halfExtentYRaw = 0;
    int64_t yawRaw = 0;
};

struct NavigationFootprintRasterResult final
{
    NavigationFootprintRasterStatus status = NavigationFootprintRasterStatus::InvalidInput;
    uint32_t requiredCount = 0;
    uint32_t writtenCount = 0;
    NavigationCellBounds bounds;
};

// Deterministic conservative cell rasterization for ZH building footprints.
// Inputs are signed Q32.32 world values. Candidate cells and output are
// row-major, so the dynamic overlay receives one canonical sorted footprint
// independent of object insertion order. The caller supplies storage; no
// allocation occurs. On CapacityExceeded requiredCount is a deterministic
// lower bound (output capacity + 1), allowing an oversized authored object to
// fault immediately instead of scanning the remainder of a world-sized AABB.
class NavigationFootprintRasterizer final
{
public:
    [[nodiscard]] static NavigationFootprintRasterResult rasterize(
        const NavigationGrid& grid,
        const NavigationFootprint& footprint,
        container::Span<NavigationCellId> output) noexcept
    {
        switch (footprint.kind)
        {
        case NavigationFootprintKind::Circle:
            return circle(grid, footprint.center, footprint.halfExtentXRaw,
                          output);
        case NavigationFootprintKind::OrientedBox:
        case NavigationFootprintKind::Fence:
        case NavigationFootprintKind::Line:
            return orientedBox(grid, footprint, output);
        }
        return {};
    }

    [[nodiscard]] static NavigationFootprintRasterResult circle(
        const NavigationGrid& grid,
        NavigationWorldPosition center,
        int64_t radiusRaw,
        container::Span<NavigationCellId> output) noexcept
    {
        NavigationFootprintRasterResult result;
        if (!grid.isInitialized() || radiusRaw < 0)
            return result;

        int64_t minimumXRaw = 0;
        int64_t minimumYRaw = 0;
        int64_t maximumXRaw = 0;
        int64_t maximumYRaw = 0;
        if (!detail::checkedSubtract(center.xRaw, radiusRaw, minimumXRaw) ||
            !detail::checkedSubtract(center.yRaw, radiusRaw, minimumYRaw) ||
            !detail::checkedAdd(center.xRaw, radiusRaw, maximumXRaw) ||
            !detail::checkedAdd(center.yRaw, radiusRaw, maximumYRaw))
            return result;

        const NavigationGridTransform transform = grid.transform();
        NavigationGridCoordinate minimum;
        NavigationGridCoordinate maximum;
        if (!worldAxisToCell(minimumXRaw, transform.originXRaw, transform.cellSizeRaw, minimum.x) ||
            !worldAxisToCell(minimumYRaw, transform.originYRaw, transform.cellSizeRaw, minimum.y) ||
            !worldAxisToCell(maximumXRaw, transform.originXRaw, transform.cellSizeRaw, maximum.x) ||
            !worldAxisToCell(maximumYRaw, transform.originYRaw, transform.cellSizeRaw, maximum.y))
            return result;

        minimum.x = clampCoordinate(minimum.x, grid.width());
        minimum.y = clampCoordinate(minimum.y, grid.height());
        maximum.x = clampCoordinate(maximum.x, grid.width());
        maximum.y = clampCoordinate(maximum.y, grid.height());
        if (minimum.x > maximum.x || minimum.y > maximum.y)
            return result;

        const UInt128 radiusSquared = square(radiusRaw);
        for (int32_t y = minimum.y; y <= maximum.y; ++y)
        {
            for (int32_t x = minimum.x; x <= maximum.x; ++x)
            {
                int64_t cellMinimumX = 0;
                int64_t cellMinimumY = 0;
                int64_t cellMaximumX = 0;
                int64_t cellMaximumY = 0;
                if (!cellBounds(x, transform.originXRaw, transform.cellSizeRaw,
                                cellMinimumX, cellMaximumX) ||
                    !cellBounds(y, transform.originYRaw, transform.cellSizeRaw,
                                cellMinimumY, cellMaximumY))
                    return {};
                const uint64_t deltaX = distanceToInterval(center.xRaw, cellMinimumX, cellMaximumX);
                const uint64_t deltaY = distanceToInterval(center.yRaw, cellMinimumY, cellMaximumY);
                if (!lessOrEqual(add(square(deltaX), square(deltaY)), radiusSquared))
                    continue;

                const NavigationCellId cell = grid.cellId({x, y});
                if (!cell)
                    return {};
                if (result.writtenCount < output.size())
                    output[result.writtenCount++] = cell;
                ++result.requiredCount;
                result.bounds.include(NavigationGridCoordinate{x, y});
                if (result.requiredCount > output.size())
                {
                    result.status =
                        NavigationFootprintRasterStatus::CapacityExceeded;
                    return result;
                }
            }
        }
        result.status = NavigationFootprintRasterStatus::Success;
        return result;
    }

private:
    [[nodiscard]] static NavigationFootprintRasterResult orientedBox(
        const NavigationGrid& grid,
        const NavigationFootprint& footprint,
        container::Span<NavigationCellId> output) noexcept
    {
        NavigationFootprintRasterResult result;
        if (!grid.isInitialized() || footprint.halfExtentXRaw < 0 ||
            footprint.halfExtentYRaw < 0)
            return result;

        using Fixed = math::q32_32;
        const Fixed halfX = Fixed::from_raw(footprint.halfExtentXRaw);
        const Fixed halfY = Fixed::from_raw(footprint.halfExtentYRaw);
        const math::q32_32_sincos heading =
            math::fixed_sincos(Fixed::from_raw(footprint.yawRaw));
        const Fixed absoluteCosine = Fixed::abs(heading.cosine);
        const Fixed absoluteSine = Fixed::abs(heading.sine);
        const Fixed extentX = halfX * absoluteCosine +
                              halfY * absoluteSine;
        const Fixed extentY = halfX * absoluteSine +
                              halfY * absoluteCosine;

        int64_t minimumXRaw = 0;
        int64_t minimumYRaw = 0;
        int64_t maximumXRaw = 0;
        int64_t maximumYRaw = 0;
        if (!detail::checkedSubtract(footprint.center.xRaw, extentX.raw(),
                                     minimumXRaw) ||
            !detail::checkedSubtract(footprint.center.yRaw, extentY.raw(),
                                     minimumYRaw) ||
            !detail::checkedAdd(footprint.center.xRaw, extentX.raw(),
                                maximumXRaw) ||
            !detail::checkedAdd(footprint.center.yRaw, extentY.raw(),
                                maximumYRaw))
            return result;

        const NavigationGridTransform transform = grid.transform();
        NavigationGridCoordinate minimum;
        NavigationGridCoordinate maximum;
        if (!worldAxisToCell(minimumXRaw, transform.originXRaw,
                             transform.cellSizeRaw, minimum.x) ||
            !worldAxisToCell(minimumYRaw, transform.originYRaw,
                             transform.cellSizeRaw, minimum.y) ||
            !worldAxisToCell(maximumXRaw, transform.originXRaw,
                             transform.cellSizeRaw, maximum.x) ||
            !worldAxisToCell(maximumYRaw, transform.originYRaw,
                             transform.cellSizeRaw, maximum.y))
            return result;

        minimum.x = clampCoordinate(minimum.x, grid.width());
        minimum.y = clampCoordinate(minimum.y, grid.height());
        maximum.x = clampCoordinate(maximum.x, grid.width());
        maximum.y = clampCoordinate(maximum.y, grid.height());
        if (minimum.x > maximum.x || minimum.y > maximum.y)
            return result;

        const Fixed cellHalf =
            Fixed::from_raw(transform.cellSizeRaw) /
            Fixed{int32_t{2}};
        const Fixed projectedCellHalf =
            cellHalf * (absoluteCosine + absoluteSine);
        for (int32_t y = minimum.y; y <= maximum.y; ++y)
        {
            for (int32_t x = minimum.x; x <= maximum.x; ++x)
            {
                int64_t cellMinimumX = 0;
                int64_t cellMinimumY = 0;
                int64_t cellMaximumX = 0;
                int64_t cellMaximumY = 0;
                if (!cellBounds(x, transform.originXRaw,
                                transform.cellSizeRaw, cellMinimumX,
                                cellMaximumX) ||
                    !cellBounds(y, transform.originYRaw,
                                transform.cellSizeRaw, cellMinimumY,
                                cellMaximumY))
                    return {};
                const Fixed cellCenterX = Fixed::from_raw(cellMinimumX) +
                                          cellHalf;
                const Fixed cellCenterY = Fixed::from_raw(cellMinimumY) +
                                          cellHalf;
                const Fixed deltaX = cellCenterX -
                    Fixed::from_raw(footprint.center.xRaw);
                const Fixed deltaY = cellCenterY -
                    Fixed::from_raw(footprint.center.yRaw);

                // Complete SAT for an authored OBB against an axis-aligned
                // navigation cell. Touching an edge counts as blocked.
                if (Fixed::abs(deltaX) > cellHalf + extentX ||
                    Fixed::abs(deltaY) > cellHalf + extentY)
                    continue;
                const Fixed along = deltaX * heading.cosine +
                                    deltaY * heading.sine;
                const Fixed across = -deltaX * heading.sine +
                                     deltaY * heading.cosine;
                if (Fixed::abs(along) > halfX + projectedCellHalf ||
                    Fixed::abs(across) > halfY + projectedCellHalf)
                    continue;

                const NavigationCellId cell = grid.cellId({x, y});
                if (!cell)
                    return {};
                if (result.writtenCount < output.size())
                    output[result.writtenCount++] = cell;
                ++result.requiredCount;
                result.bounds.include(NavigationGridCoordinate{x, y});
                if (result.requiredCount > output.size())
                {
                    result.status =
                        NavigationFootprintRasterStatus::CapacityExceeded;
                    return result;
                }
            }
        }
        result.status = NavigationFootprintRasterStatus::Success;
        return result;
    }

    struct UInt128 final
    {
        uint64_t low = 0;
        uint64_t high = 0;
    };

    [[nodiscard]] static int32_t clampCoordinate(int32_t value, uint32_t extent) noexcept
    {
        if (value < 0)
            return 0;
        const int32_t maximum = static_cast<int32_t>(extent - 1U);
        return value > maximum ? maximum : value;
    }

    [[nodiscard]] static bool cellBounds(int32_t coordinate,
                                         int64_t originRaw,
                                         int64_t cellSizeRaw,
                                         int64_t& minimumRaw,
                                         int64_t& maximumRaw) noexcept
    {
        int64_t offset = 0;
        return detail::checkedMultiply(static_cast<int64_t>(coordinate), cellSizeRaw, offset) &&
               detail::checkedAdd(originRaw, offset, minimumRaw) &&
               detail::checkedAdd(minimumRaw, cellSizeRaw, maximumRaw);
    }

    [[nodiscard]] static uint64_t magnitude(int64_t value) noexcept
    {
        const uint64_t bits = static_cast<uint64_t>(value);
        return value < 0 ? (~bits + 1U) : bits;
    }

    [[nodiscard]] static uint64_t distanceToInterval(int64_t value,
                                                     int64_t minimum,
                                                     int64_t maximum) noexcept
    {
        if (value < minimum)
            return static_cast<uint64_t>(minimum) - static_cast<uint64_t>(value);
        if (value > maximum)
            return static_cast<uint64_t>(value) - static_cast<uint64_t>(maximum);
        return 0;
    }

    [[nodiscard]] static UInt128 square(int64_t value) noexcept
    {
        return square(magnitude(value));
    }

    [[nodiscard]] static UInt128 square(uint64_t value) noexcept
    {
        UInt128 result;
        result.low = _umul128(value, value, &result.high);
        return result;
    }

    [[nodiscard]] static UInt128 add(UInt128 left, UInt128 right) noexcept
    {
        UInt128 result;
        result.low = left.low + right.low;
        result.high = left.high + right.high + static_cast<uint64_t>(result.low < left.low);
        return result;
    }

    [[nodiscard]] static bool lessOrEqual(UInt128 left, UInt128 right) noexcept
    {
        return left.high < right.high || (left.high == right.high && left.low <= right.low);
    }
};

} // namespace engine::navigation
