#pragma once

#include "core/container/container_types.h"
#include "game/object/definition/ObjectKindOf.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <optional>

namespace game {
struct ThingTemplate;
}

namespace engine {

enum class ObjectProjectileBehaviorKind : uint8_t {
    Unsupported,
    DumbBezier,
    MissileAI,
    NeutronMissile,
    PlacedHelper,
};

struct ObjectProjectileDeliveryDecalPlan final {
    uint32_t authoredOrder = 0;
    container::String texture;
    math::q32_32 radius{};
    uint32_t shadowTypeMask = 0x20u;
    math::q32_32 minimumOpacity{int32_t{1}};
    math::q32_32 maximumOpacity{int32_t{1}};
    uint32_t opacityThrobMilliseconds = 1000;
    container::Array<uint8_t, 4> color{0, 0, 0, 0};
    bool usesPlayerColor = true;
    bool onlyVisibleToOwningPlayer = true;
};

struct ObjectProjectilePlan final {
    ObjectProjectileBehaviorKind behaviorKind =
        ObjectProjectileBehaviorKind::Unsupported;
    game::ObjectKindOfMask garrisonHitRequiredKindMask{};
    game::ObjectKindOfMask garrisonHitForbiddenKindMask{};
    container::String garrisonHitFx;
    uint32_t garrisonHitKillCount = 0;
    bool detonateCallsKill = false;

    math::q32_32 firstHeight{};
    math::q32_32 secondHeight{};
    math::q32_32 firstPercentIndent{};
    math::q32_32 secondPercentIndent{};
    math::q32_32 targetAdjustDistancePerSecond{};
    uint32_t maximumLifespanMilliseconds = 10000;
    bool orientToFlightPath = true;
    bool tumbleRandomly = false;

    container::String ignitionFx;
    math::q32_32 noTurnDistance{};
    math::q32_32 diveDistance{};
    math::q32_32 lockDistance{int32_t{75}};
    math::q32_32 distanceScatterWhenJammed{int32_t{75}};
    math::q32_32 initialVelocity{};
    uint32_t ignitionDelayMilliseconds = 0;
    uint32_t fuelLifetimeMilliseconds = 0;
    std::optional<uint32_t> killSelfDelayMilliseconds;
    bool tryToFollowTarget = true;
    bool useWeaponSpeed = false;
    bool detonateOnNoFuel = false;

    container::String launchFx;
    math::q32_32 targetFromDirectlyAbove{};
    math::q32_32 maximumTurnRateRadiansPerSecond =
        math::q32_32::from_raw(74886359260ll);
    math::q32_32 forwardDamping{};
    math::q32_32 relativeSpeed{int32_t{1}};
    math::q32_32 specialSpeedHeight{};
    math::q32_32 specialAccelerationFactor{int32_t{1}};
    math::q32_32 specialJitterDistance{};
    uint32_t specialSpeedMilliseconds = 0;
    std::optional<ObjectProjectileDeliveryDecalPlan> deliveryDecal;
};

[[nodiscard]] container::SharedPtr<const ObjectProjectilePlan>
compileObjectProjectilePlan(const game::ThingTemplate& templateData);

} // namespace engine
