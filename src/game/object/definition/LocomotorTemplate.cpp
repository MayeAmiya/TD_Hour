#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentDiagnostics.h"
#include "game/data/base/ContentFloatParsing.h"
#include "LocomotorTemplate.h"

#include "debug/debug.h"
#include "core/data/ini/GeneralsIniParser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
namespace game {
namespace {

[[nodiscard]] container::String lowerAscii(container::StringView value) {
    container::String result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

[[nodiscard]] std::optional<float> parseFiniteFloat(
    container::StringView value, float fallback = 0.0f) {
    return parseContentFloat(value, {
        .source = __FILE__, .block = "Locomotor", .field = "Real",
        .fallback = fallback});
}

[[nodiscard]] std::optional<int32_t> parseInt32(container::StringView value) {
    const container::String owned(value);
    char* end = nullptr;
    const long parsed = std::strtol(owned.c_str(), &end, 10);
    if (end == owned.c_str() || parsed < std::numeric_limits<int32_t>::min() ||
        parsed > std::numeric_limits<int32_t>::max()) return std::nullopt;
    return static_cast<int32_t>(parsed);
}

[[nodiscard]] std::optional<bool> parseBool(container::StringView value) {
    const container::String lower = lowerAscii(value);
    if (lower == "yes" || lower == "true" || lower == "1") return true;
    if (lower == "no" || lower == "false" || lower == "0") return false;
    return std::nullopt;
}

[[nodiscard]] std::optional<LocomotorSurface> parseSurface(container::StringView value) {
    const container::String lower = lowerAscii(value);
    if (lower == "ground") return LocomotorSurface::Ground;
    if (lower == "water") return LocomotorSurface::Water;
    if (lower == "cliff") return LocomotorSurface::Cliff;
    if (lower == "air") return LocomotorSurface::Air;
    if (lower == "rubble") return LocomotorSurface::Rubble;
    return std::nullopt;
}

[[nodiscard]] std::optional<LocomotorSurfaceMask> parseSurfaceMask(container::StringView value) {
    LocomotorSurfaceMask result = 0;
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t,+|", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t,+|", cursor);
        const container::StringView token = value.substr(cursor, end - cursor);
        cursor = end;
        const std::optional<LocomotorSurface> surface = parseSurface(token);
        if (!surface) return std::nullopt;
        result |= locomotorSurfaceBit(*surface);
    }
    return result;
}

[[nodiscard]] std::optional<LocomotorAppearance> parseAppearance(container::StringView value) {
    const container::String lower = lowerAscii(value);
    if (lower == "two_legs") return LocomotorAppearance::TwoLegs;
    if (lower == "four_wheels") return LocomotorAppearance::FourWheels;
    if (lower == "treads") return LocomotorAppearance::Treads;
    if (lower == "hover") return LocomotorAppearance::Hover;
    if (lower == "thrust") return LocomotorAppearance::Thrust;
    if (lower == "wings") return LocomotorAppearance::Wings;
    if (lower == "climber") return LocomotorAppearance::Climber;
    if (lower == "other") return LocomotorAppearance::Other;
    if (lower == "motorcycle") return LocomotorAppearance::Motorcycle;
    return std::nullopt;
}

[[nodiscard]] std::optional<LocomotorZAxisBehavior> parseZAxisBehavior(container::StringView value) {
    const container::String lower = lowerAscii(value);
    if (lower == "no_z_motive_force") return LocomotorZAxisBehavior::NoZMotiveForce;
    if (lower == "sea_level") return LocomotorZAxisBehavior::SeaLevel;
    if (lower == "surface_relative_height") return LocomotorZAxisBehavior::SurfaceRelativeHeight;
    if (lower == "absolute_height") return LocomotorZAxisBehavior::AbsoluteHeight;
    if (lower == "fixed_surface_relative_height") return LocomotorZAxisBehavior::FixedSurfaceRelativeHeight;
    if (lower == "fixed_absolute_height") return LocomotorZAxisBehavior::FixedAbsoluteHeight;
    if (lower == "fixed_relative_to_ground_and_buildings") {
        return LocomotorZAxisBehavior::FixedRelativeToGroundAndBuildings;
    }
    if (lower == "relative_to_highest_layer") return LocomotorZAxisBehavior::SmoothRelativeToHighestLayer;
    return std::nullopt;
}

[[nodiscard]] std::optional<LocomotorGroupPriority> parseGroupPriority(container::StringView value) {
    const container::String lower = lowerAscii(value);
    if (lower == "moves_back") return LocomotorGroupPriority::MovesBack;
    if (lower == "moves_middle") return LocomotorGroupPriority::MovesMiddle;
    if (lower == "moves_front") return LocomotorGroupPriority::MovesFront;
    return std::nullopt;
}

[[nodiscard]] bool nonNegativeFinite(float value) noexcept {
    return std::isfinite(value) && value >= 0.0f;
}

[[nodiscard]] math::q32_32 quantizeAuthoritative(float value) noexcept {
    if (!std::isfinite(value)) return {};
    constexpr float minimum =
        static_cast<float>(std::numeric_limits<int32_t>::min());
    constexpr float maximum =
        static_cast<float>(std::numeric_limits<int32_t>::max());
    if (value <= minimum) {
        return math::q32_32::from_raw(std::numeric_limits<int64_t>::min());
    }
    if (value >= maximum) {
        return math::q32_32::from_raw(std::numeric_limits<int64_t>::max());
    }
    return math::q32_32{value};
}

void synchronizeAuthoritativeScalars(LocomotorTemplate& value) noexcept {
    LocomotorAuthoritativeScalars& fixed = value.fixed;
    fixed.maximumSpeed = quantizeAuthoritative(value.maxSpeedUnitsPerSecond);
    fixed.damagedMaximumSpeed =
        quantizeAuthoritative(value.damagedMaxSpeedUnitsPerSecond);
    fixed.maximumTurnRate =
        quantizeAuthoritative(value.maxTurnRateRadiansPerSecond);
    fixed.damagedMaximumTurnRate =
        quantizeAuthoritative(value.damagedMaxTurnRateRadiansPerSecond);
    fixed.acceleration =
        quantizeAuthoritative(value.accelerationUnitsPerSecondSq);
    fixed.damagedAcceleration =
        quantizeAuthoritative(value.damagedAccelerationUnitsPerSecondSq);
    fixed.lift = quantizeAuthoritative(value.liftUnitsPerSecondSq);
    fixed.damagedLift =
        quantizeAuthoritative(value.damagedLiftUnitsPerSecondSq);
    fixed.brakingIsInfinite = std::isinf(value.brakingUnitsPerSecondSq);
    fixed.hasFiniteBraking = std::isfinite(value.brakingUnitsPerSecondSq);
    fixed.braking = fixed.hasFiniteBraking
        ? quantizeAuthoritative(value.brakingUnitsPerSecondSq)
        : math::q32_32{};
    fixed.minimumSpeed =
        quantizeAuthoritative(value.minSpeedUnitsPerSecond);
    fixed.minimumTurnSpeed = std::isfinite(value.minTurnSpeedUnitsPerSecond)
        ? quantizeAuthoritative(value.minTurnSpeedUnitsPerSecond)
        : math::q32_32{};
    fixed.preferredHeight = quantizeAuthoritative(value.preferredHeight);
    fixed.preferredHeightDamping =
        quantizeAuthoritative(value.preferredHeightDamping);
    fixed.circlingRadius = quantizeAuthoritative(value.circlingRadius);
    fixed.extra2DFrictionPerSecond =
        quantizeAuthoritative(value.extra2DFrictionPerSecond);
    fixed.hasFiniteSpeedLimitZ =
        std::isfinite(value.speedLimitZUnitsPerSecond);
    fixed.speedLimitZ = fixed.hasFiniteSpeedLimitZ
        ? quantizeAuthoritative(value.speedLimitZUnitsPerSecond)
        : math::q32_32{};
    fixed.maximumThrustAngleRadians =
        quantizeAuthoritative(value.maxThrustAngleRadians);
    fixed.accelerationPitchLimitRadians =
        quantizeAuthoritative(value.accelerationPitchLimitRadians);
    fixed.decelerationPitchLimitRadians =
        quantizeAuthoritative(value.decelerationPitchLimitRadians);
    fixed.bounceAngularVelocityRadiansPerSecond =
        quantizeAuthoritative(value.bounceAngularVelocityRadiansPerSecond);
    fixed.pitchStiffness = quantizeAuthoritative(value.pitchStiffness);
    fixed.rollStiffness = quantizeAuthoritative(value.rollStiffness);
    fixed.pitchDamping = quantizeAuthoritative(value.pitchDamping);
    fixed.rollDamping = quantizeAuthoritative(value.rollDamping);
    fixed.thrustRoll = quantizeAuthoritative(value.thrustRoll);
    fixed.thrustWobbleRate = quantizeAuthoritative(value.thrustWobbleRate);
    fixed.thrustMinimumWobble =
        quantizeAuthoritative(value.thrustMinimumWobble);
    fixed.thrustMaximumWobble =
        quantizeAuthoritative(value.thrustMaximumWobble);
    fixed.pitchByZVelocityFactor =
        quantizeAuthoritative(value.pitchByZVelocityFactor);
    fixed.forwardVelocityPitchFactor =
        quantizeAuthoritative(value.forwardVelocityPitchFactor);
    fixed.lateralVelocityRollFactor =
        quantizeAuthoritative(value.lateralVelocityRollFactor);
    fixed.forwardAccelerationPitchFactor =
        quantizeAuthoritative(value.forwardAccelerationPitchFactor);
    fixed.lateralAccelerationRollFactor =
        quantizeAuthoritative(value.lateralAccelerationRollFactor);
    fixed.uniformAxialDamping =
        quantizeAuthoritative(value.uniformAxialDamping);
    fixed.turnPivotOffset = quantizeAuthoritative(value.turnPivotOffset);
    fixed.maximumWheelExtension =
        quantizeAuthoritative(value.maximumWheelExtension);
    fixed.maximumWheelCompression =
        quantizeAuthoritative(value.maximumWheelCompression);
    fixed.frontWheelTurnAngleRadians =
        quantizeAuthoritative(value.frontWheelTurnAngleRadians);
    fixed.closeEnough = quantizeAuthoritative(value.closeEnoughDistance);
    fixed.slideIntoPlaceMilliseconds =
        quantizeAuthoritative(value.slideIntoPlaceMilliseconds);
    fixed.wanderWidthFactor =
        quantizeAuthoritative(value.wanderWidthFactor);
    fixed.wanderLengthFactor =
        quantizeAuthoritative(value.wanderLengthFactor);
    fixed.wanderAboutPointRadius =
        quantizeAuthoritative(value.wanderAboutPointRadius);
    fixed.rudderCorrectionDegree =
        quantizeAuthoritative(value.rudderCorrectionDegree);
    fixed.rudderCorrectionRate =
        quantizeAuthoritative(value.rudderCorrectionRate);
    fixed.elevatorCorrectionDegree =
        quantizeAuthoritative(value.elevatorCorrectionDegree);
    fixed.elevatorCorrectionRate =
        quantizeAuthoritative(value.elevatorCorrectionRate);
    fixed.hasFiniteAirborneTargetingHeight =
        value.airborneTargetingHeight != std::numeric_limits<int32_t>::max();
    fixed.airborneTargetingHeight =
        fixed.hasFiniteAirborneTargetingHeight
            ? math::q32_32{value.airborneTargetingHeight}
            : math::q32_32{};
    fixed.accelerationIsInfinite =
        std::isinf(value.accelerationUnitsPerSecondSq);
    fixed.damagedAccelerationIsInfinite =
        std::isinf(value.damagedAccelerationUnitsPerSecondSq);
    fixed.preferredHeightIsLowest =
        value.preferredHeight == LocomotorTemplate::kPreferredHeightLowest;
}

[[nodiscard]] bool finalize(LocomotorTemplate& templateData, container::String& error) {
    if (templateData.name.empty()) {
        error = "locomotor has an empty name";
        return false;
    }
    // RefCode applies these fallbacks after all INI fields/overrides have
    // been read. Preserve the same meaning rather than inventing damaged
    // defaults such as half speed in the loader.
    if (templateData.damagedMaxSpeedUnitsPerSecond < 0.0f) {
        templateData.damagedMaxSpeedUnitsPerSecond = templateData.maxSpeedUnitsPerSecond;
    }
    if (templateData.damagedMaxTurnRateRadiansPerSecond < 0.0f) {
        templateData.damagedMaxTurnRateRadiansPerSecond = templateData.maxTurnRateRadiansPerSecond;
    }
    if (templateData.damagedAccelerationUnitsPerSecondSq < 0.0f) {
        templateData.damagedAccelerationUnitsPerSecondSq = templateData.accelerationUnitsPerSecondSq;
    }
    if (templateData.damagedLiftUnitsPerSecondSq < 0.0f) {
        templateData.damagedLiftUnitsPerSecondSq = templateData.liftUnitsPerSecondSq;
    }

    const bool preferredHeightValid = std::isfinite(templateData.preferredHeight) &&
        (templateData.preferredHeight >= 0.0f ||
         templateData.preferredHeight == LocomotorTemplate::kPreferredHeightLowest);

    const bool numericValuesValid =
        nonNegativeFinite(templateData.maxSpeedUnitsPerSecond) &&
        nonNegativeFinite(templateData.damagedMaxSpeedUnitsPerSecond) &&
        nonNegativeFinite(templateData.maxTurnRateRadiansPerSecond) &&
        nonNegativeFinite(templateData.damagedMaxTurnRateRadiansPerSecond) &&
        nonNegativeFinite(templateData.accelerationUnitsPerSecondSq) &&
        nonNegativeFinite(templateData.damagedAccelerationUnitsPerSecondSq) &&
        nonNegativeFinite(templateData.liftUnitsPerSecondSq) &&
        nonNegativeFinite(templateData.damagedLiftUnitsPerSecondSq) &&
        (std::isinf(templateData.brakingUnitsPerSecondSq) ||
         nonNegativeFinite(templateData.brakingUnitsPerSecondSq)) &&
        nonNegativeFinite(templateData.minSpeedUnitsPerSecond) &&
        (std::isinf(templateData.minTurnSpeedUnitsPerSecond) ||
         nonNegativeFinite(templateData.minTurnSpeedUnitsPerSecond)) &&
        preferredHeightValid &&
        nonNegativeFinite(templateData.preferredHeightDamping) &&
        std::isfinite(templateData.circlingRadius) &&
        nonNegativeFinite(templateData.extra2DFrictionPerSecond) &&
        nonNegativeFinite(templateData.speedLimitZUnitsPerSecond) &&
        nonNegativeFinite(templateData.maxThrustAngleRadians) &&
        nonNegativeFinite(templateData.accelerationPitchLimitRadians) &&
        nonNegativeFinite(templateData.decelerationPitchLimitRadians) &&
        nonNegativeFinite(templateData.bounceAngularVelocityRadiansPerSecond) &&
        std::isfinite(templateData.pitchStiffness) &&
        std::isfinite(templateData.rollStiffness) &&
        std::isfinite(templateData.pitchDamping) &&
        std::isfinite(templateData.rollDamping) &&
        std::isfinite(templateData.thrustRoll) &&
        std::isfinite(templateData.thrustWobbleRate) &&
        std::isfinite(templateData.thrustMinimumWobble) &&
        std::isfinite(templateData.thrustMaximumWobble) &&
        std::isfinite(templateData.pitchByZVelocityFactor) &&
        std::isfinite(templateData.forwardVelocityPitchFactor) &&
        std::isfinite(templateData.lateralVelocityRollFactor) &&
        std::isfinite(templateData.forwardAccelerationPitchFactor) &&
        std::isfinite(templateData.lateralAccelerationRollFactor) &&
        nonNegativeFinite(templateData.uniformAxialDamping) &&
        std::isfinite(templateData.turnPivotOffset) &&
        std::isfinite(templateData.maximumWheelExtension) &&
        std::isfinite(templateData.maximumWheelCompression) &&
        nonNegativeFinite(templateData.closeEnoughDistance) &&
        nonNegativeFinite(templateData.slideIntoPlaceMilliseconds) &&
        std::isfinite(templateData.wanderWidthFactor) &&
        std::isfinite(templateData.wanderLengthFactor) &&
        nonNegativeFinite(templateData.wanderAboutPointRadius) &&
        std::isfinite(templateData.rudderCorrectionDegree) &&
        std::isfinite(templateData.rudderCorrectionRate) &&
        std::isfinite(templateData.elevatorCorrectionDegree) &&
        std::isfinite(templateData.elevatorCorrectionRate);
    if (!numericValuesValid) {
        error = "locomotor contains an invalid numeric movement field";
        return false;
    }
    if (templateData.turnPivotOffset < -1.0f || templateData.turnPivotOffset > 1.0f) {
        error = "locomotor TurnPivotOffset is outside [-1, 1]";
        return false;
    }
    // RefCode treats the speed floors for Wings/Thrust as self-healing
    // safety guards, rather than malformed content.  This matters for stock
    // entries such as NapalmBombLocomotor, which deliberately omit MinSpeed.
    // Keep the exact 0.01 fallback so the future specialised controllers do
    // not receive a zero-speed aircraft, while preserving the hard Thrust
    // rejection for incompatible Z/lift settings.
    constexpr float kLegacyMinimumMovingSpeed = 0.01f;
    if (templateData.appearance == LocomotorAppearance::Wings) {
        if (templateData.minSpeedUnitsPerSecond <= 0.0f) {
            templateData.minSpeedUnitsPerSecond = kLegacyMinimumMovingSpeed;
        }
        if (templateData.minTurnSpeedUnitsPerSecond <= 0.0f) {
            templateData.minTurnSpeedUnitsPerSecond = kLegacyMinimumMovingSpeed;
        }
    }
    if (templateData.appearance == LocomotorAppearance::Thrust) {
        if (templateData.zAxisBehavior != LocomotorZAxisBehavior::NoZMotiveForce ||
            templateData.liftUnitsPerSecondSq != 0.0f ||
            templateData.damagedLiftUnitsPerSecondSq != 0.0f) {
            error = "thrust locomotor violates legacy Z/lift validation";
            return false;
        }
        if (templateData.maxSpeedUnitsPerSecond <= 0.0f) {
            templateData.maxSpeedUnitsPerSecond = kLegacyMinimumMovingSpeed;
        }
        if (templateData.damagedMaxSpeedUnitsPerSecond <= 0.0f) {
            templateData.damagedMaxSpeedUnitsPerSecond = kLegacyMinimumMovingSpeed;
        }
        if (templateData.minSpeedUnitsPerSecond <= 0.0f) {
            templateData.minSpeedUnitsPerSecond = kLegacyMinimumMovingSpeed;
        }
    }
    synchronizeAuthoritativeScalars(templateData);
    templateData.loaded = true;
    return true;
}

[[nodiscard]] bool readFloat(container::StringView value, float& destination, container::String& error,
                             container::StringView field) {
    const std::optional<float> parsed = parseFiniteFloat(value, destination);
    if (!parsed) {
        static_cast<void>(error);
        static_cast<void>(field);
        return true;
    }
    destination = *parsed;
    return true;
}

[[nodiscard]] bool readBool(container::StringView value, bool& destination, container::String& error,
                            container::StringView field) {
    const std::optional<bool> parsed = parseBool(value);
    if (!parsed) {
        error = "invalid boolean value for " + container::String(field);
        return false;
    }
    destination = *parsed;
    return true;
}

[[nodiscard]] bool applyField(LocomotorTemplate& templateData, container::StringView key,
                              container::StringView value, container::String& error,
                              bool& recognized) {
    recognized = true;
    if (key == "Surfaces") {
        const std::optional<LocomotorSurfaceMask> parsed = parseSurfaceMask(value);
        if (!parsed) { error = "invalid Locomotor Surfaces"; return false; }
        templateData.surfaces = *parsed;
        return true;
    }
    if (key == "Speed") return readFloat(value, templateData.maxSpeedUnitsPerSecond, error, key);
    if (key == "SpeedDamaged") return readFloat(value, templateData.damagedMaxSpeedUnitsPerSecond, error, key);
    if (key == "TurnRate" || key == "TurnRateDamaged") {
        const std::optional<float> degreesPerSecond = parseFiniteFloat(value);
        if (!degreesPerSecond) return true;
        float& destination = key == "TurnRate"
            ? templateData.maxTurnRateRadiansPerSecond
            : templateData.damagedMaxTurnRateRadiansPerSecond;
        destination = *degreesPerSecond * (3.14159265358979323846f / 180.0f);
        return true;
    }
    if (key == "Acceleration") return readFloat(value, templateData.accelerationUnitsPerSecondSq, error, key);
    if (key == "AccelerationDamaged") return readFloat(value, templateData.damagedAccelerationUnitsPerSecondSq, error, key);
    if (key == "Lift") return readFloat(value, templateData.liftUnitsPerSecondSq, error, key);
    if (key == "LiftDamaged") return readFloat(value, templateData.damagedLiftUnitsPerSecondSq, error, key);
    if (key == "Braking") return readFloat(value, templateData.brakingUnitsPerSecondSq, error, key);
    if (key == "MinSpeed") return readFloat(value, templateData.minSpeedUnitsPerSecond, error, key);
    if (key == "MinTurnSpeed") return readFloat(value, templateData.minTurnSpeedUnitsPerSecond, error, key);
    if (key == "PreferredHeight") return readFloat(value, templateData.preferredHeight, error, key);
    if (key == "PreferredHeightDamping") return readFloat(value, templateData.preferredHeightDamping, error, key);
    if (key == "CirclingRadius") return readFloat(value, templateData.circlingRadius, error, key);
    if (key == "Extra2DFriction") return readFloat(value, templateData.extra2DFrictionPerSecond, error, key);
    if (key == "SpeedLimitZ") return readFloat(value, templateData.speedLimitZUnitsPerSecond, error, key);
    if (key == "MaxThrustAngle") {
        const std::optional<float> degrees = parseFiniteFloat(value);
        if (!degrees) return true;
        templateData.maxThrustAngleRadians = *degrees * (3.14159265358979323846f / 180.0f);
        return true;
    }
    if (key == "AccelerationPitchLimit" || key == "DecelerationPitchLimit" ||
        key == "BounceAmount") {
        const std::optional<float> degrees = parseFiniteFloat(value);
        if (!degrees) return true;
        float* destination = key == "AccelerationPitchLimit"
            ? &templateData.accelerationPitchLimitRadians
            : key == "DecelerationPitchLimit"
                ? &templateData.decelerationPitchLimitRadians
                : &templateData.bounceAngularVelocityRadiansPerSecond;
        *destination = *degrees * (3.14159265358979323846f / 180.0f);
        return true;
    }
    if (key == "PitchStiffness") return readFloat(value, templateData.pitchStiffness, error, key);
    if (key == "RollStiffness") return readFloat(value, templateData.rollStiffness, error, key);
    if (key == "PitchDamping") return readFloat(value, templateData.pitchDamping, error, key);
    if (key == "RollDamping") return readFloat(value, templateData.rollDamping, error, key);
    if (key == "ThrustRoll") return readFloat(value, templateData.thrustRoll, error, key);
    if (key == "ThrustWobbleRate") return readFloat(value, templateData.thrustWobbleRate, error, key);
    if (key == "ThrustMinWobble") return readFloat(value, templateData.thrustMinimumWobble, error, key);
    if (key == "ThrustMaxWobble") return readFloat(value, templateData.thrustMaximumWobble, error, key);
    if (key == "PitchInDirectionOfZVelFactor") return readFloat(value, templateData.pitchByZVelocityFactor, error, key);
    if (key == "ForwardVelocityPitchFactor") return readFloat(value, templateData.forwardVelocityPitchFactor, error, key);
    if (key == "LateralVelocityRollFactor") return readFloat(value, templateData.lateralVelocityRollFactor, error, key);
    if (key == "ForwardAccelerationPitchFactor") return readFloat(value, templateData.forwardAccelerationPitchFactor, error, key);
    if (key == "LateralAccelerationRollFactor") return readFloat(value, templateData.lateralAccelerationRollFactor, error, key);
    if (key == "UniformAxialDamping") return readFloat(value, templateData.uniformAxialDamping, error, key);
    if (key == "ZAxisBehavior") {
        const std::optional<LocomotorZAxisBehavior> parsed = parseZAxisBehavior(value);
        if (!parsed) { error = "invalid Locomotor ZAxisBehavior"; return false; }
        templateData.zAxisBehavior = *parsed;
        return true;
    }
    if (key == "Appearance") {
        const std::optional<LocomotorAppearance> parsed = parseAppearance(value);
        if (!parsed) { error = "invalid Locomotor Appearance"; return false; }
        templateData.appearance = *parsed;
        return true;
    }
    if (key == "GroupMovementPriority") {
        const std::optional<LocomotorGroupPriority> parsed = parseGroupPriority(value);
        if (!parsed) { error = "invalid Locomotor GroupMovementPriority"; return false; }
        templateData.groupPriority = *parsed;
        return true;
    }
    if (key == "TurnPivotOffset") return readFloat(value, templateData.turnPivotOffset, error, key);
    if (key == "MaximumWheelExtension") {
        return readFloat(value, templateData.maximumWheelExtension, error, key);
    }
    if (key == "MaximumWheelCompression") {
        return readFloat(value, templateData.maximumWheelCompression, error, key);
    }
    if (key == "FrontWheelTurnAngle") {
        const std::optional<float> degrees = parseFiniteFloat(value);
        if (!degrees) {
            return true;
        }
        templateData.frontWheelTurnAngleRadians =
            *degrees * (3.14159265358979323846f / 180.0f);
        return true;
    }
    if (key == "CloseEnoughDist") return readFloat(value, templateData.closeEnoughDistance, error, key);
    if (key == "SlideIntoPlaceTime") return readFloat(value, templateData.slideIntoPlaceMilliseconds, error, key);
    if (key == "WanderWidthFactor") return readFloat(value, templateData.wanderWidthFactor, error, key);
    if (key == "WanderLengthFactor") return readFloat(value, templateData.wanderLengthFactor, error, key);
    if (key == "WanderAboutPointRadius") return readFloat(value, templateData.wanderAboutPointRadius, error, key);
    if (key == "RudderCorrectionDegree") return readFloat(value, templateData.rudderCorrectionDegree, error, key);
    if (key == "RudderCorrectionRate") return readFloat(value, templateData.rudderCorrectionRate, error, key);
    if (key == "ElevatorCorrectionDegree") return readFloat(value, templateData.elevatorCorrectionDegree, error, key);
    if (key == "ElevatorCorrectionRate") return readFloat(value, templateData.elevatorCorrectionRate, error, key);
    if (key == "AirborneTargetingHeight") {
        const std::optional<int32_t> parsed = parseInt32(value);
        if (!parsed) { error = "invalid integer value for AirborneTargetingHeight"; return false; }
        templateData.airborneTargetingHeight = *parsed;
        return true;
    }
    if (key == "CloseEnoughDist3D") return readBool(value, templateData.closeEnoughDistance3D, error, key);
    if (key == "StickToGround") return readBool(value, templateData.stickToGround, error, key);
    if (key == "CanMoveBackwards") return readBool(value, templateData.canMoveBackwards, error, key);
    if (key == "LocomotorWorksWhenDead") return readBool(value, templateData.locomotorWorksWhenDead, error, key);
    if (key == "AllowAirborneMotiveForce") return readBool(value, templateData.allowMotiveForceWhileAirborne, error, key);
    if (key == "Apply2DFrictionWhenAirborne") return readBool(value, templateData.apply2DFrictionWhenAirborne, error, key);
    if (key == "DownhillOnly") return readBool(value, templateData.downhillOnly, error, key);
    if (key == "HasSuspension") return readBool(value, templateData.hasSuspension, error, key);
    // Unknown Mod fields remain forward-compatible, but they must not remain
    // invisible: the caller publishes one Content diagnostic per field name
    // and continues compiling the rest of the Locomotor.
    recognized = false;
    return true;
}

void warnUnknownLocomotorField(container::StringView key) {
    // Deliberately omit definition/raw provenance, matching the Object-field
    // diagnostic policy. ContentDiagnosticCollector can then fold every
    // authored occurrence (and every load layer) into one warning per field
    // name while still recording that the ignored value has no effect.
    processContentDiagnostics().warn({
        .source = "data/ini/Locomotor",
        .block = "Locomotor",
        .module = "LocomotorStore",
        .field = container::String{key},
        .adoptedValue = "field ignored",
        .reason = "Locomotor field is not recognized by the typed compiler; "
                  "the authored value has no effect",
    });
}

} // namespace

FrozenLocomotorTemplate freezeLocomotorTemplate(
    const LocomotorTemplate& source) noexcept {
    FrozenLocomotorTemplate result;
    result.name = source.name;
    result.surfaces = source.surfaces;
    result.appearance = source.appearance;
    result.zAxisBehavior = source.zAxisBehavior;
    result.groupPriority = source.groupPriority;
    result.fixed = source.fixed;
    result.closeEnoughDistance3D = source.closeEnoughDistance3D;
    result.stickToGround = source.stickToGround;
    result.canMoveBackwards = source.canMoveBackwards;
    result.locomotorWorksWhenDead = source.locomotorWorksWhenDead;
    result.allowMotiveForceWhileAirborne =
        source.allowMotiveForceWhileAirborne;
    result.apply2DFrictionWhenAirborne =
        source.apply2DFrictionWhenAirborne;
    result.downhillOnly = source.downhillOnly;
    result.hasSuspension = source.hasSuspension;
    return result;
}

LocomotorStore& LocomotorStore::instance() {
    static LocomotorStore s_instance;
    return s_instance;
}

void LocomotorStore::clear() {
    m_locomotors.clear();
}

bool LocomotorStore::loadFromIni(
    const container::String& filePath, ini::LegacyIniLoadType) {
    GeneralsIniParser parser;
    if (!parser.parseFile(filePath)) return false;

    bool valid = true;
    for (const IniBlock& block : parser.blocks()) {
        if (block.type != "Locomotor") continue;
        if (block.name.empty()) {
            TD_LOG_WARN("[LocomotorStore] Ignored unnamed Locomotor in {}", filePath);
            valid = false;
            continue;
        }

        // ZH patches an existing Locomotor for ordinary loads and copies the
        // final value before patching for CreateOverrides. Both policies have
        // the same effective value in this flattened immutable store.
        // Start from the prior frozen value rather than constructing a fresh
        // half-default template and losing fields that were not mentioned.
        LocomotorTemplate templateData;
        if (const auto existing = m_locomotors.find(block.name); existing != m_locomotors.end()) {
            templateData = existing->second;
        }
        templateData.name = block.name;
        templateData.loaded = false;

        container::String error;
        bool fieldsValid = true;
        for (const auto& [key, value] : block.values) {
            bool recognized = false;
            if (!applyField(templateData, key, value, error, recognized)) {
                fieldsValid = false;
                break;
            }
            if (!recognized) warnUnknownLocomotorField(key);
        }
        if (!fieldsValid || !finalize(templateData, error)) {
            TD_LOG_WARN("[LocomotorStore] Ignored invalid Locomotor '{}': {}", block.name, error);
            valid = false;
            continue;
        }
        m_locomotors.insert_or_assign(templateData.name, std::move(templateData));
    }

    TD_LOG_INFO("[LocomotorStore] Loaded {} typed locomotor templates from {}",
                m_locomotors.size(), filePath);
    return valid;
}

const LocomotorTemplate* LocomotorStore::find(const container::String& name) const {
    const auto it = m_locomotors.find(name);
    return it != m_locomotors.end() ? &it->second : nullptr;
}

} // namespace game
