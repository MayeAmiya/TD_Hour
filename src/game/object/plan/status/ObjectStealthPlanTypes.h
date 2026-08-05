#pragma once

#include "core/container/container_types.h"
#include "game/object/definition/ObjectKindOf.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <limits>

namespace game {

struct ThingTemplate;

enum class ObjectStealthForbiddenCondition : uint8_t {
    Attacking,
    Moving,
    UsingAbility,
    FiringPrimary,
    FiringSecondary,
    FiringTertiary,
    NoBlackMarket,
    TakingDamage,
    RidersAttacking,
    Count,
};

using ObjectStealthForbiddenMask = uint16_t;

[[nodiscard]] constexpr ObjectStealthForbiddenMask
objectStealthForbiddenBit(ObjectStealthForbiddenCondition condition) noexcept {
    return static_cast<ObjectStealthForbiddenMask>(
        ObjectStealthForbiddenMask{1} << static_cast<uint8_t>(condition));
}

struct ObjectStealthPlan final {
    uint32_t authoredOrder = 0;
    uint32_t stealthDelayMilliseconds = std::numeric_limits<uint32_t>::max();
    uint32_t blackMarketCheckMilliseconds = 0;
    math::q32_32 moveThresholdUnitsPerSecond{};
    ObjectStealthForbiddenMask forbiddenConditions = 0;
    ObjectStatusMask hintDetectableStatuses = 0;
    ObjectStatusMask requiredStatuses = 0;
    ObjectStatusMask forbiddenStatuses = 0;
    math::q32_32 friendlyOpacityMinimum{0.5};
    math::q32_32 friendlyOpacityMaximum{int32_t{1}};
    uint32_t pulseMilliseconds = 30;
    math::q32_32 revealDistanceFromTarget{};
    container::String disguiseFx;
    container::String disguiseRevealFx;
    uint32_t disguiseTransitionMilliseconds = 0;
    uint32_t disguiseRevealTransitionMilliseconds = 0;
    container::String enemyDetectionEva;
    container::String ownDetectionEva;
    bool disguisesAsTeam = false;
    bool orderIdleEnemiesToAttackOnReveal = false;
    bool innateStealth = true;
    bool useRiderStealth = false;
    bool grantedBySpecialPower = false;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectStealthPlan>
compileObjectStealthPlan(const ThingTemplate& templateData);

// StealthDetectorUpdate is deliberately compiled independently from
// StealthUpdate: many detector objects are not themselves stealth-capable.
struct ObjectStealthDetectorPlan final {
    uint32_t authoredOrder = 0;
    uint32_t detectionRateMilliseconds = 1;
    math::q32_32 detectionRange{};
    ObjectKindOfMask extraRequiredKinds{};
    ObjectKindOfMask extraForbiddenKinds{};
    container::String pingSound;
    container::String loudPingSound;
    container::String beaconParticleSystem;
    container::String scanParticleSystem;
    container::String brightScanParticleSystem;
    container::String gridParticleSystem;
    container::String particleBone;
    bool initiallyDisabled = false;
    bool canDetectWhileGarrisoned = false;
    bool canDetectWhileContained = false;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectStealthDetectorPlan>
compileObjectStealthDetectorPlan(const ThingTemplate& templateData);

struct ObjectGrantStealthPlan final {
    uint32_t authoredOrder = 0;
    math::q32_32 startRadius{int32_t{0}};
    math::q32_32 finalRadius{int32_t{200}};
    math::q32_32 radiusGrowPerFrame{int32_t{10}};
    ObjectKindOfMask allowedKinds{};
    ObjectKindOfMask forbiddenKinds{};
    bool allKindsAllowed = true;
    container::String radiusParticleSystem;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectGrantStealthPlan>
compileObjectGrantStealthPlan(const ThingTemplate& templateData);

} // namespace game

