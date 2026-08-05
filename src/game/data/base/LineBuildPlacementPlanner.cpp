#include "LineBuildPlacementPlanner.h"

#include <algorithm>

namespace engine {

LineBuildPlacementPlan planLineBuildPlacement(
    const LineBuildPlacementRequest& request) {
    using Fixed = math::q32_32;

    LineBuildPlacementPlan plan;
    const Fixed startX = request.start.x;
    const Fixed startY = request.start.y;
    const Fixed endX = request.end.x;
    const Fixed endY = request.end.y;
    const Fixed majorRadius = request.geometryMajorRadius;
    const Fixed spacing = majorRadius * Fixed{int32_t{2}};
    plan.spacing = spacing;
    if (spacing <= Fixed{} || request.maxTiles == 0 ||
        !request.terrainHeight) {
        return plan;
    }

    const Fixed deltaX = endX - startX;
    const Fixed deltaY = endY - startY;
    const Fixed length = Fixed::sqrt(deltaX * deltaX + deltaY * deltaY);

    const Fixed intervals = length / spacing;
    const uint64_t completeIntervals =
        static_cast<uint64_t>(intervals.raw()) >> 32u;
    plan.boundedTileCount = static_cast<uint32_t>(std::min<uint64_t>(
        completeIntervals + 1u, request.maxTiles));
    plan.legalPrefix.reserve(plan.boundedTileCount);

    const Fixed unitX = length > Fixed{} ? deltaX / length : Fixed{};
    const Fixed unitY = length > Fixed{} ? deltaY / length : Fixed{};
    for (uint32_t index = 0; index < plan.boundedTileCount; ++index) {
        // The largest possible index is INT32_MAX because q32.32's positive
        // integer range bounds completeIntervals before this loop.
        const Fixed distance = spacing * Fixed(static_cast<int32_t>(index));
        const Fixed x = startX + unitX * distance;
        const Fixed y = startY + unitY * distance;
        LineBuildPosition position{.x = x, .y = y};
        if (!request.terrainHeight(
                request.callbackContext, position.x, position.y,
                position.z)) {
            break;
        }
        if (request.isLegal &&
            !request.isLegal(request.callbackContext, position, index)) {
            break;
        }
        plan.legalPrefix.push_back(position);
    }
    return plan;
}

} // namespace engine
