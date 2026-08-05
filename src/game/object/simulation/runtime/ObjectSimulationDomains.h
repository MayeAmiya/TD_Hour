#pragma once
#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/ai/contracts/AIStateServices.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/structure/ObjectBridge.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/lifecycle/ObjectRebuildHole.h"
#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
namespace engine {
class GameContentSnapshot;
class ObjectSimulation;
class ObjectOwnershipIndex;
class PlayerRegistry;
class SimulationRandom;
struct ObjectCreateExecutionReport;
struct ObjectExperienceMutation;
struct ObjectAIMovementCommand;
struct ObjectFloatEnableRequest;
struct ObjectRepairDockCommand;
struct ObjectRepairDockCommandResult;
struct ObjectAirfieldDefectionEntry;
struct ObjectAirfieldEvent;
struct ObjectAutoDepositEvent;
struct ObjectAutoHealParticleEvent;
struct ObjectBattlePlanPresentationEvent;
struct ObjectBeaconClientEvent;
struct ObjectBridgeRepairScaffoldIntent;
struct ObjectBridgeStateEvent;
struct ObjectConstructionCompletionIntent;
struct ObjectCheckpointNavigationEvent;
struct ObjectChinookRopePresentationEvent;
struct ObjectContainmentEvent;
struct ObjectCountermeasureEvent;
struct ObjectCountermeasureFlareSpawnCommand;
struct ObjectCratePickupCommand;
struct ObjectCreateCrateDieEvent;
struct ObjectCreateObjectDieEvent;
struct ObjectCrushDieEvent;
struct ObjectDeathEvent;
struct ObjectDeleteWalkState;
struct ObjectDeletePostambleEvent;
struct ObjectDeleteDestroyRequest;
struct ObjectDefectionRequest;
struct ObjectDynamicGeometryGameplayEvent;
struct ObjectDynamicGeometryPresentationEvent;
struct ObjectDynamicShroudDecalEvent;
struct ObjectDeployStyleManualFrameEvent;
struct ObjectEmpParticleEvent;
struct ObjectExperienceEvent;
struct ObjectFireAudioCommand;
struct ObjectFxListDieEffectEvent;
struct ObjectGrantStealthPulseEvent;
struct ObjectHealthEvent;
struct ObjectHeightDiePresentationEvent;
struct ObjectInstantDeathEffectEvent;
struct ObjectLeafletParticleEvent;
struct ObjectMinefieldFxEvent;
struct ObjectMineSpawnCommand;
struct ObjectMissileLauncherFxEvent;
struct ObjectMovementEvent;
struct ObjectVehicleNeutralizationRequest;
struct ObjectNeutronMissilePresentationEvent;
struct ObjectParticleUplinkBeamEvent;
struct ObjectParticleUplinkFxEvent;
struct ObjectParticleUplinkPhaseEvent;
struct ObjectParticleUplinkRemnantSpawnRequest;
struct ObjectParticleUplinkRevealRequest;
struct ObjectParticleUplinkScorchEvent;
struct ObjectPhysicsCrashCommand;
struct ObjectPhysicsEvent;
struct ObjectAIMovementObstructionEvent;
struct ObjectPilotVehicleTakeoverRequest;
struct ObjectRadiusDecalEvent;
struct ObjectRailroadCarriageSpawnRequest;
struct ObjectRebuildCompletionIntent;
struct ObjectRebuildHoleExposeIntent;
struct ObjectRebuildTargetRemapIntent;
struct ObjectRebuildWorkerSpawnIntent;
struct ObjectSlaveRepairPresentationEvent;
struct ObjectSlowDeathPhaseEvent;
struct ObjectSpecialPowerCompletionEvent;
struct ObjectSpecialPowerExecutionEvent;
struct ObjectSpecialAbilityEffectRequest;
struct ObjectSpecialAbilityFacingRequest;
struct ObjectStealthDetectorPulseEvent;
struct ObjectDisguisePresentationEvent;
struct ObjectTacticalPresentationEvent;
struct ObjectStickyBombPresentationEvent;
struct ObjectStructureEffectEvent;
struct ObjectSupplyEvent;
struct ObjectSystemWeaponFireCommand;
struct ObjectTechBuildingEvent;
struct ObjectTensileFormationEvent;
struct ObjectToppleFxEvent;
struct ObjectTopplePathfindRemovalRequest;
struct ObjectToppleStumpSpawnRequest;
struct ObjectTransitionDamageFxEvent;
struct ObjectWaveGuideEvent;
struct ObjectWaveGuideBridgeImpact;
struct ObjectWeaponBonusUpdateEvent;
class ObjectSimulationDamageDomain;
class ObjectSimulationMotionDomain;
class ObjectSimulationLifecycleDomain;
class ObjectSimulationProgressionDomain;
class ObjectSimulationConstructionDomain;
class ObjectSimulationAbilityDomain;
class ObjectSimulationContainmentDomain;
class ObjectSimulationAirOperationsDomain;
namespace object_simulation_detail {
struct ObjectSimulationState;
[[nodiscard]] ObjectSimulationState& state(ObjectSimulationDamageDomain&) noexcept;
[[nodiscard]] const ObjectSimulationState& state(const ObjectSimulationDamageDomain&) noexcept;
[[nodiscard]] ObjectSimulationState& state(ObjectSimulationMotionDomain&) noexcept;
[[nodiscard]] const ObjectSimulationState& state(const ObjectSimulationMotionDomain&) noexcept;
[[nodiscard]] ObjectSimulationState& state(ObjectSimulationLifecycleDomain&) noexcept;
[[nodiscard]] const ObjectSimulationState& state(const ObjectSimulationLifecycleDomain&) noexcept;
[[nodiscard]] ObjectSimulationState& state(ObjectSimulationProgressionDomain&) noexcept;
[[nodiscard]] const ObjectSimulationState& state(const ObjectSimulationProgressionDomain&) noexcept;
[[nodiscard]] ObjectSimulationState& state(ObjectSimulationConstructionDomain&) noexcept;
[[nodiscard]] const ObjectSimulationState& state(const ObjectSimulationConstructionDomain&) noexcept;
[[nodiscard]] ObjectSimulationState& state(ObjectSimulationAbilityDomain&) noexcept;
[[nodiscard]] const ObjectSimulationState& state(const ObjectSimulationAbilityDomain&) noexcept;
[[nodiscard]] ObjectSimulationState& state(ObjectSimulationContainmentDomain&) noexcept;
[[nodiscard]] const ObjectSimulationState& state(const ObjectSimulationContainmentDomain&) noexcept;
[[nodiscard]] ObjectSimulationState& state(ObjectSimulationAirOperationsDomain&) noexcept;
[[nodiscard]] const ObjectSimulationState& state(const ObjectSimulationAirOperationsDomain&) noexcept;
}

// A short-lived, synchronous view of one producer queue.  Construction
// detaches the producer's existing allocation without copying.  Destruction
// clears moved-from elements and returns that allocation to an otherwise empty
// producer queue.  Confirmed-event collectors must only translate values while
// a lease is live; gameplay callbacks run after the lease has been destroyed.
template <typename Event>
class ObjectSimulationEventLease final {
public:
    explicit ObjectSimulationEventLease(
        container::Vector<Event>& source) noexcept
        : m_source(&source) {
        m_events.swap(source);
    }

    ObjectSimulationEventLease(const ObjectSimulationEventLease&) = delete;
    ObjectSimulationEventLease& operator=(
        const ObjectSimulationEventLease&) = delete;
    ObjectSimulationEventLease(
        ObjectSimulationEventLease&& other) noexcept
        : m_source(other.m_source), m_events(std::move(other.m_events)) {
        other.m_source = nullptr;
    }
    ObjectSimulationEventLease& operator=(
        ObjectSimulationEventLease&&) = delete;

    ~ObjectSimulationEventLease() {
        if (!m_source) return;
        m_events.clear();
        // A producer write while a lease is live is legal to preserve, but it
        // means that call cannot reclaim the detached allocation.  The
        // confirmed collector never takes this branch because it executes no
        // gameplay callbacks while translating events into WorkItems.
        if (m_source->empty()) m_source->swap(m_events);
    }

    [[nodiscard]] auto begin() noexcept { return m_events.begin(); }
    [[nodiscard]] auto end() noexcept { return m_events.end(); }
    [[nodiscard]] auto begin() const noexcept { return m_events.begin(); }
    [[nodiscard]] auto end() const noexcept { return m_events.end(); }
    [[nodiscard]] bool empty() const noexcept { return m_events.empty(); }
    [[nodiscard]] size_t size() const noexcept { return m_events.size(); }
    [[nodiscard]] container::Vector<Event>& events() noexcept {
        return m_events;
    }

private:
    container::Vector<Event>* m_source = nullptr;
    container::Vector<Event> m_events;
};

class ObjectSimulationDamageDomain {
public:
    [[nodiscard]] container::Vector<ObjectHealthEvent> takeHealthEvents();
    // Moves elements into caller-owned storage while both sides retain their
    // capacity. Intended for the synchronous, potentially repeated confirmed
    // damage publication path.
    void drainHealthEvents(container::Vector<ObjectHealthEvent>& out);
    // Extracts authoritative OCL payloads while leaving FX/particle commands
    // for the later presentation publication pass.
    [[nodiscard]] container::Vector<ObjectTransitionDamageFxEvent>
    takeTransitionDamageGameplayEvents();
    [[nodiscard]] ObjectSimulationEventLease<ObjectTransitionDamageFxEvent>
    leaseTransitionDamageGameplayEvents();
    [[nodiscard]] container::Vector<ObjectTransitionDamageFxEvent>
    takeTransitionDamageFxEvents();
};

class ObjectSimulationMotionDomain {
public:
    [[nodiscard]] container::Vector<ObjectMovementEvent> takeMovementEvents();
    [[nodiscard]] container::Vector<ai::MovementFeedback>
    takeAIMovementFeedback();
    void drainAIMovementFeedback(
        container::Vector<ai::MovementFeedback>& out);
    [[nodiscard]] container::Vector<ai::AIFacingFeedback>
    takeAIFacingFeedback();
    void drainAIFacingFeedback(
        container::Vector<ai::AIFacingFeedback>& out);
    [[nodiscard]] container::Vector<ObjectPhysicsEvent> takePhysicsEvents();
    [[nodiscard]] container::Vector<ObjectAIMovementObstructionEvent>
    takeAIMovementObstructionEvents();
    [[nodiscard]] ObjectSimulationEventLease<ObjectAIMovementObstructionEvent>
    leaseAIMovementObstructionEvents();
    [[nodiscard]] container::Vector<ObjectPhysicsCrashCommand>
    takePhysicsCrashCommands();
    [[nodiscard]] ObjectSimulationEventLease<ObjectPhysicsCrashCommand>
    leasePhysicsCrashCommands();
};

class ObjectSimulationLifecycleDomain {
public:
    [[nodiscard]] ObjectSimulationEventLease<ObjectDeleteWalkState>
    leaseObjectDeleteWalks();
    [[nodiscard]] container::Vector<ObjectDeleteWalkState>
    takeObjectDeleteWalks();
    [[nodiscard]] container::Vector<ObjectDeletePostambleEvent>
    takeObjectDeletePostambleEvents();
    [[nodiscard]] container::Vector<ObjectDeleteDestroyRequest>
    takeObjectDeleteDestroyRequests();
    [[nodiscard]] ObjectSimulationEventLease<ObjectDeleteDestroyRequest>
    leaseObjectDeleteDestroyRequests();
    [[nodiscard]] container::Vector<ObjectDeathEvent> takeDeathEvents();
    [[nodiscard]] container::Vector<ObjectSpecialPowerCompletionEvent>
    takeSpecialPowerCompletionEvents();
    [[nodiscard]] ObjectSimulationEventLease<ObjectSpecialPowerCompletionEvent>
    leaseSpecialPowerCompletionEvents();
    [[nodiscard]] container::Vector<ObjectVehicleNeutralizationRequest>
    takeVehicleNeutralizationRequests();
    [[nodiscard]] ObjectSimulationEventLease<ObjectVehicleNeutralizationRequest>
    leaseVehicleNeutralizationRequests();
    [[nodiscard]] container::Vector<ObjectCrushDieEvent> takeCrushDieEvents();
    [[nodiscard]] container::Vector<ObjectInstantDeathEffectEvent>
    takeInstantDeathGameplayEvents();
    [[nodiscard]] ObjectSimulationEventLease<ObjectInstantDeathEffectEvent>
    leaseInstantDeathGameplayEvents();
    [[nodiscard]] container::Vector<ObjectInstantDeathEffectEvent> takeInstantDeathEffectEvents();
    [[nodiscard]] container::Vector<ObjectCreateObjectDieEvent>
    takeCreateObjectDieEvents();
    [[nodiscard]] ObjectSimulationEventLease<ObjectCreateObjectDieEvent>
    leaseCreateObjectDieEvents();
    [[nodiscard]] container::Vector<ObjectCreateCrateDieEvent>
    takeCreateCrateDieEvents();
    [[nodiscard]] ObjectSimulationEventLease<ObjectCreateCrateDieEvent>
    leaseCreateCrateDieEvents();
    [[nodiscard]] container::Vector<ObjectFxListDieEffectEvent> takeFxListDieEffectEvents();
    [[nodiscard]] container::Vector<ObjectStructureEffectEvent>
    takeStructureEffectEvents();
    [[nodiscard]] ObjectSimulationEventLease<ObjectStructureEffectEvent>
    leaseStructureEffectEvents();
    [[nodiscard]] container::Vector<ObjectHeightDiePresentationEvent>
    takeHeightDiePresentationEvents();
    [[nodiscard]] container::Vector<ObjectSlowDeathPhaseEvent>
    takeSlowDeathGameplayEvents();
    [[nodiscard]] ObjectSimulationEventLease<ObjectSlowDeathPhaseEvent>
    leaseSlowDeathGameplayEvents();
    [[nodiscard]] container::Vector<ObjectSlowDeathPhaseEvent> takeSlowDeathPhaseEvents();
    [[nodiscard]] container::Vector<ObjectCreationListInvocation>
    takeObjectCreationListInvocations();
    [[nodiscard]] ObjectSimulationEventLease<ObjectCreationListInvocation>
    leaseObjectCreationListInvocations();
    [[nodiscard]] container::Vector<ObjectReplacementInvocation>
    takeObjectReplacementInvocations();
    [[nodiscard]] ObjectSimulationEventLease<ObjectReplacementInvocation>
    leaseObjectReplacementInvocations();
};

class ObjectSimulationProgressionDomain {
public:
    [[nodiscard]] ObjectExperienceMutation setObjectVeterancyLevel(
        ecs::registry& registry, ObjectLifecycle& lifecycle, ObjectId object,
        game::ObjectVeterancyLevel level,
        const UpgradeMask& ownerCompletedUpgrades,
        uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context = {});
    [[nodiscard]] ObjectExperienceMutation addObjectExperience(
        ecs::registry& registry, ObjectLifecycle& lifecycle, ObjectId object,
        int32_t points, bool canScaleForBonus, ObjectId source,
        const UpgradeMask& ownerCompletedUpgrades,
        uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context = {});
    [[nodiscard]] bool canObjectReceiveUpgrade(
        const ecs::registry& registry, ecs::entity entity,
        const UpgradeMask& ownerCompletedUpgrades,
        UpgradeContentId prospectiveUpgrade) const noexcept;
    [[nodiscard]] bool hasObjectUpgrade(const ecs::registry& registry,
                                        ecs::entity entity,
                                        UpgradeContentId upgrade) const noexcept;
    [[nodiscard]] bool completeObjectUpgrade(
        ecs::registry& registry, ObjectLifecycle& lifecycle, ObjectId object,
        UpgradeContentId upgrade, const UpgradeMask& ownerCompletedUpgrades,
        uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context = {}) const;
    // Create callbacks are split exactly as authored: onCreate runs once
    // after every abortable spawn invariant commits, while onBuildComplete is
    // immediate for finished objects and deferred for construction. Both
    // phases preserve the shared module author order.
    [[nodiscard]] ObjectCreateExecutionReport onObjectCreated(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const ObjectOwnershipIndex& ownership, ObjectId object,
        const UpgradeMask& ownerCompletedUpgrades,
        uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context = {});
    [[nodiscard]] ObjectCreateExecutionReport onObjectBuildCompleted(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const ObjectOwnershipIndex& ownership, ObjectId object,
        const UpgradeMask& ownerCompletedUpgrades,
        uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context = {});
    // Construction writers clear their status through this explicit callback;
    // Create callbacks run before UpgradeMux re-evaluation, matching the
    // original DozerAIUpdate completion transaction.
    void onObjectConstructionCompleted(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const ObjectOwnershipIndex& ownership, ObjectId object,
        const UpgradeMask& ownerCompletedUpgrades,
        uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context = {});
    // Called by the one player-upgrade completion transaction. The stable
    // ownership index limits activation to that player's extant objects.
    void onPlayerUpgradeCompleted(ecs::registry& registry, ObjectLifecycle& lifecycle,
                                  const ObjectOwnershipIndex& ownership, PlayerId player,
                                  const UpgradeMask& completedUpgrades,
                                  uint64_t confirmedTick,
                                  ObjectUpgradeExecutionContext context = {});
    // Ownership transfer refreshes module-specific capture state without a
    // generic UpgradeMux re-evaluation. AutoDeposit receives the new player
    // explicitly so its one-shot bonus and periodic deadline remain part of
    // the same authoritative ownership transaction.
    void onObjectOwnerChanged(ecs::registry& registry, ObjectLifecycle& lifecycle,
                              ObjectId object,
                              const UpgradeMask& newOwnerCompletedUpgrades,
                              uint64_t confirmedTick,
                              ObjectUpgradeExecutionContext context = {});
    [[nodiscard]] container::Vector<ObjectExperienceEvent> takeExperienceEvents();
    [[nodiscard]] container::Vector<ObjectAutoDepositEvent> takeAutoDepositEvents();
    [[nodiscard]] container::Vector<ObjectSupplyEvent> takeSupplyEvents();
    [[nodiscard]] container::Vector<ObjectCratePickupCommand> takeCratePickupCommands();
    [[nodiscard]] ObjectSimulationEventLease<ObjectCratePickupCommand>
    leaseCratePickupCommands();
    [[nodiscard]] container::Vector<ObjectWeaponBonusUpdateEvent>
    takeWeaponBonusUpdateEvents();
    [[nodiscard]] container::Vector<ObjectUpgradeFxInvocation>
    takeObjectUpgradeFxInvocations();
    [[nodiscard]] ObjectSimulationEventLease<ObjectUpgradeFxInvocation>
    leaseObjectUpgradeFxInvocations();
};

class ObjectSimulationConstructionDomain {
public:
    [[nodiscard]] bool beginObjectConstruction(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId site, ObjectId builder, uint32_t requiredFrames,
        bool rebuild, uint64_t confirmedTick) const;
    [[nodiscard]] bool requestObjectRepair(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId builder, ObjectId target, uint64_t confirmedTick,
        uint32_t sourceSequence = 0) const;
    [[nodiscard]] bool canObjectRepair(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const PlayerRegistry& players, ObjectId builder,
        ObjectId target) const;
    [[nodiscard]] bool requestObjectRepair(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const PlayerRegistry& players, ObjectId builder, ObjectId target,
        uint64_t confirmedTick, uint32_t sourceSequence,
        bool replaceExternalOrders,
        bool requireClearTarget) const;
    [[nodiscard]] bool canObjectResumeConstruction(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const PlayerRegistry& players, ObjectId builder,
        ObjectId site) const;
    [[nodiscard]] bool resumeObjectConstruction(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const PlayerRegistry& players, ObjectId builder, ObjectId site,
        uint64_t confirmedTick, uint32_t sourceSequence,
        bool replaceExternalOrders) const;
    [[nodiscard]] bool assignObjectConstruction(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId builder, ObjectId site, uint64_t confirmedTick,
        uint32_t sourceSequence = 0) const;
    [[nodiscard]] ObjectBuilderTask objectBuilderTask(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId builder, ObjectBuilderTaskKind kind,
        size_t moduleIndex = 0) const;
    [[nodiscard]] ObjectBuilderTask mostRecentObjectBuilderTask(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId builder, size_t moduleIndex = 0) const;
    [[nodiscard]] ObjectBuilderTask currentObjectBuilderTask(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId builder, size_t moduleIndex = 0) const;
    [[nodiscard]] bool isObjectBuilderTaskPending(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId builder, ObjectBuilderTaskKind kind,
        size_t moduleIndex = 0) const;
    [[nodiscard]] bool isAnyObjectBuilderTaskPending(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId builder, size_t moduleIndex = 0) const;
    [[nodiscard]] bool cancelAllObjectBuilderTasks(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId builder, uint64_t confirmedTick);
    [[nodiscard]] ObjectRepairDockCommandResult processRepairDockCommand(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectRepairDockCommand& command) const;
    [[nodiscard]] bool startRebuildHole(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        ObjectId hole, container::StringView rebuildTemplate,
        ObjectId spawner, uint64_t confirmedTick) const;
    [[nodiscard]] bool acknowledgeRebuildWorker(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId hole, ObjectId worker, ObjectId reconstruction,
        uint64_t confirmedTick) const;
    [[nodiscard]] bool rejectRebuildWorker(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId hole, uint64_t confirmedTick) const;
    [[nodiscard]] container::Vector<ObjectBridgeStateEvent>
    takeObjectBridgeStateEvents();
    [[nodiscard]] ObjectSimulationEventLease<ObjectBridgeStateEvent>
    leaseObjectBridgeStateEvents();
    [[nodiscard]] container::Vector<
        ObjectRailedTransportDockAttachCompletion>
    takeRailedTransportDockAttachCompletions();
    [[nodiscard]] ObjectSimulationEventLease<
        ObjectRailedTransportDockAttachCompletion>
    leaseRailedTransportDockAttachCompletions();
    [[nodiscard]] container::Vector<ObjectRailroadCarriageSpawnRequest>
    takeRailroadCarriageSpawnRequests();
    [[nodiscard]] ObjectSimulationEventLease<ObjectRailroadCarriageSpawnRequest>
    leaseRailroadCarriageSpawnRequests();
    [[nodiscard]] container::Vector<ObjectRailroadDisembarkRequest>
    takeRailroadDisembarkRequests();
    [[nodiscard]] container::Vector<ObjectRailroadPresentationEvent>
    takeRailroadPresentationEvents();
    [[nodiscard]] ObjectSimulationEventLease<ObjectRailroadDisembarkRequest>
    leaseRailroadDisembarkRequests();
    [[nodiscard]] bool acknowledgeRailroadCarriageSpawn(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectRailroadCarriageSpawnRequest& request,
        ObjectId spawnedCarriage, bool accepted) const;
    [[nodiscard]] container::Vector<ObjectSpawnSlaveRequest>
    takeObjectSpawnSlaveRequests();
    [[nodiscard]] ObjectSimulationEventLease<ObjectSpawnSlaveRequest>
    leaseObjectSpawnSlaveRequests();
    [[nodiscard]] container::Vector<ObjectSlaveRepairPresentationEvent>
    takeObjectSlaveRepairPresentationEvents();
    [[nodiscard]] container::Vector<ObjectTensileFormationEvent>
    takeObjectTensileNavigationEvents();
    [[nodiscard]] ObjectSimulationEventLease<ObjectTensileFormationEvent>
    leaseObjectTensileNavigationEvents();
    [[nodiscard]] container::Vector<ObjectTensileFormationEvent>
    takeObjectTensileFormationEvents();
    [[nodiscard]] bool acknowledgeSpawnSlave(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectSpawnSlaveRequest& request, ObjectId spawned,
        bool accepted) const;
    [[nodiscard]] bool bindOclSlaveMaster(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, ObjectId master) const noexcept;
    [[nodiscard]] ObjectId closestSpawnChild(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId spawner, const LogicFixedVec3& position) const noexcept;
    [[nodiscard]] container::Vector<ObjectId> spawnChildren(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId spawner) const;
    [[nodiscard]] bool setSpawnChildSelfTasking(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId spawner, uint32_t ruleIndex, ObjectId child,
        bool selfTasking) const noexcept;
    [[nodiscard]] container::Vector<ObjectConstructionCompletionIntent>
    takeCompletedObjectConstructions();
    [[nodiscard]] ObjectSimulationEventLease<ObjectConstructionCompletionIntent>
    leaseCompletedObjectConstructions();
    [[nodiscard]] container::Vector<ObjectBridgeRepairScaffoldIntent>
    takeBridgeRepairScaffoldIntents();
    [[nodiscard]] ObjectSimulationEventLease<ObjectBridgeRepairScaffoldIntent>
    leaseBridgeRepairScaffoldIntents();
    [[nodiscard]] container::Vector<ObjectRebuildHoleExposeIntent>
    takeRebuildHoleExposeIntents();
    [[nodiscard]] ObjectSimulationEventLease<ObjectRebuildHoleExposeIntent>
    leaseRebuildHoleExposeIntents();
    [[nodiscard]] container::Vector<ObjectRebuildWorkerSpawnIntent>
    takeRebuildWorkerSpawnIntents();
    [[nodiscard]] ObjectSimulationEventLease<ObjectRebuildWorkerSpawnIntent>
    leaseRebuildWorkerSpawnIntents();
    [[nodiscard]] container::Vector<ObjectRebuildCompletionIntent>
    takeRebuildCompletionIntents();
    [[nodiscard]] ObjectSimulationEventLease<ObjectRebuildCompletionIntent>
    leaseRebuildCompletionIntents();
    [[nodiscard]] container::Vector<ObjectRebuildTargetRemapIntent>
    takeRebuildTargetRemapIntents();
    [[nodiscard]] ObjectSimulationEventLease<ObjectRebuildTargetRemapIntent>
    leaseRebuildTargetRemapIntents();
};

class ObjectSimulationAbilityDomain {
public:
    [[nodiscard]] container::Vector<ObjectSystemWeaponFireCommand>
    takeSystemWeaponFireCommands();
    [[nodiscard]] ObjectSimulationEventLease<ObjectSystemWeaponFireCommand>
    leaseSystemWeaponFireCommands();
    [[nodiscard]] container::Vector<ObjectFireAudioCommand>
    takeObjectFireAudioCommands();
    [[nodiscard]] container::Vector<ObjectEmpParticleEvent>
        takeObjectEmpParticleEvents();
    [[nodiscard]] container::Vector<ObjectAutoHealParticleEvent>
        takeObjectAutoHealParticleEvents();
    [[nodiscard]] container::Vector<ObjectLeafletParticleEvent>
    takeObjectLeafletParticleEvents();
    [[nodiscard]] container::Vector<ObjectStealthDetectorPulseEvent>
    takeStealthDetectorPulseEvents();
    [[nodiscard]] container::Vector<ObjectGrantStealthPulseEvent>
    takeGrantStealthPulseEvents();
    [[nodiscard]] container::Vector<ObjectDynamicShroudDecalEvent>
    takeDynamicShroudDecalEvents();
    [[nodiscard]] container::Vector<ObjectRadiusDecalEvent>
    takeRadiusDecalEvents();
    [[nodiscard]] container::Vector<ObjectTechBuildingEvent>
    takeTechBuildingEvents();
    [[nodiscard]] container::Vector<ObjectBeaconClientEvent>
    takeBeaconClientEvents();
    [[nodiscard]] container::Vector<ObjectCheckpointNavigationEvent>
    takeCheckpointNavigationEvents();
    [[nodiscard]] ObjectSimulationEventLease<ObjectCheckpointNavigationEvent>
    leaseCheckpointNavigationEvents();
    [[nodiscard]] container::Vector<ObjectDynamicGeometryGameplayEvent>
    takeDynamicGeometryGameplayEvents();
    [[nodiscard]] ObjectSimulationEventLease<ObjectDynamicGeometryGameplayEvent>
    leaseDynamicGeometryGameplayEvents();
    [[nodiscard]] container::Vector<ObjectDynamicGeometryPresentationEvent>
    takeDynamicGeometryPresentationEvents();
    [[nodiscard]] container::Vector<ObjectBattlePlanPresentationEvent>
    takeBattlePlanPresentationEvents();
    [[nodiscard]] container::Vector<ObjectTacticalPresentationEvent>
    takeTacticalPresentationEvents();
    [[nodiscard]] container::Vector<ObjectDisguisePresentationEvent>
    takeDisguisePresentationEvents();
    [[nodiscard]] container::Vector<ObjectDeployStyleManualFrameEvent>
    takeDeployStyleManualFrameEvents();
    [[nodiscard]] container::Vector<ObjectToppleFxEvent>
    takeToppleFxEvents();
    [[nodiscard]] container::Vector<ObjectToppleStumpSpawnRequest>
    takeToppleStumpSpawnRequests();
    [[nodiscard]] ObjectSimulationEventLease<ObjectToppleStumpSpawnRequest>
    leaseToppleStumpSpawnRequests();
    [[nodiscard]] container::Vector<ObjectTopplePathfindRemovalRequest>
    takeTopplePathfindRemovalRequests();
    [[nodiscard]] ObjectSimulationEventLease<ObjectTopplePathfindRemovalRequest>
    leaseTopplePathfindRemovalRequests();
    [[nodiscard]] container::Vector<ObjectSpecialPowerExecutionEvent>
    takeSpecialPowerExecutionEvents();
    [[nodiscard]] container::Vector<ObjectSpecialAbilityEffectRequest>
    takeSpecialAbilityEffectRequests();
    [[nodiscard]] ObjectSimulationEventLease<ObjectSpecialAbilityEffectRequest>
    leaseSpecialAbilityEffectRequests();
    [[nodiscard]] container::Vector<ObjectSpecialAbilityFacingRequest>
    takeSpecialAbilityFacingRequests();
    [[nodiscard]] container::Vector<ObjectDefectionRequest>
    takeObjectDefectionRequests();
    [[nodiscard]] ObjectSimulationEventLease<ObjectDefectionRequest>
    leaseObjectDefectionRequests();
    [[nodiscard]] container::Vector<ObjectPilotVehicleTakeoverRequest>
    takePilotVehicleTakeoverRequests();
    [[nodiscard]] ObjectSimulationEventLease<ObjectPilotVehicleTakeoverRequest>
    leasePilotVehicleTakeoverRequests();
    [[nodiscard]] container::Vector<ObjectSpecialPowerSpawnRequest>
    takeSpecialPowerSpawnRequests();
    [[nodiscard]] ObjectSimulationEventLease<ObjectSpecialPowerSpawnRequest>
    leaseSpecialPowerSpawnRequests();
    [[nodiscard]] container::Vector<ObjectCountermeasureFlareSpawnCommand>
    takeCountermeasureFlareSpawnCommands();
    [[nodiscard]] ObjectSimulationEventLease<
        ObjectCountermeasureFlareSpawnCommand>
    leaseCountermeasureFlareSpawnCommands();
    [[nodiscard]] container::Vector<ObjectCountermeasureEvent>
    takeCountermeasureEvents();
    [[nodiscard]] container::Vector<ObjectMineSpawnCommand>
    takeMineSpawnCommands();
    [[nodiscard]] ObjectSimulationEventLease<ObjectMineSpawnCommand>
    leaseMineSpawnCommands();
    [[nodiscard]] container::Vector<ObjectMinefieldFxEvent>
    takeMinefieldFxEvents();
    [[nodiscard]] container::Vector<ObjectStickyBombPresentationEvent>
    takeStickyBombPresentationEvents();
    [[nodiscard]] container::Vector<ObjectNeutronMissilePresentationEvent>
    takeNeutronMissilePresentationEvents();
    [[nodiscard]] container::Vector<ObjectWaveGuideEvent>
    takeWaveGuideEvents();
    // Extracts bridge/terrain/lifecycle work while retaining the matching
    // detached particle event for the presentation publication pass.
    [[nodiscard]] container::Vector<ObjectWaveGuideBridgeImpact>
    takeWaveGuideBridgeImpacts();
    [[nodiscard]] ObjectSimulationEventLease<ObjectWaveGuideBridgeImpact>
    leaseWaveGuideBridgeImpacts();
    [[nodiscard]] container::Vector<ObjectMissileLauncherFxEvent>
    takeMissileLauncherFxEvents();
    [[nodiscard]] container::Vector<ObjectParticleUplinkPhaseEvent>
    takeParticleUplinkPhaseEvents();
    [[nodiscard]] container::Vector<ObjectParticleUplinkBeamEvent>
    takeParticleUplinkBeamEvents();
    [[nodiscard]] container::Vector<ObjectParticleUplinkScorchEvent>
    takeParticleUplinkScorchEvents();
    [[nodiscard]] container::Vector<ObjectParticleUplinkFxEvent>
    takeParticleUplinkFxEvents();
    [[nodiscard]] container::Vector<ObjectParticleUplinkRemnantSpawnRequest>
    takeParticleUplinkRemnantSpawnRequests();
    [[nodiscard]] ObjectSimulationEventLease<
        ObjectParticleUplinkRemnantSpawnRequest>
    leaseParticleUplinkRemnantSpawnRequests();
    [[nodiscard]] bool setParticleUplinkDestination(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, const LogicFixedVec3& destination,
        uint64_t confirmedTick) const;
    [[nodiscard]] bool setParticleUplinkWaypoint(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const game::terrain::TerrainLogic& terrain, ObjectId object,
        SpecialPowerContentId specialPower, uint32_t waypointId,
        uint64_t confirmedTick) const;
    [[nodiscard]] bool setMinefieldTarget(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, const LogicFixedVec3* target) const;
    [[nodiscard]] bool configureMineScoot(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId mine, const LogicFixedVec3& start,
        const LogicFixedVec3& target, uint64_t confirmedTick) const;
    [[nodiscard]] bool setDemoTrapMode(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId trap, bool proximityMode) const;
    [[nodiscard]] bool triggerDemoTrap(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId trap) const;
    [[nodiscard]] bool disarmMine(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        ObjectId mine, uint64_t confirmedTick);
    // Bridge layout/object creation supplies a detached stable-ID value. The
    // scaffold runtime owns only deterministic motion after that transaction.
    [[nodiscard]] bool applyBridgeScaffoldMotionRequest(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectBridgeScaffoldMotionRequest& request) const;
    [[nodiscard]] bool maySpawnSelfTaskAI(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId spawner, uint32_t ruleIndex,
        math::q32_32 maximumSelfTaskersRatio) const noexcept;
};

class ObjectSimulationContainmentDomain {
public:
    [[nodiscard]] bool containObject(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectContainmentAttachRequest& request) const;
    [[nodiscard]] bool requestContainment(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectContainmentRequest& request,
        const PlayerRegistry* players = nullptr,
        const GameContentSnapshot* content = nullptr);
    [[nodiscard]] bool requestRailedTransportExecute(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId transport, uint64_t confirmedTick);
    [[nodiscard]] container::Vector<ObjectId> containmentCaptureDependents(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId container) const;
    [[nodiscard]] ObjectId recentTunnelNetworkNemesis(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId tunnelEntrance, uint64_t confirmedTick) const noexcept;
    [[nodiscard]] bool publishTunnelNetworkNemesis(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId source, ObjectId target, uint64_t confirmedTick);
    [[nodiscard]] bool ejectContainmentOnCapture(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId container, uint64_t confirmedTick,
        const PlayerRegistry* players = nullptr,
        const GameContentSnapshot* content = nullptr);
    [[nodiscard]] bool canContain(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectContainmentRequest& request,
        const PlayerRegistry* players = nullptr) const;
    [[nodiscard]] bool requestTransportBehavior(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const ObjectTransportBehaviorRequest& request);
    [[nodiscard]] bool acknowledgeTransportPayloadDrop(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId transport, ObjectId payload, uint32_t ruleIndex,
        uint32_t attempt);
    [[nodiscard]] bool beginHijackerRelease(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId hijacker, uint32_t ruleIndex, uint64_t confirmedTick);
    [[nodiscard]] bool finishHijackerRelease(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId hijacker, uint32_t ruleIndex);
    [[nodiscard]] bool beginBattleBusUndeath(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId battleBus, uint32_t ruleIndex, uint64_t confirmedTick);
    [[nodiscard]] bool finishBattleBusUndeath(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId battleBus, uint32_t ruleIndex, uint64_t confirmedTick);
    [[nodiscard]] bool beginBattleBusLanded(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId battleBus, uint32_t ruleIndex, uint64_t confirmedTick);
    [[nodiscard]] bool finishBattleBusLanded(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId battleBus, uint32_t ruleIndex, uint64_t confirmedTick);
    [[nodiscard]] bool setParachuteLandingOverride(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId parachute, const LogicFixedVec3& destination);
    [[nodiscard]] bool detachContainedObject(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, uint64_t confirmedTick);
    [[nodiscard]] container::Vector<ObjectContainmentEvent>
    takeContainmentEvents();
    [[nodiscard]] ObjectSimulationEventLease<ObjectContainmentEvent>
    leaseContainmentEvents();
    [[nodiscard]] container::Vector<ObjectTransportGameplayTransaction>
    takeTransportGameplayTransactions();
    [[nodiscard]] ObjectSimulationEventLease<ObjectTransportGameplayTransaction>
    leaseTransportGameplayTransactions();
    [[nodiscard]] container::Vector<ObjectTransportPresentationEvent>
    takeTransportPresentationEvents();
};

class ObjectSimulationAirOperationsDomain {
public:
    [[nodiscard]] bool reserveAirfieldParkingSlot(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId airfield, ObjectId aircraft, uint64_t confirmedTick,
        ObjectAirfieldReservation& outReservation);
    [[nodiscard]] bool reserveProducedAircraftParkingSlot(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId airfield, ObjectId aircraft, size_t doorIndex,
        uint64_t confirmedTick,
        ObjectAirfieldReservation& outReservation);
    [[nodiscard]] bool reserveAirfieldRunway(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId airfield, ObjectId aircraft, bool landing,
        uint64_t confirmedTick,
        ObjectAirfieldReservation& outReservation);
    [[nodiscard]] bool releaseAirfieldParkingSlot(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId airfield, ObjectId aircraft, uint64_t confirmedTick);
    [[nodiscard]] bool releaseAirfieldRunway(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId airfield, ObjectId aircraft, uint64_t confirmedTick);
    [[nodiscard]] bool releaseAirfieldReservations(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId airfield, ObjectId aircraft, uint64_t confirmedTick);
    [[nodiscard]] container::Vector<ObjectAirfieldDefectionEntry>
    airfieldDefectionEntries(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId airfield, PlayerId newOwner) const;
    [[nodiscard]] bool setAirfieldAircraftState(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId aircraft, ObjectAircraftRuntimeState state,
        uint64_t confirmedTick);
    [[nodiscard]] bool beginProducedAircraftExit(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const GameContentSnapshot& content, ObjectId aircraft,
        uint64_t confirmedTick);
    [[nodiscard]] bool requestAircraftRepairAtAirfield(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId aircraft, ObjectId airfield, uint64_t confirmedTick);
    [[nodiscard]] bool beginSpectreGunshipTargeting(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, size_t moduleIndex, LogicFixedVec3 initialTarget,
        LogicFixedVec3 overrideTarget, uint64_t confirmedTick);
    [[nodiscard]] bool updateSpectreGunshipTargeting(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, size_t moduleIndex, LogicFixedVec3 overrideTarget,
        uint64_t confirmedTick);
    [[nodiscard]] bool endSpectreGunshipTargeting(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, size_t moduleIndex, uint64_t confirmedTick);
    [[nodiscard]] bool beginChinookCombatDrop(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        SimulationRandom& random,
        const ObjectChinookCombatDropBeginRequest& request);
    [[nodiscard]] std::optional<ObjectChinookRopeReadyResult>
    nextReadyChinookRope(const ecs::registry& registry,
                         const ObjectLifecycle& lifecycle, ObjectId object,
                         size_t moduleIndex, uint64_t confirmedTick) const;
    [[nodiscard]] bool notifyChinookRappellerStarted(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        SimulationRandom& random, ObjectId object, size_t moduleIndex,
        size_t ropeIndex, uint64_t confirmedTick,
        ObjectId rappeller = INVALID_OBJECT_ID);
    [[nodiscard]] bool endChinookCombatDrop(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, size_t moduleIndex, uint64_t confirmedTick,
        bool immediate = false);
    [[nodiscard]] container::Vector<ObjectAirfieldEvent>
    takeAirfieldEvents();
    [[nodiscard]] container::Vector<ObjectAirfieldAutomaticProductionRequest>
    takeAirfieldAutomaticProductionRequests();
    [[nodiscard]] bool acknowledgeAirfieldAutomaticProduction(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectAirfieldAutomaticProductionRequest& request);
    [[nodiscard]] container::Vector<ObjectChinookRopePresentationEvent>
    takeChinookRopePresentationEvents();
};

} // namespace engine
