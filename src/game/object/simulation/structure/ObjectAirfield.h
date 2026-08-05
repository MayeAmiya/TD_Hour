#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "core/math/wwmath/vector/float3.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "core/ecs/ObjectId.h"

#include "game/object/plan/structure/ObjectAirfieldPlanTypes.h"
namespace engine
{

class ObjectLifecycle;
class GameContentSnapshot;
class PlayerRegistry;
class SimulationRandom;
struct ObjectSimulationRules;
struct ObjectDeleteDestroyRequest;
struct ObjectSlowDeathPhaseEvent;
struct ObjectRadiusDecalEvent;
struct ObjectSpecialPowerExecutionEvent;
struct ObjectSpecialPowerSpawnRequest;
struct ObjectSystemWeaponFireCommand;

enum class ObjectAirfieldEventKind : uint8_t
{
    RuntimeInitialized,
    ParkingPurged,
    ParkingReserved,
    ParkingReleased,
    RunwayReserved,
    RunwayReleased,
    RunwayQueued,
    RunwayReservationAdvanced,
    ParkingAssignmentChanged,
    AircraftRuntimeInitialized,
    AircraftStateChanged,
    AircraftSlowDeathPhase,
    AircraftTerminalDestroyRequested,
    ChinookRopeRuntimeInitialized,
    FlightDeckCatapultFx,
    SpectreRuntimeInitialized,
    SpectrePhaseChanged,
    // JetAIUpdate and SpectreGunshipUpdate own this persistent per-unit
    // sound independently of the model-condition projection.  The event
    // edge is confirmed simulation output; presentation must never infer it
    // from JETAFTERBURNER or a renderer animation state.
    AfterburnerLoopStarted,
    AfterburnerLoopStopped,
    // JetAIUpdate's dead-airfield circling state enters once after a confirmed
    // return-to-base failure. The per-unit VoiceLowFuel acknowledgement is a
    // one-shot state-entry effect, not a loop inferred from fuel or a model.
    JetLowFuel,
    // SpectreGunshipUpdate emits this only after its temporary howitzer weapon
    // was actually admitted to the confirmed weapon-command stream.
    SpectreHowitzerFired,
    SpectreStrafeFx,
    SpectreObjectDestroyRequested,
};

enum class ObjectAirfieldSlotKind : uint8_t
{
    ParkingPlace,
    FlightDeck,
    TakeoffRunway,
    LandingRunway,
};

enum class ObjectAircraftRuntimeState : uint8_t
{
    Idle,
    Parked,
    Taxiing,
    TakingOff,
    Airborne,
    Attacking,
    ReturningToBase,
    Landing,
    Reloading,
};

// JetAIUpdate's private state machine is deliberately kept separate from the
// coarse public aircraft state.  Taxiing to a runway and taxiing to a parking
// space have different reservation and service edges even though both project
// as Taxiing to rendering/physics consumers.
enum class ObjectJetAirfieldPhase : uint8_t
{
    Parked,
    AwaitTakeoffClearance,
    TaxiToTakeoff,
    PauseBeforeTakeoff,
    TakingOff,
    Airborne,
    ReturningToBase,
    AwaitLandingClearance,
    Landing,
    TaxiToParking,
    OrientForParking,
    Reloading,
    ReturningToDeadAirfield,
    CirclingDeadAirfield,
};

// ChinookAIUpdate owns a vertical-flight state machine distinct from
// JetAIUpdate.  In particular, helipad aircraft never reserve a fixed-wing
// parking slot or taxi along RunwayStart/RunwayEnd bones.
enum class ObjectHelicopterFlightPhase : uint8_t
{
    Airborne,
    ReturningForLanding,
    Landing,
    Landed,
    TakingOff,
};

enum class ObjectAircraftSlowDeathPhase : uint8_t
{
    Alive,
    InitialDeath,
    Secondary,
    HitGround,
    FinalBlowUp,
    OnGroundDeath,
    BladeDetached,
};

enum class ObjectSpectreGunshipPhase : uint8_t
{
    Idle,
    Inserting,
    Orbiting,
    Departing,
};

struct ObjectAirfieldReservation final
{
    ObjectId airfield = INVALID_OBJECT_ID;
    ObjectId aircraft = INVALID_OBJECT_ID;
    ObjectAirfieldSlotKind slotKind = ObjectAirfieldSlotKind::ParkingPlace;
    size_t moduleIndex = 0;
    size_t slotIndex = 0;
    bool active = true;
};

struct ObjectAirfieldEvent final
{
    ObjectAirfieldEventKind kind = ObjectAirfieldEventKind::RuntimeInitialized;
    ObjectId object = INVALID_OBJECT_ID;
    ObjectId aircraft = INVALID_OBJECT_ID;
    ObjectAirfieldSlotKind slotKind = ObjectAirfieldSlotKind::ParkingPlace;
    size_t moduleIndex = 0;
    size_t slotIndex = 0;
    size_t previousSlotIndex = 0;
    uint32_t authoredOrder = 0;
    uint32_t sourcePathfindLayer = 0;
    container::String moduleClass;
    uint32_t slotCount = 0;
    uint32_t runwayCount = 0;
    ObjectAircraftRuntimeState aircraftState = ObjectAircraftRuntimeState::Idle;
    ObjectJetAirfieldPhase jetPhase = ObjectJetAirfieldPhase::Parked;
    ObjectAircraftSlowDeathPhase slowDeathPhase = ObjectAircraftSlowDeathPhase::Alive;
    ObjectSpectreGunshipPhase spectrePhase = ObjectSpectreGunshipPhase::Idle;
    uint64_t dueTick = 0;
    container::String fx;
    container::String ocl;
    container::String audio;
    container::String payloadTemplate;
    container::String particleSystem;
    container::String boneName;
    LogicFixedVec3 localOffset{};
    LogicFixedVec3 worldPosition{};
    uint64_t confirmedTick = 0;
};

enum class ObjectChinookRopePresentationControl : uint8_t
{
    Begin,
    Update,
    End,
};

// The combat-drop owner resolves these two pristine-bone transforms at the
// confirmed state-entry boundary. ObjectAirfield owns the subsequent rope
// clock; it never asks the renderer for a live pose or terrain height.
struct ObjectChinookRopeEndpoint final
{
    LogicFixedVec3 ropeStart{};
    LogicFixedVec3 dropStart{};
    LogicFixedQuaternion dropOrientation{};
    math::q32_32 surfaceHeight{};
};

struct ObjectChinookCombatDropBeginRequest final
{
    ObjectId object = INVALID_OBJECT_ID;
    size_t moduleIndex = 0;
    container::Vector<ObjectChinookRopeEndpoint> endpoints;
    uint64_t confirmedTick = 0;
};

struct ObjectChinookRopeReadyResult final
{
    size_t ropeIndex = 0;
    LogicFixedVec3 dropStart{};
    LogicFixedQuaternion dropOrientation{};
    math::q32_32 rappelSpeedPerFrame{};
};

// Detached, lossless presentation fact. Update is intentionally a complete
// value snapshot: a locally hidden Begin or a presentation-epoch reset can
// reconstruct the exact W3DRopeDraw phase and falling offset from any later
// confirmed frame.
struct ObjectChinookRopePresentationEvent final
{
    ObjectChinookRopePresentationControl control =
        ObjectChinookRopePresentationControl::Begin;
    ObjectId object = INVALID_OBJECT_ID;
    uint32_t authoredOrder = 0;
    uint32_t ropeIndex = 0;
    uint64_t ropeIdentity = 0;
    container::String ropeName;
    LogicFixedVec3 anchor{};
    float maximumLength = 1.0f;
    float currentLength = 0.0f;
    float width = 0.5f;
    math::vec3 color{};
    float wobbleLength = 1.0f;
    float wobbleAmplitude = 0.0f;
    float wobbleRatePerFrame = 0.0f;
    float wobblePhase = 0.0f;
    float verticalOffset = 0.0f;
    float currentSpeedPerFrame = 0.0f;
    float maximumSpeedPerFrame = 0.0f;
    float accelerationPerFrame = 0.0f;
    uint64_t confirmedTick = 0;
};

struct ObjectAirfieldParkingRuntime final
{
    container::Vector<ObjectId> spaces;
    container::Vector<ObjectId> runwayUsers;
    container::Vector<ObjectId> nextTakeoffUsers;
    // ParkingPlaceBehavior::m_healing is independent of persistent parking
    // spaces. PRODUCED_AT_HELIPAD aircraft land near an airfield and register
    // here without consuming a hangar/runway slot.
    container::Vector<ObjectId> healees;
    uint64_t nextHealTick = 0;
};

struct ObjectAirfieldFlightDeckRuntime final
{
    container::Vector<ObjectId> spaces;
    container::Vector<ObjectId> takeoffRunwayUsers;
    container::Vector<ObjectId> landingRunwayUsers;
    container::Vector<uint64_t> nextLaunchWaveTicks;
    container::Vector<uint64_t> rampReadyTicks;
    container::Vector<uint64_t> catapultDueTicks;
    container::Vector<uint64_t> lowerRampTicks;
    container::Vector<uint8_t> rampRaised;
    uint64_t nextHealTick = 0;
    uint64_t nextCleanupTick = 0;
    uint64_t nextAllowedProductionTick = 0;
};

struct ObjectJetAiRuntime final
{
    ObjectAircraftRuntimeState state = ObjectAircraftRuntimeState::Idle;
    ObjectJetAirfieldPhase phase = ObjectJetAirfieldPhase::Parked;
    ObjectId reservedAirfield = INVALID_OBJECT_ID;
    ObjectAirfieldReservation parkingReservation;
    ObjectAirfieldReservation runwayReservation;
    uint64_t takeoffPauseUntilTick = 0;
    uint64_t attackLocomotorExpiresTick = 0;
    uint64_t attackersMissExpiresTick = 0;
    uint64_t lockonReadyTick = 0;
    container::Vector<ObjectId> lockonTargeters;
    uint64_t returnToBaseIdleDueTick = 0;
    uint64_t phaseEnteredTick = 0;
    uint64_t reloadStartedTick = 0;
    uint64_t reloadCompleteTick = 0;
    uint64_t nextAirfieldSearchTick = 0;
    uint8_t locomotorProjectedSlot = 0xffu;
    LogicFixedVec3 rememberedProducerPosition{};
    LogicFixedVec3 helipadLandingPosition{};
    container::Vector<LogicFixedVec3> route;
    size_t nextRoutePoint = 0;
    math::q32_32 parkingOrientationRadians{};
    std::optional<ObjectOrderIntent> pendingOrder;
    container::Vector<ObjectOrderIntent> pendingOrderTail;
    // Queue revision immediately after pendingOrder was detached. Commands
    // admitted later may replace this detached batch while takeoff/landing is
    // locked; the live queue stays empty so generic owners cannot execute a
    // queued tail during taxi or landing.
    uint64_t pendingQueueRevision = 0;
    // External revision also changes for Stop, whose empty queue carries no
    // order value. It is therefore required to cancel a detached batch
    // instead of resurrecting it after landing.
    uint64_t pendingExternalRevision = 0;
    bool producerPositionKnown = false;
    bool helipadLandingPositionValid = false;
    bool helipadHealingRegistered = false;
    bool countermeasuresReloadedForLanding = false;
    bool productionExitCompleted = false;
    bool runtimeInitializedEventEmitted = false;
    // Latched JETEXHAUST contribution. RefCode calls
    // set/clearModelConditionState unconditionally every frame, which is free
    // on a bit field; here every publish also dirties the object, so only the
    // transitions are republished.
    bool jetExhaustPublished = false;
    // Presentation-only lifetime is driven by confirmed JetAIUpdate phase
    // edges.  Keep the latch in the authoritative runtime so a restored or
    // externally redirected phase cannot accidentally duplicate the loop.
    bool afterburnerAudioActive = false;
};

struct ObjectAirfieldServiceRequest final
{
    ObjectId aircraft = INVALID_OBJECT_ID;
    uint64_t reloadStartedTick = 0;
    uint64_t reloadCompleteTick = 0;
    bool reloadWeapons = false;
    bool reloadCountermeasures = false;
};

// FlightDeckBehavior detects an empty payload slot and emits this value-only
// request. ObjectProduction remains the only owner of queue admission, cost,
// prerequisites and the eventual spawn transaction.
struct ObjectAirfieldAutomaticProductionRequest final
{
    ObjectId producer = INVALID_OBJECT_ID;
    size_t moduleIndex = 0;
    uint32_t authoredOrder = 0;
    container::String payloadTemplate;
    uint64_t confirmedTick = 0;
};

struct ObjectChinookAiRuntime final
{
    struct Rope final
    {
        uint64_t identity = 0;
        ObjectChinookRopeEndpoint endpoint;
        uint64_t nextDropTick = 0;
        uint64_t expirationTick = 0;
        uint64_t lastUpdateTick = 0;
        math::q32_32 targetLength{int32_t{1}};
        math::q32_32 simulatedLength{int32_t{1}};
        math::q32_32 presentedLength{};
        math::q32_32 lengthSpeedPerFrame{};
        math::q32_32 dropSpeedPerFrame{};
        math::q32_32 currentSpeedPerFrame{};
        math::q32_32 maximumSpeedPerFrame{};
        math::q32_32 accelerationPerFrame{};
        math::q32_32 wobblePhase{};
        math::q32_32 wobbleRatePerFrame{};
        math::q32_32 verticalOffset{};
        container::Vector<ObjectId> rappellers;
        uint32_t ropeIndex = 0;
        bool released = false;
    };

    container::Vector<uint64_t> ropeReadyTicks;
    container::Vector<Rope> ropes;
    uint32_t ropeGeneration = 0;
    ObjectId pendingRappeller = INVALID_OBJECT_ID;
    uint32_t pendingRopeIndex = 0;
    uint32_t pendingEventSequence = 0;
    math::q32_32 rappelSpeedPerFrame{};
    ObjectHelicopterFlightPhase flightPhase =
        ObjectHelicopterFlightPhase::Airborne;
    ObjectId healingAirfield = INVALID_OBJECT_ID;
    LogicFixedVec3 landingPosition{};
    container::Vector<LogicFixedVec3> flightRoute;
    size_t nextFlightRoutePoint = 0;
    std::optional<ObjectOrderIntent> pendingOrder;
    container::Vector<ObjectOrderIntent> pendingOrderTail;
    uint64_t pendingQueueRevision = 0;
    uint64_t pendingExternalRevision = 0;
    uint64_t flightPhaseEnteredTick = 0;
    bool landingPositionValid = false;
    bool healingRegistered = false;
    bool productionExitCompleted = false;
    bool ropesDropping = false;
    bool combatDropActive = false;
    bool runtimeInitializedEventEmitted = false;
};

struct ObjectSpectreGunshipRuntime final
{
    ObjectAircraftRuntimeState state = ObjectAircraftRuntimeState::Idle;
    ObjectSpectreGunshipPhase phase = ObjectSpectreGunshipPhase::Idle;
    ObjectId gattling = INVALID_OBJECT_ID;
    ObjectId currentTarget = INVALID_OBJECT_ID;
    uint64_t orbitEndsTick = 0;
    uint64_t nextHowitzerFireTick = 0;
    uint64_t phaseEnteredTick = 0;
    LogicFixedVec3 initialTargetPosition{};
    LogicFixedVec3 overrideTargetDestination{};
    LogicFixedVec3 satellitePosition{};
    LogicFixedVec3 gattlingTargetPosition{};
    LogicFixedVec3 positionToShootAt{};
    LogicFixedVec3 departureTarget{};
    uint32_t howitzerFollowTicks = 0;
    uint32_t nextHowitzerShotSequence = 1;
    bool targetingDecalsActive = false;
    bool cleanupRequested = false;
    bool phaseEventPending = false;
    // beginSpectreGunshipTargeting installs Inserting before the normal update
    // pass observes it.  This latch turns that pending confirmed phase entry
    // into exactly one Afterburner start/stop event.
    bool afterburnerAudioActive = false;
    ObjectSpectreGunshipPhase locomotorProjectedPhase =
        ObjectSpectreGunshipPhase::Idle;
    bool runtimeInitializedEventEmitted = false;
};

struct ObjectSpectreDeploymentRuntime final
{
    bool ready = true;
    bool runtimeInitializedEventEmitted = false;
};

struct ObjectAircraftSlowDeathRuntime final
{
    ObjectAircraftSlowDeathPhase phase = ObjectAircraftSlowDeathPhase::Alive;
    uint64_t secondaryDueTick = 0;
    uint64_t finalBlowUpDueTick = 0;
    uint64_t groundToFinalDueTick = 0;
    uint64_t bladeDetachDueTick = 0;
    uint64_t destroyDueTick = 0;
    // Live motion state. These are the fixed-point equivalents of
    // JetSlowDeathBehavior::m_rollRate and HelicopterSlowDeathBehavior's
    // m_forwardAngle / m_forwardSpeed / m_selfSpin / m_selfSpinTowardsMax /
    // m_lastSelfSpinUpdateFrame. They are authoritative simulation state, so
    // they stay in Q32.32 and are advanced only on a confirmed tick.
    math::q32_32 rollRateRadiansPerSecond{};
    math::q32_32 spiralForwardAngleRadians{};
    math::q32_32 spiralForwardSpeedUnitsPerSecond{};
    math::q32_32 selfSpinRadiansPerSecond{};
    uint64_t lastSelfSpinUpdateTick = 0;
    // The wreck's translation has been handed to the free-body physics lane.
    // Set once at InitialDeath so the per-tick pass knows the handoff already
    // happened and never re-seeds velocity from a stale locomotor.
    bool motionOwnedByPhysics = false;
    bool selfSpinTowardsMaximum = true;
    bool initialEventEmitted = false;
    bool bladeDetachEventEmitted = false;
    bool terminalDestroyEventEmitted = false;
};

struct ObjectAirfieldComponent final
{
    container::SharedPtr<const game::ObjectAirfieldPlan> plan;
    container::Vector<ObjectAirfieldParkingRuntime> parkingPlaces;
    container::Vector<ObjectAirfieldFlightDeckRuntime> flightDecks;
    container::Vector<ObjectJetAiRuntime> jetAi;
    container::Vector<ObjectChinookAiRuntime> chinookAi;
    container::Vector<ObjectSpectreGunshipRuntime> spectreGunships;
    container::Vector<ObjectSpectreDeploymentRuntime> spectreDeployments;
    container::Vector<ObjectAircraftSlowDeathRuntime> slowDeaths;
    uint64_t initializedTick = 0;
};

enum class ObjectAirfieldDefectionAction : uint8_t
{
    Defect,
    ReleaseReservation,
};

struct ObjectAirfieldDefectionEntry final
{
    ObjectId aircraft = INVALID_OBJECT_ID;
    ObjectAirfieldDefectionAction action =
        ObjectAirfieldDefectionAction::Defect;
    bool clearProducer = false;
};

class ObjectAirfieldSystem final
{
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity, const ObjectSimulationRules& rules) const;
    [[nodiscard]] bool reserveParkingSlot(ecs::registry& registry,
                                          const ObjectLifecycle& lifecycle,
                                          ObjectId airfield,
                                          ObjectId aircraft,
                                          uint64_t confirmedTick,
                                          ObjectAirfieldReservation& outReservation,
                                          container::Vector<ObjectAirfieldEvent>& outEvents) const;
    [[nodiscard]] bool reserveProducedAircraftParkingSlot(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId airfield, ObjectId aircraft, size_t doorIndex,
        uint64_t confirmedTick,
        ObjectAirfieldReservation& outReservation,
        container::Vector<ObjectAirfieldEvent>& outEvents) const;
    [[nodiscard]] bool reserveRunway(ecs::registry& registry,
                                     const ObjectLifecycle& lifecycle,
                                     ObjectId airfield,
                                     ObjectId aircraft,
                                     bool landing,
                                     uint64_t confirmedTick,
                                     uint32_t logicFramesPerSecond,
                                     ObjectAirfieldReservation& outReservation,
                                     container::Vector<ObjectAirfieldEvent>& outEvents) const;
    [[nodiscard]] bool releaseAircraftReservations(ecs::registry& registry,
                                                   const ObjectLifecycle& lifecycle,
                                                   ObjectId airfield,
                                                   ObjectId aircraft,
                                                   uint64_t confirmedTick,
                                                   container::Vector<ObjectAirfieldEvent>& outEvents) const;
    [[nodiscard]] bool releaseParkingSlot(ecs::registry& registry,
                                          const ObjectLifecycle& lifecycle,
                                          ObjectId airfield,
                                          ObjectId aircraft,
                                          uint64_t confirmedTick,
                                          container::Vector<ObjectAirfieldEvent>& outEvents) const;
    [[nodiscard]] bool releaseRunway(ecs::registry& registry,
                                     const ObjectLifecycle& lifecycle,
                                     ObjectId airfield,
                                     ObjectId aircraft,
                                     uint64_t confirmedTick,
                                     container::Vector<ObjectAirfieldEvent>& outEvents) const;
    // Returns a mutation-free, stable ObjectId snapshot for Object::defect's
    // ParkingPlace recursion. The session owns the actual team transaction;
    // this subsystem owns interpretation of parking and aircraft state.
    [[nodiscard]] container::Vector<ObjectAirfieldDefectionEntry>
    defectionEntries(const ecs::registry& registry,
                     const ObjectLifecycle& lifecycle,
                     ObjectId airfield,
                     PlayerId newOwner) const;
    [[nodiscard]] bool notifyAircraftHitGround(ecs::registry& registry,
                                               const ObjectLifecycle& lifecycle,
                                               const ObjectSimulationRules& rules,
                                               ObjectId aircraft,
                                               uint64_t confirmedTick,
                                               container::Vector<ObjectAirfieldEvent>& outEvents,
                                               container::Vector<ObjectSlowDeathPhaseEvent>& outSlowDeathPhases,
                                               container::Vector<ObjectDeleteDestroyRequest>& outDestroyRequests,
                                               uint64_t& nextGameplaySubmissionOrdinal) const;
    // Synchronous DieMux entry. Starts only the authored Jet/Helicopter
    // slow-death rule identified by authoredOrder; later update ticks advance
    // that runtime but never discover death by polling health.
    [[nodiscard]] bool beginAircraftSlowDeathOnDie(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectSimulationRules& rules,
        const game::terrain::TerrainLogic* terrain,
        ObjectId aircraft, uint32_t authoredOrder,
        uint64_t confirmedTick,
        container::Vector<ObjectAirfieldEvent>& outEvents,
        container::Vector<ObjectSlowDeathPhaseEvent>& outSlowDeathPhases,
        container::Vector<ObjectDeleteDestroyRequest>& outDestroyRequests,
        uint64_t& nextGameplaySubmissionOrdinal,
        bool bypassJetGroundDeathGate = false) const;
    [[nodiscard]] bool setAircraftState(ecs::registry& registry,
                                        const ObjectLifecycle& lifecycle,
                                        ObjectId aircraft,
                                        ObjectAircraftRuntimeState state,
                                        uint64_t confirmedTick,
                                         container::Vector<ObjectAirfieldEvent>& outEvents) const;
    [[nodiscard]] bool beginProducedAircraftExit(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const GameContentSnapshot& content, ObjectId aircraft,
        uint64_t confirmedTick,
        container::Vector<ObjectAirfieldEvent>& outEvents) const;
    [[nodiscard]] bool requestAircraftRepairAtAirfield(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId aircraft, ObjectId airfield, uint64_t confirmedTick,
        container::Vector<ObjectAirfieldEvent>& outEvents) const;
    [[nodiscard]] bool beginSpectreGunshipTargeting(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectSimulationRules& rules, ObjectId object,
        size_t moduleIndex, LogicFixedVec3 initialTarget,
        LogicFixedVec3 overrideTarget, uint64_t confirmedTick,
        container::Vector<ObjectRadiusDecalEvent>& outEvents) const;
    void emitSpectreSpecialPowerSpawns(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const GameContentSnapshot& content,
        const game::terrain::TerrainLogic& terrain,
        const ObjectSimulationRules& rules,
        const ObjectSpecialPowerExecutionEvent& event,
        uint64_t& nextEmissionSequence,
        container::Vector<ObjectSpecialPowerSpawnRequest>& outRequests,
        container::Vector<ObjectRadiusDecalEvent>& outDecalEvents) const;
    [[nodiscard]] bool assignSpectreGunshipGattling(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, size_t moduleIndex, ObjectId gattling,
        uint64_t confirmedTick) const;
    [[nodiscard]] bool updateSpectreGunshipTargeting(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectSimulationRules& rules, ObjectId object,
        size_t moduleIndex, LogicFixedVec3 overrideTarget,
        uint64_t confirmedTick,
        container::Vector<ObjectRadiusDecalEvent>& outEvents) const;
    [[nodiscard]] bool endSpectreGunshipTargeting(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectSimulationRules& rules, ObjectId object,
        size_t moduleIndex, uint64_t confirmedTick,
        container::Vector<ObjectRadiusDecalEvent>& outEvents) const;
    [[nodiscard]] bool beginChinookCombatDrop(
        ecs::registry& registry,
        const ObjectLifecycle& lifecycle,
        const ObjectSimulationRules& rules,
        class SimulationRandom& random,
        const ObjectChinookCombatDropBeginRequest& request,
        container::Vector<ObjectChinookRopePresentationEvent>& outEvents) const;
    [[nodiscard]] std::optional<ObjectChinookRopeReadyResult>
    nextReadyChinookRope(const ecs::registry& registry,
                         const ObjectLifecycle& lifecycle,
                         ObjectId object,
                         size_t moduleIndex,
                         uint64_t confirmedTick) const;
    [[nodiscard]] bool notifyChinookRappellerStarted(
        ecs::registry& registry,
        const ObjectLifecycle& lifecycle,
        const ObjectSimulationRules& rules,
        class SimulationRandom& random,
        ObjectId object,
        size_t moduleIndex,
        size_t ropeIndex,
        uint64_t confirmedTick,
        ObjectId rappeller = INVALID_OBJECT_ID) const;
    [[nodiscard]] bool endChinookCombatDrop(
        ecs::registry& registry,
        const ObjectLifecycle& lifecycle,
        const ObjectSimulationRules& rules,
        ObjectId object,
        size_t moduleIndex,
        uint64_t confirmedTick,
        bool immediate,
        container::Vector<ObjectChinookRopePresentationEvent>& outEvents) const;
    // Modern presentation resources retained by AI/update runtimes are
    // released after every authored onDelete callback has completed.
    void onObjectReclaim(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectSimulationRules& rules, ObjectId object,
        uint64_t confirmedTick,
        container::Vector<ObjectChinookRopePresentationEvent>& outRopeEvents,
        container::Vector<ObjectRadiusDecalEvent>& outRadiusDecalEvents) const;
    void update(ecs::registry& registry,
                const ObjectLifecycle& lifecycle,
                const ObjectSimulationRules& rules,
                uint64_t confirmedTick,
                container::Vector<ObjectAirfieldEvent>& outEvents,
                container::Vector<ObjectDamageRequest>& outDamage,
                container::Vector<ObjectSlowDeathPhaseEvent>& outSlowDeathPhases,
                container::Vector<ObjectDeleteDestroyRequest>& outDestroyRequests,
                uint64_t& nextGameplaySubmissionOrdinal,
                const GameContentSnapshot* content = nullptr,
                const PlayerRegistry* players = nullptr,
                const game::terrain::TerrainLogic* terrain = nullptr,
                const game::terrain::MapVisibilitySnapshot* visibility =
                    nullptr,
                SimulationRandom* random = nullptr,
                container::Vector<ObjectSystemWeaponFireCommand>*
                    outWeaponCommands = nullptr,
                container::Vector<ObjectAirfieldServiceRequest>*
                    outServiceRequests = nullptr,
                container::Vector<ObjectAirfieldAutomaticProductionRequest>*
                    outAutomaticProductionRequests = nullptr,
                container::Vector<ObjectChinookRopePresentationEvent>*
                    outRopeEvents = nullptr,
                container::Vector<ObjectRadiusDecalEvent>*
                    outRadiusDecalEvents = nullptr) const;
    [[nodiscard]] bool acknowledgeAutomaticProduction(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectSimulationRules& rules,
        const ObjectAirfieldAutomaticProductionRequest& request) const;
};

} // namespace engine
