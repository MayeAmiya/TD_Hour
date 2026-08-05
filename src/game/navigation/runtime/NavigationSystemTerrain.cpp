#include <algorithm>
#include <cmath>
#include <limits>

#include "NavigationSystem.h"
#include "game/terrain/TerrainLogic.h"

namespace engine::navigation
{
namespace
{

[[nodiscard]] bool fixedRaw(float value, int64_t& output) noexcept
{
    if (!std::isfinite(value)) return false;
    constexpr long double Scale = static_cast<long double>(uint64_t{1} << 32U);
    const long double scaled = static_cast<long double>(value) * Scale;
    if (scaled < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
        scaled > static_cast<long double>(std::numeric_limits<int64_t>::max()))
        return false;
    output = static_cast<int64_t>(scaled);
    return true;
}

} // namespace

NavigationSystemStatus NavigationSystem::synchronizeTerrainHeight(
    const game::terrain::TerrainLogic& terrain) noexcept
{
    if (!m_initialized) return NavigationSystemStatus::NotInitialized;
    if (m_topologyPublicationFailed || !terrain.isLoaded())
        return NavigationSystemStatus::StaticChangeFailed;

    const game::terrain::TerrainMap& map = terrain.map();
    const uint64_t revision = map.revision();
    if (revision == m_terrainHeightRevision)
        return NavigationSystemStatus::Success;

    // A dirty publication owns a private grid/zone snapshot. Waiting until
    // that snapshot is published prevents a later static edit from being
    // overwritten by the pending copy.
    if (m_dirtyPublicationActive)
        return NavigationSystemStatus::PublicationPending;

    const NavigationGrid* staticGrid = m_staticLayers.find(m_primaryLayer);
    if (staticGrid == nullptr || staticGrid->width() !=
            static_cast<uint32_t>(std::max(0, map.width() - 1)) ||
        staticGrid->height() !=
            static_cast<uint32_t>(std::max(0, map.height() - 1)))
        return NavigationSystemStatus::StaticChangeFailed;

    NavigationCellBounds affected;
    const auto& history = map.dirtyHistory();
    bool fullRebuild = m_terrainHeightRevision > revision || history.empty();
    if (!fullRebuild && m_terrainHeightRevision == 0)
        fullRebuild = true;

    if (!fullRebuild)
    {
        uint64_t expectedRevision = m_terrainHeightRevision + 1U;
        bool foundRevision = false;
        for (const game::terrain::TerrainDirtyRevision& entry : history)
        {
            if (entry.revision <= m_terrainHeightRevision) continue;
            if (entry.revision != expectedRevision)
            {
                fullRebuild = true;
                break;
            }
            foundRevision = true;
            const int64_t minimumX = std::max<int64_t>(
                0, static_cast<int64_t>(entry.region.minX) - 1);
            const int64_t minimumY = std::max<int64_t>(
                0, static_cast<int64_t>(entry.region.minY) - 1);
            const int64_t maximumX = std::min<int64_t>(
                staticGrid->width() - 1, entry.region.maxX);
            const int64_t maximumY = std::min<int64_t>(
                staticGrid->height() - 1, entry.region.maxY);
            if (minimumX <= maximumX && minimumY <= maximumY)
                affected.include(NavigationGridCoordinate{
                    static_cast<int32_t>(minimumX),
                    static_cast<int32_t>(minimumY)});
            if (minimumX <= maximumX && minimumY <= maximumY)
                affected.include(NavigationGridCoordinate{
                    static_cast<int32_t>(maximumX),
                    static_cast<int32_t>(maximumY)});
            if (expectedRevision == std::numeric_limits<uint64_t>::max())
            {
                fullRebuild = true;
                break;
            }
            ++expectedRevision;
        }
        if (!foundRevision || expectedRevision != revision + 1U)
            fullRebuild = true;
    }

    if (fullRebuild)
    {
        affected = {0, 0,
                    static_cast<int32_t>(staticGrid->width()) - 1,
                    static_cast<int32_t>(staticGrid->height()) - 1};
    }
    if (!affected.valid())
    {
        // Height mutations outside the navigation projection still advance
        // the terrain journal, but do not require a topology publication.
        m_terrainHeightRevision = revision;
        return NavigationSystemStatus::Success;
    }

    const uint64_t updateCount = affected.cellCount();
    if (updateCount > std::numeric_limits<size_t>::max())
        return NavigationSystemStatus::StaticChangeFailed;
    m_terrainHeightUpdateScratch.clear();
    m_terrainHeightUpdateScratch.reserve(static_cast<size_t>(updateCount));

    for (int32_t y = affected.minY; y <= affected.maxY; ++y)
    {
        for (int32_t x = affected.minX; x <= affected.maxX; ++x)
        {
            const NavigationCellId cell = staticGrid->cellId({x, y});
            if (!cell)
                return NavigationSystemStatus::StaticChangeFailed;
            const NavigationCellValue original = staticGrid->cell(cell);
            NavigationCellValue value = original;
            value.heightRaw = terrain.map().navigationCellCenterHeightRaw(
                x, y);

            // Keep authored air/water/other flags, but refresh the ground
            // surface classification when a non-authored height mutation
            // changes the cell slope. Dynamic blockers are not stored in this
            // base grid and are therefore unaffected by this write.
            // Water cells belong to the water raster, which publishes them as
            // exactly Water|Air.  Re-deriving a ground surface here would OR
            // Ground back into a submerged cell, and after any runtime height
            // mutation whose dirty region overlaps a lake (crater, terraform)
            // ground units would path straight across the water until the next
            // water-level change happened to re-raster those cells.
            if ((value.movementMask & NavigationMovement::Water) == 0) {
                const bool cliff = terrain.map().navigationCellIsCliff(x, y);
                value.movementMask &=
                    ~(NavigationMovement::Ground | NavigationMovement::Cliff);
                value.movementMask |= cliff ? NavigationMovement::Cliff
                                            : NavigationMovement::Ground;
            }
            // A map-import height batch deliberately retains one broad dirty
            // rectangle so renderer consumers that skip revisions remain
            // safe. Most cells inside that rectangle are nevertheless
            // unchanged. Do not turn those untouched cells into static
            // topology writes: besides needless copies, debug iterator and
            // validation costs made the first confirmed frame scale with the
            // complete map instead of the actual flattened footprints.
            if (value == original) continue;
            m_terrainHeightUpdateScratch.push_back({cell, value});
        }
    }

    const NavigationSystemStatus staged =
        stageStaticCells(m_terrainHeightUpdateScratch);
    if (staged != NavigationSystemStatus::Success)
        return staged;
    m_terrainHeightRevision = revision;
    return NavigationSystemStatus::Success;
}

} // namespace engine::navigation
