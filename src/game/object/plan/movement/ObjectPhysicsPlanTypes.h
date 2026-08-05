#pragma once

#include "core/container/container_types.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "math/fixed/q32_32.h"

namespace game {

struct ThingTemplate;

// Immutable, pointer-free projection of the final inherited
// PhysicsBehavior recipe. Spawn copies this shared plan into live Q32.32
// state; confirmed ticks never inspect ModuleData or parse strings.
struct ObjectPhysicsPlan final {
    using Scalar = math::q32_32;

    Scalar mass{int32_t{1}};
    Scalar shockResistance{};
    Scalar shockMaxYaw{0.05f};
    Scalar shockMaxPitch{0.025f};
    Scalar shockMaxRoll{0.025f};
    Scalar forwardFrictionPerSecond{4.5f};
    Scalar lateralFrictionPerSecond{4.5f};
    Scalar zFrictionPerSecond{24.0f};
    Scalar aerodynamicFrictionPerSecond{};
    Scalar centerOfMassOffset{};
    Scalar minimumFallHeight{40.0f};
    Scalar fallHeightDamageFactor{int32_t{1}};
    Scalar pitchRollYawFactor{int32_t{2}};
    container::String crashIntoBuildingWeapon{
        "VehicleCrashesIntoBuildingWeapon"};
    container::String crashIntoNonBuildingWeapon{
        "VehicleCrashesIntoNonBuildingWeapon"};
    bool allowBouncing = false;
    bool allowCollideForce = true;
    bool killWhenRestingOnGround = false;
};

[[nodiscard]] container::SharedPtr<const ObjectPhysicsPlan>
compileObjectPhysicsPlan(const ThingTemplate& templateData);

} // namespace game
