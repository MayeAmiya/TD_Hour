#include "game/render/LocomotorDrawPresentation.h"

#include <algorithm>
#include <cstdint>

#include "core/math/fixed/q32_32_trig.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/definition/ObjectKindOf.h"

namespace engine {
namespace {

using Fixed = math::q32_32;

const Fixed kZero{};
const Fixed kOne{int32_t{1}};
const Fixed kTwo{int32_t{2}};
const Fixed kHalf = Fixed::from_fraction(1, 2);
const Fixed kPointTwo = Fixed::from_fraction(1, 5);
const Fixed kPointEight = Fixed::from_fraction(4, 5);
const Fixed kHalfPi = Fixed::from_raw(6746518852ll);
const Fixed kTwoPi = Fixed::from_raw(26986075409ll);

[[nodiscard]] Fixed wrapRadians(Fixed value) noexcept {
    if (value > kTwoPi || value < -kTwoPi) {
        value = Fixed::from_raw(value.raw() % kTwoPi.raw());
    }
    return value;
}

void clearAttitude(ObjectLocomotorDrawPresentationState& state) noexcept {
    state.pitch = {};
    state.pitchRate = {};
    state.roll = {};
    state.rollRate = {};
    state.yaw = {};
    state.accelerationPitch = {};
    state.accelerationPitchRate = {};
    state.accelerationRoll = {};
    state.accelerationRollRate = {};
    state.yawModulator = {};
    state.pitchModulator = {};
    state.overlapHeight = {};
    state.overlapHeightVelocity = {};
    state.outputPitch = {};
    state.outputRoll = {};
    state.outputYaw = {};
    state.outputVerticalOffset = {};
    state.wobbleDirection = 1;
    state.velocityInitialized = false;
}

void advanceThrust(
    const ObjectLocomotionComponent& locomotion,
    ObjectLocomotorDrawPresentationState& state) noexcept {
    const Fixed wobbleRate = Fixed::abs(locomotion.thrustWobbleRate);
    Fixed minimum = locomotion.thrustMinimumWobble;
    Fixed maximum = locomotion.thrustMaximumWobble;
    if (minimum > maximum) std::swap(minimum, maximum);
    if (wobbleRate > kZero && minimum != maximum) {
        if (state.wobbleDirection >= 0) {
            const Fixed step = state.pitch < maximum - wobbleRate * kTwo
                ? wobbleRate : wobbleRate * kHalf;
            state.pitch += step;
            state.yaw += step;
            if (state.pitch >= maximum) state.wobbleDirection = -1;
        } else {
            const Fixed step = state.pitch >= minimum + wobbleRate * kTwo
                ? wobbleRate : wobbleRate * kHalf;
            state.pitch -= step;
            state.yaw -= step;
            if (state.pitch <= minimum) state.wobbleDirection = 1;
        }
        state.pitch = Fixed::clamp(state.pitch, minimum, maximum);
        state.yaw = wrapRadians(state.yaw);
    }
    state.roll = wrapRadians(state.roll + locomotion.thrustRoll);
    state.outputPitch = state.pitch;
    state.outputRoll = state.roll;
    state.outputYaw = state.yaw;
}

void advanceHoverOrWings(
    const ObjectLocomotionComponent& locomotion,
    const ObjectFixedTransformComponent& transform,
    const ObjectPhysicsComponent* physics, Fixed deltaSeconds,
    ObjectLocomotorDrawPresentationState& state) noexcept {
    const Fixed pitchStiffness = Fixed::clamp(
        locomotion.pitchStiffness, kZero, kOne);
    const Fixed rollStiffness = Fixed::clamp(
        locomotion.rollStiffness, kZero, kOne);
    const Fixed pitchDamping = Fixed::clamp(
        locomotion.pitchDamping, kZero, kOne);
    const Fixed rollDamping = Fixed::clamp(
        locomotion.rollDamping, kZero, kOne);
    const Fixed axialDamping = Fixed::clamp(
        locomotion.uniformAxialDamping, kZero, kOne);

    state.pitchRate += -pitchStiffness * state.pitch -
        pitchDamping * state.pitchRate;
    state.rollRate += -rollStiffness * state.roll -
        rollDamping * state.rollRate;
    state.pitch += state.pitchRate * axialDamping;
    state.roll += state.rollRate * axialDamping;

    state.accelerationPitchRate +=
        -pitchStiffness * state.accelerationPitch -
        pitchDamping * state.accelerationPitchRate;
    state.accelerationRollRate +=
        -rollStiffness * state.accelerationRoll -
        rollDamping * state.accelerationRollRate;
    state.accelerationPitch += state.accelerationPitchRate;
    state.accelerationRoll += state.accelerationRollRate;

    // RefCode publishes total pitch/roll before this frame's velocity and
    // acceleration impulses are accumulated; those impulses affect the next
    // Drawable update. Preserve that one-frame phase relationship.
    state.outputPitch = state.pitch + state.accelerationPitch;
    state.outputRoll = state.roll + state.accelerationRoll;

    const math::q32_32_sincos heading =
        math::fixed_sincos(transform.yawRadians);
    Fixed velocityX = heading.cosine * locomotion.forwardSpeed;
    Fixed velocityY = heading.sine * locomotion.forwardSpeed;
    Fixed velocityZ = locomotion.verticalSpeed;
    if (physics &&
        (physics->velocityUnitsPerSecond.x != kZero ||
         physics->velocityUnitsPerSecond.y != kZero ||
         physics->velocityUnitsPerSecond.z != kZero)) {
        velocityX = physics->velocityUnitsPerSecond.x;
        velocityY = physics->velocityUnitsPerSecond.y;
        velocityZ = physics->velocityUnitsPerSecond.z;
    }
    const Fixed frameVelocityX = velocityX * deltaSeconds;
    const Fixed frameVelocityY = velocityY * deltaSeconds;
    const Fixed frameVelocityZ = velocityZ * deltaSeconds;
    const Fixed accelerationX = state.velocityInitialized
        ? frameVelocityX - state.previousVelocityXPerFrame : kZero;
    const Fixed accelerationY = state.velocityInitialized
        ? frameVelocityY - state.previousVelocityYPerFrame : kZero;
    const Fixed accelerationZ = state.velocityInitialized
        ? frameVelocityZ - state.previousVelocityZPerFrame : kZero;
    state.previousVelocityXPerFrame = frameVelocityX;
    state.previousVelocityYPerFrame = frameVelocityY;
    state.previousVelocityZPerFrame = frameVelocityZ;
    state.velocityInitialized = true;

    const Fixed forwardVelocity =
        heading.cosine * frameVelocityX + heading.sine * frameVelocityY;
    const Fixed lateralVelocity =
        -heading.sine * frameVelocityX + heading.cosine * frameVelocityY;
    const Fixed forwardAcceleration =
        heading.cosine * accelerationX + heading.sine * accelerationY;
    const Fixed lateralAcceleration =
        -heading.sine * accelerationX + heading.cosine * accelerationY;
    const bool motive = frameVelocityX != kZero || frameVelocityY != kZero ||
        frameVelocityZ != kZero || accelerationZ != kZero;
    if (motive) {
        const Fixed horizontalSpeed = Fixed::sqrt(
            frameVelocityX * frameVelocityX +
            frameVelocityY * frameVelocityY);
        if (locomotion.pitchByZVelocityFactor != kZero &&
            (horizontalSpeed != kZero || frameVelocityZ != kZero)) {
            state.pitch -= locomotion.pitchByZVelocityFactor *
                math::fixed_atan2(frameVelocityZ, horizontalSpeed);
        }
        state.pitch -=
            locomotion.forwardVelocityPitchFactor * forwardVelocity;
        state.roll -=
            locomotion.lateralVelocityRollFactor * lateralVelocity;
        state.accelerationPitchRate -=
            locomotion.forwardAccelerationPitchFactor * forwardAcceleration;
        state.accelerationRollRate -=
            locomotion.lateralAccelerationRollFactor * lateralAcceleration;
    }

    const Fixed accelerationLimit = Fixed::abs(
        locomotion.accelerationPitchLimitRadians);
    const Fixed decelerationLimit = Fixed::abs(
        locomotion.decelerationPitchLimitRadians);
    state.accelerationPitch = Fixed::clamp(
        state.accelerationPitch, -accelerationLimit, decelerationLimit);
    state.accelerationRoll = Fixed::clamp(
        state.accelerationRoll, -accelerationLimit, decelerationLimit);

    state.yawModulator = wrapRadians(
        state.yawModulator + locomotion.rudderCorrectionRate);
    state.pitchModulator = wrapRadians(
        state.pitchModulator + locomotion.elevatorCorrectionRate);
    state.outputYaw = locomotion.rudderCorrectionDegree *
        math::fixed_sin(state.yawModulator);
    state.outputPitch += locomotion.elevatorCorrectionDegree *
        math::fixed_cos(state.pitchModulator);
}

[[nodiscard]] uint64_t stablePresentationRandom(
    ObjectId object, uint64_t tick, uint32_t lane) noexcept {
    uint64_t value = static_cast<uint64_t>(object.value) |
        (static_cast<uint64_t>(lane) << 32u);
    value ^= tick + 0x9e3779b97f4a7c15ull + (value << 6u) +
        (value >> 2u);
    value ^= value >> 30u;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27u;
    value *= 0x94d049bb133111ebull;
    value ^= value >> 31u;
    return value;
}

[[nodiscard]] Fixed stableSignedUnit(
    ObjectId object, uint64_t tick, uint32_t lane) noexcept {
    const uint64_t value = stablePresentationRandom(object, tick, lane);
    const int32_t sample = static_cast<int32_t>(
        (value >> 48u) & 0xffffu) - 32768;
    return Fixed::from_fraction(sample, 32768);
}

// RefCode Drawable::calcPhysicsXformWheels/Motorcycle adds a client-random
// rough-road kick on top of the ordinary terrain/acceleration chassis spring.
// TD already publishes that ordinary chassis attitude through Physics, so this
// routine owns only the additive Drawable-local bounce. Advancing it once per
// confirmed frame keeps the visual deterministic without feeding it back into
// movement, collision, targeting, replay or networking state.
void advanceWheelBounce(
    const ObjectIdentityComponent& identity,
    const ObjectLocomotionComponent& locomotion,
    const ObjectPhysicsComponent* physics, Fixed deltaSeconds,
    bool grounded, uint64_t confirmedTick,
    ObjectLocomotorDrawPresentationState& state) noexcept {
    Fixed speed = Fixed::abs(locomotion.forwardSpeed);
    if (physics) {
        const LogicFixedVec3& velocity = physics->velocityUnitsPerSecond;
        const Fixed physicsSpeedSquared =
            velocity.x * velocity.x + velocity.y * velocity.y +
            velocity.z * velocity.z;
        if (physicsSpeedSquared > kZero)
            speed = Fixed::sqrt(physicsSpeedSquared);
    }

    const Fixed maximumSpeed = Fixed::abs(locomotion.maximumSpeed);
    const Fixed kickPerFrame = Fixed::abs(
        locomotion.bounceAngularVelocityRadiansPerSecond) * deltaSeconds;
    if (grounded && maximumSpeed > kZero &&
        speed > maximumSpeed / Fixed{int32_t{10}} &&
        kickPerFrame > kZero) {
        const Fixed factor = speed / maximumSpeed;
        if (Fixed::abs(state.pitchRate) < factor * kickPerFrame /
                Fixed{int32_t{4}} &&
            Fixed::abs(state.rollRate) < factor * kickPerFrame /
                Fixed{int32_t{8}}) {
            const Fixed pitchKick = factor * kickPerFrame;
            const Fixed rollKick = pitchKick / Fixed{int32_t{2}};
            switch (stablePresentationRandom(
                        identity.id, confirmedTick, 2u) & 3u) {
            case 0u:
                state.pitchRate -= pitchKick;
                state.rollRate -= rollKick;
                break;
            case 1u:
                state.pitchRate += pitchKick;
                state.rollRate -= rollKick;
                break;
            case 2u:
                state.pitchRate -= pitchKick;
                state.rollRate += rollKick;
                break;
            default:
                state.pitchRate += pitchKick;
                state.rollRate += rollKick;
                break;
            }
        }
    }

    const Fixed pitchStiffness = Fixed::clamp(
        locomotion.pitchStiffness, kZero, kOne);
    const Fixed rollStiffness = Fixed::clamp(
        locomotion.rollStiffness, kZero, kOne);
    const Fixed pitchDamping = Fixed::clamp(
        locomotion.pitchDamping, kZero, kOne);
    const Fixed rollDamping = Fixed::clamp(
        locomotion.rollDamping, kZero, kOne);
    const Fixed axialDamping = Fixed::clamp(
        locomotion.uniformAxialDamping, kZero, kOne);
    if (grounded) {
        state.pitchRate +=
            -pitchStiffness * state.pitch - pitchDamping * state.pitchRate;
        if (state.pitchRate > kZero) state.pitchRate *= kHalf;
        state.rollRate +=
            -rollStiffness * state.roll - rollDamping * state.rollRate;
    }
    state.pitch += state.pitchRate * axialDamping;
    state.roll += state.rollRate * axialDamping;
    state.outputPitch = state.pitch;
    state.outputRoll = state.roll;
    state.outputYaw = {};
    state.outputVerticalOffset = {};
}

// RefCode's tread-over-object response belongs to DrawableLocoInfo.  Keep the
// same fake-Z/suspension state here so a tank climbs and rocks visually over a
// crushable object without moving its authoritative transform or footprint.
void advanceTreadOverlap(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity entity, const ObjectIdentityComponent& identity,
    const ObjectLocomotionComponent& locomotion,
    const ObjectFixedTransformComponent& transform,
    const ObjectPhysicsComponent& physics, Fixed deltaSeconds,
    uint64_t confirmedTick,
    ObjectLocomotorDrawPresentationState& state) noexcept {
    Fixed desiredHeight{};
    Fixed groundPitch{};
    Fixed groundRoll{};
    bool hasOverlap = false;

    const std::optional<ecs::entity> overlappedEntity =
        lifecycle.entityFromIdIncludingPending(physics.currentOverlap);
    const ObjectKindOfComponent* overlappedKinds = overlappedEntity
        ? ecs::try_get<ObjectKindOfComponent>(registry, *overlappedEntity)
        : nullptr;
    const bool shrubbery = overlappedKinds && game::objectHasKind(
        overlappedKinds->mask, game::ObjectKindOf::Shrubbery);
    if (overlappedEntity && !shrubbery) {
        const ObjectGeometryComponent* ourGeometry =
            ecs::try_get<ObjectGeometryComponent>(registry, entity);
        const ObjectGeometryComponent* otherGeometry =
            ecs::try_get<ObjectGeometryComponent>(registry,
                                                   *overlappedEntity);
        const ObjectFixedTransformComponent* otherTransform =
            ecs::try_get<ObjectFixedTransformComponent>(registry,
                                                         *overlappedEntity);
        if (ourGeometry && otherGeometry && otherTransform) {
            const Fixed dx = otherTransform->position.x - transform.position.x;
            const Fixed dy = otherTransform->position.y - transform.position.y;
            const Fixed distanceSquared = dx * dx + dy * dy;
            const Fixed maximumDistance =
                (ourGeometry->boundingCircleRadiusFixed +
                 otherGeometry->boundingCircleRadiusFixed) * kPointEight;
            if (maximumDistance > kZero &&
                distanceSquared < maximumDistance * maximumDistance) {
                const Fixed distance = Fixed::sqrt(distanceSquared);
                const Fixed amount = Fixed::clamp(
                    kOne - distance / maximumDistance, kZero, kOne);
                Fixed height = otherGeometry->heightFixed;
                const bool flat = overlappedKinds && (
                    game::objectHasKind(overlappedKinds->mask,
                                        game::ObjectKindOf::LowOverlappable) ||
                    game::objectHasKind(overlappedKinds->mask,
                                        game::ObjectKindOf::Infantry));
                if (flat) height = kHalf;

                Fixed normalX{};
                Fixed normalY{};
                const Fixed frameVelocityX =
                    physics.velocityUnitsPerSecond.x * deltaSeconds;
                const Fixed frameVelocityY =
                    physics.velocityUnitsPerSecond.y * deltaSeconds;
                const Fixed rough = Fixed::min(
                    kHalf,
                    (frameVelocityX * frameVelocityX +
                     frameVelocityY * frameVelocityY) *
                        Fixed{int32_t{5}});
                if (!flat && amount < kHalf && distance > kZero) {
                    desiredHeight = height * kTwo * amount;
                    normalX = -(dx / distance) * kPointTwo;
                    normalY = -(dy / distance) * kPointTwo;
                } else {
                    desiredHeight = height;
                    normalX = stableSignedUnit(
                        identity.id, confirmedTick, 0u) * rough;
                    normalY = stableSignedUnit(
                        identity.id, confirmedTick, 1u) * rough;
                }
                Fixed normalZ = kOne;
                const Fixed normalLength = Fixed::sqrt(
                    normalX * normalX + normalY * normalY +
                    normalZ * normalZ);
                if (normalLength > kZero) {
                    normalX /= normalLength;
                    normalY /= normalLength;
                    normalZ /= normalLength;
                }
                const math::q32_32_sincos heading =
                    math::fixed_sincos(transform.yawRadians);
                groundPitch =
                    (normalX * heading.cosine + normalY * heading.sine) *
                    kHalfPi;
                groundRoll =
                    (normalX * -heading.sine + normalY * heading.cosine) *
                    kHalfPi;
                hasOverlap = true;
            }
        }
    }

    if (!hasOverlap && physics.previousOverlap &&
        state.overlapHeight > kZero) {
        state.pitchRate += Fixed::from_raw(105414357ll); // PI / 128
    }

    const Fixed pitchStiffness = Fixed::clamp(
        locomotion.pitchStiffness, kZero, kOne);
    const Fixed rollStiffness = Fixed::clamp(
        locomotion.rollStiffness, kZero, kOne);
    const Fixed pitchDamping = Fixed::clamp(
        locomotion.pitchDamping, kZero, kOne);
    const Fixed rollDamping = Fixed::clamp(
        locomotion.rollDamping, kZero, kOne);
    const Fixed axialDamping = Fixed::clamp(
        locomotion.uniformAxialDamping, kZero, kOne);
    if (hasOverlap || state.overlapHeight <= kZero) {
        state.pitchRate += -pitchStiffness * (state.pitch - groundPitch) -
            pitchDamping * state.pitchRate;
        if (state.pitchRate > kZero) state.pitchRate *= kHalf;
        state.rollRate += -rollStiffness * (state.roll - groundRoll) -
            rollDamping * state.rollRate;
    }
    state.pitch += state.pitchRate * axialDamping;
    state.roll += state.rollRate * axialDamping;
    state.outputPitch = state.pitch;
    state.outputRoll = state.roll;
    state.outputYaw = {};

    if (desiredHeight > state.overlapHeight) {
        state.overlapHeight = desiredHeight;
        state.overlapHeightVelocity = {};
    }
    state.outputVerticalOffset = state.overlapHeight * kHalf;
    if (state.overlapHeight > kZero) {
        state.overlapHeightVelocity -= kPointTwo;
        state.overlapHeight += state.overlapHeightVelocity;
    }
    if (state.overlapHeight <= kZero) {
        state.overlapHeight = {};
        state.overlapHeightVelocity = {};
    }
}

} // namespace

void updateLocomotorDrawPresentation(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    math::q32_32 logicDeltaSeconds,
    uint64_t confirmedTick) noexcept {
    logicDeltaSeconds = Fixed::max(kZero, logicDeltaSeconds);
    const auto view = ecs::view<
        RenderModelComponent, const ObjectIdentityComponent,
        const ObjectLocomotionComponent,
        const ObjectFixedTransformComponent>(registry);
    for (const ecs::entity entity : view) {
        RenderModelComponent& visual =
            view.template get<RenderModelComponent>(entity);
        const ObjectLocomotionComponent& locomotion =
            view.template get<const ObjectLocomotionComponent>(entity);
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        const ObjectFixedTransformComponent& transform =
            view.template get<const ObjectFixedTransformComponent>(entity);
        ObjectLocomotorDrawPresentationState& state = visual.locomotorDraw;
        const ObjectLocomotorDrawPresentationState previous = state;
        const uint8_t appearance = static_cast<uint8_t>(locomotion.appearance);
        if (!state.initialized || state.appearanceTag != appearance) {
            clearAttitude(state);
            state.previousVelocityXPerFrame = {};
            state.previousVelocityYPerFrame = {};
            state.previousVelocityZPerFrame = {};
            state.appearanceTag = appearance;
            state.initialized = true;
        }

        switch (locomotion.appearance) {
        case game::LocomotorAppearance::Treads:
            if (const ObjectPhysicsComponent* physics =
                    ecs::try_get<ObjectPhysicsComponent>(registry, entity)) {
                advanceTreadOverlap(
                    registry, lifecycle, entity, identity, locomotion,
                    transform, *physics, logicDeltaSeconds, confirmedTick,
                    state);
            } else {
                clearAttitude(state);
            }
            break;
        case game::LocomotorAppearance::Thrust:
            advanceThrust(locomotion, state);
            break;
        case game::LocomotorAppearance::Hover:
        case game::LocomotorAppearance::Wings:
            advanceHoverOrWings(
                locomotion, transform,
                ecs::try_get<ObjectPhysicsComponent>(registry, entity),
                logicDeltaSeconds, state);
            break;
        case game::LocomotorAppearance::FourWheels:
        case game::LocomotorAppearance::Motorcycle: {
            const ObjectPhysicsComponent* physics =
                ecs::try_get<ObjectPhysicsComponent>(registry, entity);
            const ObjectAirborneComponent* airborne =
                ecs::try_get<ObjectAirborneComponent>(registry, entity);
            const bool grounded =
                !(airborne && airborne->isAirborne) &&
                (!physics ||
                 physics->state != ObjectPhysicsMotionState::Airborne);
            advanceWheelBounce(
                identity, locomotion, physics, logicDeltaSeconds,
                grounded, confirmedTick, state);
            break;
        }
        default:
            state.outputPitch = {};
            state.outputRoll = {};
            state.outputYaw = {};
            state.outputVerticalOffset = {};
            break;
        }
        if (!(state == previous)) {
            markObjectDirty(
                registry, entity, ObjectDirtyDomain::RenderExtraction);
        }
    }
}

} // namespace engine
