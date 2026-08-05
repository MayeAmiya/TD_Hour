#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/combat/ObjectWeaponDamage.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>
#include "game/object/simulation/combat/ObjectCombatDetail.h"

namespace engine::object_combat_detail {

void initializeTurretRuntime(
    const ObjectCombatInitializationPlan* plan,
    ObjectWeaponComponent& weapons) {
    if (!plan) return;
    for (size_t index = 0; index < weapons.turrets.size(); ++index) {
        ObjectTurretRuntime& runtime = weapons.turrets[index];
        const ObjectTurretRecipe& recipe = plan->turrets[index];
        runtime.turnRateRadiansPerSecond = recipe.turnRateRadiansPerSecond;
        runtime.pitchRateRadiansPerSecond = recipe.pitchRateRadiansPerSecond;
        runtime.minimumPitchRadians = recipe.minimumPitchRadians;
        runtime.firePitchRadians = recipe.firePitchRadians;
        runtime.groundUnitPitchRadians = recipe.groundUnitPitchRadians;
        runtime.naturalYawRadians = recipe.naturalYawRadians;
        runtime.naturalPitchRadians = recipe.naturalPitchRadians;
        runtime.yawRadians = recipe.naturalYawRadians;
        runtime.pitchRadians = recipe.naturalPitchRadians;
        runtime.fireAngleSweepRadians = recipe.fireAngleSweepRadians;
        runtime.sweepSpeedModifier = recipe.sweepSpeedModifier;
        runtime.minimumIdleScanAngleRadians =
            recipe.minimumIdleScanAngleRadians;
        runtime.maximumIdleScanAngleRadians =
            recipe.maximumIdleScanAngleRadians;
        runtime.recenterMilliseconds = recipe.recenterMilliseconds;
        runtime.minimumIdleScanIntervalMilliseconds =
            recipe.minimumIdleScanIntervalMilliseconds;
        runtime.maximumIdleScanIntervalMilliseconds =
            recipe.maximumIdleScanIntervalMilliseconds;
        runtime.controlledWeaponSlots = recipe.controlledWeaponSlots;
        runtime.positiveSweep = recipe.positiveSweep;
        runtime.allowsPitch = recipe.allowsPitch;
        runtime.firesWhileTurning = recipe.firesWhileTurning;
    }
    weapons.turretsLinked = plan->turretsLinked;
}

[[nodiscard]] Fixed normalizeSignedRadians(Fixed radians) noexcept {
    int64_t raw = radians.raw() % kFixedFullTurn.raw();
    if (raw > kFixedPi.raw()) raw -= kFixedFullTurn.raw();
    if (raw < -kFixedPi.raw()) raw += kFixedFullTurn.raw();
    return Fixed::from_raw(raw);
}

[[nodiscard]] Fixed approachAngle(
    Fixed current, Fixed desired, Fixed maximumStep) noexcept {
    const Fixed delta = normalizeSignedRadians(desired - current);
    if (maximumStep <= kFixedZero) return current;
    if (Fixed::abs(delta) <= maximumStep)
        return normalizeSignedRadians(desired);
    return normalizeSignedRadians(
        current + (delta < kFixedZero ? -maximumStep : maximumStep));
}

// RefCode TurretAI::friend_turnTowardsAngle branches on
// `fabs(angleDiff) < turnRate`: the snap branch clears
// MODELCONDITION_TURRET_ROTATE, the stepping branch sets it and raises
// m_playRotSound. Share approachAngle's exact predicate so the published
// condition can never disagree with the motion this tick produced. A turret with
// no effective turn rate cannot rotate, so it also cannot latch the condition.
[[nodiscard]] bool turretAngleStepRotates(
    Fixed current, Fixed desired, Fixed maximumStep) noexcept {
    if (maximumStep <= kFixedZero) return false;
    return Fixed::abs(normalizeSignedRadians(desired - current)) > maximumStep;
}

[[nodiscard]] Fixed approachLinear(
    Fixed current, Fixed desired, Fixed maximumStep) noexcept {
    if (maximumStep <= kFixedZero) return current;
    const Fixed delta = desired - current;
    if (Fixed::abs(delta) <= maximumStep) return desired;
    return current + (delta < kFixedZero ? -maximumStep : maximumStep);
}

void advanceTurretsTowardTarget(
    ObjectWeaponComponent& weapons,
    const LogicFixedVec3& source, Fixed sourceYaw,
    const LogicFixedVec3& target,
    const ObjectGeometryComponent* sourceGeometry,
    const ObjectGeometryComponent* targetGeometry,
    game::WeaponSlot selectedSlot,
    Fixed selectedAttackRange,
    bool selectedTargetUsesGroundPitch,
    uint32_t logicFramesPerSecond,
    uint64_t confirmedTick) {
    Fixed targetZ = target.z;
    if (targetGeometry) {
        targetZ += Fixed::max(kFixedZero, targetGeometry->heightFixed) *
                   kFixedHalf;
    }
    Fixed sourceZ = source.z;
    if (sourceGeometry) {
        sourceZ += Fixed::max(kFixedZero, sourceGeometry->heightFixed) *
                   kFixedHalf;
    }
    const Fixed dx = target.x - source.x;
    const Fixed dy = target.y - source.y;
    const Fixed horizontal = Fixed::sqrt(dx * dx + dy * dy);
    if (horizontal <= kFixedHorizontalEpsilon) return;
    const Fixed desiredYaw = normalizeSignedRadians(
        math::fixed_atan2(dy, dx) - sourceYaw);
    const Fixed heightDelta = targetZ - sourceZ;
    const Fixed actualPitch = math::fixed_atan2(heightDelta, horizontal);
    const Fixed distance = Fixed::sqrt(
        horizontal * horizontal + heightDelta * heightDelta);
    const uint8_t selectedSlotBit = static_cast<uint8_t>(
        1u << static_cast<size_t>(selectedSlot));
    const Fixed inverseRate = Fixed::from_fraction(
        1, static_cast<int64_t>(std::max<uint32_t>(1, logicFramesPerSecond)));
    for (ObjectTurretRuntime& turret : weapons.turrets) {
        if (turret.controlledWeaponSlots == 0 || !turret.enabled) continue;
        turret.recenterAtTick = 0;
        turret.nextIdleScanTick = 0;
        turret.idlePhase = ObjectTurretIdlePhase::Holding;
        const bool ownsSelectedSlot = weapons.turretsLinked ||
            (turret.controlledWeaponSlots & selectedSlotBit) != 0;
        const size_t selectedSlotIndex = static_cast<size_t>(selectedSlot);
        const Fixed sweep = ownsSelectedSlot
            ? Fixed::max(kFixedZero,
                         turret.fireAngleSweepRadians[selectedSlotIndex])
            : kFixedZero;
        const bool sweepActive = sweep > kFixedZero &&
            turret.sweepEnabledUntilTick > confirmedTick;
        const Fixed aimedYaw = sweepActive
            ? normalizeSignedRadians(
                  desiredYaw + (turret.positiveSweep ? sweep : -sweep))
            : desiredYaw;
        const Fixed speedModifier = sweepActive
            ? Fixed::max(kFixedZero,
                         turret.sweepSpeedModifier[selectedSlotIndex])
            : kFixedOne;
        const Fixed aimYawStep =
            turret.turnRateRadiansPerSecond * speedModifier * inverseRate;
        // TurretAI.cpp:1080, the aim state's friend_turnTowardsAngle call.
        turret.rotating = turretAngleStepRotates(
            turret.yawRadians, aimedYaw, aimYawStep);
        turret.yawRadians = approachAngle(
            turret.yawRadians, aimedYaw, aimYawStep);
        // TurretAIAimTurretState flips at the same ~2 degree alignment
        // threshold used by friend_turnTowardsAngle(), and it does so for an
        // authored sweep even before notifyFired() opens the three-frame
        // sweep window. Requiring exact equality and an already-active window
        // made short-cadence weapons repeatedly approach one side without
        // reproducing the original oscillation phase.
        const bool alignedToAimedYaw = Fixed::abs(normalizeSignedRadians(
            aimedYaw - turret.yawRadians)) <= kFixedYawAlignmentTolerance;
        if (sweep > kFixedZero && alignedToAimedYaw) {
            turret.positiveSweep = !turret.positiveSweep;
        }
        if (!turret.allowsPitch) continue;
        Fixed desiredPitch = turret.firePitchRadians > kFixedZero
            ? turret.firePitchRadians : actualPitch;
        if (turret.firePitchRadians <= kFixedZero &&
            selectedTargetUsesGroundPitch &&
            (weapons.turretsLinked ||
             (turret.controlledWeaponSlots & selectedSlotBit) != 0) &&
            turret.groundUnitPitchRadians > kFixedZero) {
            // TurretAIAimTurretState biases ground/immobile targets upward by
            // GroundUnitPitch * distance / weapon range. Close targets receive
            // proportionally less bias so the projectile does not pass over
            // them. Weapon::getAttackRange supplies at least one unit to the
            // original division; preserve that guard for malformed content.
            const Fixed safeRange = Fixed::max(kFixedOne, selectedAttackRange);
            desiredPitch = actualPitch +
                turret.groundUnitPitchRadians * (distance / safeRange);
        }
        const Fixed clampedPitch = Fixed::max(
            desiredPitch, turret.minimumPitchRadians);
        turret.pitchRadians = approachLinear(
            turret.pitchRadians, clampedPitch,
            turret.pitchRateRadiansPerSecond * inverseRate);
    }
}

void advanceTurretsTowardNatural(
    ObjectWeaponComponent& weapons, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick, SimulationRandom& random) noexcept {
    const uint64_t rate = std::max<uint32_t>(1, logicFramesPerSecond);
    const Fixed halfRate = Fixed::from_fraction(
        1, static_cast<int64_t>(rate) * 2);
    for (ObjectTurretRuntime& turret : weapons.turrets) {
        if (turret.controlledWeaponSlots == 0) continue;

        // TurretAI.cpp:1222, the recenter state's friend_turnTowardsAngle call
        // (rateModifier 0.5, mirrored by halfRate).
        const Fixed naturalYawStep =
            turret.turnRateRadiansPerSecond * halfRate;
        if (turret.forcedRecentering) {
            turret.rotating = turretAngleStepRotates(
                turret.yawRadians, turret.naturalYawRadians, naturalYawStep);
            turret.yawRadians = approachAngle(
                turret.yawRadians, turret.naturalYawRadians, naturalYawStep);
            if (turret.allowsPitch) {
                turret.pitchRadians = approachLinear(
                    turret.pitchRadians, turret.naturalPitchRadians,
                    turret.pitchRateRadiansPerSecond * halfRate);
            }
            const bool yawAligned = normalizeSignedRadians(
                turret.naturalYawRadians - turret.yawRadians) == kFixedZero;
            const bool pitchAligned = !turret.allowsPitch ||
                turret.pitchRadians == turret.naturalPitchRadians;
            if (yawAligned && pitchAligned) {
                turret.forcedRecentering = false;
                turret.idlePhase = ObjectTurretIdlePhase::Waiting;
                turret.recenterAtTick = 0;
                turret.nextIdleScanTick = 0;
            }
            continue;
        }
        if (!turret.enabled) continue;

        const auto scheduleIdleScan = [&]() noexcept {
            const uint32_t minimum = std::min(
                turret.minimumIdleScanIntervalMilliseconds,
                turret.maximumIdleScanIntervalMilliseconds);
            const uint32_t maximum = std::max(
                turret.minimumIdleScanIntervalMilliseconds,
                turret.maximumIdleScanIntervalMilliseconds);
            const int32_t boundedMinimum = static_cast<int32_t>(std::min<uint32_t>(
                minimum, static_cast<uint32_t>(INT32_MAX)));
            const int32_t boundedMaximum = static_cast<int32_t>(std::min<uint32_t>(
                maximum, static_cast<uint32_t>(INT32_MAX)));
            const uint32_t milliseconds = static_cast<uint32_t>(
                random.integerInclusive(boundedMinimum, boundedMaximum));
            turret.nextIdleScanTick = saturatingTickAdd(
                confirmedTick,
                millisecondsToFrames(milliseconds,
                                     logicFramesPerSecond));
        };
        const auto startHolding = [&]() noexcept {
            turret.idlePhase = ObjectTurretIdlePhase::Holding;
            turret.recenterAtTick = saturatingTickAdd(
                confirmedTick,
                millisecondsToFrames(turret.recenterMilliseconds,
                                     logicFramesPerSecond));
        };

        if (turret.idlePhase == ObjectTurretIdlePhase::Waiting) {
            if (turret.nextIdleScanTick == 0) scheduleIdleScan();
            if (confirmedTick < turret.nextIdleScanTick) continue;
            turret.nextIdleScanTick = 0;
            const Fixed minimum = Fixed::min(
                turret.minimumIdleScanAngleRadians,
                turret.maximumIdleScanAngleRadians);
            const Fixed maximum = Fixed::max(
                turret.minimumIdleScanAngleRadians,
                turret.maximumIdleScanAngleRadians);
            if (maximum <= kFixedZero) {
                startHolding();
                continue;
            }
            Fixed offset = random.fixedInclusive(
                Fixed::max(kFixedZero, minimum), maximum);
            if (random.integerInclusive(0, 1) == 0) offset = -offset;
            turret.idleScanTargetYawRadians = normalizeSignedRadians(
                turret.naturalYawRadians + offset);
            turret.idlePhase = ObjectTurretIdlePhase::Scanning;
        }

        if (turret.idlePhase == ObjectTurretIdlePhase::Scanning) {
            // TurretAI.cpp:1374, the idle-scan state's turn call.
            turret.rotating = turretAngleStepRotates(
                turret.yawRadians, turret.idleScanTargetYawRadians,
                naturalYawStep);
            turret.yawRadians = approachAngle(
                turret.yawRadians, turret.idleScanTargetYawRadians,
                naturalYawStep);
            if (turret.allowsPitch) {
                turret.pitchRadians = approachLinear(
                    turret.pitchRadians, turret.naturalPitchRadians,
                    turret.pitchRateRadiansPerSecond * halfRate);
            }
            const bool yawAligned = normalizeSignedRadians(
                turret.idleScanTargetYawRadians - turret.yawRadians) ==
                kFixedZero;
            const bool pitchAligned = !turret.allowsPitch ||
                turret.pitchRadians == turret.naturalPitchRadians;
            if (yawAligned && pitchAligned) startHolding();
            continue;
        }

        if (turret.idlePhase == ObjectTurretIdlePhase::Holding) {
            if (turret.recenterAtTick == 0) startHolding();
            if (confirmedTick < turret.recenterAtTick) continue;
            turret.idlePhase = ObjectTurretIdlePhase::Recentering;
            turret.recenterAtTick = 0;
        }

        if (turret.idlePhase == ObjectTurretIdlePhase::Recentering) {
            turret.rotating = turretAngleStepRotates(
                turret.yawRadians, turret.naturalYawRadians, naturalYawStep);
            turret.yawRadians = approachAngle(
                turret.yawRadians, turret.naturalYawRadians, naturalYawStep);
            if (turret.allowsPitch) {
                turret.pitchRadians = approachLinear(
                    turret.pitchRadians, turret.naturalPitchRadians,
                    turret.pitchRateRadiansPerSecond * halfRate);
            }
            const bool yawAligned = normalizeSignedRadians(
                turret.naturalYawRadians - turret.yawRadians) == kFixedZero;
            const bool pitchAligned = !turret.allowsPitch ||
                turret.pitchRadians == turret.naturalPitchRadians;
            if (yawAligned && pitchAligned) {
                turret.idlePhase = ObjectTurretIdlePhase::Waiting;
                scheduleIdleScan();
            }
        }
    }
}

void notifyTurretWeaponFired(
    ObjectWeaponComponent& weapons, game::WeaponSlot slot,
    uint64_t confirmedTick) noexcept {
    const size_t slotIndex = static_cast<size_t>(slot);
    const uint8_t slotBit = static_cast<uint8_t>(1u << slotIndex);
    for (ObjectTurretRuntime& turret : weapons.turrets) {
        if (!turret.enabled ||
            (turret.controlledWeaponSlots & slotBit) == 0) {
            continue;
        }
        if (turret.firesWhileTurning) {
            turret.continuousFireSoundUntilTick = std::max(
                turret.continuousFireSoundUntilTick,
                saturatingTickAdd(confirmedTick, 3u));
        }
        if (turret.fireAngleSweepRadians[slotIndex] <= kFixedZero)
            continue;
        turret.sweepEnabledUntilTick = std::max(
            turret.sweepEnabledUntilTick,
            saturatingTickAdd(confirmedTick, 3u));
    }
}

[[nodiscard]] const ObjectTurretRuntime* turretForWeaponSlot(
    const ObjectWeaponComponent& weapons, game::WeaponSlot slot) noexcept {
    const uint8_t slotBit = static_cast<uint8_t>(
        1u << static_cast<size_t>(slot));
    for (const ObjectTurretRuntime& turret : weapons.turrets) {
        if ((turret.controlledWeaponSlots & slotBit) != 0) return &turret;
    }
    return nullptr;
}

[[nodiscard]] bool turretAlignedForWeaponSlot(
    const ObjectWeaponComponent& weapons, game::WeaponSlot slot,
    const LogicFixedVec3& source, Fixed sourceYaw,
    const LogicFixedVec3& target,
    const ObjectGeometryComponent* sourceGeometry,
    const ObjectGeometryComponent* targetGeometry,
    Fixed attackRange, Fixed acceptableAimDelta,
    bool targetUsesGroundPitch) noexcept {
    const ObjectTurretRuntime* turret = turretForWeaponSlot(weapons, slot);
    if (!turret) {
        const Fixed dx = target.x - source.x;
        const Fixed dy = target.y - source.y;
        if (Fixed::sqrt(dx * dx + dy * dy) <=
            kFixedHorizontalEpsilon) return true;
        const Fixed relative = normalizeSignedRadians(
            math::fixed_atan2(dy, dx) - sourceYaw);
        return Fixed::abs(relative) <= Fixed::max(
            kFixedYawAlignmentTolerance,
            Fixed::max(kFixedZero, acceptableAimDelta));
    }
    if (!turret->enabled) return false;
    if (turret->firesWhileTurning) return true;

    Fixed targetZ = target.z;
    if (targetGeometry) {
        targetZ += Fixed::max(kFixedZero, targetGeometry->heightFixed) *
                   kFixedHalf;
    }
    Fixed sourceZ = source.z;
    if (sourceGeometry) {
        sourceZ += Fixed::max(kFixedZero, sourceGeometry->heightFixed) *
                   kFixedHalf;
    }
    const Fixed dx = target.x - source.x;
    const Fixed dy = target.y - source.y;
    const Fixed horizontal = Fixed::sqrt(dx * dx + dy * dy);
    // FireWeaponPower's no-target form deliberately attacks the owner's own
    // position. No direction exists to align against, so preserve the
    // current turret pose and allow the weapon cycle to proceed.
    if (horizontal <= kFixedHorizontalEpsilon) return true;

    // TurretAIAimTurretState uses a 0.035-radian relative yaw tolerance.
    // Pitch snaps to the requested value before the state may enter FIRE.
    const Fixed desiredYaw = normalizeSignedRadians(
        math::fixed_atan2(dy, dx) - sourceYaw);
    const Fixed sweepTolerance = Fixed::max(
        kFixedYawAlignmentTolerance,
        Fixed::max(kFixedZero,
                   turret->fireAngleSweepRadians[static_cast<size_t>(slot)]));
    if (Fixed::abs(normalizeSignedRadians(
            desiredYaw - turret->yawRadians)) > sweepTolerance) {
        return false;
    }
    if (!turret->allowsPitch) return true;

    const Fixed authoredFirePitch = turret->firePitchRadians;
    const Fixed actualPitch = math::fixed_atan2(targetZ - sourceZ, horizontal);
    Fixed desiredPitch = authoredFirePitch > kFixedZero
        ? authoredFirePitch
        : actualPitch;
    if (authoredFirePitch <= kFixedZero && targetUsesGroundPitch &&
        turret->groundUnitPitchRadians > kFixedZero) {
        const Fixed heightDelta = targetZ - sourceZ;
        const Fixed distance = Fixed::sqrt(
            horizontal * horizontal + heightDelta * heightDelta);
        desiredPitch = actualPitch +
            turret->groundUnitPitchRadians *
                (distance / Fixed::max(kFixedOne, attackRange));
    }
    const Fixed clampedPitch = Fixed::max(
        desiredPitch, turret->minimumPitchRadians);
    return Fixed::abs(clampedPitch - turret->pitchRadians) <=
        kFixedPitchAlignmentTolerance;
}

} // namespace engine::object_combat_detail

namespace engine {

void setObjectTurretsEnabled(ObjectWeaponComponent& weapons,
                             bool enabled) noexcept {
    for (ObjectTurretRuntime& turret : weapons.turrets) {
        if (turret.controlledWeaponSlots == 0) continue;
        turret.enabled = enabled;
        if (enabled) {
            turret.forcedRecentering = false;
            turret.recenterAtTick = 0;
            turret.nextIdleScanTick = 0;
            turret.idlePhase = ObjectTurretIdlePhase::Waiting;
        }
    }
}

void requestObjectTurretRecentering(
    ObjectWeaponComponent& weapons) noexcept {
    for (ObjectTurretRuntime& turret : weapons.turrets) {
        if (turret.controlledWeaponSlots == 0) continue;
        turret.enabled = false;
        turret.forcedRecentering = true;
        turret.recenterAtTick = 0;
        turret.nextIdleScanTick = 0;
        turret.sweepEnabledUntilTick = 0;
        turret.idlePhase = ObjectTurretIdlePhase::Recentering;
    }
}

bool objectTurretsInNaturalPosition(
    const ObjectWeaponComponent& weapons) noexcept {
    for (const ObjectTurretRuntime& turret : weapons.turrets) {
        if (turret.controlledWeaponSlots == 0) continue;
        if (object_combat_detail::normalizeSignedRadians(
                turret.naturalYawRadians - turret.yawRadians) !=
                object_combat_detail::kFixedZero ||
            (turret.allowsPitch &&
             turret.pitchRadians != turret.naturalPitchRadians)) {
            return false;
        }
    }
    return true;
}

bool objectTurretsAreForcedRecentering(
    const ObjectWeaponComponent& weapons) noexcept {
    return std::any_of(
        weapons.turrets.begin(), weapons.turrets.end(),
        [](const ObjectTurretRuntime& turret) noexcept {
            return turret.controlledWeaponSlots != 0 &&
                turret.forcedRecentering;
        });
}

} // namespace engine
