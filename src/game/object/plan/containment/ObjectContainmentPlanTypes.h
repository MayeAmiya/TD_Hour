#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/definition/ObjectKindOf.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

namespace game {
struct ThingTemplate;
}

namespace engine {

enum class ObjectContainmentKind : uint8_t {
    Open,
    MobNexus,
    Cave,
    Heal,
    Garrison,
    Transport,
    RiderChange,
    Tunnel,
    Overlord,
    Helix,
    Parachute,
};

using ObjectContainmentKindMask = uint16_t;

[[nodiscard]] constexpr ObjectContainmentKindMask
objectContainmentKindBit(ObjectContainmentKind kind) noexcept {
    return static_cast<ObjectContainmentKindMask>(
        ObjectContainmentKindMask{1} << static_cast<uint8_t>(kind));
}

// Value-only continuation emitted by one authored Contain::onDie capability.
// Damage children close first in the Session transaction stack; this command
// then removes exactly the edges owned by that same module occurrence before
// DeathWalk advances to the next authored Behavior.
enum class ObjectContainmentDeathFinalizePhase : uint8_t {
    CullBlockedRiders,
    DetachRemaining,
};

struct ObjectContainmentDeathFinalizeCommand final {
    ObjectId container = INVALID_OBJECT_ID;
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
    ObjectContainmentDeathFinalizePhase phase =
        ObjectContainmentDeathFinalizePhase::CullBlockedRiders;
};

enum class ObjectContainmentDeathFinalizeAdvance : uint8_t {
    ChildrenEmitted,
    Completed,
};

enum class ObjectTransportBehaviorKind : uint8_t {
    BunkerBuster,
    BattleBusSlowDeath,
    AssaultTransportAI,
    DeliverPayloadAI,
    PilotFindVehicle,
    Hijacker,
    TransportAI,
};

// Immutable projection of the seven behavior/update modules which coordinate
// with Contain.  Names remain content handles at this layer; GameSession's
// command consumers resolve them through the sealed content snapshot.
struct ObjectTransportBehaviorRule final {
    ObjectTransportBehaviorKind kind = ObjectTransportBehaviorKind::TransportAI;
    uint32_t authoredOrder = 0;
    ::container::String upgradeRequired;
    UpgradeContentId upgradeRequiredId = INVALID_UPGRADE_CONTENT_ID;
    ::container::String fxStart;
    ::container::String oclStart;
    ::container::String fxFinish;
    ::container::String oclFinish;
    ::container::String shockwaveWeapon;
    ::container::String occupantDamageWeapon;
    ::container::String attachBone;
    ::container::String parachuteTemplate;
    ::container::String payloadTemplate;
    ::container::String visibleDropBoneBaseName;
    ::container::String visibleSubObjectBaseName;
    ::container::String visiblePayloadTemplate;
    ::container::String visiblePayloadWeapon;
    ::container::String strafingWeaponSlot;
    ::container::String strafeWeaponFx;
    ::container::String deliveryDecal;
    math::q32_32 passengerDamageFraction{};
    math::q32_32 minimumHealthFraction{};
    math::q32_32 healMembersAtLifeFraction{};
    math::q32_32 seismicRadius{};
    math::q32_32 seismicMagnitude{};
    math::q32_32 scanRange{};
    math::q32_32 clearRange{};
    math::q32_32 deliveryDistance{};
    math::q32_32 exitPitchRate{};
    math::q32_32 diveStartDistance{};
    math::q32_32 diveEndDistance{};
    math::q32_32 strafeLength{};
    math::q32_32 deliveryDecalRadius{};
    uint32_t deliveryDecalShadowTypeMask = 0x20u;
    math::q32_32 deliveryDecalMinimumOpacity{int32_t{1}};
    math::q32_32 deliveryDecalMaximumOpacity{int32_t{1}};
    uint32_t deliveryDecalOpacityThrobMilliseconds = 1000;
    ::container::Array<uint8_t, 4> deliveryDecalColor{0, 0, 0, 0};
    bool deliveryDecalUsesPlayerColor = true;
    bool deliveryDecalOnlyVisibleToOwningPlayer = true;
    math::q32_32 throwForce{};
    math::q32_32 dropOffsetX{};
    math::q32_32 dropOffsetY{};
    math::q32_32 dropOffsetZ{};
    math::q32_32 dropVarianceX{};
    math::q32_32 dropVarianceY{};
    math::q32_32 dropVarianceZ{};
    uint32_t emptyDestructionDelayMilliseconds = 0;
    uint32_t scanRateMilliseconds = 0;
    uint32_t doorDelayMilliseconds = 0;
    uint32_t dropDelayMilliseconds = 0;
    uint32_t maximumAttempts = 1;
    uint32_t visibleItemsDroppedPerInterval = 0;
    uint32_t visiblePayloadCount = 0;
    uint32_t crashThroughBunkerFxFrequency = 4;
    bool inheritTransportVelocity = false;
    bool parachuteDirectly = false;
    bool selfDestructAfterDelivery = false;
    bool fireWeaponPayload = false;
    bool putInContainer = false;
};

struct ObjectContainmentRule final {
    ObjectContainmentKind kind = ObjectContainmentKind::Transport;
    uint32_t authoredOrder = 0;
    // CaveContain groups entry points by this signed authored network index.
    // It remains mutable in ObjectContainmentRuntimeComponent for SET_CAVE_INDEX.
    int32_t caveIndex = 0;
    uint32_t containMax = 1;
    game::ObjectKindOfMask allowInsideKindOf{};
    game::ObjectKindOfMask forbidInsideKindOf{};
    ::container::Vector<::container::String> payloadTemplateNames;
    ::container::String enterSound;
    ::container::String exitSound;
    ::container::String exitBone;
    ::container::String parachuteOpenSound;
    math::q32_32 damagePercentToUnits{};
    math::q32_32 exitPitchRate{};
    math::q32_32 pitchRateMax{};
    math::q32_32 rollRateMax{};
    math::q32_32 lowAltitudeDamping = math::q32_32::from_fraction(1, 5);
    math::q32_32 parachuteOpenDistance{};
    math::q32_32 freeFallDamageFraction = math::q32_32::from_fraction(1, 2);
    math::q32_32 killWhenLandingInWaterSlop{10};
    uint32_t timeForFullHealMilliseconds = 0;
    uint32_t exitDelayMilliseconds = 0;
    uint32_t doorOpenTimeMilliseconds = 0;
    uint32_t numberOfExitPaths = 1;
    math::q32_32 healAmountPerSecond{};
    bool passengersAllowedToFire = false;
    bool passengersInTurret = false;
    bool weaponBonusPassedToPassengers = false;
    bool allowAlliesInside = true;
    bool allowEnemiesInside = true;
    bool allowNeutralInside = true;
    bool burnedDeathToUnits = true;
    bool armedRidersUpgradeMyWeaponSet = false;
    bool mobileGarrison = false;
    bool healGarrisonObjects = false;
    bool enclosingContainer = true;
    bool destroyPassengersWithContainer = false;
    bool followsContainerTransform = true;
    bool exposeStealthUnits = true;
    bool experienceSinkForRider = false;
    bool shouldDrawPips = true;
    bool scatterNearbyOnExit = true;
    bool orientLikeContainerOnExit = false;
    bool keepContainerVelocityOnExit = false;
    bool goAggressiveOnExit = false;
    bool resetMoodCheckTimeOnExit = true;
    bool destroyRidersWhoAreNotFreeToExit = false;
    bool delayExitInAir = false;
    // GarrisonContain owns this admission/clear policy. Projectile impact
    // reads the immutable containment plan rather than reparsing ModuleData
    // from the live archetype on every collision.
    bool immuneToClearBuildingAttacks = false;
    // RailedTransportContain overrides OpenContain::exitObjectViaDoor and
    // delegates placement to RailedTransportDockUpdate; it must not consume
    // the ordinary ExitStart/ExitEnd table merely because it inherits the
    // common OpenContain fields.
    bool railedDockOwnsExit = false;
    struct RiderInfo final {
        ::container::String templateName;
        ::container::String modelCondition;
        ::container::String weaponSetCondition;
        ::container::String objectStatus;
        ::container::String commandSet;
        ::container::String locomotorSet;
    };
    ::container::Vector<RiderInfo> riders;
    uint32_t scuttleDelayMilliseconds = 0;
    ::container::String scuttleStatus = "TOPPLED";
};

struct ObjectContainmentPlan final {
    ::container::Vector<ObjectContainmentRule> rules;
    ::container::Vector<ObjectTransportBehaviorRule> behaviorRules;
    ::container::Vector<::container::String> diagnostics;
    // Load-time capability projection. Runtime systems use this instead of
    // rescanning authored module names or the typed rule array for simple
    // interface-presence questions.
    ObjectContainmentKindMask kindMask = 0;
};


} // namespace engine

namespace game {

[[nodiscard]] container::SharedPtr<const engine::ObjectContainmentPlan>
compileObjectContainmentPlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog = nullptr);

} // namespace game
