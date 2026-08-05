#pragma once

#include "../grid/NavigationDynamicOverlay.h"
#include "../grid/NavigationLayers.h"
#include "../contracts/NavigationPathContracts.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace engine::navigation
{

namespace detail
{

[[nodiscard]] inline bool navigationObjectCellBlocked(
    container::Span<const engine::ai::AIPathObjectCellSnapshot> objectCells,
    NavigationLayerId layer,
    NavigationCellId cell) noexcept
{
    const auto first = std::lower_bound(
        objectCells.begin(), objectCells.end(),
        std::pair<uint32_t, uint32_t>{layer.value, cell.value},
        [](const engine::ai::AIPathObjectCellSnapshot& value,
           const std::pair<uint32_t, uint32_t>& wanted) noexcept {
            return value.layer < wanted.first ||
                (value.layer == wanted.first &&
                 value.cell < wanted.second);
        });
    for (auto current = first;
         current != objectCells.end() &&
             current->layer == layer.value &&
             current->cell == cell.value;
         ++current) {
        if (current->effect ==
                engine::ai::AIPathObjectCellEffect::EnemyBlock ||
            current->effect ==
                engine::ai::AIPathObjectCellEffect::NeutralBlock)
            return true;
    }
    return false;
}

[[nodiscard]] inline bool navigationCellPinched(
    const NavigationGrid& grid,
    const NavigationDynamicOverlay* dynamicOverlay,
    NavigationCellId cell) noexcept
{
    if (grid.pinched(cell)) return true;
    if (!dynamicOverlay || !grid.contains(cell)) return false;
    const NavigationGridCoordinate coordinate = grid.coordinate(cell);
    constexpr NavigationDirectionDelta orthogonal[4] = {
        {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (const NavigationDirectionDelta delta : orthogonal) {
        const NavigationCellId neighbor = grid.cellId({
            coordinate.x + delta.x, coordinate.y + delta.y});
        if (neighbor && dynamicOverlay->ownerCount(neighbor) != 0)
            return true;
    }
    return false;
}

[[nodiscard]] inline bool navigationLineOfSight(
    const NavigationGrid& grid,
    NavigationCellId from,
    NavigationCellId to,
    NavigationMovementMask movementMask,
    NavigationLayerId layer,
    NavigationClearanceClass clearance,
    const NavigationDynamicOverlay* dynamicOverlay = nullptr,
    container::Span<const engine::ai::AIPathObjectCellSnapshot>
        objectCells = {}) noexcept
{
    if (!grid.traversable(from, movementMask, layer, clearance) ||
        !grid.traversable(to, movementMask, layer, clearance) ||
        navigationCellPinched(grid, dynamicOverlay, from) ||
        navigationCellPinched(grid, dynamicOverlay, to) ||
        navigationObjectCellBlocked(objectCells, layer, from) ||
        navigationObjectCellBlocked(objectCells, layer, to))
        return false;
    if (from == to)
        return true;

    const NavigationGridCoordinate start = grid.coordinate(from);
    const NavigationGridCoordinate goal = grid.coordinate(to);
    const int64_t deltaX = static_cast<int64_t>(goal.x) - start.x;
    const int64_t deltaY = static_cast<int64_t>(goal.y) - start.y;
    const int32_t stepX = deltaX < 0 ? -1 : 1;
    const int32_t stepY = deltaY < 0 ? -1 : 1;
    const uint64_t stepsX = static_cast<uint64_t>(deltaX < 0 ? -deltaX : deltaX);
    const uint64_t stepsY = static_cast<uint64_t>(deltaY < 0 ? -deltaY : deltaY);

    int32_t x = start.x;
    int32_t y = start.y;
    uint64_t advancedX = 0;
    uint64_t advancedY = 0;
    NavigationCellId current = from;
    while (advancedX != stepsX || advancedY != stepsY)
    {
        // Compare the next vertical and horizontal grid-boundary crossing
        // using integers. Equality is a corner crossing; RefCode rejects it
        // only when both adjacent cardinal cells are closed.
        const uint64_t horizontal = (advancedX * 2U + 1U) * stepsY;
        const uint64_t vertical = (advancedY * 2U + 1U) * stepsX;
        if (horizontal == vertical)
        {
            const NavigationCellId sideX = grid.cellId({x + stepX, y});
            const NavigationCellId sideY = grid.cellId({x, y + stepY});
            const bool sideXOpen = grid.traversable(
                    sideX, movementMask, layer, clearance) &&
                !navigationObjectCellBlocked(
                    objectCells, layer, sideX);
            const bool sideYOpen = grid.traversable(
                    sideY, movementMask, layer, clearance) &&
                !navigationObjectCellBlocked(
                    objectCells, layer, sideY);
            if (!sideXOpen && !sideYOpen)
                return false;
            x += stepX;
            y += stepY;
            ++advancedX;
            ++advancedY;
            const NavigationCellId next = grid.cellId({x, y});
            if (!grid.traversableEdge(current, next, movementMask, layer, clearance))
                return false;
            if (navigationObjectCellBlocked(objectCells, layer, next))
                return false;
            if (navigationCellPinched(grid, dynamicOverlay, next))
                return false;
            current = next;
        }
        else if (horizontal < vertical)
        {
            x += stepX;
            ++advancedX;
            const NavigationCellId next = grid.cellId({x, y});
            if (!grid.traversableEdge(current, next, movementMask, layer, clearance))
                return false;
            if (navigationObjectCellBlocked(objectCells, layer, next))
                return false;
            if (navigationCellPinched(grid, dynamicOverlay, next))
                return false;
            current = next;
        }
        else
        {
            y += stepY;
            ++advancedY;
            const NavigationCellId next = grid.cellId({x, y});
            if (!grid.traversableEdge(current, next, movementMask, layer, clearance))
                return false;
            if (navigationObjectCellBlocked(objectCells, layer, next))
                return false;
            if (navigationCellPinched(grid, dynamicOverlay, next))
                return false;
            current = next;
        }
    }
    return current == to;
}

[[nodiscard]] inline const NavigationGrid* smoothingGrid(
    const NavigationLayerSet* layers,
    const NavigationGrid* fallback,
    NavigationLayerId fallbackLayer,
    NavigationLayerId requestedLayer) noexcept
{
    if (layers != nullptr)
        return layers->find(requestedLayer);
    return requestedLayer == fallbackLayer ? fallback : nullptr;
}

} // namespace detail

// Read-only confirmed-topology query used when an actor has continued moving
// while a replacement path was solved.  A repository segment is known to be
// legal from its authored start, but the actor's newer position still needs a
// legal connection back to that segment before movement may treat it as
// navigation-validated.
[[nodiscard]] inline bool navigationLinePassable(
    const NavigationLayerSet& layers,
    const NavigationWorldPosition& from,
    const NavigationWorldPosition& to,
    NavigationMovementMask movementMask,
    NavigationLayerId layer,
    NavigationClearanceClass clearance,
    container::Span<const engine::ai::AIPathObjectCellSnapshot>
        objectCells = {}) noexcept
{
    const NavigationGrid* grid = layers.find(layer);
    if (grid == nullptr || movementMask == 0 ||
        !validClearanceClass(clearance))
        return false;
    const NavigationCellId fromCell = grid->cellAt(from, clearance);
    const NavigationCellId toCell = grid->cellAt(to, clearance);
    if (!fromCell || !toCell)
        return false;
    return detail::navigationLineOfSight(
        *grid, fromCell, toCell, movementMask, layer, clearance,
        nullptr, objectCells);
}

// Compact only confirmed, same-layer cell paths. A bounded look-ahead keeps
// smoothing linear for long straight routes while preserving a fixed result
// for the same grid, movement mask and clearance class. Layer changes are hard
// barriers: a portal transition is never smoothed across.
[[nodiscard]] inline bool smoothNavigationPath(
    const NavigationLayerSet* layers,
    const NavigationGrid* fallbackGrid,
    const NavigationDynamicOverlay* dynamicOverlay,
    NavigationLayerId fallbackLayer,
    NavigationMovementMask movementMask,
    NavigationClearanceClass clearance,
    container::Span<NavigationLayerPathPoint> points,
    size_t& pointCount,
    container::Span<const engine::ai::AIPathObjectCellSnapshot>
        objectCells = {}) noexcept
{
    if (pointCount == 0 || pointCount > points.size())
        return false;
    if (pointCount < 3)
        return true;

    constexpr size_t MaxLookAhead = 64;
    size_t anchor = 0;
    size_t write = 1;
    while (anchor + 1U < pointCount)
    {
        size_t furthest = anchor + 1U;
        const size_t limit =
            std::min(pointCount - 1U, anchor + MaxLookAhead);
        for (size_t candidate = anchor + 1U; candidate <= limit; ++candidate)
        {
            const NavigationLayerPathPoint& from = points[anchor];
            const NavigationLayerPathPoint& to = points[candidate];
            if (from.location.layer != to.location.layer)
                break;
            const NavigationGrid* grid = detail::smoothingGrid(
                layers, fallbackGrid, fallbackLayer, from.location.layer);
            if (grid == nullptr || !detail::navigationLineOfSight(
                                       *grid, from.location.cell, to.location.cell,
                                       movementMask, from.location.layer,
                                       clearance, dynamicOverlay,
                                       objectCells))
                break;
            furthest = candidate;
        }
        points[write++] = points[furthest];
        anchor = furthest;
    }
    pointCount = write;
    return true;
}

} // namespace engine::navigation
