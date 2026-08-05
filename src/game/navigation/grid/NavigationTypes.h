#pragma once

#include "core/container/container_types.h"

#include <compare>
#include <cstdint>
#include <limits>

namespace engine::navigation
{

struct NavigationCellId final
{
    uint32_t value = std::numeric_limits<uint32_t>::max();
    [[nodiscard]] constexpr bool isValid() const noexcept { return value != std::numeric_limits<uint32_t>::max(); }
    explicit constexpr operator bool() const noexcept { return isValid(); }
    constexpr auto operator<=>(const NavigationCellId&) const noexcept = default;
};

struct NavigationLayerId final
{
    uint32_t value = 0;
    [[nodiscard]] constexpr bool isValid() const noexcept { return value != 0; }
    explicit constexpr operator bool() const noexcept { return isValid(); }
    constexpr auto operator<=>(const NavigationLayerId&) const noexcept = default;
};

struct NavigationPortalId final
{
    uint32_t value = 0;
    [[nodiscard]] constexpr bool isValid() const noexcept { return value != 0; }
    explicit constexpr operator bool() const noexcept { return isValid(); }
    constexpr auto operator<=>(const NavigationPortalId&) const noexcept = default;
};

struct NavigationProfileId final
{
    uint32_t value = 0;
    [[nodiscard]] constexpr bool isValid() const noexcept { return value != 0; }
    explicit constexpr operator bool() const noexcept { return isValid(); }
    constexpr auto operator<=>(const NavigationProfileId&) const noexcept = default;
};

struct NavigationRevision final
{
    uint64_t value = 0;
    [[nodiscard]] constexpr bool isValid() const noexcept { return value != 0; }
    explicit constexpr operator bool() const noexcept { return isValid(); }
    constexpr auto operator<=>(const NavigationRevision&) const noexcept = default;
};

inline constexpr NavigationCellId InvalidNavigationCell{};
inline constexpr NavigationLayerId InvalidNavigationLayer{};
inline constexpr NavigationPortalId InvalidNavigationPortal{};
inline constexpr NavigationProfileId InvalidNavigationProfile{};
inline constexpr NavigationRevision InvalidNavigationRevision{};

// Exact bounded output of ZH Pathfinder::getRadiusAndCenter().  Even values
// are centered on a pathfind cell; odd values use the legacy grid-line phase
// and therefore cover an even number of cells.  Keep the legacy aliases for
// callers which only need the old coarse names, but navigation requests and
// zone keys always retain one of the five exact values.
enum class NavigationClearanceClass : uint8_t
{
    Centered1x1 = 0,
    Offset2x2 = 1,
    Centered3x3 = 2,
    Offset4x4 = 3,
    Centered5x5 = 4,

    Infantry = Centered1x1,
    Vehicle = Centered3x3,
    Large = Centered5x5,
};

using NavigationClearanceMask = uint8_t;

namespace NavigationClearance
{
inline constexpr NavigationClearanceMask Centered1x1 = 1U << 0U;
inline constexpr NavigationClearanceMask Offset2x2 = 1U << 1U;
inline constexpr NavigationClearanceMask Centered3x3 = 1U << 2U;
inline constexpr NavigationClearanceMask Offset4x4 = 1U << 3U;
inline constexpr NavigationClearanceMask Centered5x5 = 1U << 4U;
inline constexpr NavigationClearanceMask Infantry = Centered1x1;
inline constexpr NavigationClearanceMask Vehicle = Centered3x3;
inline constexpr NavigationClearanceMask Large = Centered5x5;
inline constexpr NavigationClearanceMask All = Centered1x1 | Offset2x2 |
    Centered3x3 | Offset4x4 | Centered5x5;
inline constexpr uint32_t MaximumRadiusCells = 2;
} // namespace NavigationClearance

inline constexpr container::Array<NavigationClearanceClass, 5>
    NavigationClearanceProfiles = {
        NavigationClearanceClass::Centered1x1,
        NavigationClearanceClass::Offset2x2,
        NavigationClearanceClass::Centered3x3,
        NavigationClearanceClass::Offset4x4,
        NavigationClearanceClass::Centered5x5,
    };

[[nodiscard]] constexpr bool validClearanceClass(NavigationClearanceClass value) noexcept
{
    return static_cast<uint8_t>(value) <=
           static_cast<uint8_t>(NavigationClearanceClass::Centered5x5);
}

[[nodiscard]] constexpr NavigationClearanceMask clearanceBit(
    NavigationClearanceClass value) noexcept
{
    return static_cast<NavigationClearanceMask>(1U << static_cast<uint8_t>(value));
}

[[nodiscard]] constexpr uint32_t clearanceRadiusCells(
    NavigationClearanceClass value) noexcept
{
    return (static_cast<uint32_t>(value) + 1U) / 2U;
}

[[nodiscard]] constexpr bool clearanceCenterInCell(
    NavigationClearanceClass value) noexcept
{
    return validClearanceClass(value) &&
           (static_cast<uint8_t>(value) & 1U) == 0;
}

[[nodiscard]] constexpr NavigationClearanceClass clearanceClassForGeometry(
    uint32_t radiusCells,
    bool centerInCell) noexcept
{
    if (radiusCells == 0)
        return NavigationClearanceClass::Centered1x1;
    if (radiusCells == 1)
        return centerInCell ? NavigationClearanceClass::Centered3x3
                            : NavigationClearanceClass::Offset2x2;
    return centerInCell ? NavigationClearanceClass::Centered5x5
                        : NavigationClearanceClass::Offset4x4;
}

// Fixed-only equivalent of ZH Pathfinder::getRadiusAndCenter.  It preserves
// both radius and phase: rounded diameters 1/2/3/4/5 map to centered 1x1,
// offset 2x2, centered 3x3, offset 4x4 and centered 5x5 respectively.
[[nodiscard]] constexpr NavigationClearanceClass clearanceClassForRadiusRaw(
    int64_t radiusRaw,
    int64_t cellSizeRaw) noexcept
{
    if (radiusRaw <= 0 || cellSizeRaw <= 0)
        return NavigationClearanceClass::Centered1x1;
    if (radiusRaw > std::numeric_limits<int64_t>::max() / 2)
        return NavigationClearanceClass::Centered5x5;

    int64_t diameterRaw = radiusRaw * 2;
    if (diameterRaw > cellSizeRaw &&
        (cellSizeRaw > std::numeric_limits<int64_t>::max() / 2 ||
         diameterRaw < cellSizeRaw * 2))
        diameterRaw = cellSizeRaw * 2;

    const int64_t wholeCells = diameterRaw / cellSizeRaw;
    const int64_t remainder = diameterRaw % cellSizeRaw;
    // ceil(0.7 * cellSize), without an overflowing multiplication.
    const int64_t threshold = (cellSizeRaw / 10) * 7 +
        ((cellSizeRaw % 10) * 7 + 9) / 10;
    const int64_t roundedDiameter = wholeCells +
        (remainder >= threshold ? 1 : 0);
    if (roundedDiameter <= 1)
        return NavigationClearanceClass::Centered1x1;
    if (roundedDiameter == 2)
        return NavigationClearanceClass::Offset2x2;
    if (roundedDiameter == 3)
        return NavigationClearanceClass::Centered3x3;
    if (roundedDiameter == 4)
        return NavigationClearanceClass::Offset4x4;
    return NavigationClearanceClass::Centered5x5;
}

// Navigation admission and output use signed Q32.32 raw world coordinates.
// Search itself operates only on integer cells and costs.
struct NavigationWorldPosition final
{
    int64_t xRaw = 0;
    int64_t yRaw = 0;
    int64_t zRaw = 0;
    constexpr bool operator==(const NavigationWorldPosition&) const noexcept = default;
};

struct NavigationGridCoordinate final
{
    int32_t x = 0;
    int32_t y = 0;
    constexpr auto operator<=>(const NavigationGridCoordinate&) const noexcept = default;
};

enum class NavigationDirection8 : uint8_t
{
    East = 0,
    North = 1,
    West = 2,
    South = 3,
    NorthEast = 4,
    NorthWest = 5,
    SouthWest = 6,
    SouthEast = 7,
};

struct NavigationDirectionDelta final
{
    int8_t x = 0;
    int8_t y = 0;
};

inline constexpr container::Array<NavigationDirection8, 8> NavigationDirectionOrder = {
    NavigationDirection8::East,
    NavigationDirection8::North,
    NavigationDirection8::West,
    NavigationDirection8::South,
    NavigationDirection8::NorthEast,
    NavigationDirection8::NorthWest,
    NavigationDirection8::SouthWest,
    NavigationDirection8::SouthEast,
};

inline constexpr container::Array<NavigationDirectionDelta, 8> NavigationDirectionDeltas = {{
    {1, 0},
    {0, 1},
    {-1, 0},
    {0, -1},
    {1, 1},
    {-1, 1},
    {-1, -1},
    {1, -1},
}};

[[nodiscard]] constexpr NavigationDirectionDelta directionDelta(NavigationDirection8 direction) noexcept
{
    return NavigationDirectionDeltas[static_cast<uint8_t>(direction)];
}

[[nodiscard]] constexpr bool isDiagonal(NavigationDirection8 direction) noexcept
{
    const NavigationDirectionDelta delta = directionDelta(direction);
    return delta.x != 0 && delta.y != 0;
}

enum class NavigationPassability : uint8_t
{
    Blocked = 0,
    Traversable = 1,
};

using NavigationMovementMask = uint32_t;

namespace NavigationMovement
{
inline constexpr NavigationMovementMask Ground = 1U << 0U;
inline constexpr NavigationMovementMask Water = 1U << 1U;
inline constexpr NavigationMovementMask Cliff = 1U << 2U;
inline constexpr NavigationMovementMask Air = 1U << 3U;
inline constexpr NavigationMovementMask Rubble = 1U << 4U;
} // namespace NavigationMovement

struct NavigationGridTransform final
{
    int64_t originXRaw = 0;
    int64_t originYRaw = 0;
    int64_t cellSizeRaw = 0;
    constexpr bool operator==(const NavigationGridTransform&) const noexcept = default;
};

namespace detail
{
[[nodiscard]] constexpr bool checkedSubtract(int64_t left, int64_t right, int64_t& result) noexcept
{
    constexpr int64_t Minimum = std::numeric_limits<int64_t>::min();
    constexpr int64_t Maximum = std::numeric_limits<int64_t>::max();
    if ((right > 0 && left < Minimum + right) || (right < 0 && left > Maximum + right))
        return false;
    result = left - right;
    return true;
}

[[nodiscard]] constexpr bool checkedAdd(int64_t left, int64_t right, int64_t& result) noexcept
{
    constexpr int64_t Minimum = std::numeric_limits<int64_t>::min();
    constexpr int64_t Maximum = std::numeric_limits<int64_t>::max();
    if ((right > 0 && left > Maximum - right) || (right < 0 && left < Minimum - right))
        return false;
    result = left + right;
    return true;
}

[[nodiscard]] constexpr bool checkedMultiply(int64_t left, int64_t right, int64_t& result) noexcept
{
    constexpr int64_t Minimum = std::numeric_limits<int64_t>::min();
    constexpr int64_t Maximum = std::numeric_limits<int64_t>::max();
    if (left == 0 || right == 0)
    {
        result = 0;
        return true;
    }
    if ((left == -1 && right == Minimum) || (right == -1 && left == Minimum))
        return false;
    if (left > 0)
    {
        if ((right > 0 && left > Maximum / right) || (right < 0 && right < Minimum / left))
            return false;
    }
    else if ((right > 0 && left < Minimum / right) || (right < 0 && left < Maximum / right))
    {
        return false;
    }
    result = left * right;
    return true;
}
} // namespace detail

// Unlike C++ signed division, this rounds toward negative infinity so cells
// immediately below the origin map to -1 rather than 0.
[[nodiscard]] constexpr bool worldAxisToCell(int64_t worldRaw,
                                             int64_t originRaw,
                                             int64_t cellSizeRaw,
                                             int32_t& coordinate) noexcept
{
    if (cellSizeRaw <= 0)
        return false;
    int64_t relative = 0;
    if (!detail::checkedSubtract(worldRaw, originRaw, relative))
        return false;
    int64_t quotient = relative / cellSizeRaw;
    if (relative % cellSizeRaw < 0)
        --quotient;
    if (quotient < std::numeric_limits<int32_t>::min() || quotient > std::numeric_limits<int32_t>::max())
        return false;
    coordinate = static_cast<int32_t>(quotient);
    return true;
}

[[nodiscard]] constexpr bool cellAxisCenter(int32_t coordinate,
                                            int64_t originRaw,
                                            int64_t cellSizeRaw,
                                            int64_t& centerRaw) noexcept
{
    if (cellSizeRaw <= 0)
        return false;
    int64_t cellOffset = 0;
    int64_t edge = 0;
    return detail::checkedMultiply(static_cast<int64_t>(coordinate), cellSizeRaw, cellOffset) &&
           detail::checkedAdd(originRaw, cellOffset, edge) &&
           detail::checkedAdd(edge, cellSizeRaw / 2, centerRaw);
}

[[nodiscard]] constexpr NavigationCellId cellIdFromCoordinate(NavigationGridCoordinate coordinate,
                                                               uint32_t width,
                                                               uint32_t height) noexcept
{
    if (coordinate.x < 0 || coordinate.y < 0 || static_cast<uint32_t>(coordinate.x) >= width ||
        static_cast<uint32_t>(coordinate.y) >= height)
        return InvalidNavigationCell;
    const uint64_t value = static_cast<uint64_t>(static_cast<uint32_t>(coordinate.y)) * width +
                           static_cast<uint32_t>(coordinate.x);
    if (value >= std::numeric_limits<uint32_t>::max())
        return InvalidNavigationCell;
    return {static_cast<uint32_t>(value)};
}

[[nodiscard]] constexpr NavigationGridCoordinate coordinateFromCellId(NavigationCellId cell,
                                                                       uint32_t width) noexcept
{
    if (!cell || width == 0)
        return {-1, -1};
    return {static_cast<int32_t>(cell.value % width), static_cast<int32_t>(cell.value / width)};
}

} // namespace engine::navigation
