#include "game/object/simulation/movement/ObjectFootprintEvacuation.h"

#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>

namespace engine {

std::optional<LogicFixedVec3> objectFootprintEvacuationTarget(
    const LogicFixedVec3& obstaclePosition, math::q32_32 obstacleYaw,
    const ObjectGeometryComponent& obstacleGeometry,
    const LogicFixedVec3& subjectPosition,
    const ObjectGeometryComponent& subjectGeometry,
    ObjectId subject, math::q32_32 clearance) noexcept {
    using Fixed = math::q32_32;
    const Fixed zero{};
    const Fixed one{int32_t{1}};
    const Fixed subjectRadius = Fixed::max(
        zero, subjectGeometry.boundingCircleRadiusFixed);
    const Fixed effectiveClearance = Fixed::max(zero, clearance);
    const Fixed cosine = math::fixed_cos(obstacleYaw);
    const Fixed sine = math::fixed_sin(obstacleYaw);
    const Fixed dx = subjectPosition.x - obstaclePosition.x;
    const Fixed dy = subjectPosition.y - obstaclePosition.y;

    if (obstacleGeometry.shape != ObjectGeometryShape::Box) {
        const Fixed obstacleRadius = Fixed::max(
            zero, obstacleGeometry.majorRadiusFixed);
        const Fixed required = obstacleRadius + subjectRadius;
        const Fixed distanceSquared = dx * dx + dy * dy;
        if (distanceSquared > required * required) return std::nullopt;
        Fixed directionX{};
        Fixed directionY{};
        const Fixed distance = Fixed::sqrt(distanceSquared);
        if (distance > zero) {
            directionX = dx / distance;
            directionY = dy / distance;
        } else {
            switch (subject.value & 3u) {
            case 0u: directionX = one; break;
            case 1u: directionX = -one; break;
            case 2u: directionY = one; break;
            default: directionY = -one; break;
            }
        }
        const Fixed targetDistance = required + effectiveClearance;
        return LogicFixedVec3{
            obstaclePosition.x + directionX * targetDistance,
            obstaclePosition.y + directionY * targetDistance,
            subjectPosition.z,
        };
    }

    const Fixed halfX = Fixed::max(
        zero, obstacleGeometry.majorRadiusFixed);
    const Fixed halfY = Fixed::max(
        zero, obstacleGeometry.minorRadiusFixed);
    const Fixed localX = cosine * dx + sine * dy;
    const Fixed localY = -sine * dx + cosine * dy;
    const Fixed closestX = std::clamp(localX, -halfX, halfX);
    const Fixed closestY = std::clamp(localY, -halfY, halfY);
    const Fixed fromClosestX = localX - closestX;
    const Fixed fromClosestY = localY - closestY;
    const Fixed closestDistanceSquared =
        fromClosestX * fromClosestX + fromClosestY * fromClosestY;
    if (closestDistanceSquared > subjectRadius * subjectRadius)
        return std::nullopt;

    Fixed targetLocalX{};
    Fixed targetLocalY{};
    if (fromClosestX.raw() != 0 || fromClosestY.raw() != 0) {
        const Fixed distance = Fixed::sqrt(closestDistanceSquared);
        const Fixed directionX = fromClosestX / distance;
        const Fixed directionY = fromClosestY / distance;
        const Fixed edgeClearance = subjectRadius + effectiveClearance;
        targetLocalX = closestX + directionX * edgeClearance;
        targetLocalY = closestY + directionY * edgeClearance;
    } else {
        const Fixed distanceX = halfX - Fixed::abs(localX);
        const Fixed distanceY = halfY - Fixed::abs(localY);
        const bool useX = distanceX < distanceY ||
            (distanceX == distanceY && (subject.value & 1u) == 0u);
        if (useX) {
            Fixed sign = localX < zero ? -one : one;
            if (localX == zero && (subject.value & 2u) != 0u) sign = -one;
            targetLocalX = sign * (
                halfX + subjectRadius + effectiveClearance);
            targetLocalY = localY;
        } else {
            Fixed sign = localY < zero ? -one : one;
            if (localY == zero && (subject.value & 2u) != 0u) sign = -one;
            targetLocalX = localX;
            targetLocalY = sign * (
                halfY + subjectRadius + effectiveClearance);
        }
    }

    return LogicFixedVec3{
        obstaclePosition.x + cosine * targetLocalX - sine * targetLocalY,
        obstaclePosition.y + sine * targetLocalX + cosine * targetLocalY,
        subjectPosition.z,
    };
}

} // namespace engine
