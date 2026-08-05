#pragma once

#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <limits>

namespace engine {

// Shared deterministic contact projection for gameplay collision consumers.
// GeometryInfo's canonical Z convention is preserved: Sphere position is its
// centre; Cylinder/Box position is the bottom centre.
struct ObjectCollisionContact final {
    LogicFixedVec3 normal{}; // from left toward right
    math::q32_32 penetration{};
};

namespace collision_detail {

using Fixed = math::q32_32;

struct Shape final {
    ObjectGeometryShape kind = ObjectGeometryShape::Sphere;
    LogicFixedVec3 position{};
    Fixed yaw{};
    Fixed major{int32_t{1}};
    Fixed minor{int32_t{1}};
    Fixed height{int32_t{1}};
    Fixed sphereRadius{int32_t{1}};
};

[[nodiscard]] inline Shape shapeFrom(
    const LogicFixedVec3& position, Fixed yaw,
    const ObjectGeometryComponent& geometry) noexcept {
    return {
        .kind = geometry.shape,
        .position = position,
        .yaw = yaw,
        .major = Fixed::max(Fixed{}, geometry.majorRadiusFixed),
        .minor = Fixed::max(Fixed{}, geometry.minorRadiusFixed),
        .height = Fixed::max(Fixed{}, geometry.heightFixed),
        // RefCode's Sphere collision dispatch uses GeometryInfo major radius;
        // boundingSphereRadius is only a broad-phase projection and may be
        // deliberately larger for authored models.
        .sphereRadius = Fixed::max(Fixed{}, geometry.majorRadiusFixed),
    };
}

struct Axis2 final { Fixed x{}; Fixed y{}; };

[[nodiscard]] inline Fixed dot(Axis2 a, Axis2 b) noexcept {
    return a.x * b.x + a.y * b.y;
}

[[nodiscard]] inline Fixed dot(Axis2 axis, Fixed x, Fixed y) noexcept {
    return axis.x * x + axis.y * y;
}

[[nodiscard]] inline Axis2 axisX(const Shape& shape) noexcept {
    return {math::fixed_cos(shape.yaw), math::fixed_sin(shape.yaw)};
}

[[nodiscard]] inline Axis2 axisY(const Shape& shape) noexcept {
    const Axis2 x = axisX(shape);
    return {-x.y, x.x};
}

[[nodiscard]] inline Fixed horizontalRadius(const Shape& shape) noexcept {
    return shape.kind == ObjectGeometryShape::Sphere
        ? shape.sphereRadius : shape.major;
}

struct Interval final { Fixed minimum{}; Fixed maximum{}; };

[[nodiscard]] inline Interval zInterval(const Shape& shape) noexcept {
    if (shape.kind == ObjectGeometryShape::Sphere) {
        return {shape.position.z - shape.sphereRadius,
                shape.position.z + shape.sphereRadius};
    }
    return {shape.position.z, shape.position.z + shape.height};
}

[[nodiscard]] inline bool selectVerticalContact(
    const Shape& left, const Shape& right, Fixed horizontalPenetration,
    ObjectCollisionContact& contact) noexcept {
    const Interval a = zInterval(left);
    const Interval b = zInterval(right);
    const Fixed overlap = Fixed::min(a.maximum, b.maximum) -
        Fixed::max(a.minimum, b.minimum);
    if (overlap < Fixed{}) return false;
    if (overlap < horizontalPenetration) {
        const Fixed centreA = (a.minimum + a.maximum) / Fixed{int32_t{2}};
        const Fixed centreB = (b.minimum + b.maximum) / Fixed{int32_t{2}};
        contact.normal = {Fixed{}, Fixed{},
            centreB >= centreA ? Fixed{int32_t{1}}
                               : Fixed{int32_t{-1}}};
        contact.penetration = overlap;
    }
    return true;
}

[[nodiscard]] inline bool circleCircle(
    const Shape& left, const Shape& right,
    ObjectCollisionContact& contact) noexcept {
    const Fixed dx = right.position.x - left.position.x;
    const Fixed dy = right.position.y - left.position.y;
    const Fixed radius = horizontalRadius(left) + horizontalRadius(right);
    const Fixed distanceSquared = dx * dx + dy * dy;
    if (distanceSquared > radius * radius) return false;
    const Fixed distance = Fixed::sqrt(distanceSquared);
    if (distance > Fixed{}) {
        contact.normal = {dx / distance, dy / distance, Fixed{}};
        contact.penetration = radius - distance;
    } else {
        contact.normal = {Fixed{int32_t{1}}, Fixed{}, Fixed{}};
        contact.penetration = radius;
    }
    return selectVerticalContact(left, right, contact.penetration, contact);
}

[[nodiscard]] inline bool sphereSphere(
    const Shape& left, const Shape& right,
    ObjectCollisionContact& contact) noexcept {
    const Fixed dx = right.position.x - left.position.x;
    const Fixed dy = right.position.y - left.position.y;
    const Fixed dz = right.position.z - left.position.z;
    const Fixed radius = left.sphereRadius + right.sphereRadius;
    const Fixed distanceSquared = dx * dx + dy * dy + dz * dz;
    if (distanceSquared > radius * radius) return false;
    const Fixed distance = Fixed::sqrt(distanceSquared);
    if (distance > Fixed{}) {
        contact.normal = {dx / distance, dy / distance, dz / distance};
        contact.penetration = Fixed::max(Fixed{}, radius - distance);
    } else {
        contact.normal = {Fixed{int32_t{1}}, Fixed{}, Fixed{}};
        contact.penetration = radius;
    }
    return true;
}

// Exact closest-point test between a sphere and a finite vertical cylinder.
// `contact.normal` is always directed from sphere toward cylinder.
[[nodiscard]] inline bool sphereCylinder(
    const Shape& sphere, const Shape& cylinder,
    ObjectCollisionContact& contact) noexcept {
    const Fixed dx = sphere.position.x - cylinder.position.x;
    const Fixed dy = sphere.position.y - cylinder.position.y;
    const Fixed radialSquared = dx * dx + dy * dy;
    const Fixed radialDistance = Fixed::sqrt(radialSquared);
    const Fixed cylinderRadius = cylinder.major;
    Fixed closestX = sphere.position.x;
    Fixed closestY = sphere.position.y;
    if (radialDistance > cylinderRadius && radialDistance > Fixed{}) {
        closestX = cylinder.position.x + dx * cylinderRadius / radialDistance;
        closestY = cylinder.position.y + dy * cylinderRadius / radialDistance;
    }
    const Fixed bottom = cylinder.position.z;
    const Fixed top = bottom + cylinder.height;
    const Fixed closestZ = std::clamp(sphere.position.z, bottom, top);
    const Fixed toShapeX = closestX - sphere.position.x;
    const Fixed toShapeY = closestY - sphere.position.y;
    const Fixed toShapeZ = closestZ - sphere.position.z;
    const Fixed distanceSquared = toShapeX * toShapeX +
        toShapeY * toShapeY + toShapeZ * toShapeZ;
    const Fixed radiusSquared = sphere.sphereRadius * sphere.sphereRadius;
    if (distanceSquared > radiusSquared) return false;
    if (distanceSquared > Fixed{}) {
        const Fixed distance = Fixed::sqrt(distanceSquared);
        contact.normal = {toShapeX / distance, toShapeY / distance,
                          toShapeZ / distance};
        contact.penetration = Fixed::max(
            Fixed{}, sphere.sphereRadius - distance);
        return true;
    }

    // The centre is inside the cylinder volume. Choose the nearest face and
    // direct the normal toward the cylinder interior, matching the existing
    // left-to-right contact convention for contained shapes.
    const Fixed sideDistance = Fixed::max(
        Fixed{}, cylinderRadius - radialDistance);
    const Fixed bottomDistance = sphere.position.z - bottom;
    const Fixed topDistance = top - sphere.position.z;
    if (sideDistance <= bottomDistance && sideDistance <= topDistance) {
        if (radialDistance > Fixed{}) {
            contact.normal = {-dx / radialDistance, -dy / radialDistance,
                              Fixed{}};
        } else {
            contact.normal = {Fixed{int32_t{1}}, Fixed{}, Fixed{}};
        }
        contact.penetration = sphere.sphereRadius + sideDistance;
    } else if (bottomDistance <= topDistance) {
        contact.normal = {Fixed{}, Fixed{}, Fixed{int32_t{1}}};
        contact.penetration = sphere.sphereRadius + bottomDistance;
    } else {
        contact.normal = {Fixed{}, Fixed{}, Fixed{int32_t{-1}}};
        contact.penetration = sphere.sphereRadius + topDistance;
    }
    return true;
}

// Exact closest-point test between a sphere and an oriented finite box.
// Cylinder-vs-box continues to use the 2D footprint plus canonical Z interval
// path below; only a true sphere needs the rounded 3D corner treatment.
[[nodiscard]] inline bool sphereBox(
    const Shape& sphere, const Shape& box,
    ObjectCollisionContact& contact) noexcept {
    const Axis2 bx = axisX(box);
    const Axis2 by = axisY(box);
    const Fixed dx = sphere.position.x - box.position.x;
    const Fixed dy = sphere.position.y - box.position.y;
    const Fixed localX = dot(bx, dx, dy);
    const Fixed localY = dot(by, dx, dy);
    const Fixed localZ = sphere.position.z - box.position.z;
    const Fixed closestX = std::clamp(localX, -box.major, box.major);
    const Fixed closestY = std::clamp(localY, -box.minor, box.minor);
    const Fixed closestZ = std::clamp(localZ, Fixed{}, box.height);
    const Fixed toShapeX = closestX - localX;
    const Fixed toShapeY = closestY - localY;
    const Fixed toShapeZ = closestZ - localZ;
    const Fixed distanceSquared = toShapeX * toShapeX +
        toShapeY * toShapeY + toShapeZ * toShapeZ;
    const Fixed radiusSquared = sphere.sphereRadius * sphere.sphereRadius;
    if (distanceSquared > radiusSquared) return false;

    Axis2 normalLocal{};
    Fixed normalZ{};
    if (distanceSquared > Fixed{}) {
        const Fixed distance = Fixed::sqrt(distanceSquared);
        normalLocal = {toShapeX / distance, toShapeY / distance};
        normalZ = toShapeZ / distance;
        contact.penetration = Fixed::max(
            Fixed{}, sphere.sphereRadius - distance);
    } else {
        const Fixed faceX = box.major - Fixed::abs(localX);
        const Fixed faceY = box.minor - Fixed::abs(localY);
        const Fixed faceBottom = localZ;
        const Fixed faceTop = box.height - localZ;
        Fixed faceDistance = faceX;
        normalLocal = {localX >= Fixed{} ? Fixed{int32_t{-1}}
                                         : Fixed{int32_t{1}},
                       Fixed{}};
        if (faceY < faceDistance) {
            faceDistance = faceY;
            normalLocal = {Fixed{},
                           localY >= Fixed{} ? Fixed{int32_t{-1}}
                                             : Fixed{int32_t{1}}};
        }
        if (faceBottom < faceDistance) {
            faceDistance = faceBottom;
            normalLocal = {};
            normalZ = Fixed{int32_t{1}};
        }
        if (faceTop < faceDistance) {
            faceDistance = faceTop;
            normalLocal = {};
            normalZ = Fixed{int32_t{-1}};
        }
        contact.penetration = sphere.sphereRadius + faceDistance;
    }
    contact.normal = {
        bx.x * normalLocal.x + by.x * normalLocal.y,
        bx.y * normalLocal.x + by.y * normalLocal.y,
        normalZ,
    };
    return true;
}

[[nodiscard]] inline bool circleBox(
    const Shape& circle, const Shape& box,
    ObjectCollisionContact& contact) noexcept {
    const Axis2 bx = axisX(box);
    const Axis2 by = axisY(box);
    const Fixed dx = circle.position.x - box.position.x;
    const Fixed dy = circle.position.y - box.position.y;
    const Fixed localX = dot(bx, dx, dy);
    const Fixed localY = dot(by, dx, dy);
    const Fixed closestX = std::clamp(localX, -box.major, box.major);
    const Fixed closestY = std::clamp(localY, -box.minor, box.minor);
    const Fixed fromCircleX = closestX - localX;
    const Fixed fromCircleY = closestY - localY;
    const Fixed radius = horizontalRadius(circle);
    const Fixed distanceSquared = fromCircleX * fromCircleX +
        fromCircleY * fromCircleY;
    if (distanceSquared > radius * radius) return false;
    Axis2 normalLocal;
    Fixed penetration{};
    if (distanceSquared > Fixed{}) {
        const Fixed distance = Fixed::sqrt(distanceSquared);
        normalLocal = {fromCircleX / distance, fromCircleY / distance};
        penetration = radius - distance;
    } else {
        const Fixed faceX = box.major - Fixed::abs(localX);
        const Fixed faceY = box.minor - Fixed::abs(localY);
        if (faceX <= faceY) {
            normalLocal = {localX >= Fixed{} ? Fixed{int32_t{-1}}
                                             : Fixed{int32_t{1}},
                           Fixed{}};
            penetration = radius + faceX;
        } else {
            normalLocal = {Fixed{},
                           localY >= Fixed{} ? Fixed{int32_t{-1}}
                                             : Fixed{int32_t{1}}};
            penetration = radius + faceY;
        }
    }
    contact.normal = {
        bx.x * normalLocal.x + by.x * normalLocal.y,
        bx.y * normalLocal.x + by.y * normalLocal.y,
        Fixed{},
    };
    contact.penetration = penetration;
    return selectVerticalContact(circle, box, penetration, contact);
}

[[nodiscard]] inline bool boxBox(
    const Shape& left, const Shape& right,
    ObjectCollisionContact& contact) noexcept {
    const Axis2 ax = axisX(left);
    const Axis2 ay = axisY(left);
    const Axis2 bx = axisX(right);
    const Axis2 by = axisY(right);
    const Fixed dx = right.position.x - left.position.x;
    const Fixed dy = right.position.y - left.position.y;
    const Axis2 axes[4] = {ax, ay, bx, by};
    Fixed minimumOverlap = Fixed::from_raw(
        std::numeric_limits<int64_t>::max());
    Axis2 selected = ax;
    for (const Axis2 axis : axes) {
        const Fixed leftProjection = left.major * Fixed::abs(dot(axis, ax)) +
            left.minor * Fixed::abs(dot(axis, ay));
        const Fixed rightProjection = right.major * Fixed::abs(dot(axis, bx)) +
            right.minor * Fixed::abs(dot(axis, by));
        const Fixed separation = Fixed::abs(dot(axis, dx, dy));
        const Fixed overlap = leftProjection + rightProjection - separation;
        if (overlap < Fixed{}) return false;
        if (overlap < minimumOverlap) {
            minimumOverlap = overlap;
            selected = dot(axis, dx, dy) >= Fixed{}
                ? axis : Axis2{-axis.x, -axis.y};
        }
    }
    contact.normal = {selected.x, selected.y, Fixed{}};
    contact.penetration = minimumOverlap;
    return selectVerticalContact(left, right, minimumOverlap, contact);
}

[[nodiscard]] inline Fixed intervalGap(Interval left,
                                       Interval right) noexcept {
    if (left.maximum < right.minimum)
        return right.minimum - left.maximum;
    if (right.maximum < left.minimum)
        return left.minimum - right.maximum;
    return Fixed{};
}

[[nodiscard]] inline Fixed circleCircleHorizontalGap(
    const Shape& left, const Shape& right) noexcept {
    const Fixed dx = right.position.x - left.position.x;
    const Fixed dy = right.position.y - left.position.y;
    const Fixed distance = Fixed::sqrt(dx * dx + dy * dy);
    return Fixed::max(Fixed{}, distance - horizontalRadius(left) -
        horizontalRadius(right));
}

[[nodiscard]] inline Fixed circleBoxHorizontalGap(
    const Shape& circle, const Shape& box) noexcept {
    const Axis2 bx = axisX(box);
    const Axis2 by = axisY(box);
    const Fixed dx = circle.position.x - box.position.x;
    const Fixed dy = circle.position.y - box.position.y;
    const Fixed localX = dot(bx, dx, dy);
    const Fixed localY = dot(by, dx, dy);
    const Fixed closestX = std::clamp(localX, -box.major, box.major);
    const Fixed closestY = std::clamp(localY, -box.minor, box.minor);
    const Fixed gapX = localX - closestX;
    const Fixed gapY = localY - closestY;
    return Fixed::max(Fixed{}, Fixed::sqrt(gapX * gapX + gapY * gapY) -
        horizontalRadius(circle));
}

// Maximum positive SAT axis separation is a conservative lower bound on the
// Euclidean distance between two OBBs. It is zero exactly when their 2D
// footprints overlap, which is sufficient for conservative advancement.
[[nodiscard]] inline Fixed boxBoxHorizontalGap(
    const Shape& left, const Shape& right) noexcept {
    const Axis2 ax = axisX(left);
    const Axis2 ay = axisY(left);
    const Axis2 bx = axisX(right);
    const Axis2 by = axisY(right);
    const Fixed dx = right.position.x - left.position.x;
    const Fixed dy = right.position.y - left.position.y;
    const Axis2 axes[4] = {ax, ay, bx, by};
    Fixed maximumGap{};
    for (const Axis2 axis : axes) {
        const Fixed leftProjection =
            left.major * Fixed::abs(dot(axis, ax)) +
            left.minor * Fixed::abs(dot(axis, ay));
        const Fixed rightProjection =
            right.major * Fixed::abs(dot(axis, bx)) +
            right.minor * Fixed::abs(dot(axis, by));
        const Fixed gap = Fixed::abs(dot(axis, dx, dy)) -
            leftProjection - rightProjection;
        maximumGap = Fixed::max(maximumGap, gap);
    }
    return maximumGap;
}

[[nodiscard]] inline Fixed sphereSphereGap(
    const Shape& left, const Shape& right) noexcept {
    const Fixed dx = right.position.x - left.position.x;
    const Fixed dy = right.position.y - left.position.y;
    const Fixed dz = right.position.z - left.position.z;
    return Fixed::max(Fixed{}, Fixed::sqrt(
        dx * dx + dy * dy + dz * dz) -
        left.sphereRadius - right.sphereRadius);
}

[[nodiscard]] inline Fixed sphereCylinderGap(
    const Shape& sphere, const Shape& cylinder) noexcept {
    const Fixed dx = sphere.position.x - cylinder.position.x;
    const Fixed dy = sphere.position.y - cylinder.position.y;
    const Fixed radialDistance = Fixed::sqrt(dx * dx + dy * dy);
    const Fixed radialGap = Fixed::max(
        Fixed{}, radialDistance - cylinder.major);
    Fixed verticalGap{};
    if (sphere.position.z < cylinder.position.z)
        verticalGap = cylinder.position.z - sphere.position.z;
    else if (sphere.position.z > cylinder.position.z + cylinder.height)
        verticalGap = sphere.position.z -
            (cylinder.position.z + cylinder.height);
    return Fixed::max(Fixed{}, Fixed::sqrt(
        radialGap * radialGap + verticalGap * verticalGap) -
        sphere.sphereRadius);
}

[[nodiscard]] inline Fixed sphereBoxGap(
    const Shape& sphere, const Shape& box) noexcept {
    const Axis2 bx = axisX(box);
    const Axis2 by = axisY(box);
    const Fixed dx = sphere.position.x - box.position.x;
    const Fixed dy = sphere.position.y - box.position.y;
    const Fixed localX = dot(bx, dx, dy);
    const Fixed localY = dot(by, dx, dy);
    const Fixed localZ = sphere.position.z - box.position.z;
    const Fixed closestX = std::clamp(localX, -box.major, box.major);
    const Fixed closestY = std::clamp(localY, -box.minor, box.minor);
    const Fixed closestZ = std::clamp(localZ, Fixed{}, box.height);
    const Fixed gapX = localX - closestX;
    const Fixed gapY = localY - closestY;
    const Fixed gapZ = localZ - closestZ;
    return Fixed::max(Fixed{}, Fixed::sqrt(
        gapX * gapX + gapY * gapY + gapZ * gapZ) -
        sphere.sphereRadius);
}

[[nodiscard]] inline Fixed shapeSeparationLowerBound(
    const Shape& left, const Shape& right) noexcept {
    if (left.kind == ObjectGeometryShape::Sphere &&
        right.kind == ObjectGeometryShape::Sphere)
        return sphereSphereGap(left, right);
    if (left.kind == ObjectGeometryShape::Sphere &&
        right.kind == ObjectGeometryShape::Cylinder)
        return sphereCylinderGap(left, right);
    if (left.kind == ObjectGeometryShape::Cylinder &&
        right.kind == ObjectGeometryShape::Sphere)
        return sphereCylinderGap(right, left);
    if (left.kind == ObjectGeometryShape::Sphere &&
        right.kind == ObjectGeometryShape::Box)
        return sphereBoxGap(left, right);
    if (left.kind == ObjectGeometryShape::Box &&
        right.kind == ObjectGeometryShape::Sphere)
        return sphereBoxGap(right, left);

    Fixed horizontalGap{};
    if (left.kind == ObjectGeometryShape::Box &&
        right.kind == ObjectGeometryShape::Box) {
        horizontalGap = boxBoxHorizontalGap(left, right);
    } else if (left.kind == ObjectGeometryShape::Box) {
        horizontalGap = circleBoxHorizontalGap(right, left);
    } else if (right.kind == ObjectGeometryShape::Box) {
        horizontalGap = circleBoxHorizontalGap(left, right);
    } else {
        horizontalGap = circleCircleHorizontalGap(left, right);
    }
    const Fixed verticalGap = intervalGap(zInterval(left), zInterval(right));
    return Fixed::sqrt(horizontalGap * horizontalGap +
                       verticalGap * verticalGap);
}

[[nodiscard]] inline LogicFixedVec3 lerpPosition(
    const LogicFixedVec3& start, const LogicFixedVec3& travel,
    Fixed time) noexcept {
    return {
        start.x + travel.x * time,
        start.y + travel.y * time,
        start.z + travel.z * time,
    };
}

constexpr Fixed kPi = Fixed::from_raw(13493037705ll);
constexpr Fixed kTwoPi = Fixed::from_raw(26986075409ll);

// Choose one deterministic path around the circle.  Interpolating raw yaw
// values directly can turn a small +pi/-pi boundary crossing into an almost
// complete revolution and makes angular CCD depend on angle normalization.
[[nodiscard]] inline Fixed shortestAngleDelta(
    Fixed start, Fixed end) noexcept {
    int64_t delta = end.raw() % kTwoPi.raw() -
        start.raw() % kTwoPi.raw();
    if (delta > kPi.raw()) delta -= kTwoPi.raw();
    if (delta < -kPi.raw()) delta += kTwoPi.raw();
    return Fixed::from_raw(delta);
}

[[nodiscard]] inline Fixed rotationalSweepRadius(
    const Shape& shape) noexcept {
    if (shape.kind != ObjectGeometryShape::Box) return Fixed{};
    return Fixed::sqrt(shape.major * shape.major +
                       shape.minor * shape.minor);
}

} // namespace collision_detail

[[nodiscard]] inline bool computeObjectCollisionContact(
    const LogicFixedVec3& leftPosition, math::q32_32 leftYaw,
    const ObjectGeometryComponent& leftGeometry,
    const LogicFixedVec3& rightPosition, math::q32_32 rightYaw,
    const ObjectGeometryComponent& rightGeometry,
    ObjectCollisionContact& contact) noexcept {
    using namespace collision_detail;
    const Shape left = shapeFrom(leftPosition, leftYaw, leftGeometry);
    const Shape right = shapeFrom(rightPosition, rightYaw, rightGeometry);
    if (left.kind == ObjectGeometryShape::Sphere &&
        right.kind == ObjectGeometryShape::Sphere) {
        return sphereSphere(left, right, contact);
    }
    if (left.kind == ObjectGeometryShape::Sphere &&
        right.kind == ObjectGeometryShape::Cylinder) {
        return sphereCylinder(left, right, contact);
    }
    if (left.kind == ObjectGeometryShape::Cylinder &&
        right.kind == ObjectGeometryShape::Sphere) {
        if (!sphereCylinder(right, left, contact)) return false;
        contact.normal = {-contact.normal.x, -contact.normal.y,
                          -contact.normal.z};
        return true;
    }
    if (left.kind == ObjectGeometryShape::Sphere &&
        right.kind == ObjectGeometryShape::Box) {
        return sphereBox(left, right, contact);
    }
    if (left.kind == ObjectGeometryShape::Box &&
        right.kind == ObjectGeometryShape::Sphere) {
        if (!sphereBox(right, left, contact)) return false;
        contact.normal = {-contact.normal.x, -contact.normal.y,
                          -contact.normal.z};
        return true;
    }
    if (left.kind == ObjectGeometryShape::Box &&
        right.kind == ObjectGeometryShape::Box) {
        return boxBox(left, right, contact);
    }
    if (left.kind == ObjectGeometryShape::Box) {
        if (!circleBox(right, left, contact)) return false;
        contact.normal = {-contact.normal.x, -contact.normal.y,
                          -contact.normal.z};
        return true;
    }
    if (right.kind == ObjectGeometryShape::Box) {
        return circleBox(left, right, contact);
    }
    return circleCircle(left, right, contact);
}

// Deterministic continuous collision for translated and yaw-rotated authored
// shapes. Conservative advancement bounds both linear travel and the arc
// travelled by every box corner, so it cannot step past the first contact.
// A fixed bisection then returns the earliest Q32.32 time in [0,1].
[[nodiscard]] inline bool computeObjectSweptCollisionContact(
    const LogicFixedVec3& leftStart,
    const LogicFixedVec3& leftEnd, math::q32_32 leftStartYaw,
    math::q32_32 leftEndYaw,
    const ObjectGeometryComponent& leftGeometry,
    const LogicFixedVec3& rightStart,
    const LogicFixedVec3& rightEnd, math::q32_32 rightStartYaw,
    math::q32_32 rightEndYaw,
    const ObjectGeometryComponent& rightGeometry,
    math::q32_32& timeOfImpact,
    ObjectCollisionContact& contact) noexcept {
    using namespace collision_detail;
    const Fixed zero{};
    const Fixed one{int32_t{1}};
    const LogicFixedVec3 leftTravel{
        leftEnd.x - leftStart.x,
        leftEnd.y - leftStart.y,
        leftEnd.z - leftStart.z,
    };
    const LogicFixedVec3 rightTravel{
        rightEnd.x - rightStart.x,
        rightEnd.y - rightStart.y,
        rightEnd.z - rightStart.z,
    };
    const LogicFixedVec3 relativeTravel{
        rightTravel.x - leftTravel.x,
        rightTravel.y - leftTravel.y,
        rightTravel.z - leftTravel.z,
    };
    const Fixed relativeSpeed = Fixed::sqrt(
        relativeTravel.x * relativeTravel.x +
        relativeTravel.y * relativeTravel.y +
        relativeTravel.z * relativeTravel.z);
    const Fixed leftYawTravel = shortestAngleDelta(
        leftStartYaw, leftEndYaw);
    const Fixed rightYawTravel = shortestAngleDelta(
        rightStartYaw, rightEndYaw);
    const Shape leftAuthoredShape = shapeFrom(
        leftStart, leftStartYaw, leftGeometry);
    const Shape rightAuthoredShape = shapeFrom(
        rightStart, rightStartYaw, rightGeometry);
    const Fixed closingSpeed = relativeSpeed +
        Fixed::abs(leftYawTravel) * rotationalSweepRadius(leftAuthoredShape) +
        Fixed::abs(rightYawTravel) * rotationalSweepRadius(rightAuthoredShape);
    const auto positionsAt = [&](Fixed time,
                                 LogicFixedVec3& leftPosition,
                                 LogicFixedVec3& rightPosition) {
        leftPosition = lerpPosition(leftStart, leftTravel, time);
        rightPosition = lerpPosition(rightStart, rightTravel, time);
    };
    const auto contactAt = [&](Fixed time,
                               ObjectCollisionContact& output) {
        LogicFixedVec3 leftPosition;
        LogicFixedVec3 rightPosition;
        positionsAt(time, leftPosition, rightPosition);
        return computeObjectCollisionContact(
            leftPosition, leftStartYaw + leftYawTravel * time, leftGeometry,
            rightPosition, rightStartYaw + rightYawTravel * time,
            rightGeometry, output);
    };

    if (contactAt(zero, contact)) {
        timeOfImpact = zero;
        return true;
    }
    if (closingSpeed <= zero) return false;

    Fixed lower = zero;
    constexpr size_t kMaximumAdvanceIterations = 64;
    for (size_t iteration = 0;
         iteration < kMaximumAdvanceIterations; ++iteration) {
        LogicFixedVec3 leftPosition;
        LogicFixedVec3 rightPosition;
        positionsAt(lower, leftPosition, rightPosition);
        Shape left = shapeFrom(
            leftPosition, leftStartYaw + leftYawTravel * lower,
            leftGeometry);
        Shape right = shapeFrom(
            rightPosition, rightStartYaw + rightYawTravel * lower,
            rightGeometry);
        const Fixed separation = shapeSeparationLowerBound(left, right);
        Fixed advance = separation / closingSpeed;
        if (advance.raw() <= 0) advance = Fixed::from_raw(1);
        if (advance > one - lower) return false;
        const Fixed upper = lower + advance;
        ObjectCollisionContact upperContact;
        if (!contactAt(upper, upperContact)) {
            lower = upper;
            continue;
        }

        Fixed firstNonContact = lower;
        Fixed firstContact = upper;
        constexpr size_t kBisectionIterations = 36;
        for (size_t bisection = 0;
             bisection < kBisectionIterations; ++bisection) {
            if (firstContact.raw() - firstNonContact.raw() <= 1) break;
            const Fixed middle = Fixed::from_raw(
                firstNonContact.raw() +
                (firstContact.raw() - firstNonContact.raw()) / 2);
            ObjectCollisionContact middleContact;
            if (contactAt(middle, middleContact))
                firstContact = middle;
            else
                firstNonContact = middle;
        }
        timeOfImpact = firstContact;
        return contactAt(firstContact, contact);
    }
    return false;
}

// Fixed-yaw convenience retained for specialized consumers whose controller
// owns translation but does not publish an angular interval.
[[nodiscard]] inline bool computeObjectSweptCollisionContact(
    const LogicFixedVec3& leftStart,
    const LogicFixedVec3& leftEnd, math::q32_32 leftYaw,
    const ObjectGeometryComponent& leftGeometry,
    const LogicFixedVec3& rightStart,
    const LogicFixedVec3& rightEnd, math::q32_32 rightYaw,
    const ObjectGeometryComponent& rightGeometry,
    math::q32_32& timeOfImpact,
    ObjectCollisionContact& contact) noexcept {
    return computeObjectSweptCollisionContact(
        leftStart, leftEnd, leftYaw, leftYaw, leftGeometry,
        rightStart, rightEnd, rightYaw, rightYaw, rightGeometry,
        timeOfImpact, contact);
}

} // namespace engine
