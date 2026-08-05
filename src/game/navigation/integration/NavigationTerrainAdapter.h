#pragma once

#include "NavigationTerrainLayerMapping.h"
#include "game/navigation/runtime/NavigationSystem.h"
#include "game/terrain/MapHeightfieldLoader.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32.h"

#include "core/container/container_types.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace engine::navigation
{

struct NavigationTerrainPolicy final
{
    NavigationLayerId groundLayer{kGroundNavigationLayer};
    NavigationProfileId groundProfile{1};
    uint32_t dynamicEntityCapacity = 0;
    uint32_t bridgeCapacity = 0;
    uint32_t dynamicEventCapacity = 0;
    uint32_t maxCellsPerFootprint = 0;
    uint32_t pathCapacity = 0;
    uint32_t maxPointsPerPath = 0;
    uint32_t requestCapacity = 0;
    uint32_t feedbackCapacity = 0;
    uint32_t layerCapacity = 0;
    uint32_t portalCapacity = 0;
};

enum class NavigationTerrainAdapterStatus : uint8_t
{
    Success = 0,
    InvalidTerrain,
    InvalidPolicy,
    CoordinateOverflow,
    SystemInitializationFailed,
    StaticIngestionFailed,
    StaticPublicationFailed,
    ElevatedLayerIngestionFailed,
    PortalIngestionFailed,
    ElevatedTopologyCapacityExceeded,
};

struct NavigationTerrainAdapterResult final
{
    NavigationTerrainAdapterStatus status = NavigationTerrainAdapterStatus::InvalidTerrain;
    NavigationSystemStatus systemStatus = NavigationSystemStatus::NotInitialized;
    uint32_t cliffCellCount = 0;
    uint32_t elevatedLayerCount = 0;
    uint32_t portalCount = 0;
    uint32_t lowClearanceCellCount = 0;
};

// Startup adapter from the detached integer map source to authoritative fixed
// navigation values. Runtime navigation never retains a TerrainMap or source
// pointer. Authored cliff cells win; older maps fall back to the exact integer
// equivalent of the legacy 9.8-world-unit slope threshold.
class NavigationTerrainAdapter final
{
public:
    [[nodiscard]] static NavigationTerrainAdapterResult initialize(
        NavigationSystem& system,
        const game::terrain::TerrainHeightfieldData& terrain,
        const NavigationTerrainPolicy& policy,
        const game::terrain::TerrainLogic* terrainLogic = nullptr)
    {
        NavigationTerrainAdapterResult result;
        if (!validTerrain(terrain))
            return result;
        if (!validPolicy(policy))
        {
            result.status = NavigationTerrainAdapterStatus::InvalidPolicy;
            return result;
        }

        constexpr int64_t CellSizeRaw = int64_t{10} << 32;
        if (terrain.borderSize < 0 ||
            static_cast<int64_t>(terrain.borderSize) > std::numeric_limits<int64_t>::max() / CellSizeRaw)
        {
            result.status = NavigationTerrainAdapterStatus::CoordinateOverflow;
            return result;
        }
        const int64_t originRaw = -static_cast<int64_t>(terrain.borderSize) * CellSizeRaw;
        const uint32_t width = static_cast<uint32_t>(terrain.width - 1);
        const uint32_t height = static_cast<uint32_t>(terrain.height - 1);
        const NavigationCellValue clearCell{NavigationPassability::Traversable,
                                            0,
                                            NavigationMovement::Ground | NavigationMovement::Air,
                                            policy.groundLayer,
                                            0,
                                            InvalidNavigationPortal};
        const NavigationSystemConfig config{width,
                                            height,
                                            {originRaw, originRaw, CellSizeRaw},
                                            clearCell,
                                            policy.groundLayer,
                                            policy.groundProfile,
                                            NavigationMovement::Ground,
                                            policy.dynamicEntityCapacity,
                                            policy.bridgeCapacity,
                                            policy.dynamicEventCapacity,
                                            policy.maxCellsPerFootprint,
                                            policy.pathCapacity,
                                            policy.maxPointsPerPath,
                                            policy.requestCapacity,
                                            policy.feedbackCapacity,
                                            policy.layerCapacity,
                                            policy.portalCapacity};
        result.systemStatus = system.initialize(config);
        if (result.systemStatus != NavigationSystemStatus::Success)
        {
            result.status = NavigationTerrainAdapterStatus::SystemInitializationFailed;
            return result;
        }

        const size_t sampleCount = terrain.heights.size();
        const bool hasAuthoredCliffs = terrain.blendTiles &&
            terrain.blendTiles->cliffCells.size() == sampleCount;
        container::Vector<NavigationStaticCellUpdate> staticCells;
        staticCells.reserve(static_cast<size_t>(width) * height);
        const NavigationCellValue cliffCell{NavigationPassability::Traversable,
                                            0,
                                            NavigationMovement::Cliff | NavigationMovement::Air,
                                            policy.groundLayer,
                                            0,
                                            InvalidNavigationPortal};
        uint32_t cliffCellCount = 0;
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                const bool cliff =
                    isCliff(terrain, x, y, hasAuthoredCliffs);
                if (cliff)
                    ++cliffCellCount;
                if (!cliff && !terrainLogic)
                    continue;
                const NavigationCellId cell{y * width + x};
                NavigationCellValue value = cliff ? cliffCell : clearCell;
                if (terrainLogic) {
                    NavigationWorldPosition center;
                    if (!system.staticGrid().cellCenter(cell, center)) {
                        result.status = NavigationTerrainAdapterStatus::
                            CoordinateOverflow;
                        return result;
                    }
                    value.heightRaw = terrainLogic->groundHeightRaw(
                        center.xRaw, center.yRaw);
                }
                staticCells.push_back({cell, value});
            }
        }
        result.cliffCellCount = cliffCellCount;
        result.systemStatus = system.stageStaticCells(staticCells);
        if (result.systemStatus != NavigationSystemStatus::Success)
        {
            result.status = NavigationTerrainAdapterStatus::StaticIngestionFailed;
            return result;
        }
        if (terrainLogic && !ingestElevatedTopology(
                system, *terrainLogic, policy, result)) {
            return result;
        }
        if (terrainLogic) {
            result.systemStatus = system.synchronizeWaterRaster(*terrainLogic);
            if (result.systemStatus != NavigationSystemStatus::Success) {
                result.status =
                    NavigationTerrainAdapterStatus::StaticIngestionFailed;
                return result;
            }
        }
        result.systemStatus = system.publishStagedStaticTopology();
        if (result.systemStatus != NavigationSystemStatus::Success)
        {
            result.status = NavigationTerrainAdapterStatus::StaticPublicationFailed;
            return result;
        }
        if (terrainLogic) {
            // The adapter just populated the complete static height field and
            // published it.  Without this baseline, the first logic tick sees
            // revision zero and repeats the full terrain-height synchronization
            // before any command or script can run.
            system.markTerrainHeightSynchronized(
                terrainLogic->map().revision());
        }
        result.status = NavigationTerrainAdapterStatus::Success;
        return result;
    }

private:
    [[nodiscard]] static bool ingestElevatedTopology(
        NavigationSystem& system,
        const game::terrain::TerrainLogic& terrain,
        const NavigationTerrainPolicy& policy,
        NavigationTerrainAdapterResult& result)
    {
        const NavigationGrid& ground = system.staticGrid();
        if (!ground.isInitialized()) {
            result.status =
                NavigationTerrainAdapterStatus::ElevatedLayerIngestionFailed;
            return false;
        }

        container::Vector<NavigationCellId> lowClearanceCells;
        container::Vector<NavigationCellId> groundEntryCells;
        uint32_t nextPortalValue = 1;
        for (const game::terrain::TerrainElevatedPathfindSurface& surface :
             terrain.elevatedPathfindSurfaces()) {
            if (surface.layer == game::terrain::kGroundPathfindLayer ||
                surface.boundaryRaw.size() < 3) {
                result.status = NavigationTerrainAdapterStatus::
                    ElevatedLayerIngestionFailed;
                return false;
            }
            NavigationLayerId navigationLayer;
            if (!tryNavigationLayerFromTerrainPathfindLayer(
                    surface.layer, navigationLayer)) {
                result.status = NavigationTerrainAdapterStatus::
                    ElevatedTopologyCapacityExceeded;
                return false;
            }

            int64_t minimumXRaw = surface.boundaryRaw.front()[0];
            int64_t maximumXRaw = minimumXRaw;
            int64_t minimumYRaw = surface.boundaryRaw.front()[1];
            int64_t maximumYRaw = minimumYRaw;
            for (const auto& vertex : surface.boundaryRaw) {
                minimumXRaw = std::min(minimumXRaw, vertex[0]);
                maximumXRaw = std::max(maximumXRaw, vertex[0]);
                minimumYRaw = std::min(minimumYRaw, vertex[1]);
                maximumYRaw = std::max(maximumYRaw, vertex[1]);
            }
            const NavigationGridTransform groundTransform =
                ground.transform();
            int32_t minimumCellX = 0;
            int32_t maximumCellX = 0;
            int32_t minimumCellY = 0;
            int32_t maximumCellY = 0;
            if (!worldAxisToCell(
                    minimumXRaw, groundTransform.originXRaw,
                    groundTransform.cellSizeRaw, minimumCellX) ||
                !worldAxisToCell(
                    maximumXRaw, groundTransform.originXRaw,
                    groundTransform.cellSizeRaw, maximumCellX) ||
                !worldAxisToCell(
                    minimumYRaw, groundTransform.originYRaw,
                    groundTransform.cellSizeRaw, minimumCellY) ||
                !worldAxisToCell(
                    maximumYRaw, groundTransform.originYRaw,
                    groundTransform.cellSizeRaw, maximumCellY) ||
                minimumCellX == std::numeric_limits<int32_t>::min() ||
                minimumCellY == std::numeric_limits<int32_t>::min() ||
                maximumCellX == std::numeric_limits<int32_t>::max() ||
                maximumCellY == std::numeric_limits<int32_t>::max()) {
                result.status =
                    NavigationTerrainAdapterStatus::CoordinateOverflow;
                return false;
            }
            --minimumCellX;
            --minimumCellY;
            ++maximumCellX;
            ++maximumCellY;
            const uint64_t localWidth = static_cast<uint64_t>(
                static_cast<int64_t>(maximumCellX) - minimumCellX + 1);
            const uint64_t localHeight = static_cast<uint64_t>(
                static_cast<int64_t>(maximumCellY) - minimumCellY + 1);
            if (localWidth == 0 || localHeight == 0 ||
                localWidth > std::numeric_limits<uint32_t>::max() ||
                localHeight > std::numeric_limits<uint32_t>::max()) {
                result.status = NavigationTerrainAdapterStatus::
                    ElevatedTopologyCapacityExceeded;
                return false;
            }
            int64_t localOriginX = 0;
            int64_t localOriginY = 0;
            if (!translatedAxisOrigin(
                    groundTransform.originXRaw, minimumCellX,
                    groundTransform.cellSizeRaw, localOriginX) ||
                !translatedAxisOrigin(
                    groundTransform.originYRaw, minimumCellY,
                    groundTransform.cellSizeRaw, localOriginY)) {
                result.status =
                    NavigationTerrainAdapterStatus::CoordinateOverflow;
                return false;
            }

            NavigationGrid bridgeGrid;
            const NavigationCellValue blockedCell{
                NavigationPassability::Blocked,
                0,
                NavigationMovement::Ground,
                navigationLayer,
                0,
                InvalidNavigationPortal,
                0};
            if (bridgeGrid.initialize(
                    static_cast<uint32_t>(localWidth),
                    static_cast<uint32_t>(localHeight),
                    {localOriginX, localOriginY,
                     groundTransform.cellSizeRaw},
                    blockedCell) != NavigationGridResult::Success) {
                result.status = NavigationTerrainAdapterStatus::
                    ElevatedLayerIngestionFailed;
                return false;
            }

            NavigationWorldPosition endpointCenters[2];
            NavigationCellId groundEndpoints[2];
            NavigationCellId bridgeEndpoints[2];
            const math::q32_32 centerlineX = math::q32_32::from_raw(
                surface.toRaw[0] - surface.fromRaw[0]);
            const math::q32_32 centerlineY = math::q32_32::from_raw(
                surface.toRaw[1] - surface.fromRaw[1]);
            const math::q32_32 centerlineLength = math::q32_32::sqrt(
                centerlineX * centerlineX + centerlineY * centerlineY);
            if (centerlineLength <= math::q32_32{}) {
                result.status = NavigationTerrainAdapterStatus::
                    ElevatedLayerIngestionFailed;
                return false;
            }
            const math::q32_32 entryOffset = math::q32_32::from_raw(
                groundTransform.cellSizeRaw) *
                math::q32_32::from_fraction(7, 10);
            const math::q32_32 entryX = centerlineX / centerlineLength *
                entryOffset;
            const math::q32_32 entryY = centerlineY / centerlineLength *
                entryOffset;
            const NavigationWorldPosition endpoints[2] = {
                {surface.fromRaw[0] - entryX.raw(),
                 surface.fromRaw[1] - entryY.raw(), surface.fromRaw[2]},
                {surface.toRaw[0] + entryX.raw(),
                 surface.toRaw[1] + entryY.raw(), surface.toRaw[2]}};
            for (size_t endpointIndex = 0; endpointIndex < 2;
                 ++endpointIndex) {
                const NavigationWorldPosition authored =
                    endpoints[endpointIndex];
                groundEndpoints[endpointIndex] = ground.cellAt(authored);
                if (!groundEndpoints[endpointIndex] ||
                    !ground.cellCenter(groundEndpoints[endpointIndex],
                                       endpointCenters[endpointIndex])) {
                    result.status = NavigationTerrainAdapterStatus::
                        ElevatedLayerIngestionFailed;
                    return false;
                }
                groundEntryCells.push_back(
                    groundEndpoints[endpointIndex]);
                bridgeEndpoints[endpointIndex] = bridgeGrid.cellAt(
                    endpointCenters[endpointIndex]);
                if (!bridgeEndpoints[endpointIndex]) {
                    result.status = NavigationTerrainAdapterStatus::
                        ElevatedLayerIngestionFailed;
                    return false;
                }
            }

            for (size_t index = 0; index < bridgeGrid.cellCount(); ++index) {
                const NavigationCellId cell{static_cast<uint32_t>(index)};
                NavigationWorldPosition center;
                if (!bridgeGrid.cellCenter(cell, center)) {
                    result.status = NavigationTerrainAdapterStatus::
                        ElevatedLayerIngestionFailed;
                    return false;
                }
                NavigationCellValue value = blockedCell;
                value.heightRaw = centerlineHeightRaw(
                    surface, center.xRaw, center.yRaw);
                const int64_t halfCell =
                    groundTransform.cellSizeRaw / 2;
                const bool inside = cellCornersInside(
                    surface, center.xRaw, center.yRaw, halfCell);
                const bool endpoint =
                    cell == bridgeEndpoints[0] ||
                    cell == bridgeEndpoints[1];
                if (inside || endpoint)
                    value.passability = NavigationPassability::Traversable;
                if (bridgeGrid.setCell(cell, value) !=
                    NavigationGridResult::Success) {
                    result.status = NavigationTerrainAdapterStatus::
                        ElevatedLayerIngestionFailed;
                    return false;
                }

                if (value.passability !=
                        NavigationPassability::Traversable || endpoint)
                    continue;
                const NavigationCellId groundCell = ground.cellAt(center);
                if (!groundCell) continue;
                const math::q32_32 groundHeight = math::q32_32::from_raw(
                    terrain.groundHeightRaw(center.xRaw, center.yRaw));
                if (groundHeight + math::q32_32{int32_t{10}} >
                    math::q32_32::from_raw(value.heightRaw))
                    lowClearanceCells.push_back(groundCell);
            }

            result.systemStatus = system.addStartupElevatedLayer(
                navigationLayer, bridgeGrid);
            if (result.systemStatus != NavigationSystemStatus::Success) {
                result.status = NavigationTerrainAdapterStatus::
                    ElevatedLayerIngestionFailed;
                return false;
            }
            ++result.elevatedLayerCount;

            for (size_t endpointIndex = 0; endpointIndex < 2;
                 ++endpointIndex) {
                if (nextPortalValue == 0) {
                    result.status = NavigationTerrainAdapterStatus::
                        ElevatedTopologyCapacityExceeded;
                    return false;
                }
                const NavigationPortalId portal{nextPortalValue++};
                result.systemStatus = system.addStartupPortal({
                    .id = portal,
                    .sideA = {policy.groundLayer,
                              groundEndpoints[endpointIndex]},
                    .sideB = {navigationLayer,
                              bridgeEndpoints[endpointIndex]},
                    .direction = NavigationPortalDirection::TwoWay,
                    .profile = policy.groundProfile,
                    .traversalCost = 0,
                    .active = surface.active,
                });
                if (result.systemStatus != NavigationSystemStatus::Success) {
                    result.status = NavigationTerrainAdapterStatus::
                        PortalIngestionFailed;
                    return false;
                }
                ++result.portalCount;
            }
        }

        std::sort(lowClearanceCells.begin(), lowClearanceCells.end());
        lowClearanceCells.erase(
            std::unique(lowClearanceCells.begin(),
                        lowClearanceCells.end()),
            lowClearanceCells.end());
        std::sort(groundEntryCells.begin(), groundEntryCells.end());
        groundEntryCells.erase(
            std::unique(groundEntryCells.begin(),
                        groundEntryCells.end()),
            groundEntryCells.end());
        container::Vector<NavigationCellId> changedGroundCells =
            lowClearanceCells;
        changedGroundCells.insert(
            changedGroundCells.end(), groundEntryCells.begin(),
            groundEntryCells.end());
        std::sort(changedGroundCells.begin(), changedGroundCells.end());
        changedGroundCells.erase(
            std::unique(changedGroundCells.begin(),
                        changedGroundCells.end()),
            changedGroundCells.end());
        container::Vector<NavigationStaticCellUpdate> clearanceUpdates;
        clearanceUpdates.reserve(changedGroundCells.size());
        for (const NavigationCellId cell : changedGroundCells) {
            NavigationCellValue value = system.staticGrid().cell(cell);
            if (std::binary_search(
                    groundEntryCells.begin(), groundEntryCells.end(),
                    cell)) {
                value.passability = NavigationPassability::Traversable;
                value.movementMask |= NavigationMovement::Ground;
            } else {
                value.passability = NavigationPassability::Blocked;
            }
            clearanceUpdates.push_back({cell, value});
        }
        result.systemStatus = system.stageStaticCells(clearanceUpdates);
        if (result.systemStatus != NavigationSystemStatus::Success) {
            result.status =
                NavigationTerrainAdapterStatus::StaticIngestionFailed;
            return false;
        }
        result.lowClearanceCellCount =
            static_cast<uint32_t>(lowClearanceCells.size());
        return true;
    }

    [[nodiscard]] static bool fixedRaw(float value, int64_t& output) noexcept
    {
        if (!std::isfinite(value)) return false;
        constexpr long double Scale =
            static_cast<long double>(uint64_t{1} << 32U);
        const long double scaled = static_cast<long double>(value) * Scale;
        if (scaled < static_cast<long double>(
                std::numeric_limits<int64_t>::min()) ||
            scaled > static_cast<long double>(
                std::numeric_limits<int64_t>::max()))
            return false;
        output = static_cast<int64_t>(scaled);
        return true;
    }

    [[nodiscard]] static bool translatedAxisOrigin(
        int64_t origin, int32_t cell, int64_t cellSize,
        int64_t& output) noexcept
    {
        const long double translated =
            static_cast<long double>(origin) +
            static_cast<long double>(cell) *
                static_cast<long double>(cellSize);
        if (translated < static_cast<long double>(
                std::numeric_limits<int64_t>::min()) ||
            translated > static_cast<long double>(
                std::numeric_limits<int64_t>::max()))
            return false;
        output = static_cast<int64_t>(translated);
        return true;
    }

    [[nodiscard]] static int64_t centerlineHeightRaw(
        const game::terrain::TerrainElevatedPathfindSurface& surface,
        int64_t xRaw, int64_t yRaw) noexcept
    {
        using Fixed = math::q32_32;
        const Fixed deltaX = Fixed::from_raw(
            surface.toRaw[0] - surface.fromRaw[0]);
        const Fixed deltaY = Fixed::from_raw(
            surface.toRaw[1] - surface.fromRaw[1]);
        const Fixed lengthSquared = deltaX * deltaX + deltaY * deltaY;
        if (lengthSquared <= Fixed{}) return surface.heightRaw;
        const Fixed factor = Fixed::clamp(
            ((Fixed::from_raw(xRaw - surface.fromRaw[0]) * deltaX) +
             (Fixed::from_raw(yRaw - surface.fromRaw[1]) * deltaY)) /
                lengthSquared,
            Fixed{}, Fixed{int32_t{1}});
        return (Fixed::from_raw(surface.fromRaw[2]) +
            Fixed::from_raw(surface.toRaw[2] - surface.fromRaw[2]) *
                factor).raw();
    }

    [[nodiscard]] static bool pointInsideConvexBoundary(
        const game::terrain::TerrainElevatedPathfindSurface& surface,
        int64_t xRaw, int64_t yRaw) noexcept
    {
        using Fixed = math::q32_32;
        bool positive = false;
        bool negative = false;
        const Fixed epsilon = Fixed::from_fraction(1, 10'000);
        for (size_t index = 0; index < surface.boundaryRaw.size(); ++index) {
            const auto& first = surface.boundaryRaw[index];
            const auto& second = surface.boundaryRaw[
                (index + 1U) % surface.boundaryRaw.size()];
            const Fixed cross =
                Fixed::from_raw(second[0] - first[0]) *
                    Fixed::from_raw(yRaw - first[1]) -
                Fixed::from_raw(second[1] - first[1]) *
                    Fixed::from_raw(xRaw - first[0]);
            positive |= cross > epsilon;
            negative |= cross < -epsilon;
            if (positive && negative) return false;
        }
        return true;
    }

    [[nodiscard]] static bool cellCornersInside(
        const game::terrain::TerrainElevatedPathfindSurface& surface,
        int64_t centerXRaw, int64_t centerYRaw,
        int64_t halfCellRaw) noexcept
    {
        const int64_t xRaw[2] = {
            centerXRaw - halfCellRaw,
            centerXRaw + halfCellRaw};
        const int64_t yRaw[2] = {
            centerYRaw - halfCellRaw,
            centerYRaw + halfCellRaw};
        for (const int64_t x : xRaw) {
            for (const int64_t y : yRaw) {
                    if (!pointInsideConvexBoundary(surface, x, y))
                    return false;
            }
        }
        return true;
    }

    [[nodiscard]] static bool validTerrain(const game::terrain::TerrainHeightfieldData& terrain) noexcept
    {
        if (terrain.width < 2 || terrain.height < 2 || terrain.borderSize < 0)
            return false;
        const uint64_t sampleCount = static_cast<uint64_t>(terrain.width) *
                                     static_cast<uint64_t>(terrain.height);
        const uint64_t cellCount = static_cast<uint64_t>(terrain.width - 1) *
                                   static_cast<uint64_t>(terrain.height - 1);
        return sampleCount == terrain.heights.size() &&
               cellCount < std::numeric_limits<uint32_t>::max();
    }

    [[nodiscard]] static bool validPolicy(const NavigationTerrainPolicy& policy) noexcept
    {
        return policy.groundLayer && policy.groundProfile &&
               policy.dynamicEntityCapacity != 0 && policy.dynamicEventCapacity != 0 &&
               policy.maxCellsPerFootprint != 0 && policy.pathCapacity != 0 &&
               policy.maxPointsPerPath != 0 && policy.requestCapacity != 0 &&
               policy.feedbackCapacity != 0;
    }

    [[nodiscard]] static bool isCliff(const game::terrain::TerrainHeightfieldData& terrain,
                                      uint32_t x,
                                      uint32_t y,
                                      bool hasAuthoredCliffs) noexcept
    {
        const size_t width = static_cast<size_t>(terrain.width);
        const size_t first = static_cast<size_t>(y) * width + x;
        if (hasAuthoredCliffs)
            return terrain.blendTiles->cliffCells[first] != 0;
        const uint8_t p0 = terrain.heights[first];
        const uint8_t p1 = terrain.heights[first + 1];
        const uint8_t p2 = terrain.heights[first + width + 1];
        const uint8_t p3 = terrain.heights[first + width];
        const uint8_t minimum = std::min({p0, p1, p2, p3});
        const uint8_t maximum = std::max({p0, p1, p2, p3});
        return static_cast<uint32_t>(maximum) - minimum >= 16U;
    }
};

} // namespace engine::navigation
