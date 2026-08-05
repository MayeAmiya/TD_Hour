#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/player/PlayerTypes.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/definition/ObjectKindOf.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "core/ecs/ObjectId.h"
#include "game/base/DamageTypes.h"
#include "game/base/ObjectVeterancy.h"
#include "math/fixed/q32_32.h"
#include "game/object/plan/containment/ObjectContainmentPlanTypes.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <variant>

namespace game {
struct ThingTemplate;
namespace terrain { class TerrainLogic; }
}

namespace engine {

namespace navigation { class NavigationSystem; }

class ObjectLifecycle;
class PlayerRegistry;
class GameContentSnapshot;
struct ObjectSimulationRules;
struct ObjectOrderIntent;
struct ObjectDeleteDestroyRequest;
enum class ObjectBodyDamageState : uint8_t;

enum class ObjectTransportBehaviorPhase : uint8_t {
    Idle,
    BattleBusUndeath,
    BattleBusLanded,
    HijackerAttached,
    DeliveryApproach,
    DeliveryRecoveringOffMap,
    DeliveryReapproach,
    AwaitingDelivery,
    DeliveryHeadingOffMap,
    AssaultActive,
    AssaultRecalling,
};

struct ObjectAssaultTransportMemberState final {
    ObjectId object = INVALID_OBJECT_ID;
    bool newlyObserved = true;
};

struct ObjectTransportBehaviorState final {
    ObjectId target = INVALID_OBJECT_ID;
    uint64_t nextTick = 0;
    uint64_t nextPollTick = 0;
    uint32_t attempts = 0;
    uint32_t deliveredCount = 0;
    ObjectTransportBehaviorPhase phase = ObjectTransportBehaviorPhase::Idle;
    bool targetWasAirborne = false;
    // PilotFindVehicle's legacy m_didMoveToBase latch: a failed scan emits
    // one return request, then waits until a viable vehicle is found before
    // allowing another request.
    bool pilotReturnToBaseIssued = false;
    bool hadMapStatus = false;
    bool previousOffMap = false;
    bool hasDeliveryOverride = false;
    ::container::String deliveryContainerTemplate;
    ::container::String deliveryPayloadWeapon;
    math::q32_32 deliveryTargetX{};
    math::q32_32 deliveryTargetY{};
    math::q32_32 deliveryTargetZ{};
    // The effect/weapon target may converge or scatter independently of the
    // route followed by the delivery aircraft.  RefCode keeps m_targetPos and
    // m_moveToPos as two distinct values for exactly this reason.
    math::q32_32 deliveryRouteX{};
    math::q32_32 deliveryRouteY{};
    math::q32_32 deliveryRouteZ{};
    math::q32_32 deliveryDistance{};
    math::q32_32 exitPitchRate{};
    math::q32_32 preOpenDistance{};
    math::q32_32 previousDeliveryDistanceSquared{};
    math::q32_32 deliveryManeuverX{};
    math::q32_32 deliveryManeuverY{};
    math::q32_32 deliveryManeuverZ{};
    math::q32_32 diveStartDistance{};
    math::q32_32 diveEndDistance{};
    math::q32_32 strafeLength{};
    math::q32_32 dropOffsetX{};
    math::q32_32 dropOffsetY{};
    math::q32_32 dropOffsetZ{};
    math::q32_32 dropVarianceX{};
    math::q32_32 dropVarianceY{};
    math::q32_32 dropVarianceZ{};
    uint32_t dropDelayMilliseconds = 0;
    uint32_t maximumAttempts = 0;
    uint32_t visibleItemsDroppedPerInterval = 0;
    uint32_t visiblePayloadCount = 0;
    uint32_t visiblePayloadsDelivered = 0;
    uint8_t deliveryDiveState = 0;
    math::q32_32 deliveryExitHeadingYaw{};
    bool inheritTransportVelocity = false;
    bool parachuteDirectly = false;
    bool selfDestructAfterDelivery = false;
    bool fireWeaponPayload = false;
    bool hasPreviousDeliveryDistance = false;
    bool deliveryDoorOpen = false;
    // Door animation and passenger release are separate legacy latches.
    // DeliveringState starts opening first, waits DoorDelay, and only then
    // allows the contained payload to leave.
    bool deliveryFreeToExit = false;
    bool deliveryExitHeadingArmed = false;
    ::container::String deliveryDecal;
    math::q32_32 deliveryDecalRadius{};
    uint32_t deliveryDecalShadowTypeMask = 0x20u;
    math::q32_32 deliveryDecalMinimumOpacity{int32_t{1}};
    math::q32_32 deliveryDecalMaximumOpacity{int32_t{1}};
    uint64_t deliveryDecalOpacityThrobTicks = 30;
    ::container::Array<uint8_t, 4> deliveryDecalColor{0, 0, 0, 0};
    bool deliveryDecalUsesPlayerColor = true;
    bool deliveryDecalOnlyVisibleToOwningPlayer = true;
    ::container::String visiblePayloadTemplate;
    ::container::String visiblePayloadWeapon;
    ::container::String visibleDropBoneBaseName;
    ::container::String visibleSubObjectBaseName;
    ::container::String strafingWeaponSlot;
    ::container::String strafeWeaponFx;
    ::container::Vector<ObjectAssaultTransportMemberState> assaultMembers;
    uint64_t observedExternalRevision = 0;
    uint64_t observedAssaultOrderIssuedTick = 0;
    uint32_t observedAssaultOrderSourceSequence = 0;
    uint8_t observedAssaultOrderSource = 0;
    math::q32_32 assaultGoalX{};
    math::q32_32 assaultGoalY{};
    math::q32_32 assaultGoalZ{};
    bool assaultAttackMove = false;
    bool assaultOrderArmed = false;
    bool assaultRosterInitialized = false;
    bool hasObservedAssaultOrder = false;
};

struct ObjectContainmentExitPath final {
    math::q32_32 startX{};
    math::q32_32 startY{};
    math::q32_32 startZ{};
    math::q32_32 endX{};
    math::q32_32 endY{};
    math::q32_32 endZ{};
    bool valid = false;
    bool hasEnd = true;
};

struct ObjectContainmentExitPathSet final {
    ::container::Vector<ObjectContainmentExitPath> paths;
};

// ScriptAction NAMED_SET_EVAC_LEFT_OR_RIGHT persists this policy on the
// container. Values deliberately match RefCode's EvacDisposition ordinals;
// Invalid and BurstFromCenter both retain the normal authored exit paths.
enum class ObjectContainmentEvacuationDisposition : uint8_t {
    Invalid = 0,
    Left = 1,
    Right = 2,
    BurstFromCenter = 3,
};

// Value-only TunnelTracker combat handoff.  Every completed entrance in one
// player's network receives the same snapshot, allowing the AI adapter to
// resolve a passenger's priority target through its direct entrance without
// retaining a tracker/module pointer or scanning all tunnels.
struct ObjectTunnelNetworkCombatHandoffComponent final {
    PlayerId networkOwner = INVALID_PLAYER_ID;
    ObjectId recentNemesis = INVALID_OBJECT_ID;
    uint64_t observedTick = 0;
    uint64_t expiresTick = 0;
    uint64_t revision = 0;
};

struct ObjectContainmentRuntimeComponent final {
    ::container::SharedPtr<const ObjectContainmentPlan> plan;
    ::container::Vector<ObjectTransportBehaviorState> behaviorStates;
    // InitialRoster/InitialPayload is a one-shot module lifecycle action.
    // Keep the latch on the host rather than inferring it from current
    // contents: passengers can legitimately leave before a later lifecycle
    // callback observes the object again.
    bool initialPayloadsCreated = false;
    int32_t caveIndex = 0;
    uint64_t caveIndexRevision = 0;
    bool hasCave = false;
    uint32_t networkCapacity = 10;
    PlayerId caveOriginalOwner = INVALID_PLAYER_ID;
    ObjectTeamId caveOriginalTeam = INVALID_OBJECT_TEAM_ID;
    bool caveHasOriginalOwnership = false;
    PlayerId garrisonOriginalOwner = INVALID_PLAYER_ID;
    ObjectTeamId garrisonOriginalTeam = INVALID_OBJECT_TEAM_ID;
    bool garrisonHasOriginalOwnership = false;
    uint64_t exitNotBusyTick = 0;
    // OpenContain's DoorOpenTime is a model-condition countdown and is not
    // the TransportContain interval between passenger exits.
    uint64_t doorCloseTick = 0;
    // TransportContain::onCapture orders ordinary passengers to leave through
    // the normal door/ExitDelay path.  Keep that asynchronous transaction on
    // the host; ObjectSimulation consumes it through the public containment
    // request boundary so Rider/experience/locomotor projections are not
    // bypassed.  DISABLED_UNMANNED uses the immediate path instead.
    bool ownerChangeEvacuationPending = false;
    uint32_t nextExitPath = 1;
    uint32_t nextExitCommandSequence = 1;
    ObjectContainmentEvacuationDisposition evacuationDisposition =
        ObjectContainmentEvacuationDisposition::Invalid;
    uint64_t evacuationDispositionRevision = 0;
    // One frozen pristine-pose path table per final containment rule.  These
    // model-space Q32.32 values are resolved once at spawn from the sealed
    // content catalog; renderer animation state is never queried at exit.
    ::container::Vector<ObjectContainmentExitPathSet> exitPathSets;
    // RiderChange/Parachute own per-host deterministic state. These values
    // are simulation projections only; presentation consumes model flags and
    // audio events through its existing extraction boundaries.
    uint32_t activeRiderRule = std::numeric_limits<uint32_t>::max();
    uint64_t riderScuttleTick = 0;
    math::q32_32 parachuteStartZ{};
    math::q32_32 parachutePitch{};
    math::q32_32 parachuteRoll{};
    math::q32_32 parachutePitchRate{};
    math::q32_32 parachuteRollRate{};
    bool parachuteHasStartZ = false;
    bool parachuteOpened = false;
    bool parachuteOpenLocomotorProjected = false;
    bool parachuteHasLandingOverride = false;
    bool parachuteHasLandingTarget = false;
    math::q32_32 parachuteLandingX{};
    math::q32_32 parachuteLandingY{};
    math::q32_32 parachuteLandingZ{};
    math::q32_32 parachuteOverrideStartX{};
    math::q32_32 parachuteOverrideStartY{};
    math::q32_32 parachuteOverrideStartZ{};
};

struct ObjectContainmentAttachRequest final {
    ObjectId container = INVALID_OBJECT_ID;
    ObjectId object = INVALID_OBJECT_ID;
    uint32_t containmentRuleIndex = std::numeric_limits<uint32_t>::max();
    uint64_t confirmedEnteredTick = 0;
    bool destroyWithContainer = false;
    bool enclosing = true;
    bool followsContainerTransform = true;
};

enum class ObjectContainmentRequestKind : uint8_t {
    Attach,
    Detach,
    EjectAll,
};

struct ObjectContainmentRequest final {
    ObjectContainmentRequestKind kind = ObjectContainmentRequestKind::Attach;
    ObjectId container = INVALID_OBJECT_ID;
    ObjectId object = INVALID_OBJECT_ID;
    uint64_t confirmedTick = 0;
    bool force = false;
    bool exposeStealthUnits = false;
    // Set only by the confirmed ParachuteContain ground-contact transition.
    // The detached event retains the transport provenance after the chute is
    // queued for destruction, allowing the command transaction to reproduce
    // the original transport -> producer -> ExitInterface landing policy.
    ObjectId parachuteLandingTransport = INVALID_OBJECT_ID;
};

struct ObjectContainmentEvent final {
    ObjectContainmentRequestKind kind = ObjectContainmentRequestKind::Attach;
    ObjectId container = INVALID_OBJECT_ID;
    ObjectId object = INVALID_OBJECT_ID;
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
    bool accepted = false;
    bool exposeStealthUnits = false;
    ObjectId parachuteLandingTransport = INVALID_OBJECT_ID;
};

enum class ObjectTransportBehaviorRequestKind : uint8_t {
    BunkerBust,
    BattleBusStartUndeath,
    BattleBusLanded,
    HijackTarget,
    ReleaseHijacker,
    PilotFindVehicle,
    AssaultTransportUpdate,
    DeliverPayload,
};

struct ObjectTransportBehaviorRequest final {
    ObjectTransportBehaviorRequestKind kind =
        ObjectTransportBehaviorRequestKind::BunkerBust;
    ObjectId object = INVALID_OBJECT_ID;
    ObjectId target = INVALID_OBJECT_ID;
    // DeathWalk must select the exact authored behavior occurrence. Other
    // command/update callers retain the legacy first-module lookup.
    uint32_t authoredOrder = std::numeric_limits<uint32_t>::max();
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
    math::q32_32 routeX{};
    math::q32_32 routeY{};
    math::q32_32 routeZ{};
    ::container::String payloadContainerTemplate;
    ::container::String payloadWeaponTemplate;
    math::q32_32 deliveryDistance{};
    math::q32_32 exitPitchRate{};
    math::q32_32 diveStartDistance{};
    math::q32_32 diveEndDistance{};
    math::q32_32 strafeLength{};
    math::q32_32 preOpenDistance{};
    math::q32_32 dropOffsetX{};
    math::q32_32 dropOffsetY{};
    math::q32_32 dropOffsetZ{};
    math::q32_32 dropVarianceX{};
    math::q32_32 dropVarianceY{};
    math::q32_32 dropVarianceZ{};
    uint32_t dropDelayMilliseconds = 0;
    uint32_t maximumAttempts = 0;
    uint32_t visibleItemsDroppedPerInterval = 0;
    uint32_t visiblePayloadCount = 0;
    bool hasDeliveryOverride = false;
    bool hasDeliveryRouteTarget = false;
    bool inheritTransportVelocity = false;
    bool parachuteDirectly = false;
    bool selfDestructAfterDelivery = false;
    bool fireWeaponPayload = false;
    ::container::String deliveryDecal;
    math::q32_32 deliveryDecalRadius{};
    uint32_t deliveryDecalShadowTypeMask = 0x20u;
    math::q32_32 deliveryDecalMinimumOpacity{int32_t{1}};
    math::q32_32 deliveryDecalMaximumOpacity{int32_t{1}};
    uint64_t deliveryDecalOpacityThrobTicks = 30;
    ::container::Array<uint8_t, 4> deliveryDecalColor{0, 0, 0, 0};
    bool deliveryDecalUsesPlayerColor = true;
    bool deliveryDecalOnlyVisibleToOwningPlayer = true;
    ::container::String visiblePayloadTemplate;
    ::container::String visiblePayloadWeapon;
    ::container::String visibleDropBoneBaseName;
    ::container::String visibleSubObjectBaseName;
    ::container::String strafingWeaponSlot;
    ::container::String strafeWeaponFx;
    game::DamageType bunkerOccupantDamageType = game::DamageType::UNRESISTABLE;
    game::DeathType bunkerOccupantDeathType = game::DeathType::NORMAL;
    bool hasBunkerOccupantDamage = false;
    bool requiredUpgradeSatisfied = true;
    uint64_t confirmedTick = 0;
};

struct ObjectTransportPayloadPlacementTransaction {
    ObjectId object = INVALID_OBJECT_ID;
    ObjectId target = INVALID_OBJECT_ID;
    ::container::String payload;
    ::container::String auxiliaryPayload;
    ::container::String attachmentBone;
    ::container::String subObject;
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
    math::q32_32 targetX{};
    math::q32_32 targetY{};
    math::q32_32 targetZ{};
    math::q32_32 routeX{};
    math::q32_32 routeY{};
    math::q32_32 routeZ{};
    math::q32_32 pitchRate{};
    uint32_t authoredOrder = 0;
    uint32_t ruleIndex = std::numeric_limits<uint32_t>::max();
    uint32_t attempt = 0;
    uint64_t confirmedTick = 0;
    bool inheritTransportVelocity = false;
    bool directLanding = false;
};

struct ObjectTransportPayloadDropTransaction final
    : ObjectTransportPayloadPlacementTransaction {};

struct ObjectTransportVisiblePayloadDropTransaction final
    : ObjectTransportPayloadPlacementTransaction {};

struct ObjectTransportFxPresentation final {
    ObjectId object = INVALID_OBJECT_ID;
    ObjectId target = INVALID_OBJECT_ID;
    ::container::String fxList;
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
};

struct ObjectTransportSeismicPresentation final {
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
    math::q32_32 radius{};
    math::q32_32 magnitude{};
};

struct ObjectTransportAudioPresentation final {
    ObjectId object = INVALID_OBJECT_ID;
    ::container::String eventName;
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
};

struct ObjectTransportDeliveryStartedPresentation final {
    ObjectId transport = INVALID_OBJECT_ID;
    ::container::String decalTexture;
    ::container::String subObjectBaseName;
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
    math::q32_32 radius{};
    uint32_t decalShadowTypeMask = 0x20u;
    math::q32_32 decalMinimumOpacity{int32_t{1}};
    math::q32_32 decalMaximumOpacity{int32_t{1}};
    uint64_t decalOpacityThrobTicks = 30;
    ::container::Array<uint8_t, 4> decalColor{0, 0, 0, 0};
    uint32_t visibleSubObjectCount = 0;
    uint64_t confirmedTick = 0;
    bool decalUsesPlayerColor = true;
    bool decalOnlyVisibleToOwningPlayer = true;
};

struct ObjectTransportPayloadStrafeTransaction final {
    ObjectId transport = INVALID_OBJECT_ID;
    ::container::String weaponSlot;
    ::container::String fxList;
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectTransportPayloadWeaponTransaction final {
    ObjectId transport = INVALID_OBJECT_ID;
    ObjectId payloadObject = INVALID_OBJECT_ID;
    ::container::String weaponTemplate;
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectTransportPayloadFinishedTransaction final {
    ObjectId transport = INVALID_OBJECT_ID;
    uint64_t confirmedTick = 0;
    bool destroyTransport = false;
};

struct ObjectTransportOclTransaction final {
    ObjectId source = INVALID_OBJECT_ID;
    ::container::String objectCreationList;
    PlayerId owner = INVALID_PLAYER_ID;
    ObjectTeamId primaryTeam = INVALID_OBJECT_TEAM_ID;
    math::q32_32 primaryX{};
    math::q32_32 primaryY{};
    math::q32_32 primaryZ{};
    math::q32_32 sourceVelocityX{};
    math::q32_32 sourceVelocityY{};
    math::q32_32 sourceVelocityZ{};
    math::q32_32 orientationRadians{};
    math::q32_32 pitchRadians{};
    math::q32_32 rollRadians{};
    uint32_t sourcePathfindLayer = 0;
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
    bool hasFrozenSource = false;
    bool sourceAirborne = false;
    bool sourceOwnsFullAttitude = false;
};

struct ObjectTransportWeaponAtPositionTransaction final {
    ObjectId source = INVALID_OBJECT_ID;
    ::container::String weaponTemplate;
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectTransportBunkerBustOccupant final {
    ObjectId entrance = INVALID_OBJECT_ID;
    ObjectDamageRequest damage;
};

// BunkerBusterBehavior::onDie is one synchronous parent callback. Each
// passenger exits and closes its complete Body reaction before the next
// passenger is read; DetonationFX/Seismic/Shockwave follow that loop.
struct ObjectTransportBunkerBustTransaction final {
    ObjectId source = INVALID_OBJECT_ID;
    ObjectId target = INVALID_OBJECT_ID;
    ::container::Vector<ObjectTransportBunkerBustOccupant> occupants;
    ::container::String detonationFx;
    ::container::String shockwaveWeapon;
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
    math::q32_32 seismicRadius{};
    math::q32_32 seismicMagnitude{};
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectTransportVeterancySyncTransaction final {
    ObjectId lower = INVALID_OBJECT_ID;
    ObjectId higher = INVALID_OBJECT_ID;
    game::ObjectVeterancyLevel level =
        game::ObjectVeterancyLevel::Regular;
    uint64_t confirmedTick = 0;
};

struct ObjectTransportHijackerReleaseTransaction final {
    ObjectId hijacker = INVALID_OBJECT_ID;
    ::container::String parachuteTemplate;
    uint32_t ruleIndex = std::numeric_limits<uint32_t>::max();
    uint64_t confirmedTick = 0;
};

// BattleBusSlowDeath is one capability transaction, not three unrelated
// stream entries.  The parent preserves ZH's beginSlowDeath order:
// mark first-death state -> FX/OCL -> idle/physics -> passenger damage.
struct ObjectTransportBattleBusStartTransaction final {
    ObjectId battleBus = INVALID_OBJECT_ID;
    ::container::String fxList;
    std::optional<ObjectTransportOclTransaction> objectCreationList;
    ::container::Vector<ObjectDamageRequest> passengerDamage;
    uint32_t ruleIndex = std::numeric_limits<uint32_t>::max();
    uint64_t confirmedTick = 0;
};

// The ground-hit callback follows the same parent rule: transition the
// capability state first, close FX/OCL, then stop and disable the hulk.
struct ObjectTransportBattleBusLandedTransaction final {
    ObjectId battleBus = INVALID_OBJECT_ID;
    ::container::String fxList;
    std::optional<ObjectTransportOclTransaction> objectCreationList;
    uint32_t ruleIndex = std::numeric_limits<uint32_t>::max();
    uint64_t confirmedTick = 0;
};

using ObjectTransportGameplayPayload = std::variant<
    ObjectTransportPayloadStrafeTransaction,
    ObjectTransportPayloadWeaponTransaction,
    ObjectTransportPayloadFinishedTransaction,
    ObjectTransportOclTransaction,
    ObjectTransportWeaponAtPositionTransaction,
    ObjectTransportBunkerBustTransaction,
    ObjectTransportVeterancySyncTransaction,
    ObjectTransportHijackerReleaseTransaction,
    ObjectTransportBattleBusStartTransaction,
    ObjectTransportBattleBusLandedTransaction,
    ObjectTransportPayloadDropTransaction,
    ObjectTransportVisiblePayloadDropTransaction>;

using ObjectTransportPresentationPayload = std::variant<
    ObjectTransportFxPresentation,
    ObjectTransportSeismicPresentation,
    ObjectTransportAudioPresentation,
    ObjectTransportDeliveryStartedPresentation>;

// One authoritative transport occurrence. The common ObjectSimulation clock
// is reserved where the occurrence is authored, before the value crosses the
// session boundary; consumers must not reconstruct order from variant kind or
// from a producer-private sequence.
struct ObjectTransportGameplayTransaction final {
    ObjectTransportGameplayPayload payload;
    uint64_t submissionOrdinal = 0;
};

// Presentation is deliberately detached from authoritative transport work.
// Missing FX/audio/decal resources may drop one of these values but can never
// suppress a weapon, payload, containment or lifecycle transaction.
struct ObjectTransportPresentationEvent final {
    ObjectTransportPresentationPayload payload;
};

struct ObjectTransportEventStream final {
    ::container::Vector<ObjectTransportGameplayTransaction> gameplay;
    ::container::Vector<ObjectTransportPresentationEvent> presentation;
};

// Read-only equivalent of ContainModuleInterface::isPassengerAllowedToFire.
// The exact admitted containment edge selects the authored module occurrence;
// nested Open/Transport/Overlord/Helix delegation remains value-only and
// deterministic. No caller may infer permission from the host aggregate bit
// alone because kind restrictions and subdued garrisons are edge-specific.
[[nodiscard]] bool objectPassengerAllowedToFire(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity passenger, uint64_t confirmedTick) noexcept;

// Stable-ID structural service shared by OCL upgrades and the AI containment
// adapter. It owns only ECS relationship state; module-specific capacity,
// doors, firing slots and exit placement stay in the typed Contain families.
class ObjectContainmentSystem final {
public:
    void initializeObject(
        ecs::registry& registry, ecs::entity entity,
        const GameContentSnapshot* content,
        const ObjectSimulationRules& rules) const;

    [[nodiscard]] bool canAttach(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectContainmentAttachRequest& request) const noexcept;
    [[nodiscard]] bool attach(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectContainmentAttachRequest& request) const;
    [[nodiscard]] bool detach(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, uint64_t confirmedTick) const;
    [[nodiscard]] std::optional<ObjectContainmentAttachRequest>
    prepareAttach(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectContainmentRequest& request,
        const PlayerRegistry* players = nullptr) const;
    [[nodiscard]] bool commitPreparedAttach(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectContainmentAttachRequest& prepared,
        uint64_t confirmedTick,
        ::container::Vector<ObjectContainmentEvent>& events) const;
    [[nodiscard]] bool requestAttach(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectContainmentRequest& request,
        ::container::Vector<ObjectContainmentEvent>& events,
        const PlayerRegistry* players = nullptr) const;
    [[nodiscard]] bool requestDetach(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectContainmentRequest& request,
        ::container::Vector<ObjectContainmentEvent>& events) const;
    [[nodiscard]] bool requestEjectAll(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectContainmentRequest& request,
        ::container::Vector<ObjectContainmentEvent>& events) const;
    // Capture is deliberately planned from stable ObjectId edges before any
    // OwnerComponent mutation. OverlordContain transfers its first carried
    // add-on and redirects passenger ejection to that add-on's Contain;
    // ordinary families eject their own passengers, while Cave/Tunnel retain
    // theirs.
    [[nodiscard]] ::container::Vector<ObjectId> captureDependents(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId container) const;
    // O(1) consumer boundary for GuardTunnelNetwork. `tunnelEntrance` may be
    // any completed entrance in the network; stale, dead, or hidden-stealth
    // targets are rejected at read time just like TunnelTracker::getCurNemesis.
    [[nodiscard]] ObjectId recentTunnelNetworkNemesis(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId tunnelEntrance, uint64_t confirmedTick) const noexcept;
    // TunnelTracker::updateNemesis equivalent. The first valid target owns the
    // player network for four seconds; only that same target may refresh the
    // lease until it expires or becomes invalid.
    [[nodiscard]] bool publishTunnelNetworkNemesis(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId source, ObjectId target, uint64_t confirmedTick,
        uint32_t logicFramesPerSecond) const;
    // TransportAIUpdate extends only direct Player/Script Attack commands.
    // Propagate to fire-enabled passengers at command admission so host and
    // passengers enter Combat in the same confirmed tick.
    [[nodiscard]] static size_t fanoutDirectAttackOrder(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId container, const ObjectOrderIntent& order, bool queued,
        uint64_t confirmedTick);
    [[nodiscard]] bool requestBehavior(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const ObjectSimulationRules& rules,
        const ObjectTransportBehaviorRequest& request,
        ::container::Vector<ObjectDamageRequest>& outDamage,
        ::container::Vector<ObjectContainmentEvent>& containmentEvents,
        ObjectTransportEventStream& behaviorEvents,
        uint64_t& nextGameplaySubmissionOrdinal) const;
    [[nodiscard]] bool acknowledgePayloadDrop(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId transport, ObjectId payload, uint32_t ruleIndex,
        uint32_t attempt) const;
    [[nodiscard]] bool beginHijackerRelease(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId hijacker, uint32_t ruleIndex,
        uint64_t confirmedTick) const;
    [[nodiscard]] bool finishHijackerRelease(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId hijacker, uint32_t ruleIndex) const;
    [[nodiscard]] bool cancelBattleBusUndeathForRealDeath(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId battleBus, uint32_t authoredOrder) const;
    [[nodiscard]] bool beginBattleBusUndeath(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId battleBus, uint32_t ruleIndex,
        uint64_t confirmedTick) const;
    [[nodiscard]] bool finishBattleBusUndeath(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId battleBus, uint32_t ruleIndex,
        uint64_t confirmedTick) const;
    [[nodiscard]] bool beginBattleBusLanded(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId battleBus, uint32_t ruleIndex,
        uint64_t confirmedTick) const;
    [[nodiscard]] bool finishBattleBusLanded(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId battleBus, uint32_t ruleIndex,
        uint64_t confirmedTick) const;
    [[nodiscard]] bool setParachuteLandingOverride(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId parachute, math::q32_32 x, math::q32_32 y,
        math::q32_32 z) const;
    void onContainerDie(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        ObjectId container, ObjectId damageSource, uint32_t authoredOrder,
        uint64_t confirmedTick,
        ::container::Vector<ObjectDamageRequest>& outDamage,
        std::optional<ObjectContainmentDeathFinalizeCommand>&
            outFinalize) const;
    [[nodiscard]] ObjectContainmentDeathFinalizeAdvance advanceContainerDie(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        ObjectContainmentDeathFinalizeCommand& command,
        const game::terrain::TerrainLogic* terrain,
        const navigation::NavigationSystem* navigation,
        uint64_t& nextGameplaySubmissionOrdinal,
        ::container::Vector<ObjectDamageRequest>& outDamage,
        ::container::Vector<ObjectDeleteDestroyRequest>& outDestroy) const;
    // OpenContain::onDelete for one authored occurrence. Every literal rider
    // owned by that module is structurally destroyed; unlike onDie this does
    // not apply passenger damage/ejection policy.
    void onContainerDelete(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId container, uint32_t authoredOrder,
        uint64_t confirmedTick,
        ::container::Vector<ObjectDeleteDestroyRequest>& outDestroy) const;
    void update(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const ObjectSimulationRules& rules, uint64_t confirmedTick,
        uint64_t& nextGameplaySubmissionOrdinal,
        ::container::Vector<ObjectDamageRequest>& outDamage,
        ::container::Vector<ObjectBodyStateProjection>&
            bodyStateProjections,
        ::container::Vector<ObjectContainmentEvent>* containmentEvents = nullptr,
        ObjectTransportEventStream* behaviorEvents = nullptr,
        const PlayerRegistry* players = nullptr,
        const game::terrain::TerrainLogic* terrain = nullptr,
        const navigation::NavigationSystem* navigation = nullptr) const;

    // Called after all position writers and before collision/spatial
    // consumers. Contained add-ons inherit the host's current deterministic
    // FIREPOINT transform (or its root fallback), velocity and attitude
    // without asking the renderer for an animated pose.
    void synchronizeTransforms(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const GameContentSnapshot* content) const;
};

} // namespace engine
