#pragma once

#include "core/container/container_types.h"
#include "game/navigation/grid/NavigationLayers.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

namespace engine {

enum class ObjectOclSpreadPlacementStatus : uint8_t {
    Found = 0,
    FallbackToCenter,
    InvalidRequest,
};

struct ObjectOclSpreadPlacementRequest final {
    navigation::NavigationWorldPosition center;
    math::q32_32 minimumRadius{};
    math::q32_32 maximumRadius{};
    math::q32_32 startAngleRadians{};
    math::q32_32 footprintRadius{};
    navigation::NavigationMovementMask movementMask = 0;
};

struct ObjectOclSpreadPlacementResult final {
    ObjectOclSpreadPlacementStatus status =
        ObjectOclSpreadPlacementStatus::InvalidRequest;
    navigation::NavigationWorldPosition position;
    navigation::NavigationLayerId navigationLayer =
        navigation::InvalidNavigationLayer;

    [[nodiscard]] bool found() const noexcept {
        return status == ObjectOclSpreadPlacementStatus::Found;
    }
};

// Deterministic modern equivalent of PartitionManager::findPositionAround for
// Generic OCL SpreadFormation. Candidate ordering retains the original
// 5-world-unit concentric rings and +angle/-angle ping-pong. Admission is
// stronger than the old 5-unit point sphere: the complete frozen archetype
// bounding-circle footprint must fit traversable cells on one layer. Layers
// are tested highest-first and ties retain canonical candidate order.
//
// All gameplay coordinates and trigonometry are Q32.32. The caller supplies
// reusable footprint storage, so candidate evaluation performs no allocation.
[[nodiscard]] ObjectOclSpreadPlacementResult findObjectOclSpreadPlacement(
    const navigation::NavigationLayerSet& layers,
    const ObjectOclSpreadPlacementRequest& request,
    container::Span<navigation::NavigationCellId> footprintScratch) noexcept;

} // namespace engine
