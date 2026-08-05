#include "VehicleDrawPresentation.h"

#include <algorithm>
#include <cmath>

#include "core/math/wwmath/base/wwmath_core.h"
#include "core/math/fixed/q32_32_trig.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"

namespace engine
{
namespace
{

[[nodiscard]] float wrapUnit(float value) noexcept
{
    if (!std::isfinite(value))
        return 0.0f;
    return value - std::floor(value);
}

[[nodiscard]] float wrapRadians(float value) noexcept
{
    return std::isfinite(value) ? math::normalize_angle(value) : 0.0f;
}

void dampWheelHeight(float target, float extension,
                     float& current) noexcept
{
    // RefCode damps only downward travel; upward compression is immediate.
    current = target < current ? current + (target - current) * 0.5f
                               : target;
    current = std::max(current, extension);
}

void advanceWheelSuspension(
    const game::terrain::TerrainLogic& terrain,
    const ObjectFixedTransformComponent& transform,
    const ObjectLocomotionComponent& locomotion,
    const ObjectPhysicsComponent* physics,
    const ObjectGeometryComponent* geometry, bool airborne,
    VehicleDrawChannelPresentationState& state) noexcept
{
    const bool wheelAppearance =
        locomotion.appearance == game::LocomotorAppearance::FourWheels ||
        locomotion.appearance == game::LocomotorAppearance::Motorcycle;
    if (!locomotion.hasSuspension || !wheelAppearance ||
        !terrain.isLoaded() || !physics || !geometry) {
        state.frontLeftWheelHeight = 0.0f;
        state.frontRightWheelHeight = 0.0f;
        state.rearLeftWheelHeight = 0.0f;
        state.rearRightWheelHeight = 0.0f;
        return;
    }

    using Fixed = math::q32_32;
    const Fixed zero{};
    const Fixed halfPi = Fixed{3.14159265358979323846} /
        Fixed{int32_t{2}};
    const Fixed extension = locomotion.maximumWheelExtension;
    if (airborne) {
        const Fixed ground = Fixed::from_raw(terrain.groundHeightRaw(
            transform.position.x.raw(), transform.position.y.raw()));
        const Fixed heightAboveGround = transform.position.z - ground;
        const Fixed target = heightAboveGround > -extension
            ? extension : zero;
        // ZH's four-wheel path extends only the rear suspension while
        // airborne. The front pair keeps its last grounded travel.
        dampWheelHeight(
            target.to_float(), extension.to_float(),
            state.rearLeftWheelHeight);
        dampWheelHeight(
            target.to_float(), extension.to_float(),
            state.rearRightWheelHeight);
        return;
    }

    Fixed groundPitch{};
    Fixed groundRoll{};
    const container::Array<int64_t, 3> normal =
        terrain.map().groundNormalRaw(
            transform.position.x.raw(), transform.position.y.raw());
    const Fixed normalX = Fixed::from_raw(normal[0]);
    const Fixed normalY = Fixed::from_raw(normal[1]);
    const Fixed normalZ = Fixed::from_raw(normal[2]);
    if (normalZ > zero) {
        const math::q32_32_sincos heading =
            math::fixed_sincos(transform.yawRadians);
        groundPitch = (normalX * heading.cosine +
                       normalY * heading.sine) * halfPi;
        groundRoll = (normalX * -heading.sine +
                      normalY * heading.cosine) * halfPi;
    }
    const Fixed pitchHeight = geometry->majorRadiusFixed * math::fixed_sin(
        physics->pitch - groundPitch);
    const Fixed rollHeight = geometry->minorRadiusFixed * math::fixed_sin(
        physics->roll - groundRoll);
    const Fixed spring = Fixed::from_fraction(9, 10);
    Fixed frontLeft{};
    Fixed frontRight{};
    Fixed rearLeft{};
    Fixed rearRight{};
    if (pitchHeight < zero) {
        frontLeft = spring *
            (pitchHeight / Fixed{int32_t{3}} +
             pitchHeight / Fixed{int32_t{2}});
        frontRight = frontLeft;
        rearLeft = -pitchHeight / Fixed{int32_t{2}} +
            pitchHeight / Fixed{int32_t{4}};
        rearRight = rearLeft;
    } else {
        frontLeft = -pitchHeight / Fixed{int32_t{4}} +
            pitchHeight / Fixed{int32_t{2}};
        frontRight = frontLeft;
        rearLeft = spring *
            (-pitchHeight / Fixed{int32_t{2}} -
             pitchHeight / Fixed{int32_t{3}});
        rearRight = rearLeft;
    }
    if (rollHeight > zero) {
        const Fixed lowered = -spring *
            (rollHeight / Fixed{int32_t{3}} +
             rollHeight / Fixed{int32_t{2}});
        const Fixed raised = rollHeight / Fixed{int32_t{2}} -
            rollHeight / Fixed{int32_t{4}};
        frontRight += lowered;
        rearRight += lowered;
        frontLeft += raised;
        rearLeft += raised;
    } else {
        const Fixed raised = -rollHeight / Fixed{int32_t{2}} +
            rollHeight / Fixed{int32_t{4}};
        const Fixed lowered = spring *
            (rollHeight / Fixed{int32_t{3}} +
             rollHeight / Fixed{int32_t{2}});
        frontRight += raised;
        rearRight += raised;
        frontLeft += lowered;
        rearLeft += lowered;
    }
    const float extensionFloat = extension.to_float();
    dampWheelHeight(frontLeft.to_float(), extensionFloat,
                    state.frontLeftWheelHeight);
    dampWheelHeight(frontRight.to_float(), extensionFloat,
                    state.frontRightWheelHeight);
    dampWheelHeight(rearLeft.to_float(), extensionFloat,
                    state.rearLeftWheelHeight);
    dampWheelHeight(rearRight.to_float(), extensionFloat,
                    state.rearRightWheelHeight);
    // MaxWheelCompression is intentionally not clamped here: the matching
    // ZH block is commented out, so the authored field has no reachable
    // retail behavior. Enabling it would be a new rule, not compatibility.
}

[[nodiscard]] math::q32_32 wheelChassisVerticalLift(
    const game::terrain::TerrainLogic& terrain,
    const ObjectFixedTransformComponent& transform,
    const ObjectLocomotionComponent& locomotion,
    const ObjectPhysicsComponent* physics,
    const ObjectGeometryComponent* geometry, bool airborne,
    const ObjectLocomotorDrawPresentationState& drawState) noexcept
{
    using Fixed = math::q32_32;
    const Fixed zero{};
    const bool wheelAppearance =
        locomotion.appearance == game::LocomotorAppearance::FourWheels ||
        locomotion.appearance == game::LocomotorAppearance::Motorcycle;
    if (!wheelAppearance || !terrain.isLoaded() || !physics || !geometry)
        return zero;

    const Fixed halfPi = Fixed{3.14159265358979323846} /
        Fixed{int32_t{2}};
    Fixed groundPitch{};
    Fixed groundRoll{};
    const container::Array<int64_t, 3> normal =
        terrain.map().groundNormalRaw(
            transform.position.x.raw(), transform.position.y.raw());
    const Fixed normalX = Fixed::from_raw(normal[0]);
    const Fixed normalY = Fixed::from_raw(normal[1]);
    const Fixed normalZ = Fixed::from_raw(normal[2]);
    if (normalZ > zero) {
        const math::q32_32_sincos heading =
            math::fixed_sincos(transform.yawRadians);
        groundPitch = (normalX * heading.cosine +
                       normalY * heading.sine) * halfPi;
        groundRoll = (normalX * -heading.sine +
                      normalY * heading.cosine) * halfPi;
    }

    const Fixed relativePitch =
        physics->pitch + drawState.outputPitch - groundPitch;
    const Fixed relativeRoll =
        physics->roll + drawState.outputRoll - groundRoll;
    const Fixed pitchHeight = geometry->majorRadiusFixed *
        math::fixed_sin(relativePitch);
    const Fixed rollHeight = geometry->minorRadiusFixed *
        math::fixed_sin(relativeRoll);
    Fixed divisor{int32_t{4}};
    const Fixed pitch = Fixed::abs(relativePitch);
    const Fixed piOverEight = halfPi / Fixed{int32_t{4}};
    if (!airborne && pitch > piOverEight) {
        // Literal Drawable::calcPhysicsXformWheels high-angle clearance:
        // ((4*PI/8) + (pitch-PI/8)) / pitch.
        divisor = (halfPi + pitch - piOverEight) / pitch;
    }
    if (divisor <= zero) divisor = Fixed{int32_t{4}};
    return Fixed::abs(pitchHeight) / divisor +
        Fixed::abs(rollHeight) / divisor;
}

} // namespace

VehicleMotiveSample sampleVehicleMotive(
    const ObjectPhysicsComponent* physics,
    const ObjectLocomotionComponent* locomotion) noexcept
{
    if (physics)
    {
        const float x = physics->velocityUnitsPerSecond.x.to_float();
        const float y = physics->velocityUnitsPerSecond.y.to_float();
        if (std::isfinite(x) && std::isfinite(y))
        {
            const float speed = std::sqrt(x * x + y * y);
            // Physics is authoritative whenever it publishes real planar
            // motion (impulse, collision slide, scripted force, free body).
            // A locomotion-owned ground vehicle normally leaves this velocity
            // at zero, which is the explicit boundary at which its confirmed
            // forward speed becomes the fallback instead.
            if (speed > 0.0f)
            {
                return {
                    .planarSpeedUnitsPerSecond = speed,
                    .moving = true,
                };
            }
        }
    }
    if (locomotion)
    {
        const float value = locomotion->forwardSpeed.to_float();
        if (std::isfinite(value))
        {
            const float speed = std::abs(value);
            return {
                .planarSpeedUnitsPerSecond = speed,
                .moving = speed > 0.0f ||
                    (locomotion->hasActiveMove &&
                     locomotion->state != ObjectLocomotionState::Idle),
            };
        }
    }
    return {};
}

void advanceVehicleDrawChannel(const game::VehicleDrawVisualRecipe& recipe,
                               const VehicleDrawConfirmedInput& input,
                               VehicleDrawChannelPresentationState& state) noexcept
{
    if (!recipe.enabled() || !(input.deltaSeconds > 0.0f) || !std::isfinite(input.deltaSeconds))
    {
        return;
    }
    const float speed =
        std::isfinite(input.planarSpeedUnitsPerSecond) ? std::max(0.0f, input.planarSpeedUnitsPerSecond) : 0.0f;
    const float legacySpeed = speed * input.deltaSeconds;
    const float yaw = wrapRadians(input.yawRadians);
    const float yawDelta = state.initialized ? wrapRadians(yaw - state.previousYaw) : 0.0f;
    state.turningLeft = yawDelta > kVehicleDrawTurningEpsilon;
    state.turningRight = yawDelta < -kVehicleDrawTurningEpsilon;
    const bool turning = state.turningLeft || state.turningRight;
    state.moving = input.moving;
    state.grounded = input.grounded;

    float targetSteering =
        !turning ? 0.0f : (state.turningLeft ? 1.0f : -1.0f) * std::max(0.0f, input.frontWheelTurnAngleRadians);
    if (input.movingBackward)
        targetSteering = -targetSteering;
    state.wheelSteeringAngle += (targetSteering - state.wheelSteeringAngle) / kVehicleDrawWheelSteeringSmoothness;

    if (recipe.kind == game::VehicleDrawKind::Truck && input.relativeGoalAngleRadians)
    {
        const float goal = wrapRadians(*input.relativeGoalAngleRadians);
        float desired = state.wheelSteeringAngle * recipe.cabRotationMultiplier;
        if (goal < 0.0f)
        {
            desired = std::clamp(desired, goal, 0.0f);
        }
        else
        {
            desired = std::clamp(desired, 0.0f, goal);
        }
        state.cabRotation += (desired - state.cabRotation) * recipe.rotationDamping;
        const float trailerDesired = -state.wheelSteeringAngle * recipe.trailerRotationMultiplier;
        state.trailerRotation += (trailerDesired - state.trailerRotation) * recipe.rotationDamping;
    }
    else if (recipe.kind == game::VehicleDrawKind::Truck)
    {
        state.cabRotation +=
            (state.wheelSteeringAngle * recipe.cabRotationMultiplier - state.cabRotation) * recipe.rotationDamping;
        state.trailerRotation +=
            (-state.wheelSteeringAngle * recipe.trailerRotationMultiplier - state.trailerRotation) *
            recipe.rotationDamping;
    }

    // W3DTruckDraw explicitly reverses wheel motion through the active
    // locomotor. W3DTankTruckDraw does not perform that check in RefCode.
    const bool reversesWheelMotion =
        recipe.kind == game::VehicleDrawKind::Truck && input.movingBackward;
    const float signedLegacySpeed = reversesWheelMotion ? -legacySpeed : legacySpeed;
    if (recipe.kind == game::VehicleDrawKind::Truck || recipe.kind == game::VehicleDrawKind::TankTruck)
    {
        const float oldPowerslideAddition = state.powersliding ? recipe.powerslideRotationAddition : 0.0f;
        state.frontWheelRotation =
            wrapRadians(state.frontWheelRotation + recipe.tireRotationMultiplier * signedLegacySpeed);
        state.rearWheelRotation = wrapRadians(
            state.rearWheelRotation +
            recipe.tireRotationMultiplier *
                (signedLegacySpeed +
                 (reversesWheelMotion ? -oldPowerslideAddition : oldPowerslideAddition)));
    }

    float acceleration =
        std::isfinite(input.planarAccelerationUnitsPerSecondSq) ? input.planarAccelerationUnitsPerSecondSq : 0.0f;
    float velocityAccelerationDot = std::isfinite(input.velocityAccelerationDot)
        ? input.velocityAccelerationDot : 0.0f;
    if (state.initialized)
    {
        // Ordinary TD locomotion owns fixed Transform directly, so its
        // acceleration is not necessarily mirrored into PhysicsBehavior.
        // Recover the same Draw-side acceleration fact from consecutive
        // confirmed speed samples without feeding it back into simulation.
        const float speedDeltaPerSecond =
            (speed - state.previousSpeed) / input.deltaSeconds;
        if (std::abs(speedDeltaPerSecond) > acceleration)
        {
            acceleration = std::abs(speedDeltaPerSecond);
            velocityAccelerationDot = speedDeltaPerSecond * speed;
        }
    }
    state.accelerating =
        acceleration > kVehicleDrawAccelerationThresholdPerSecondSq && velocityAccelerationDot > 0.0f;
    const uint32_t landedAfterFrames = state.airborneFrames;
    if (input.grounded)
    {
        state.airborneFrames = 0;
    }
    else if (state.airborneFrames != UINT32_MAX)
    {
        ++state.airborneFrames;
    }
    state.powersliding = input.moving && input.grounded && turning;
    const bool wheelEffects = input.moving && input.grounded && !input.hidden;
    state.dustActive = wheelEffects && !recipe.dustParticleSystem.empty();
    state.dirtActive = wheelEffects && state.accelerating && !recipe.dirtParticleSystem.empty();
    state.powerslideActive = wheelEffects && state.powersliding && !recipe.powerslideParticleSystem.empty();
    state.dustSizeMultiplier = std::min(legacySpeed, kVehicleDrawDustSizeCap);
    if (wheelEffects && landedAfterFrames > 3u)
    {
        const float landingFactor = std::min(2.0f, 1.0f + static_cast<float>(landedAfterFrames) / 16.0f);
        state.dustSizeMultiplier = landingFactor * kVehicleDrawDustSizeCap;
        state.landingTriggerSequence = input.confirmedTick;
    }

    const float speedSquared = legacySpeed * legacySpeed;
    state.treadDebrisActive =
        speedSquared > kVehicleDrawDebrisSpeedSquaredThreshold && !input.hidden;
    state.debrisVelocityXyMultiplier = std::min(1.0f, 0.5f * legacySpeed + 0.1f);
    state.debrisVelocityZMultiplier = std::min(1.0f, legacySpeed + 0.1f);
    state.debrisBurstMultiplier = state.debrisVelocityZMultiplier;

    const float maximumSpeed =
        std::isfinite(input.maximumSpeedUnitsPerSecond) ? std::max(0.0f, input.maximumSpeedUnitsPerSecond) : 0.0f;
    const float speedFraction = maximumSpeed > 0.0f ? speed / maximumSpeed : 0.0f;
    const float treadDelta = recipe.treadAnimationRatePerSecond * input.deltaSeconds;
    if (recipe.kind == game::VehicleDrawKind::Tank && turning && speedFraction < recipe.treadPivotSpeedFraction)
    {
        const float pivotDelta = state.turningRight ? -treadDelta : treadDelta;
        state.treadLeftOffset = wrapUnit(state.treadLeftOffset + pivotDelta);
        state.treadRightOffset = wrapUnit(state.treadRightOffset - pivotDelta);
    }
    else if (input.moving && speedFraction >= recipe.treadDriveSpeedFraction)
    {
        state.treadLeftOffset = wrapUnit(state.treadLeftOffset - treadDelta);
        state.treadRightOffset = wrapUnit(state.treadRightOffset - treadDelta);
        state.treadMiddleOffset = wrapUnit(state.treadMiddleOffset - treadDelta);
    }

    state.previousYaw = yaw;
    state.previousSpeed = speed;
    state.initialized = true;
}

void updateVehicleDrawPresentation(
    ecs::registry& registry, const game::terrain::TerrainLogic& terrain,
    float deltaSeconds, uint64_t confirmedTick) noexcept
{
    if (!(deltaSeconds > 0.0f) || !std::isfinite(deltaSeconds))
        return;
    const auto view =
        ecs::view<VehicleDrawPresentationComponent, const TransformComponent, const ThingTemplateComponent>(registry);
    for (const ecs::entity entity : view)
    {
        VehicleDrawPresentationComponent& presentation = view.template get<VehicleDrawPresentationComponent>(entity);
        const TransformComponent& transform = view.template get<const TransformComponent>(entity);
        const ThingTemplateComponent& source = view.template get<const ThingTemplateComponent>(entity);
        if (!source.archetype)
            continue;
        const game::ThingTemplate& type = source.archetype->templateData;
        const ObjectLocomotionComponent* locomotion = ecs::try_get<ObjectLocomotionComponent>(registry, entity);
        const ObjectPhysicsComponent* physics = ecs::try_get<ObjectPhysicsComponent>(registry, entity);
        const ObjectAirborneComponent* airborne = ecs::try_get<ObjectAirborneComponent>(registry, entity);
        const ObjectFixedTransformComponent* fixedTransform =
            ecs::try_get<ObjectFixedTransformComponent>(registry, entity);
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(registry, entity);
        RenderModelComponent* visual =
            ecs::try_get<RenderModelComponent>(registry, entity);
        const VehicleMotiveSample motive =
            sampleVehicleMotive(physics, locomotion);
        const float speed = motive.planarSpeedUnitsPerSecond;
        float acceleration = 0.0f;
        float velocityAccelerationDot = 0.0f;
        if (physics)
        {
            const float ax = physics->previousAcceleration.x.to_float();
            const float ay = physics->previousAcceleration.y.to_float();
            const float vx = physics->velocityUnitsPerSecond.x.to_float();
            const float vy = physics->velocityUnitsPerSecond.y.to_float();
            acceleration = std::sqrt(ax * ax + ay * ay);
            velocityAccelerationDot = ax * vx + ay * vy;
        }
        const bool grounded =
            !(airborne && airborne->isAirborne) && (!physics || physics->state != ObjectPhysicsMotionState::Airborne);
        const bool moving = motive.moving;
        std::optional<float> relativeGoalAngle;
        if (locomotion && locomotion->hasActiveMove)
        {
            relativeGoalAngle = math::normalize_angle(
                std::atan2(locomotion->goal.y.to_float() - transform.y,
                           locomotion->goal.x.to_float() - transform.x) -
                transform.rotation);
        }
        bool presentationChanged = false;
        for (VehicleDrawChannelPresentationState& state : presentation.channels)
        {
            if (state.channelIndex >= type.drawVisualChannels.size())
                continue;
            const game::VehicleDrawVisualRecipe& recipe = type.drawVisualChannels[state.channelIndex].vehicleDraw;
            const VehicleDrawChannelPresentationState previous = state;
            advanceVehicleDrawChannel(
                recipe,
                {
                    .deltaSeconds = deltaSeconds,
                    .yawRadians = transform.rotation,
                    .planarSpeedUnitsPerSecond = speed,
                    .maximumSpeedUnitsPerSecond = locomotion
                        ? locomotion->maximumSpeed.to_float() : 0.0f,
                    .planarAccelerationUnitsPerSecondSq = acceleration,
                    .velocityAccelerationDot = velocityAccelerationDot,
                    .frontWheelTurnAngleRadians = locomotion
                        ? locomotion->frontWheelTurnAngleRadians.to_float()
                        : 0.0f,
                    .relativeGoalAngleRadians = relativeGoalAngle,
                    .confirmedTick = confirmedTick,
                    .moving = moving,
                    .movingBackward = locomotion && locomotion->movingBackward,
                    .grounded = grounded,
                    .hidden = visual && visual->hidden,
                },
                state);
            if (fixedTransform && locomotion) {
                advanceWheelSuspension(
                    terrain, *fixedTransform, *locomotion, physics,
                    geometry, !grounded, state);
            }
            presentationChanged = presentationChanged || !(state == previous);
        }
        if (visual && fixedTransform && locomotion &&
            (locomotion->appearance == game::LocomotorAppearance::FourWheels ||
             locomotion->appearance == game::LocomotorAppearance::Motorcycle)) {
            const math::q32_32 previousLift =
                visual->locomotorDraw.outputVerticalOffset;
            visual->locomotorDraw.outputVerticalOffset =
                wheelChassisVerticalLift(
                    terrain, *fixedTransform, *locomotion, physics,
                    geometry, !grounded, visual->locomotorDraw);
            presentationChanged = presentationChanged ||
                previousLift != visual->locomotorDraw.outputVerticalOffset;
        }
        presentation.confirmedTick = confirmedTick;
        if (presentationChanged) {
            markObjectDirty(
                registry, entity,
                ObjectDirtyDomain::RenderExtraction);
        }
    }
}

} // namespace engine
