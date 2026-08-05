#include "ObjectOclSpreadPlacement.h"

#include "game/navigation/integration/NavigationDestinationAdjustment.h"
#include "game/navigation/integration/NavigationFootprintRasterizer.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace engine {
namespace {

using Fixed = math::q32_32;

constexpr Fixed kRingSpacing{int32_t{5}};
constexpr Fixed kOne{int32_t{1}};
constexpr Fixed kPi = Fixed::from_raw(13493037705ll);
constexpr Fixed kTwoPi = Fixed::from_raw(26986075409ll);
constexpr Fixed kPiOverThree = Fixed::from_raw(4497679235ll);

[[nodiscard]] Fixed normalizedAngle(Fixed angle) noexcept {
    int64_t raw = angle.raw() % kTwoPi.raw();
    if (raw < 0) raw += kTwoPi.raw();
    return Fixed::from_raw(raw);
}

[[nodiscard]] Fixed advanceAngle(Fixed angle, Fixed delta) noexcept {
    const int64_t raw = angle.raw() + delta.raw();
    return Fixed::from_raw(raw >= kTwoPi.raw() ? raw - kTwoPi.raw() : raw);
}

[[nodiscard]] Fixed retreatAngle(Fixed angle, Fixed delta) noexcept {
    const int64_t raw = angle.raw() - delta.raw();
    return Fixed::from_raw(raw < 0 ? raw + kTwoPi.raw() : raw);
}

[[nodiscard]] bool fullyInsideGrid(
    const navigation::NavigationGrid& grid,
    navigation::NavigationWorldPosition center,
    int64_t radiusRaw) noexcept {
    int64_t minimumX = 0;
    int64_t minimumY = 0;
    int64_t maximumX = 0;
    int64_t maximumY = 0;
    if (!navigation::detail::checkedSubtract(
            center.xRaw, radiusRaw, minimumX) ||
        !navigation::detail::checkedSubtract(
            center.yRaw, radiusRaw, minimumY) ||
        !navigation::detail::checkedAdd(
            center.xRaw, radiusRaw, maximumX) ||
        !navigation::detail::checkedAdd(
            center.yRaw, radiusRaw, maximumY)) {
        return false;
    }
    return grid.cellAt({minimumX, minimumY, 0}) &&
           grid.cellAt({minimumX, maximumY, 0}) &&
           grid.cellAt({maximumX, minimumY, 0}) &&
           grid.cellAt({maximumX, maximumY, 0});
}

[[nodiscard]] bool tryLayer(
    const navigation::NavigationGrid& grid,
    navigation::NavigationLayerId layer,
    navigation::NavigationMovementMask movementMask,
    Fixed footprintRadius,
    navigation::NavigationWorldPosition& position,
    container::Span<navigation::NavigationCellId> footprintScratch) noexcept {
    if (footprintScratch.empty() ||
        !fullyInsideGrid(grid, position, footprintRadius.raw())) {
        return false;
    }

    const navigation::NavigationFootprintRasterResult raster =
        navigation::NavigationFootprintRasterizer::circle(
            grid, position, footprintRadius.raw(), footprintScratch);
    if (raster.status !=
            navigation::NavigationFootprintRasterStatus::Success ||
        raster.writtenCount == 0) {
        return false;
    }
    for (uint32_t index = 0; index < raster.writtenCount; ++index) {
        if (!grid.traversable(
                footprintScratch[index], movementMask, layer)) {
            return false;
        }
    }

    const navigation::NavigationCellId centerCell = grid.cellAt(position);
    if (!centerCell) return false;
    position.zRaw = grid.cell(centerCell).heightRaw;
    return true;
}

[[nodiscard]] bool tryCandidate(
    const navigation::NavigationLayerSet& layers,
    const ObjectOclSpreadPlacementRequest& request,
    Fixed distance,
    Fixed angle,
    container::Span<navigation::NavigationCellId> footprintScratch,
    ObjectOclSpreadPlacementResult& result) noexcept {
    const math::q32_32_sincos direction = math::fixed_sincos(angle);
    const Fixed deltaX = distance * direction.cosine;
    const Fixed deltaY = distance * direction.sine;
    navigation::NavigationWorldPosition candidate = request.center;
    if (!navigation::detail::checkedAdd(
            candidate.xRaw, deltaX.raw(), candidate.xRaw) ||
        !navigation::detail::checkedAdd(
            candidate.yRaw, deltaY.raw(), candidate.yRaw)) {
        return false;
    }

    const container::Span<const navigation::NavigationLayerRecord> records =
        layers.layers();
    for (size_t reverseIndex = records.size(); reverseIndex != 0;
         --reverseIndex) {
        const navigation::NavigationLayerRecord& record =
            records[reverseIndex - 1];
        const navigation::NavigationClearanceClass clearance =
            navigation::clearanceClassForRadiusRaw(
                request.footprintRadius.raw(),
                record.grid.transform().cellSizeRaw);
        const navigation::NavigationDestinationAdjustmentResult admission =
            navigation::adjustNavigationDestination(
                layers,
                {
                    .desired = candidate,
                    .layer = record.id,
                    .movementMask = request.movementMask,
                    .clearance = clearance,
                    .allowAdjustment = false,
                });
        if (!admission.accepted()) continue;
        navigation::NavigationWorldPosition admitted = admission.position;
        if (!tryLayer(record.grid, record.id, request.movementMask,
                      request.footprintRadius, admitted,
                      footprintScratch)) {
            continue;
        }
        result.status = ObjectOclSpreadPlacementStatus::Found;
        result.position = admitted;
        result.navigationLayer = record.id;
        return true;
    }
    return false;
}

} // namespace

ObjectOclSpreadPlacementResult findObjectOclSpreadPlacement(
    const navigation::NavigationLayerSet& layers,
    const ObjectOclSpreadPlacementRequest& request,
    container::Span<navigation::NavigationCellId> footprintScratch) noexcept {
    ObjectOclSpreadPlacementResult result;
    result.position = request.center;
    if (layers.size() == 0 || request.movementMask == 0 ||
        request.minimumRadius < Fixed{} ||
        request.maximumRadius < Fixed{} ||
        request.footprintRadius < Fixed{} || footprintScratch.empty()) {
        return result;
    }

    result.status = ObjectOclSpreadPlacementStatus::FallbackToCenter;
    if (request.minimumRadius > request.maximumRadius) return result;

    Fixed distance = request.minimumRadius;
    const Fixed startAngle = normalizedAngle(request.startAngleRadians);
    bool firstRing = true;
    for (;;) {
        const Fixed angleSpacing = firstRing
            ? kTwoPi
            : (kRingSpacing / (distance + kOne)) * kPiOverThree;
        if (angleSpacing <= Fixed{}) return result;

        const uint64_t spacingRaw =
            static_cast<uint64_t>(angleSpacing.raw());
        const uint64_t piRaw = static_cast<uint64_t>(kPi.raw());
        const uint64_t sampleCount =
            piRaw / spacingRaw + static_cast<uint64_t>(piRaw % spacingRaw != 0);
        Fixed positiveAngle = startAngle;
        Fixed negativeAngle = startAngle;
        for (uint64_t sample = 0; sample < sampleCount; ++sample) {
            if (tryCandidate(layers, request, distance, positiveAngle,
                             footprintScratch, result)) {
                return result;
            }
            if (sample != 0 &&
                tryCandidate(layers, request, distance, negativeAngle,
                             footprintScratch, result)) {
                return result;
            }
            positiveAngle = advanceAngle(positiveAngle, angleSpacing);
            negativeAngle = retreatAngle(negativeAngle, angleSpacing);
        }

        firstRing = false;
        if (request.maximumRadius - distance < kRingSpacing) break;
        distance += kRingSpacing;
    }
    return result;
}

} // namespace engine
