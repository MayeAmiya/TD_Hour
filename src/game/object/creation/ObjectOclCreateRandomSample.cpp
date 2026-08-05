#include "game/object/creation/ObjectOclCreateRandomSample.h"

#include "game/base/SimulationRandom.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <limits>

namespace engine {
namespace {

const math::q32_32 kFullTurn{6.28318530717958647692};

[[nodiscard]] bool hasDisposition(
    game::ObjectCreationDispositionMask mask,
    game::ObjectCreationDisposition flag) noexcept {
    return (mask & game::objectCreationDispositionBit(flag)) != 0;
}

[[nodiscard]] math::q32_32 randomFixed(
    SimulationRandom& random, math::q32_32 minimum,
    math::q32_32 maximum) noexcept {
    return random.fixedInclusive(minimum, maximum);
}

[[nodiscard]] ObjectOclCreateFixedVector randomForce(
    SimulationRandom& random, const game::ObjectCreationGenericFields& common) noexcept {
    const math::q32_32 angle = random.fixedInclusive(
        math::q32_32{}, kFullTurn);
    const math::q32_32 pitch = randomFixed(
        random, common.minimumForcePitchRadians,
        common.maximumForcePitchRadians);
    const math::q32_32 magnitude = randomFixed(
        random, common.minimumForceMagnitude,
        common.maximumForceMagnitude);
    const math::q32_32 horizontal = math::fixed_cos(pitch) * magnitude;
    return {
        .x = math::fixed_cos(angle) * horizontal,
        .y = math::fixed_sin(angle) * horizontal,
        .z = math::fixed_sin(pitch) * magnitude,
    };
}

[[nodiscard]] uint32_t millisecondsToFrames(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    const uint64_t product = static_cast<uint64_t>(milliseconds) *
        static_cast<uint64_t>(std::max(1u, framesPerSecond));
    return static_cast<uint32_t>(std::min<uint64_t>(
        std::numeric_limits<uint32_t>::max(),
        product / 1000u + (product % 1000u != 0 ? 1u : 0u)));
}

} // namespace

ObjectOclCreateRandomSample sampleObjectOclCreateRandom(
    const ObjectOclCreateRandomSampleRequest& request,
    SimulationRandom& random) noexcept {
    ObjectOclCreateRandomSample sample;
    if (!request.common || request.candidates.empty()) return sample;
    const game::ObjectCreationGenericFields& common = *request.common;

    if (request.chooseModel) {
        sample.modelIndex = static_cast<size_t>(random.integerInclusive(
            0, static_cast<int32_t>(request.candidates.size() - 1u)));
    }
    if (sample.modelIndex >= request.candidates.size()) return sample;
    const ObjectOclCreateCandidateTraits& traits =
        request.candidates[sample.modelIndex];
    sample.candidateAvailable = traits.available;
    sample.hasPhysics = traits.hasPhysics;
    sample.hasLifetimeUpdate = traits.hasLifetimeUpdate;
    if (!sample.candidateAvailable) return sample;

    // This is the legacy causal order: reallyCreate chooses the model and
    // SpreadFormation first; doStuffToObj then overrides lifetime, configures
    // debris animation, health, orientation, flight and finally whirling.
    if (request.allowSpread && common.spreadFormation) {
        sample.hasSpread = true;
        sample.spreadMinimumRadius = randomFixed(
            random, common.minimumFormationDistanceA,
            common.minimumFormationDistanceB);
        sample.spreadStartAngleRadians = random.fixedInclusive(
            math::q32_32{}, kFullTurn);
    }

    if (sample.hasLifetimeUpdate) {
        if (request.lifetimeOverrideFrames > 0) {
            sample.lifetimeFrames = request.lifetimeOverrideFrames;
        } else if (common.maximumLifetimeMilliseconds > 0) {
            const int32_t minimum = static_cast<int32_t>(std::min<uint32_t>(
                common.minimumLifetimeMilliseconds,
                static_cast<uint32_t>(std::numeric_limits<int32_t>::max())));
            const int32_t maximum = static_cast<int32_t>(std::min<uint32_t>(
                common.maximumLifetimeMilliseconds,
                static_cast<uint32_t>(std::numeric_limits<int32_t>::max())));
            sample.lifetimeFrames = millisecondsToFrames(
                static_cast<uint32_t>(random.integerInclusive(minimum, maximum)),
                request.logicFramesPerSecond);
        }
    }

    if (request.allowDebrisAnimation && request.animationSetCount > 0) {
        sample.hasAnimationSet = true;
        sample.animationSetIndex = static_cast<size_t>(random.integerInclusive(
            0, static_cast<int32_t>(request.animationSetCount - 1u)));
    }

    sample.healthFraction = randomFixed(
        random, common.minimumHealth, common.maximumHealth);

    if (hasDisposition(common.disposition,
            game::ObjectCreationDisposition::OnGroundAligned)) {
        sample.hasOnGroundOrientation = true;
        sample.onGroundOrientationRadians = random.fixedInclusive(
            math::q32_32{}, kFullTurn);
    }

    if (hasDisposition(common.disposition,
            game::ObjectCreationDisposition::SendItOut)) {
        sample.sendOutOrientationRadians = random.fixedInclusive(
            math::q32_32{}, kFullTurn);
        sample.hasSendOutOrientation = true;
        if (sample.hasPhysics) {
            const math::q32_32 horizontal =
                math::q32_32{int32_t{4}} * common.dispositionIntensity;
            sample.sendOutForce = {
                .x = randomFixed(random, -horizontal, horizontal),
                .y = randomFixed(random, -horizontal, horizontal),
            };
            sample.hasSendOutForce = true;
        }
    }

    const bool hasFlight = sample.hasPhysics &&
        (hasDisposition(common.disposition,
             game::ObjectCreationDisposition::SendItFlying) ||
         hasDisposition(common.disposition,
             game::ObjectCreationDisposition::SendItUp) ||
         hasDisposition(common.disposition,
             game::ObjectCreationDisposition::RandomForce));
    if (hasFlight) {
        const math::q32_32 intensitySpin =
            math::q32_32{0.0981747704246810387} *
            common.dispositionIntensity;
        const math::q32_32 spin = common.spinRate.raw() < 0
            ? intensitySpin : common.spinRate;
        const auto magnitude = [spin](math::q32_32 authored) noexcept {
            return authored.raw() < 0 ? spin : authored;
        };
        const math::q32_32 yawMagnitude = magnitude(common.yawRate);
        const math::q32_32 rollMagnitude = magnitude(common.rollRate);
        const math::q32_32 pitchMagnitude = magnitude(common.pitchRate);
        sample.flightYawRate = randomFixed(
            random, -yawMagnitude, yawMagnitude);
        sample.flightRollRate = randomFixed(
            random, -rollMagnitude, rollMagnitude);
        sample.flightPitchRate = randomFixed(
            random, -pitchMagnitude, pitchMagnitude);

        if (hasDisposition(common.disposition,
                game::ObjectCreationDisposition::SendItFlying)) {
            const math::q32_32 horizontal =
                math::q32_32{int32_t{4}} * common.dispositionIntensity;
            const math::q32_32 vertical =
                math::q32_32{int32_t{3}} * common.dispositionIntensity;
            sample.flightForce = {
                .x = randomFixed(random, -horizontal, horizontal),
                .y = randomFixed(random, -horizontal, horizontal),
                .z = randomFixed(random,
                    math::q32_32{0.33f} * vertical, vertical),
            };
        } else if (hasDisposition(common.disposition,
                       game::ObjectCreationDisposition::SendItUp)) {
            const math::q32_32 horizontal =
                math::q32_32{int32_t{2}} * common.dispositionIntensity;
            const math::q32_32 vertical =
                math::q32_32{int32_t{4}} * common.dispositionIntensity;
            sample.flightForce = {
                .x = randomFixed(random, -horizontal, horizontal),
                .y = randomFixed(random, -horizontal, horizontal),
                .z = randomFixed(random,
                    math::q32_32{0.75f} * vertical, vertical),
            };
        } else {
            sample.flightForce = randomForce(random, common);
        }
        sample.hasFlightForce = true;
    }

    if (sample.hasPhysics && hasDisposition(common.disposition,
            game::ObjectCreationDisposition::Whirling)) {
        const math::q32_32 magnitude = common.dispositionIntensity;
        sample.whirlingYawRate = randomFixed(random, -magnitude, magnitude);
        sample.whirlingRollRate = randomFixed(random, -magnitude, magnitude);
        sample.whirlingPitchRate = randomFixed(random, -magnitude, magnitude);
        sample.hasWhirlingRates = true;
    }
    return sample;
}

} // namespace engine
