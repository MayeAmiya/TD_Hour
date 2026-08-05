#include "game/object/simulation/combat/ObjectProjectileSystemDetail.h"

#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/combat/ObjectWeaponDamage.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace engine::object_projectile_detail {

[[nodiscard]] uint64_t millisecondsToFrames(uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t rate = std::max<uint32_t>(1, framesPerSecond);
    return (static_cast<uint64_t>(milliseconds) * rate + 999u) / 1000u;
}

[[nodiscard]] math::vec3 presentationPosition(const FixedVec3& value) noexcept {
    return {value.x.to_float(), value.y.to_float(), value.z.to_float()};
}

[[nodiscard]] uint32_t snapshotPathfindLayer(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint32_t fallback) noexcept {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    if (!entity) return fallback;
    const ObjectTerrainLayerComponent* layer =
        ecs::try_get<ObjectTerrainLayerComponent>(registry, *entity);
    return layer ? layer->pathfindLayer : fallback;
}

[[nodiscard]] FixedVec3 add(const FixedVec3& left, const FixedVec3& right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] FixedVec3 subtract(const FixedVec3& left, const FixedVec3& right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

[[nodiscard]] FixedVec3 scale(const FixedVec3& value, Fixed amount) noexcept {
    return {value.x * amount, value.y * amount, value.z * amount};
}

[[nodiscard]] Fixed dot(const FixedVec3& left, const FixedVec3& right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

[[nodiscard]] Fixed squaredLength(const FixedVec3& value) noexcept {
    return dot(value, value);
}

[[nodiscard]] Fixed length(const FixedVec3& value) noexcept {
    const Fixed lengthSquared = squaredLength(value);
    return lengthSquared > kFixedZero ? Fixed::sqrt(lengthSquared) : kFixedZero;
}

[[nodiscard]] Fixed planarLength(const FixedVec3& value) noexcept {
    const Fixed lengthSquared = value.x * value.x + value.y * value.y;
    return lengthSquared > kFixedZero ? Fixed::sqrt(lengthSquared) : kFixedZero;
}

[[nodiscard]] Fixed minFixed(Fixed left, Fixed right) noexcept {
    return left < right ? left : right;
}

[[nodiscard]] Fixed maxFixed(Fixed left, Fixed right) noexcept {
    return left > right ? left : right;
}

[[nodiscard]] Fixed clampUnit(Fixed value) noexcept {
    return maxFixed(kFixedZero, minFixed(value, kFixedOne));
}

[[nodiscard]] FixedVec3 normalizedOr(const FixedVec3& value,
                                     const FixedVec3& fallback) noexcept {
    const Fixed magnitude = length(value);
    return magnitude > kFixedSegmentEpsilon ? scale(value, kFixedOne / magnitude)
                                             : fallback;
}

[[nodiscard]] FixedVec3 quaternionForward(
    const LogicFixedQuaternion& rotation) noexcept {
    // Rotate the model's local +X axis by q.  This is the direction consumed
    // by MissileAIUpdate immediately after Weapon installs the launch-bone
    // Matrix3D; retaining it avoids silently retargeting the first launch
    // impulse toward the victim center.
    return {
        kFixedOne - kFixedTwo *
            (rotation.y * rotation.y + rotation.z * rotation.z),
        kFixedTwo *
            (rotation.x * rotation.y + rotation.w * rotation.z),
        kFixedTwo *
            (rotation.x * rotation.z - rotation.w * rotation.y),
    };
}

[[nodiscard]] FixedVec3 quaternionLocalY(
    const LogicFixedQuaternion& rotation) noexcept {
    return {
        kFixedTwo *
            (rotation.x * rotation.y - rotation.w * rotation.z),
        kFixedOne - kFixedTwo *
            (rotation.x * rotation.x + rotation.z * rotation.z),
        kFixedTwo *
            (rotation.y * rotation.z + rotation.w * rotation.x),
    };
}

[[nodiscard]] FixedVec3 quaternionLocalZ(
    const LogicFixedQuaternion& rotation) noexcept {
    return {
        kFixedTwo *
            (rotation.x * rotation.z + rotation.w * rotation.y),
        kFixedTwo *
            (rotation.y * rotation.z - rotation.w * rotation.x),
        kFixedOne - kFixedTwo *
            (rotation.x * rotation.x + rotation.y * rotation.y),
    };
}

[[nodiscard]] Fixed clampFixed(Fixed value, Fixed minimum, Fixed maximum) noexcept {
    return maxFixed(minimum, minFixed(value, maximum));
}

[[nodiscard]] FixedVec3 deterministicPerpendicular(const FixedVec3& forward) noexcept {
    const Fixed absX = forward.x < kFixedZero ? -forward.x : forward.x;
    const Fixed absY = forward.y < kFixedZero ? -forward.y : forward.y;
    const Fixed absZ = forward.z < kFixedZero ? -forward.z : forward.z;
    FixedVec3 axis = absX <= absY && absX <= absZ
        ? FixedVec3{kFixedOne, kFixedZero, kFixedZero}
        : absY <= absZ
            ? FixedVec3{kFixedZero, kFixedOne, kFixedZero}
            : FixedVec3{kFixedZero, kFixedZero, kFixedOne};
    return normalizedOr({
        forward.y * axis.z - forward.z * axis.y,
        forward.z * axis.x - forward.x * axis.z,
        forward.x * axis.y - forward.y * axis.x,
    }, {kFixedZero, kFixedZero, kFixedOne});
}

[[nodiscard]] FixedVec3 turnToward(const FixedVec3& current,
                                   const FixedVec3& desired,
                                   Fixed maximumRadians) noexcept {
    const FixedVec3 from = normalizedOr(current, {kFixedOne, kFixedZero, kFixedZero});
    const FixedVec3 to = normalizedOr(desired, from);
    const Fixed pi = Fixed::from_raw(13493037704ll);
    const Fixed step = clampFixed(maximumRadians, kFixedZero, pi);
    if (step <= kFixedZero) return from;
    const math::q32_32_sincos rotation = math::fixed_sincos(step);
    const Fixed cosine = clampFixed(dot(from, to), -kFixedOne, kFixedOne);
    if (cosine >= rotation.cosine) return to;
    FixedVec3 perpendicular = subtract(to, scale(from, cosine));
    perpendicular = normalizedOr(perpendicular, deterministicPerpendicular(from));
    return normalizedOr(add(scale(from, rotation.cosine),
                            scale(perpendicular, rotation.sine)), from);
}

[[nodiscard]] Fixed deterministicSignedUnit(
    ObjectId object, uint64_t tick, uint64_t lane) noexcept {
    uint64_t value = object.value ^ (tick + 0x9E3779B97F4A7C15ull +
        (lane << 32u));
    value ^= value >> 30u;
    value *= 0xBF58476D1CE4E5B9ull;
    value ^= value >> 27u;
    value *= 0x94D049BB133111EBull;
    value ^= value >> 31u;
    const int64_t signedSample = static_cast<int64_t>(
        (value >> 32u) & 0xFFFFFFFFull) - 0x80000000ll;
    return Fixed::from_fraction(signedSample, 0x80000000ll);
}

// Positive fixed values share their Q32.32 scale, so dividing their raw
// values yields the unscaled ratio. This avoids a floating ceil at the
// simulation boundary and caps malformed content before it can allocate or
// iterate an impractical path length.
[[nodiscard]] uint32_t ceilPositiveRatio(Fixed numerator, Fixed denominator) noexcept {
    if (numerator <= kFixedZero || denominator <= kFixedZero) return 0;
    const uint64_t top = static_cast<uint64_t>(numerator.raw());
    const uint64_t bottom = static_cast<uint64_t>(denominator.raw());
    if (bottom == 0) return 0;
    uint64_t result = top / bottom;
    if (top % bottom != 0 && result < kMaximumPathSegments) ++result;
    return static_cast<uint32_t>(std::min<uint64_t>(result, kMaximumPathSegments));
}

[[nodiscard]] Fixed fraction(uint32_t numerator, uint32_t denominator) noexcept {
    if (denominator == 0) return kFixedZero;
    return Fixed::from_fraction(static_cast<int64_t>(numerator),
                                static_cast<int64_t>(denominator));
}

[[nodiscard]] FixedVec3 cubicPoint(const ObjectProjectileComponent& projectile, Fixed t) noexcept {
    t = clampUnit(t);
    const Fixed inverse = kFixedOne - t;
    const Fixed b0 = inverse * inverse * inverse;
    const Fixed b1 = kFixedThree * inverse * inverse * t;
    const Fixed b2 = kFixedThree * inverse * t * t;
    const Fixed b3 = t * t * t;
    return {
        b0 * projectile.start.x + b1 * projectile.controlOne.x + b2 * projectile.controlTwo.x + b3 * projectile.target.x,
        b0 * projectile.start.y + b1 * projectile.controlOne.y + b2 * projectile.controlTwo.y + b3 * projectile.target.y,
        b0 * projectile.start.z + b1 * projectile.controlOne.z + b2 * projectile.controlTwo.z + b3 * projectile.target.z,
    };
}

[[nodiscard]] FixedVec3 cubicPointAtStep(const ObjectProjectileComponent& projectile,
                                          uint32_t step) noexcept {
    // BezierSegment::getSegmentPoints() uses N samples including P0 and P3.
    // Its legacy N <= 1 corner case is uninitialised; the modern system makes
    // that edge deterministic by using the authored destination.
    const Fixed t = projectile.pathSegments > 1u
        ? fraction(step, projectile.pathSegments - 1u)
        : kFixedOne;
    return cubicPoint(projectile, t);
}

[[nodiscard]] FixedVec3 flightPathDirectionAtStep(
    const ObjectProjectileComponent& projectile, uint32_t step) noexcept {
    // RefCode's first path point is the launch point, but it already orients
    // that first frame toward point one.  Later frames use the immediately
    // preceding sampled curve point; after the final point the last tangent
    // remains in effect for lifespan-triggered detonation.
    if (projectile.pathSegments <= 1u) {
        return subtract(projectile.target, projectile.start);
    }
    const uint32_t clampedStep = std::min(step, projectile.pathSegments - 1u);
    if (clampedStep == 0u) {
        return subtract(cubicPointAtStep(projectile, 1u),
                        cubicPointAtStep(projectile, 0u));
    }
    return subtract(cubicPointAtStep(projectile, clampedStep),
                    cubicPointAtStep(projectile, clampedStep - 1u));
}

void refreshFlightPathForward(ObjectProjectileComponent& projectile,
                              uint32_t step) noexcept {
    if (!projectile.orientToFlightPath) {
        projectile.hasFlightPathForward = false;
        return;
    }
    const FixedVec3 tangent = flightPathDirectionAtStep(projectile, step);
    const Fixed tangentLength = length(tangent);
    if (tangentLength <= kFixedSegmentEpsilon) return;
    projectile.flightPathForward = scale(tangent, kFixedOne / tangentLength);
    projectile.hasFlightPathForward = true;
}

void projectFlightPathYaw(ecs::registry& registry, ecs::entity entity,
                           TransformComponent& transform,
                           const ObjectProjectileComponent& projectile) noexcept {
    // RefCode deliberately leaves the current Physics attitude alone for a
    // TumbleRandomly shell, even if OrientToFlightPath was also authored.
    // Keep the fixed tangent separately for later collision/damage work;
    // only this legacy yaw projection is suppressed.
    if (!projectile.orientToFlightPath || projectile.tumbleRandomly ||
        !projectile.hasFlightPathForward) {
        return;
    }
    const Fixed planar = planarLength(projectile.flightPathForward);
    if (planar <= kFixedSegmentEpsilon) return;
    const Fixed yaw = math::fixed_atan2(
        projectile.flightPathForward.y,
        projectile.flightPathForward.x);
    writeAuthoritativeObjectYaw(registry, entity, yaw);
    static_cast<void>(transform);
}

[[nodiscard]] std::optional<Fixed> maxTerrainHeightAlongLine(
    const game::terrain::TerrainLogic& terrain, const FixedVec3& start,
    const FixedVec3& target) noexcept {
    Fixed highest = maxFixed(start.z, target.z);
    if (!terrain.isLoaded()) return highest;

    // RefCode traverses physical PartitionCells, deliberately independent of
    // the active logical boundary, and succeeds if any cell is touched. The
    // modern TerrainMap exposes the same boundary rule; it uses heightfield
    // cells (four-corner maxima) instead of legacy cached PartitionCell
    // heights, which is a conservative, finer-grained approximation.
    const std::optional<int64_t> terrainMaximum =
        terrain.map().maxPhysicalCellHeightAlongLineRaw(
            start.x.raw(), start.y.raw(), target.x.raw(), target.y.raw());
    if (!terrainMaximum) return std::nullopt;
    highest = maxFixed(highest, Fixed::from_raw(*terrainMaximum));
    return highest;
}

[[nodiscard]] bool rebuildDumbControls(ObjectProjectileComponent& projectile,
                                       const game::terrain::TerrainLogic& terrain) noexcept {
    const std::optional<Fixed> highest =
        maxTerrainHeightAlongLine(terrain, projectile.start, projectile.target);
    if (!highest) return false;
    const FixedVec3 delta = subtract(projectile.target, projectile.start);
    projectile.controlOne = {
        projectile.start.x + delta.x * projectile.firstPercentIndent,
        projectile.start.y + delta.y * projectile.firstPercentIndent,
        *highest + projectile.firstHeight,
    };
    projectile.controlTwo = {
        projectile.start.x + delta.x * projectile.secondPercentIndent,
        projectile.start.y + delta.y * projectile.secondPercentIndent,
        *highest + projectile.secondHeight,
    };
    return true;
}

[[nodiscard]] Fixed adaptiveBezierLength(const FixedVec3& p0, const FixedVec3& p1,
                                         const FixedVec3& p2, const FixedVec3& p3,
                                         uint32_t depth = 0) noexcept {
    constexpr uint32_t kMaximumSubdivisionDepth = 16;
    const Fixed chord = length(subtract(p3, p0));
    const Fixed controlPolygon = length(subtract(p1, p0)) + length(subtract(p2, p1)) +
        length(subtract(p3, p2));
    // BezierSegment::getApproximateLength() uses a tolerance of one game
    // unit, recursively splits at 0.5, then returns the mean of chord and
    // control-polygon lengths.
    if (depth >= kMaximumSubdivisionDepth || controlPolygon - chord <= kFixedOne) {
        return (chord + controlPolygon) * kFixedHalf;
    }

    const FixedVec3 p01 = scale(add(p0, p1), kFixedHalf);
    const FixedVec3 p12 = scale(add(p1, p2), kFixedHalf);
    const FixedVec3 p23 = scale(add(p2, p3), kFixedHalf);
    const FixedVec3 p012 = scale(add(p01, p12), kFixedHalf);
    const FixedVec3 p123 = scale(add(p12, p23), kFixedHalf);
    const FixedVec3 middle = scale(add(p012, p123), kFixedHalf);
    return adaptiveBezierLength(p0, p01, p012, middle, depth + 1u) +
        adaptiveBezierLength(middle, p123, p23, p3, depth + 1u);
}

[[nodiscard]] Fixed approximateBezierLength(const ObjectProjectileComponent& projectile) noexcept {
    return adaptiveBezierLength(projectile.start, projectile.controlOne,
                                projectile.controlTwo, projectile.target);
}

} // namespace engine::object_projectile_detail
