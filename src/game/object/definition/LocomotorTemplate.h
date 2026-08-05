#pragma once

#include "core/container/hash_containers.h"
#include "game/data/base/LegacyIniLoadType.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <limits>
namespace game {

// These values mirror the original LocomotorSurfaceType mask.  A typed mask
// keeps surface selection out of string comparisons while preserving the
// authored ability for one object to own several locomotors.
enum class LocomotorSurface : uint32_t {
    Ground = 1u << 0u,
    Water = 1u << 1u,
    Cliff = 1u << 2u,
    Air = 1u << 3u,
    Rubble = 1u << 4u,
};

using LocomotorSurfaceMask = uint32_t;

[[nodiscard]] constexpr LocomotorSurfaceMask locomotorSurfaceBit(LocomotorSurface surface) noexcept {
    return static_cast<LocomotorSurfaceMask>(surface);
}

enum class LocomotorAppearance : uint8_t {
    TwoLegs,
    FourWheels,
    Treads,
    Hover,
    Thrust,
    Wings,
    Climber,
    Other,
    Motorcycle,
};

enum class LocomotorZAxisBehavior : uint8_t {
    NoZMotiveForce,
    SeaLevel,
    SurfaceRelativeHeight,
    AbsoluteHeight,
    FixedSurfaceRelativeHeight,
    FixedAbsoluteHeight,
    FixedRelativeToGroundAndBuildings,
    SmoothRelativeToHighestLayer,
};

enum class LocomotorGroupPriority : uint8_t {
    MovesBack,
    MovesMiddle,
    MovesFront,
};

// Simulation-authoritative locomotor scalars. INI parsing may use temporary
// float values, but finalize() quantizes them once into this frozen record.
// Object, AI and projectile runtimes copy these values directly and must not
// perform another float -> fixed conversion when selecting a locomotor.
struct LocomotorAuthoritativeScalars final {
    math::q32_32 maximumSpeed{};
    math::q32_32 damagedMaximumSpeed{};
    math::q32_32 maximumTurnRate{};
    math::q32_32 damagedMaximumTurnRate{};
    math::q32_32 acceleration{};
    math::q32_32 damagedAcceleration{};
    math::q32_32 lift{};
    math::q32_32 damagedLift{};
    math::q32_32 braking{};
    math::q32_32 minimumSpeed{};
    math::q32_32 minimumTurnSpeed{};
    math::q32_32 preferredHeight{};
    math::q32_32 preferredHeightDamping{int32_t{1}};
    math::q32_32 circlingRadius{};
    math::q32_32 extra2DFrictionPerSecond{};
    math::q32_32 speedLimitZ{};
    math::q32_32 maximumThrustAngleRadians{};
    math::q32_32 accelerationPitchLimitRadians{};
    math::q32_32 decelerationPitchLimitRadians{};
    math::q32_32 bounceAngularVelocityRadiansPerSecond{};
    math::q32_32 pitchStiffness{};
    math::q32_32 rollStiffness{};
    math::q32_32 pitchDamping{};
    math::q32_32 rollDamping{};
    math::q32_32 thrustRoll{};
    math::q32_32 thrustWobbleRate{};
    math::q32_32 thrustMinimumWobble{};
    math::q32_32 thrustMaximumWobble{};
    math::q32_32 pitchByZVelocityFactor{};
    math::q32_32 forwardVelocityPitchFactor{};
    math::q32_32 lateralVelocityRollFactor{};
    math::q32_32 forwardAccelerationPitchFactor{};
    math::q32_32 lateralAccelerationRollFactor{};
    math::q32_32 uniformAxialDamping{int32_t{1}};
    math::q32_32 turnPivotOffset{};
    math::q32_32 maximumWheelExtension{};
    math::q32_32 maximumWheelCompression{};
    math::q32_32 frontWheelTurnAngleRadians{};
    math::q32_32 closeEnough{int32_t{1}};
    math::q32_32 slideIntoPlaceMilliseconds{};
    math::q32_32 wanderWidthFactor{};
    math::q32_32 wanderLengthFactor{int32_t{1}};
    math::q32_32 wanderAboutPointRadius{};
    math::q32_32 rudderCorrectionDegree{};
    math::q32_32 rudderCorrectionRate{};
    math::q32_32 elevatorCorrectionDegree{};
    math::q32_32 elevatorCorrectionRate{};
    math::q32_32 airborneTargetingHeight{};
    bool accelerationIsInfinite = false;
    bool damagedAccelerationIsInfinite = false;
    bool brakingIsInfinite = false;
    bool hasFiniteBraking = true;
    bool hasFiniteSpeedLimitZ = false;
    bool hasFiniteAirborneTargetingHeight = false;
    bool preferredHeightIsLowest = false;
};

// Session/runtime database value. It is deliberately incapable of carrying
// loader floats: all real-valued INI fields have already been validated,
// normalized and quantized before this type enters GameContentSnapshot.
// Presentation converts selected values (wheel pose, etc.) to float only at
// extraction time and may never write them back into simulation state.
struct FrozenLocomotorTemplate final {
    container::String name;
    LocomotorSurfaceMask surfaces = 0;
    LocomotorAppearance appearance = LocomotorAppearance::Other;
    LocomotorZAxisBehavior zAxisBehavior =
        LocomotorZAxisBehavior::NoZMotiveForce;
    LocomotorGroupPriority groupPriority =
        LocomotorGroupPriority::MovesMiddle;
    LocomotorAuthoritativeScalars fixed;
    bool closeEnoughDistance3D = false;
    bool stickToGround = false;
    bool canMoveBackwards = false;
    bool locomotorWorksWhenDead = false;
    bool allowMotiveForceWhileAirborne = false;
    bool apply2DFrictionWhenAirborne = false;
    bool downhillOnly = false;
    bool hasSuspension = false;

    [[nodiscard]] bool supportsSurface(LocomotorSurface surface) const noexcept {
        return (surfaces & locomotorSurfaceBit(surface)) != 0;
    }
    [[nodiscard]] bool supportsRuntimeLocomotion() const noexcept {
        return surfaces != 0;
    }
};

// Immutable parsed source for a locomotor. Values intentionally use modern
// seconds-based units because GameSession supplies an explicit deltaSeconds
// to fixed systems. The original parser divided values by its 30 Hz logic
// rate; doing that here and multiplying by deltaSeconds again would slow all
// units by 30x.
struct LocomotorTemplate final {
    // RefCode accepts -1 as a semantic sentinel: move at the lowest viable
    // altitude rather than trying to maintain a positive preferred height.
    // Keep it explicit for the future air/hover controller instead of
    // rejecting valid parachute/freefall locomotors at content load.
    static constexpr float kPreferredHeightLowest = -1.0f;

    container::String name;
    LocomotorSurfaceMask surfaces = 0;
    LocomotorAppearance appearance = LocomotorAppearance::Other;
    LocomotorZAxisBehavior zAxisBehavior = LocomotorZAxisBehavior::NoZMotiveForce;
    LocomotorGroupPriority groupPriority = LocomotorGroupPriority::MovesMiddle;

    float maxSpeedUnitsPerSecond = 0.0f;
    // Negative values are only permitted before finalization; finalize()
    // copies the corresponding undamaged value, exactly like RefCode.
    float damagedMaxSpeedUnitsPerSecond = -1.0f;
    float maxTurnRateRadiansPerSecond = 0.0f;
    float damagedMaxTurnRateRadiansPerSecond = -1.0f;
    float accelerationUnitsPerSecondSq = 0.0f;
    float damagedAccelerationUnitsPerSecondSq = -1.0f;
    float liftUnitsPerSecondSq = 0.0f;
    float damagedLiftUnitsPerSecondSq = -1.0f;
    // Infinity represents the legacy BIGNUM default (instant braking). It
    // is an explicit semantic sentinel, not a magic finite value.
    float brakingUnitsPerSecondSq = std::numeric_limits<float>::infinity();
    float minSpeedUnitsPerSecond = 0.0f;
    float minTurnSpeedUnitsPerSecond = std::numeric_limits<float>::infinity();
    float preferredHeight = 0.0f;
    float preferredHeightDamping = 1.0f;
    float circlingRadius = 0.0f;
    float extra2DFrictionPerSecond = 0.0f;
    float speedLimitZUnitsPerSecond = std::numeric_limits<float>::max();
    float maxThrustAngleRadians = 0.0f;
    float accelerationPitchLimitRadians = 0.0f;
    float decelerationPitchLimitRadians = 0.0f;
    float bounceAngularVelocityRadiansPerSecond = 0.0f;
    float pitchStiffness = 0.1f;
    float rollStiffness = 0.1f;
    float pitchDamping = 0.9f;
    float rollDamping = 0.9f;
    float thrustRoll = 0.0f;
    float thrustWobbleRate = 0.0f;
    float thrustMinimumWobble = 0.0f;
    float thrustMaximumWobble = 0.0f;
    float pitchByZVelocityFactor = 0.0f;
    float forwardVelocityPitchFactor = 0.0f;
    float lateralVelocityRollFactor = 0.0f;
    float forwardAccelerationPitchFactor = 0.0f;
    float lateralAccelerationRollFactor = 0.0f;
    float uniformAxialDamping = 1.0f;
    float turnPivotOffset = 0.0f;
    // Drawable::updateLocoInfo consumes these authored locomotor values for
    // the vehicle Draw wheel pose. Keep them with the immutable locomotor so
    // the confirmed presentation update never invents a steering constant.
    float maximumWheelExtension = 0.0f;
    float maximumWheelCompression = 0.0f;
    float frontWheelTurnAngleRadians = 0.0f;
    float closeEnoughDistance = 1.0f;
    // Authored in milliseconds. It remains independent of the match logic
    // rate until a confirmed controller consumes ultra-accurate movement.
    float slideIntoPlaceMilliseconds = 0.0f;
    float wanderWidthFactor = 0.0f;
    float wanderLengthFactor = 1.0f;
    float wanderAboutPointRadius = 0.0f;
    float rudderCorrectionDegree = 0.0f;
    float rudderCorrectionRate = 0.0f;
    float elevatorCorrectionDegree = 0.0f;
    float elevatorCorrectionRate = 0.0f;
    int32_t airborneTargetingHeight = std::numeric_limits<int32_t>::max();
    bool closeEnoughDistance3D = false;
    bool stickToGround = false;
    bool canMoveBackwards = false;
    bool locomotorWorksWhenDead = false;
    bool allowMotiveForceWhileAirborne = false;
    bool apply2DFrictionWhenAirborne = false;
    bool downhillOnly = false;
    bool hasSuspension = false;
    LocomotorAuthoritativeScalars fixed;
    bool loaded = false;

    [[nodiscard]] bool supportsSurface(LocomotorSurface surface) const noexcept {
        return (surfaces & locomotorSurfaceBit(surface)) != 0;
    }
    [[nodiscard]] bool supportsRuntimeLocomotion() const noexcept {
        // Runtime selection is surface-driven, like RefCode's LocomotorSet.
        // Appearance and Z behavior choose a controller after admission; they
        // must not erase valid aircraft, boats or cliff profiles at spawn.
        return loaded && surfaces != 0;
    }
};

// The only supported authoring -> runtime conversion. Callers must invoke it
// after all base/patch/Map.ini overrides have been merged and finalized.
[[nodiscard]] FrozenLocomotorTemplate freezeLocomotorTemplate(
    const LocomotorTemplate& source) noexcept;

class LocomotorStore {
public:
    static LocomotorStore& instance();

    void clear();
    bool loadFromIni(
        const container::String& filePath,
        ini::LegacyIniLoadType loadType = ini::LegacyIniLoadType::Overwrite);
    const LocomotorTemplate* find(const container::String& name) const;

private:
    container::HashMap<container::String, LocomotorTemplate> m_locomotors;
};

} // namespace game
