#pragma once

#include "core/container/container_types.h"

#include "core/ecs/registry.h"
#include "game/data/base/EnergySimulationRules.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>
#include <optional>
#include "game/object/plan/economy/ObjectProductionPlanTypes.h"
namespace engine {

class ObjectLifecycle;
class PlayerRegistry;
class GameContentSnapshot;
struct PlayerState;

// Shared authoritative build-cost projection. Production admission and
// kill-bounty settlement must use the same frozen archetype and live player
// modifiers; callers never reparse ThingTemplate data or duplicate its
// KindOf modifier scan.
[[nodiscard]] int64_t calculateObjectBuildCost(
    const game::ObjectArchetype& product,
    const PlayerState& player,
    const ecs::registry& registry,
    const ObjectLifecycle& lifecycle);

[[nodiscard]] bool canObjectBuildTemplate(
    ecs::registry& registry, ecs::entity builder,
    const GameContentSnapshot& content,
    const game::CommandBarOverrideState& commandBarOverrides,
    const PlayerRegistry& players, PlayerId player,
    const game::ObjectArchetype& product,
    bool ignorePrerequisites = false);

// Player-wide portions of Player::canBuild.  They are public so script/UI
// availability queries can share the authoritative production semantics
// without routing a fake command through a particular factory.
[[nodiscard]] bool playerSatisfiesObjectProductionPrerequisites(
    const ecs::registry& registry, const PlayerRegistry& players,
    const GameContentSnapshot& content,
    PlayerId player, const game::ObjectArchetype& product);

[[nodiscard]] bool playerCanBuildMoreOfObjectType(
    const ecs::registry& registry, PlayerId player,
    const game::ObjectArchetype& product) noexcept;

[[nodiscard]] uint32_t calculateObjectBuildFrames(
    const game::ObjectArchetype& product, const PlayerState& player,
    uint32_t framesPerSecond, const EnergySimulationRules& energyRules,
    uint64_t confirmedTick) noexcept;

// Unit, PLAYER research and OBJECT upgrade work share the one authored
// ProductionUpdate FIFO and queue capacity, but only units have an exit/spawn
// suffix. OBJECT completion remains local to its producer; it never inserts a
// player-wide technology or reserves another factory's queue.
enum class ObjectProductionJobKind : uint8_t {
    Unit,
    PlayerUpgrade,
    ObjectUpgrade,
};

// A factory-local production ID deliberately does not reuse an ECS entity or
// a global counter.  RefCode's ProductionID was unique only within one
// ProductionUpdate host; `(producer, productionId)` is therefore the stable
// modern external handle for cancel/UI/network commands.
struct ObjectProductionJob final {
    ObjectProductionJobKind kind = ObjectProductionJobKind::Unit;
    uint32_t productionId = 0;
    container::SharedPtr<const game::ObjectArchetype> product;
    UpgradeContentId upgrade = INVALID_UPGRADE_CONTENT_ID;
    // Transitional compatibility projection for existing UpgradeMux consumers;
    // the frozen UpgradeContentId remains the authoritative queue identity.
    container::String upgradeName;
    // The charged price is frozen at admission.  In contrast with the old
    // cancel path, a later player modifier/ownership change can never refund
    // a different amount from the one actually paid.
    int64_t paidCost = 0;
    PlayerId payer = INVALID_PLAYER_ID;
    uint32_t sourceSequence = 0;
    // Script BUILD_TEAM production remains attached to the inactive Team for
    // the whole factory transaction.  A plain unit command leaves this
    // invalid and therefore still lands in the player's default Team.
    ObjectTeamId targetTeam = INVALID_OBJECT_TEAM_ID;
    uint32_t targetTeamRosterIndex = UINT32_MAX;
    uint32_t targetTeamQuantityLimit = 0;
    math::q32_32 targetRallyX{};
    math::q32_32 targetRallyY{};
    math::q32_32 targetRallyZ{};
    bool hasTargetRallyPoint = false;
    uint64_t queuedAtTick = 0;
    uint64_t firstConstructionTick = 0;
    uint32_t framesUnderConstruction = 0;
    uint32_t lastRequiredFrames = 1;
    uint32_t quantityTotal = 1;
    uint32_t quantityProduced = 0;
    // DefaultProductionExitUpdate has no typed parking/door reservation
    // service yet. ProductionUpdate therefore assigns a stable authored door
    // lane when this unit first reaches completion and retains it until the
    // whole quantity batch has exited.
    uint8_t exitDoorIndex = 0;
    bool exitDoorAssigned = false;
    bool constructionComplete = false;
};

// DefaultProductionExitUpdate retains one mutable player-selected rally
// point.  It belongs to the producer entity rather than its immutable plan;
// spawned units receive a value-only route snapshot and never retain a
// pointer back to the factory.
struct ObjectProductionRallyPoint final {
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
    bool exists = false;
};

inline constexpr size_t kObjectProductionSpawnPointCount = 10;

struct ObjectProductionSpawnPointRuntime final {
    math::q32_32 localX{};
    math::q32_32 localY{};
    math::q32_32 localZ{};
    math::q32_32 localYaw{};
    ObjectId occupier = INVALID_OBJECT_ID;
    uint32_t reservationToken = 0;
};

// ExitInterface is an independent host in RefCode: SpawnBehavior and Slaved
// consumers use it even when no ProductionUpdate exists. Keep its mutable
// reservation/occupancy state in a separate sparse component so the factory
// FIFO is only one client of the service.
struct ObjectProductionExitComponent final {
    container::SharedPtr<const game::ObjectProductionExitPlan> plan;
    ObjectProductionRallyPoint rallyPoint;
    container::Array<ObjectProductionSpawnPointRuntime,
                     kObjectProductionSpawnPointCount> spawnPoints;
    uint64_t nextExitTick = 0;
    uint64_t revision = 0;
    uint32_t initialBurstRemaining = 0;
    uint32_t nextReservationToken = 1;
    uint32_t activeReservationToken = 0;
    uint8_t spawnPointCount = 0;
    bool spawnPointsInitialized = false;
};

struct ObjectProductionExitReservation final {
    game::ObjectProductionExitKind kind =
        game::ObjectProductionExitKind::Default;
    uint32_t token = 0;
    uint8_t slot = 0;
    // Freeze the selected logical exit pose into the value ticket. SpawnBehavior
    // reserves an ExitInterface before the central lifecycle transaction runs;
    // retaining only the slot made that transaction fall back to UnitCreatePoint
    // and placed every SpawnPoint child at the host root.
    math::q32_32 localX{};
    math::q32_32 localY{};
    math::q32_32 localZ{};
    math::q32_32 localYaw{};
    bool hasLocalTransform = false;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return token != 0;
    }
};

inline constexpr size_t kObjectProductionDoorCount = 4;

enum class ObjectProductionDoorPhase : uint8_t {
    Closed,
    Opening,
    WaitingOpen,
    Closing,
};

// Mutable counterpart to ProductionUpdate::DoorInfo. A zero start tick is a
// valid first confirmed frame, so phase (rather than a zero sentinel) owns
// liveness. The ModelCondition authority maps this value to the corresponding
// DOOR_n_* condition and never advances the clock itself.
struct ObjectProductionDoorRuntime final {
    ObjectProductionDoorPhase phase = ObjectProductionDoorPhase::Closed;
    // A phase selected below ProductionUpdate's flag-publication boundary is
    // not visible until the next confirmed tick. Retain the phase that the
    // frozen drawable still presents during that one-tick handoff.
    ObjectProductionDoorPhase previousVisiblePhase = ObjectProductionDoorPhase::Closed;
    uint64_t phaseStartedTick = 0;
    uint64_t conditionVisibleTick = 0;
    // ParkingPlaceBehavior owns this latch for the door associated with an
    // occupied hangar space. ProductionUpdate advances the same door clock,
    // but may not close it while the parked aircraft still owns the slot.
    bool holdOpen = false;
};

struct ObjectProductionComponent final {
    container::SharedPtr<const game::ObjectProductionPlan> plan;
    container::SharedPtr<const game::ObjectProductionExitPlan> exitPlan;
    container::Vector<ObjectProductionJob> jobs;
    ObjectProductionRallyPoint rallyPoint;
    container::Array<ObjectProductionDoorRuntime, kObjectProductionDoorCount> doors;
    uint64_t constructionCompleteStartedTick = 0;
    uint64_t constructionCompleteVisibleTick = 0;
    bool constructionCompleteActive = false;
    uint32_t nextProductionId = 1;
    uint64_t revision = 0;
};

[[nodiscard]] bool setProductionDoorHoldOpen(
    ObjectProductionComponent& component, size_t doorIndex, bool holdOpen,
    uint64_t confirmedTick) noexcept;

// Command and tool callers get a typed rejection rather than relying on a
// log string.  The enum is part of confirmed simulation diagnostics; text is
// intentionally generated only at the UI/log boundary.
enum class ObjectProductionRejectionReason : uint8_t {
    None,
    ProducerNotFound,
    ProducerPendingDestroy,
    Unauthorized,
    NotAProducer,
    ProducerDisabled,
    UnsupportedExit,
    ProductNotFound,
    ProductNotTrainable,
    ProductNotAvailable,
    PrerequisitesNotMet,
    MaximumSimultaneousReached,
    UpgradeNotFound,
    UpgradeNotAvailable,
    UpgradeAlreadyComplete,
    UpgradeAlreadyInProgress,
    UpgradeNotInQueue,
    QueueFull,
    ParkingPlacesFull,
    QueueAllocationFailed,
    InsufficientFunds,
    ProductionIdExhausted,
    ProductionIdNotFound,
    InvalidRallyPoint,
    InvalidConfirmedTick,
};

// Ordinary player commands require a usable PLAYER_UPGRADE/OBJECT_UPGRADE
// button including Science gating. RefCode AIPlayer::buildUpgrade only asks
// whether the current CommandSet references the UpgradeTemplate, so script AI
// requests use the narrower legacy admission without bypassing ownership,
// queue, cost or completion rules.
enum class ObjectUpgradeProductionAdmission : uint8_t {
    PlayerCommand,
    ScriptAi,
};

struct ObjectProductionRequestResult final {
    bool accepted = false;
    ObjectProductionRejectionReason rejection = ObjectProductionRejectionReason::None;
    uint32_t productionId = 0;
};

[[nodiscard]] container::StringView objectProductionRejectionMessage(
    ObjectProductionRejectionReason reason) noexcept;

// A ready job is translated into this detached intent.  Only GameSession may
// turn it into an ObjectSpawnRequest, preserving the one authoritative
// lifecycle/team/name publication transaction.  `quantityIndex` is zero
// based and remains stable if a later spawn in the same batch is blocked.
struct ObjectProductionRoutePoint final {
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
};

// Value-only projection of ExitInterface::exitObjectViaDoor for consumers
// that were not created by the factory itself (notably a parachuted rider).
// It freezes the producer, authored exit kind and current rally points before
// the command queue is mutated.
struct ObjectProductionExitRoute final {
    ObjectId producer = INVALID_OBJECT_ID;
    PlayerId owner = INVALID_PLAYER_ID;
    uint32_t sourceSequence = 1;
    game::ObjectProductionExitKind kind =
        game::ObjectProductionExitKind::Default;
    container::Vector<ObjectProductionRoutePoint> points;
};

struct ObjectProductionSpawnIntent final {
    ObjectId producer = INVALID_OBJECT_ID;
    uint32_t productionId = 0;
    uint32_t quantityIndex = 0;
    uint32_t sourceSequence = 0;
    PlayerId owner = INVALID_PLAYER_ID;
    ObjectTeamId targetTeam = INVALID_OBJECT_TEAM_ID;
    uint32_t targetTeamRosterIndex = UINT32_MAX;
    container::SharedPtr<const game::ObjectArchetype> product;
    LogicFixedVec3 position{};
    math::q32_32 yawRadians{};
    std::optional<uint32_t> initialPathfindLayer;
    container::Vector<ObjectProductionRoutePoint> exitRoute;
    ObjectProductionExitReservation exitReservation;
    uint8_t exitDoorIndex = 0;
    bool exitDoorAssigned = false;
    LogicFixedVec3 producerVelocity{};
    uint32_t temporaryStealthFrames = 0;
    bool holdAfterSpawn = false;
    bool inheritProducerKinematics = false;
    bool forceSupplyWanting = false;
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
};

// Completion stays detached until GameSession commits the correctly scoped
// technology state and fan-outs UpgradeMux consumers. No production job gets
// removed merely because its timer reached the deadline.
struct ObjectProductionUpgradeCompletionIntent final {
    ObjectId producer = INVALID_OBJECT_ID;
    PlayerId payer = INVALID_PLAYER_ID;
    UpgradeContentId upgrade = INVALID_UPGRADE_CONTENT_ID;
    UpgradeDefinitionType type = UpgradeDefinitionType::Player;
    int64_t paidCost = 0;
    uint32_t sourceSequence = 0;
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
};

// ProductionUpdate's runtime counterpart.  It owns only mutable queue state,
// payment/refund decisions and fixed-frame progress; it never reparses INI,
// creates a legacy Object, touches render state, or owns an ECS lifecycle.
class ObjectProductionSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity) const;

    [[nodiscard]] ObjectProductionRequestResult queueUnit(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        PlayerRegistry& players, const GameContentSnapshot& content,
        const game::CommandBarOverrideState& commandBarOverrides,
        ObjectId producer, PlayerId requester,
        container::SharedPtr<const game::ObjectArchetype> product,
        uint64_t confirmedTick, uint32_t sourceSequence,
        uint32_t framesPerSecond,
        const EnergySimulationRules& energyRules,
        bool ignorePrerequisites = false,
        ObjectTeamId targetTeam = INVALID_OBJECT_TEAM_ID,
        const std::optional<ObjectProductionRoutePoint>& targetRallyPoint =
            std::nullopt,
        uint32_t targetTeamRosterIndex = UINT32_MAX) const;

    // Read-only half of queueUnit's authored airfield parking admission.
    // Command UI and confirmed execution call the same rule so an aircraft
    // button cannot appear enabled only to be rejected one layer later.
    [[nodiscard]] bool hasQueueCapacityForProduct(
        const ecs::registry& registry, ecs::entity producer,
        const ObjectProductionComponent& production,
        const game::ObjectArchetype& product) const noexcept;

    // Scenario Team assembly derives its outstanding work from the real
    // factory FIFOs instead of maintaining a second mutable job mirror.
    [[nodiscard]] uint32_t pendingUnitCountForTeam(
        const ecs::registry& registry, ObjectTeamId team,
        container::StringView productTemplate = {},
        uint32_t targetTeamRosterIndex = UINT32_MAX) const noexcept;
    // Explicit administrative cancellation refunds and removes only jobs
    // owned by that assembly. Timed BUILD_TEAM failure uses detach below to
    // preserve RefCode's paid in-flight work instead.
    [[nodiscard]] uint32_t cancelTeamProduction(
        ecs::registry& registry, PlayerRegistry& players,
        ObjectTeamId team) const noexcept;
    // Legacy TeamInQueue::disband drops WorkOrder correlation without
    // cancelling paid ProductionUpdate work. Detached jobs complete into the
    // player's default Team and retain their original cost/progress.
    [[nodiscard]] uint32_t detachTeamProduction(
        ecs::registry& registry, ObjectTeamId team) const noexcept;

    [[nodiscard]] ObjectProductionRequestResult queuePlayerUpgrade(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        PlayerRegistry& players, const GameContentSnapshot& content,
        const game::CommandBarOverrideState& commandBarOverrides,
        ObjectId producer, PlayerId requester, const UpgradeDefinition& upgrade,
        uint64_t confirmedTick, uint32_t sourceSequence,
        uint32_t framesPerSecond,
        ObjectUpgradeProductionAdmission admission =
            ObjectUpgradeProductionAdmission::PlayerCommand) const;

    [[nodiscard]] ObjectProductionRequestResult cancelUnit(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        PlayerRegistry& players, ObjectId producer, PlayerId requester,
        uint32_t productionId) const;

    [[nodiscard]] ObjectProductionRequestResult cancelPlayerUpgrade(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        PlayerRegistry& players, ObjectId producer, PlayerId requester,
        UpgradeContentId upgrade) const;

    // RefCode cancels ProductionUpdate before an Object defects or changes
    // controlling player.  Keep that ownership boundary explicit: every
    // frozen charge is refunded to its original payer and a PLAYER upgrade's
    // global in-progress reservation is released before OwnerComponent moves.
    [[nodiscard]] bool cancelAndRefundForOwnershipTransfer(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        PlayerRegistry& players, ObjectId producer) const;
    [[nodiscard]] bool cancelAndRefundAll(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        PlayerRegistry& players, ObjectId producer) const;
    // Authored ProductionUpdate::onDie. Unlike command cancellation this is
    // valid after an earlier DestroyDie has marked the object PendingDestroy.
    [[nodiscard]] bool onDie(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        PlayerRegistry& players, ObjectId producer,
        uint32_t authoredOrder) const;

    // Lifecycle may be flushed outside the normal per-tick production pass
    // (for example after a script barrier). Drain only factories already
    // marked PendingDestroy, in stable ObjectId order, so paid jobs and global
    // PLAYER-upgrade reservations cannot outlive their producer.
    void cancelPendingDestroyed(ecs::registry& registry,
                                const ObjectLifecycle& lifecycle,
                                PlayerRegistry& players) const;

    // ExitInterface is also consumed by SpawnBehavior and other non-factory
    // systems.  These value-only operations expose the same authoritative
    // Queue/SpawnPoint reservation clock used by ProductionUpdate without
    // leaking an EnTT entity or mutable component pointer to the caller.
    [[nodiscard]] std::optional<ObjectProductionExitReservation>
    reserveExternalExit(ecs::registry& registry,
                        const ObjectLifecycle& lifecycle,
                        const GameContentSnapshot& content, ObjectId host,
                        uint64_t confirmedTick) const;
    [[nodiscard]] bool commitExternalExit(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId host, ObjectProductionExitReservation reservation,
        ObjectId spawnedObject, uint64_t confirmedTick,
        uint32_t framesPerSecond) const;
    void releaseExternalExit(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId host,
        ObjectProductionExitReservation reservation) const noexcept;

    [[nodiscard]] std::optional<ObjectProductionExitRoute>
    spawnRallyRoute(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId producer, PlayerId owner,
        uint32_t sourceSequence = 1) const;

    [[nodiscard]] ObjectProductionRequestResult setRallyPoint(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId producer, PlayerId requester, ObjectProductionRallyPoint rallyPoint) const;

    // Pending-destroy producers are refunded before the normal lifecycle
    // flush.  Surviving producers advance exactly their queue head; a ready
    // batch remains in-place until GameSession explicitly acknowledges each
    // successful ObjectSpawnRequest below.
    void update(ecs::registry& registry, const ObjectLifecycle& lifecycle,
                PlayerRegistry& players, const GameContentSnapshot& content,
                const game::terrain::TerrainLogic& terrain,
                 uint64_t confirmedTick, uint32_t framesPerSecond,
                 const EnergySimulationRules& energyRules,
                container::Vector<ObjectProductionSpawnIntent>& outSpawns,
                container::Vector<ObjectProductionUpgradeCompletionIntent>& outUpgrades) const;

    [[nodiscard]] bool acknowledgeSpawn(ecs::registry& registry,
                                         const ObjectLifecycle& lifecycle,
                                          ObjectId producer, uint32_t productionId,
                                          uint32_t quantityIndex,
                                          ObjectId spawnedObject,
                                          ObjectProductionExitReservation reservation,
                                          uint64_t confirmedTick,
                                          uint32_t framesPerSecond) const;

    // A failed central ObjectSpawnRequest releases only its detached exit
    // reservation; the paid queue head remains ready and retries next tick.
    void releaseSpawnReservation(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId producer, ObjectProductionExitReservation reservation) const;

    [[nodiscard]] bool acknowledgePlayerUpgrade(ecs::registry& registry,
                                                 const ObjectLifecycle& lifecycle,
                                                 ObjectId producer,
                                                 UpgradeContentId upgrade) const;
};

} // namespace engine
