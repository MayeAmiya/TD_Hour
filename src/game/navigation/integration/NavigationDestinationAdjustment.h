#pragma once

#include "game/navigation/grid/NavigationLayers.h"

#include <cstdint>

namespace engine::navigation
{

enum class NavigationDestinationAdjustmentStatus : uint8_t
{
    Exact = 0,
    Adjusted,
    InvalidRequest,
    NoTraversableCell,
};

struct NavigationDestinationAdjustmentRequest final
{
    NavigationWorldPosition desired;
    NavigationLayerId layer = InvalidNavigationLayer;
    NavigationMovementMask movementMask = 0;
    NavigationClearanceClass clearance = NavigationClearanceClass::Infantry;
    bool allowAdjustment = true;
};

struct NavigationDestinationAdjustmentResult final
{
    NavigationDestinationAdjustmentStatus status =
        NavigationDestinationAdjustmentStatus::InvalidRequest;
    NavigationLayerCell location;
    NavigationWorldPosition position;

    [[nodiscard]] bool accepted() const noexcept
    {
        return status == NavigationDestinationAdjustmentStatus::Exact ||
               status == NavigationDestinationAdjustmentStatus::Adjusted;
    }
};

// Shared value-only destination admission for AI paths, containment exits,
// railed ferry egress and production rally routes. It performs no allocation
// and chooses an adjusted cell by (squared cell distance, canonical CellId),
// so every caller observes the same deterministic tie break.
[[nodiscard]] inline NavigationDestinationAdjustmentResult
adjustNavigationDestination(
    const NavigationLayerSet& layers,
    const NavigationDestinationAdjustmentRequest& request) noexcept
{
    NavigationDestinationAdjustmentResult result;
    const NavigationGrid* grid = layers.find(request.layer);
    if (!grid || request.movementMask == 0 ||
        !validClearanceClass(request.clearance))
        return result;

    const NavigationCellId desiredCell =
        grid->cellAt(request.desired, request.clearance);
    if (grid->traversable(
            desiredCell, request.movementMask, request.layer,
            request.clearance)) {
        result.status = NavigationDestinationAdjustmentStatus::Exact;
        result.location = {request.layer, desiredCell};
        if (!grid->cellPosition(desiredCell, request.clearance,
                                result.position)) {
            result = {};
        }
        return result;
    }
    if (!request.allowAdjustment) {
        result.status =
            NavigationDestinationAdjustmentStatus::NoTraversableCell;
        return result;
    }

    NavigationGridCoordinate desiredCoordinate{};
    if (desiredCell) {
        desiredCoordinate = grid->coordinate(desiredCell);
    } else {
        if (!worldAxisToCell(
                request.desired.xRaw, grid->transform().originXRaw,
                grid->transform().cellSizeRaw, desiredCoordinate.x) ||
            !worldAxisToCell(
                request.desired.yRaw, grid->transform().originYRaw,
                grid->transform().cellSizeRaw, desiredCoordinate.y)) {
            return result;
        }
    }

    // RefCode's Pathfinder uses a bounded counter-clockwise spiral.  The
    // bound is deliberate: destination adjustment must not become a full-map
    // scan for every AI order, and failure after the bound is a valid no-path
    // outcome which the caller can retry on the next confirmed frame.
    constexpr size_t kMaximumAdjustmentCellCount = 400;
    NavigationCellId selected = InvalidNavigationCell;
    int64_t x = desiredCoordinate.x;
    int64_t y = desiredCoordinate.y;
    size_t remaining = kMaximumAdjustmentCellCount;
    size_t segmentLength = 1;
    constexpr int32_t directions[4][2] = {
        {1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    while (remaining != 0 && !selected) {
        for (size_t direction = 0; direction < 4 && remaining != 0 && !selected;
             ++direction) {
            for (size_t step = 0; step < segmentLength && remaining != 0;
                 ++step, --remaining) {
                x += directions[direction][0];
                y += directions[direction][1];
                if (x < 0 || y < 0 ||
                    x >= static_cast<int64_t>(grid->width()) ||
                    y >= static_cast<int64_t>(grid->height())) {
                    continue;
                }
                const NavigationCellId candidate = grid->cellId({
                    static_cast<int32_t>(x), static_cast<int32_t>(y)});
                if (grid->traversable(
                        candidate, request.movementMask, request.layer,
                        request.clearance)) {
                    selected = candidate;
                    break;
                }
            }
            if ((direction & 1U) != 0)
                ++segmentLength;
        }
    }
    if (!selected) {
        result.status =
            NavigationDestinationAdjustmentStatus::NoTraversableCell;
        return result;
    }
    result.status = NavigationDestinationAdjustmentStatus::Adjusted;
    result.location = {request.layer, selected};
    if (!grid->cellPosition(selected, request.clearance, result.position)) {
        result = {};
        return result;
    }
    return result;
}

} // namespace engine::navigation
