#pragma once

#include "game/navigation/grid/NavigationTypes.h"
#include "game/terrain/TerrainLogic.h"

#include <cstdint>
#include <limits>

namespace engine::navigation
{

// Terrain reserves zero for the heightfield while Navigation reserves zero
// as an invalid sentinel. Keep the translation centralized instead of
// leaking either subsystem's numeric convention across their boundary.
inline constexpr NavigationLayerId kGroundNavigationLayer{1};

[[nodiscard]] constexpr bool tryNavigationLayerFromTerrainPathfindLayer(
    game::terrain::TerrainPathfindLayerId terrainLayer,
    NavigationLayerId& navigationLayer) noexcept
{
    if (terrainLayer == std::numeric_limits<game::terrain::TerrainPathfindLayerId>::max())
        return false;
    navigationLayer = NavigationLayerId{static_cast<uint32_t>(terrainLayer + 1U)};
    return true;
}

[[nodiscard]] constexpr bool tryTerrainPathfindLayerFromNavigationLayer(
    NavigationLayerId navigationLayer,
    game::terrain::TerrainPathfindLayerId& terrainLayer) noexcept
{
    if (!navigationLayer)
        return false;
    terrainLayer = static_cast<game::terrain::TerrainPathfindLayerId>(navigationLayer.value - 1U);
    return true;
}

static_assert([] {
    NavigationLayerId navigationLayer;
    return tryNavigationLayerFromTerrainPathfindLayer(
               game::terrain::kGroundPathfindLayer, navigationLayer) &&
           navigationLayer == kGroundNavigationLayer;
}());

} // namespace engine::navigation
