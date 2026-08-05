#pragma once

#include "core/container/container_types.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

namespace engine {

// Immutable authored turret recipe.  Current yaw/pitch, idle phase, scan
// deadlines, and enable/recenter latches are created in ObjectTurretRuntime
// when a live object is initialized; they never belong to a Plan.
struct ObjectTurretRecipe final {
    math::q32_32 turnRateRadiansPerSecond{math::q32_32::from_fraction(3, 10)};
    math::q32_32 pitchRateRadiansPerSecond{math::q32_32::from_fraction(3, 10)};
    math::q32_32 minimumPitchRadians{};
    math::q32_32 firePitchRadians{};
    math::q32_32 groundUnitPitchRadians{};
    math::q32_32 naturalYawRadians{};
    math::q32_32 naturalPitchRadians{};
    container::Array<math::q32_32, 3> fireAngleSweepRadians{};
    container::Array<math::q32_32, 3> sweepSpeedModifier{
        math::q32_32{int32_t{1}}, math::q32_32{int32_t{1}},
        math::q32_32{int32_t{1}}};
    math::q32_32 minimumIdleScanAngleRadians{};
    math::q32_32 maximumIdleScanAngleRadians{};
    uint32_t recenterMilliseconds = 2000;
    uint32_t minimumIdleScanIntervalMilliseconds = 333333300u;
    uint32_t maximumIdleScanIntervalMilliseconds = 333333300u;
    uint8_t controlledWeaponSlots = 0;
    bool positiveSweep = true;
    bool allowsPitch = false;
    bool firesWhileTurning = false;
};

} // namespace engine
