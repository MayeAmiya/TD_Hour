#include "NavigationSystem.h"

#include "game/terrain/TerrainLogic.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::navigation
{
namespace
{

[[nodiscard]] bool integerWorldRaw(int32_t value, int64_t& output) noexcept
{
    constexpr int64_t Scale = int64_t{1} << 32U;
    output = static_cast<int64_t>(value) * Scale;
    return true;
}

[[nodiscard]] bool checkedCellAxis(int64_t origin,
                                   int64_t cellSize,
                                   int32_t coordinate,
                                   int64_t& output) noexcept
{
    const long double value = static_cast<long double>(origin) +
        static_cast<long double>(cellSize) * coordinate;
    if (value < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
        value > static_cast<long double>(std::numeric_limits<int64_t>::max()))
        return false;
    output = static_cast<int64_t>(value);
    return true;
}

[[nodiscard]] bool waterAtCellCorners(
    const game::terrain::TerrainLogic& terrain,
    const NavigationGrid& grid,
    NavigationGridCoordinate coordinate,
    bool& water) noexcept
{
    const NavigationGridTransform transform = grid.transform();
    int64_t left = 0;
    int64_t top = 0;
    int64_t right = 0;
    int64_t bottom = 0;
    if (!checkedCellAxis(transform.originXRaw, transform.cellSizeRaw,
                         coordinate.x, left) ||
        !checkedCellAxis(transform.originYRaw, transform.cellSizeRaw,
                         coordinate.y, top) ||
        !checkedCellAxis(transform.originXRaw, transform.cellSizeRaw,
                         coordinate.x + 1, right) ||
        !checkedCellAxis(transform.originYRaw, transform.cellSizeRaw,
                         coordinate.y + 1, bottom))
        return false;

    water = terrain.cellTouchesUnderwaterLegacyRaw(
        left, top, right, bottom);
    return true;
}

[[nodiscard]] bool affectedBounds(
    const game::terrain::TerrainWaterArea& area,
    const NavigationGrid& grid,
    NavigationCellBounds& output) noexcept
{
    if (area.polygon.empty())
        return false;
    int32_t minimumX = area.polygon.front().x;
    int32_t maximumX = minimumX;
    int32_t minimumY = area.polygon.front().y;
    int32_t maximumY = minimumY;
    for (const math::int3& point : area.polygon)
    {
        minimumX = std::min(minimumX, point.x);
        maximumX = std::max(maximumX, point.x);
        minimumY = std::min(minimumY, point.y);
        maximumY = std::max(maximumY, point.y);
    }

    int64_t minimumXRaw = 0;
    int64_t maximumXRaw = 0;
    int64_t minimumYRaw = 0;
    int64_t maximumYRaw = 0;
    if (!integerWorldRaw(minimumX, minimumXRaw) ||
        !integerWorldRaw(maximumX, maximumXRaw) ||
        !integerWorldRaw(minimumY, minimumYRaw) ||
        !integerWorldRaw(maximumY, maximumYRaw))
        return false;

    NavigationGridCoordinate minimum;
    NavigationGridCoordinate maximum;
    const NavigationGridTransform transform = grid.transform();
    if (!worldAxisToCell(minimumXRaw, transform.originXRaw,
                         transform.cellSizeRaw, minimum.x) ||
        !worldAxisToCell(minimumYRaw, transform.originYRaw,
                         transform.cellSizeRaw, minimum.y) ||
        !worldAxisToCell(maximumXRaw, transform.originXRaw,
                         transform.cellSizeRaw, maximum.x) ||
        !worldAxisToCell(maximumYRaw, transform.originYRaw,
                         transform.cellSizeRaw, maximum.y))
        return false;

    // ZH samples all four cell corners. Include the cell immediately before
    // each minimum edge because its far corner can lie on the water polygon.
    minimum.x = minimum.x > 0 ? minimum.x - 1 : 0;
    minimum.y = minimum.y > 0 ? minimum.y - 1 : 0;
    maximum.x = std::min<int32_t>(static_cast<int32_t>(grid.width()) - 1,
                                  maximum.x);
    maximum.y = std::min<int32_t>(static_cast<int32_t>(grid.height()) - 1,
                                  maximum.y);
    if (minimum.x > maximum.x || minimum.y > maximum.y)
    {
        output = {};
        return true;
    }
    output = {minimum.x, minimum.y, maximum.x, maximum.y};
    return true;
}

} // namespace

NavigationSystemStatus NavigationSystem::synchronizeWaterRaster(
    const game::terrain::TerrainLogic& terrain) noexcept
{
    if (!m_initialized)
        return NavigationSystemStatus::NotInitialized;
    if (m_topologyPublicationFailed || !terrain.isLoaded())
        return NavigationSystemStatus::StaticChangeFailed;
    if (m_waterRasterInitialized &&
        m_waterRasterSourceRevision == terrain.pathfindWaterRevision())
        return NavigationSystemStatus::Success;

    NavigationGrid* staticGrid = m_staticLayers.findMutable(m_primaryLayer);
    if (staticGrid == nullptr)
    {
        m_topologyPublicationFailed = true;
        return NavigationSystemStatus::StaticChangeFailed;
    }
    const auto& waterAreas = terrain.waterAreas();
    if (m_waterRasterInitialized &&
        (waterAreas.size() != m_waterRasterAreas.size() ||
         m_waterRasterLandMasks.size() != staticGrid->cellCount()))
    {
        m_topologyPublicationFailed = true;
        return NavigationSystemStatus::StaticChangeFailed;
    }

    NavigationCellBounds scanBounds;
    if (!m_waterRasterInitialized)
    {
        m_waterRasterAreas.clear();
        m_waterRasterAreas.reserve(waterAreas.size());
        for (const game::terrain::TerrainWaterArea& area : waterAreas)
        {
            if (area.polygon.empty())
            {
                m_topologyPublicationFailed = true;
                return NavigationSystemStatus::StaticChangeFailed;
            }
            m_waterRasterAreas.push_back({area.triggerId,
                                          area.surfaceHeightRaw});
        }
        m_waterRasterLandMasks.assign(staticGrid->movementMask().begin(),
                                      staticGrid->movementMask().end());
        m_waterRasterUpdateScratch.clear();
        m_waterRasterUpdateScratch.reserve(staticGrid->cellCount());
        scanBounds = {0, 0, static_cast<int32_t>(staticGrid->width()) - 1,
                      static_cast<int32_t>(staticGrid->height()) - 1};
    }
    else
    {
        for (size_t index = 0; index < waterAreas.size(); ++index)
        {
            if (waterAreas[index].triggerId !=
                    m_waterRasterAreas[index].triggerId)
            {
                m_topologyPublicationFailed = true;
                return NavigationSystemStatus::StaticChangeFailed;
            }
            if (waterAreas[index].surfaceHeightRaw ==
                m_waterRasterAreas[index].surfaceHeightRaw)
                continue;
            NavigationCellBounds areaBounds;
            if (!affectedBounds(waterAreas[index], *staticGrid, areaBounds))
            {
                m_topologyPublicationFailed = true;
                return NavigationSystemStatus::StaticChangeFailed;
            }
            scanBounds.include(areaBounds);
        }
    }

    m_waterRasterUpdateScratch.clear();
    NavigationCellBounds changedBounds;
    if (scanBounds.valid())
    {
        for (int32_t y = scanBounds.minY; y <= scanBounds.maxY; ++y)
        {
            for (int32_t x = scanBounds.minX; x <= scanBounds.maxX; ++x)
            {
                const NavigationCellId cell = staticGrid->cellId({x, y});
                bool water = false;
                if (!cell || !waterAtCellCorners(terrain, *staticGrid,
                                                  {x, y}, water))
                {
                    m_topologyPublicationFailed = true;
                    return NavigationSystemStatus::StaticChangeFailed;
                }
                NavigationCellValue value = staticGrid->cell(cell);
                const NavigationMovementMask desired = water
                    ? NavigationMovement::Water | NavigationMovement::Air
                    : m_waterRasterLandMasks[cell.value];
                if (value.movementMask == desired)
                    continue;
                value.movementMask = desired;
                m_waterRasterUpdateScratch.push_back({cell, value});
                changedBounds.include(cell, staticGrid->width());
            }
        }
    }

    const NavigationSystemStatus staged =
        stageStaticCells(m_waterRasterUpdateScratch);
    if (staged == NavigationSystemStatus::PublicationPending)
        return staged;
    if (staged != NavigationSystemStatus::Success)
    {
        m_topologyPublicationFailed = true;
        return staged;
    }
    for (size_t index = 0; index < waterAreas.size(); ++index)
    {
        m_waterRasterAreas[index].surfaceHeightRaw =
            waterAreas[index].surfaceHeightRaw;
    }
    m_waterRasterSourceRevision = terrain.pathfindWaterRevision();
    m_waterRasterPendingDirty.include(changedBounds);
    m_waterRasterInitialized = true;
    return NavigationSystemStatus::Success;
}

} // namespace engine::navigation
