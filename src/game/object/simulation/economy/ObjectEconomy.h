#pragma once

#include "core/container/container_types.h"
#include "game/data/base/UpgradeCatalog.h"
#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

#include "game/object/plan/economy/ObjectEconomyPlanTypes.h"
namespace engine {

class ObjectLifecycle;
class PlayerRegistry;
class GameContentSnapshot;
class UpgradeCatalog;
struct ObjectDamageRequest;
struct ObjectSimulationRules;

struct ObjectAutoFindHealingRuntime final {
    uint64_t nextScanTick = 0;
    uint64_t observedExternalOrderRevision = 0;
    uint32_t nextCommandSequence = 1;
    ObjectId targetDock = INVALID_OBJECT_ID;

    bool operator==(const ObjectAutoFindHealingRuntime&) const = default;
};

enum class ObjectSupplyTruckRuntimeState : uint8_t {
    SeekingWarehouse,
    MovingToApproach,
    WaitingToEnter,
    MovingToEnter,
    MovingToAction,
    Acting,
    MovingToExit,
    Regrouping,
};

struct ObjectSupplyTruckRuntime final {
    uint32_t boxes = 0;
    uint32_t nextCommandSequence = 1;
    uint64_t nextActionTick = 0;
    uint64_t observedExternalOrderRevision = 0;
    ObjectId targetDock = INVALID_OBJECT_ID;
    ObjectId preferredDock = INVALID_OBJECT_ID;
    uint32_t targetDockModule = 0;
    int32_t approachPosition = -1;
    bool targetIsCenter = false;
    ObjectSupplyTruckRuntimeState state =
        ObjectSupplyTruckRuntimeState::SeekingWarehouse;
    // IDLE_ALL_UNITS suppresses autonomous supply reacquisition until the
    // matching RESUME_SUPPLY_TRUCKING script action clears this bit.
    bool scriptIdleSuppressed = false;
    // A replacement player/scenario order leaves the legacy SupplyTruck state
    // in Idle after that order completes. Manual Dock, production Wanting, or
    // RESUME_SUPPLY_TRUCKING explicitly releases this latch.
    bool externalIdleSuppressed = false;
    // RegroupingState chooses its destination only on entry.  This latch
    // distinguishes an empty queue before that one-shot move is issued from
    // an empty queue after movement has completed or failed.
    bool regroupMoveIssued = false;
    // WorkerAIUpdate starts in AS_DOZER. Its supply sub-brain is entered only
    // after production/AI force-wanting or an explicit Dock command; ordinary
    // SupplyTruckAIUpdate instances are active from admission.
    bool workerSupplyActive = true;

    bool operator==(const ObjectSupplyTruckRuntime&) const = default;
};

enum class ObjectHackInternetRuntimePhase : uint8_t {
    Idle,
    Unpacking,
    Hacking,
    Packing,
};

struct ObjectHackInternetRuntime final {
    uint64_t phaseEndTick = 0;
    uint64_t nextCashTick = 0;
    uint64_t observedExternalOrderRevision = 0;
    uint64_t revision = 0;
    ObjectId internetCenter = INVALID_OBJECT_ID;
    ObjectHackInternetRuntimePhase phase =
        ObjectHackInternetRuntimePhase::Idle;
    bool autoStartedByContainment = false;

    bool operator==(const ObjectHackInternetRuntime&) const = default;
};

struct ObjectSupplyDockRuntime final {
    // Stable ObjectIds are the reservation authority.  EnTT entities and
    // pointers never survive a structural mutation or confirmed frame.
    container::Vector<ObjectId> approachOwners;
    container::Vector<bool> approachReached;
    // RefCode obtains these once from Drawable::getPristineBonePositions().
    // TD freezes the same model-space coordinates from the immutable content
    // catalog at object instantiation, so authoritative docking never queries
    // Renderer state or converts float poses during a logic tick.
    container::Vector<LogicFixedVec3> approachPositionsLocal;
    container::Vector<bool> approachPositionValid;
    LogicFixedVec3 enterPositionLocal{};
    LogicFixedVec3 actionPositionLocal{};
    LogicFixedVec3 exitPositionLocal{};
    ObjectId activeDocker = INVALID_OBJECT_ID;
    bool activeDockerInside = false;
    // DockUpdate::m_dockOpen is independent from crippled state. Closing a
    // dock rejects new approach/enter work, while an already-inside docker
    // retains the authority needed to complete its exit.
    bool open = true;
    bool enterPositionValid = false;
    bool actionPositionValid = false;
    bool exitPositionValid = false;
    uint64_t revision = 0;

    bool operator==(const ObjectSupplyDockRuntime&) const = default;
};

struct ObjectRepairDockRuntime final {
    ObjectSupplyDockRuntime dock;
    container::Vector<PlayerId> approachOwnerPlayers;
    ObjectId repairSubject = INVALID_OBJECT_ID;
    ObjectId pendingDrone = INVALID_OBJECT_ID;
    PlayerId reservationDockOwner = INVALID_PLAYER_ID;
    PlayerId dockOwnerAtAdmission = INVALID_PLAYER_ID;
    PlayerId dockerOwnerAtAdmission = INVALID_PLAYER_ID;
    math::q32_32 healthToAddPerTick{};
    uint64_t pendingActionTick = 0;
    uint64_t revision = 0;
    bool activeDockerAtActionPoint = false;
    bool actionPending = false;

    bool operator==(const ObjectRepairDockRuntime&) const = default;
};

struct ObjectSupplyCenterDockRuntime final {
    ObjectSupplyDockRuntime dock;
    uint64_t revision = 0;

    bool operator==(const ObjectSupplyCenterDockRuntime&) const = default;
};

struct ObjectSupplyWarehouseDockRuntime final {
    ObjectSupplyDockRuntime dock;
    uint32_t boxesStored = 0;
    uint64_t revision = 0;

    bool operator==(const ObjectSupplyWarehouseDockRuntime&) const = default;
};

enum class ObjectSupplyEventKind : uint8_t {
    DockReserved,
    DockEntered,
    BoxTransferred,
    CashDelivered,
    DockExited,
    SuppliesDepletedVoice,
};

// Confirmed value output.  Audio/presentation consumes this without an
// AudioEvent handle, renderer pointer or EnTT entity crossing the boundary.
struct ObjectSupplyEvent final {
    ObjectSupplyEventKind kind = ObjectSupplyEventKind::DockReserved;
    ObjectId truck = INVALID_OBJECT_ID;
    ObjectId dock = INVALID_OBJECT_ID;
    container::String resource;
    uint32_t boxes = 0;
    int64_t cash = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectEconomyComponent final {
    container::SharedPtr<const game::ObjectEconomyPlan> plan;
    container::Vector<ObjectAutoFindHealingRuntime> autoFindHealing;
    container::Vector<ObjectRepairDockRuntime> repairDocks;
    container::Vector<ObjectSupplyTruckRuntime> supplyTrucks;
    container::Vector<ObjectHackInternetRuntime> hackInternet;
    container::Vector<ObjectSupplyCenterDockRuntime> supplyCenterDocks;
    container::Vector<ObjectSupplyWarehouseDockRuntime> supplyWarehouseDocks;
};

// Value-only confirmed-frame state. The immutable authored plan and every
// external side-effect owner (notably PlayerRegistry cash/XP) stay outside the
// snapshot. HackInternet payout and XP are committed atomically by the economy
// update; nextCashTick plus revision are therefore sufficient to resume the
// cadence without replaying a committed payout.
struct ObjectEconomyRuntimeSnapshot final {
    static constexpr uint32_t SchemaVersion = 7;

    uint32_t schemaVersion = SchemaVersion;
    container::Vector<ObjectAutoFindHealingRuntime> autoFindHealing;
    container::Vector<ObjectRepairDockRuntime> repairDocks;
    container::Vector<ObjectSupplyTruckRuntime> supplyTrucks;
    container::Vector<ObjectHackInternetRuntime> hackInternet;
    container::Vector<ObjectSupplyCenterDockRuntime> supplyCenterDocks;
    container::Vector<ObjectSupplyWarehouseDockRuntime> supplyWarehouseDocks;

    bool operator==(const ObjectEconomyRuntimeSnapshot&) const = default;
};

enum class ObjectEconomySnapshotStatus : uint8_t {
    Success,
    MissingPlan,
    SchemaMismatch,
    ShapeMismatch,
};

// Economy-owned ingress for the shared AI Dock state.  The adapter translates
// its protocol to these value commands; it never receives component pointers
// and never becomes a second writer of dock reservations or repair state.
enum class ObjectRepairDockCommandKind : uint8_t {
    ReserveApproach,
    PollClearance,
    AdvanceApproach,
    QueryEntryPosition,
    QueryDockPosition,
    QueryExitPosition,
    NotifyApproachReached,
    NotifyEnterReached,
    NotifyDockReached,
    NotifyExitReached,
    ProcessAction,
    Cancel,
};

enum class ObjectRepairDockCommandStatus : uint8_t {
    Accepted,
    Denied,
    DockMissing,
    DockClosed,
    ClearanceWaiting,
    ClearToAdvance,
    ClearToEnter,
    ActionContinue,
    ActionComplete,
};

enum class ObjectEconomyDockKind : uint8_t {
    Repair,
    SupplyCenter,
    SupplyWarehouse,
};

struct ObjectRepairDockCommand final {
    ObjectRepairDockCommandKind kind =
        ObjectRepairDockCommandKind::ReserveApproach;
    ObjectId dock = INVALID_OBJECT_ID;
    ObjectId docker = INVALID_OBJECT_ID;
    uint32_t moduleIndex = 0;
    int32_t approachPosition = -1;
    uint64_t confirmedTick = 0;
};

struct ObjectRepairDockCommandResult final {
    ObjectRepairDockCommandStatus status =
        ObjectRepairDockCommandStatus::Denied;
    LogicFixedVec3 position{};
    ObjectId drone = INVALID_OBJECT_ID;
    int32_t approachPosition = -1;
    bool allowsPassthrough = false;
};

[[nodiscard]] ObjectEconomySnapshotStatus captureSnapshot(
    const ObjectEconomyComponent& component,
    ObjectEconomyRuntimeSnapshot& outSnapshot);

// Restore is transactional with respect to ObjectEconomyComponent and never
// calls PlayerRegistry, ObjectExperienceSystem, or any other side-effect owner.
[[nodiscard]] ObjectEconomySnapshotStatus restoreSnapshot(
    ObjectEconomyComponent& component,
    const ObjectEconomyRuntimeSnapshot& snapshot);

// FNV-1a over an explicit little-endian semantic stream. No object
// representation, allocation address, or container spare capacity is read.
[[nodiscard]] uint64_t stableDigest(
    const ObjectEconomyRuntimeSnapshot& snapshot) noexcept;

class ObjectEconomySystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          const GameContentSnapshot* content,
                          const ObjectSimulationRules& rules,
                          uint64_t confirmedTick) const;

    void updateAutoFindHealing(ecs::registry& registry,
                               const ObjectLifecycle& lifecycle,
                               const PlayerRegistry* players,
                               const ObjectSimulationRules& rules,
                               uint64_t confirmedTick) const;

    void updateRepairDocks(ecs::registry& registry,
                           const ObjectLifecycle& lifecycle,
                           const ObjectSimulationRules& rules,
                           uint64_t confirmedTick,
                           container::Vector<ObjectDamageRequest>& outDamage) const;

    // Executes one RepairDockUpdate/DockUpdate protocol operation.  Healing
    // itself is staged by ProcessAction and committed by updateRepairDocks
    // through the central Body damage barrier later in the same logic frame.
    [[nodiscard]] ObjectRepairDockCommandResult processRepairDockCommand(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectSimulationRules& rules,
        const ObjectRepairDockCommand& command) const;

    // Typed owner for DockUpdate::setDockOpen(). This gate is intentionally
    // separate from ObjectDockCrippleComponent and object disabled state.
    [[nodiscard]] bool setDockOpen(
        ecs::registry& registry, ecs::entity entity,
        ObjectEconomyDockKind kind, uint32_t moduleIndex, bool open) const;

    void updateSupplyTrucks(ecs::registry& registry,
                            ObjectLifecycle& lifecycle,
                            PlayerRegistry& players,
                            const GameContentSnapshot* content,
                            const ObjectSimulationRules& rules,
                            uint64_t confirmedTick,
                            container::Vector<ObjectSupplyEvent>& outEvents,
                            container::Vector<ObjectDamageRequest>& outDamage) const;

    void updateHackInternet(ecs::registry& registry,
                            const ObjectLifecycle& lifecycle,
                            PlayerRegistry& players,
                            const GameContentSnapshot& content,
                            const ObjectSimulationRules& rules,
                            uint64_t confirmedTick) const;

    // Typed ingress for the future command-button/AI adapter. InternetHackContain
    // uses the same state transition internally, so neither caller needs an
    // AI-module pointer or a legacy string command.
    [[nodiscard]] bool requestHackInternet(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, const ObjectSimulationRules& rules,
        uint64_t confirmedTick) const;
};

} // namespace engine
