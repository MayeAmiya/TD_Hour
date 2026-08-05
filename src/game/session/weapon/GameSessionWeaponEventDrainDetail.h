#pragma once

#include "game/session/transaction/GameplayTransaction.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"
#include "game/session/transaction/GameSessionObjectLifecycleTransactions.h"
#include "game/session/transaction/GameSessionObjectOwnershipTransactions.h"
#include "game/session/transaction/GameSessionNavigationTransactions.h"
#include "game/session/transaction/GameSessionNavigationFootprintTransactions.h"
#include "game/session/transaction/GameSessionObjectTargetRemapTransactions.h"
#include "game/session/transaction/GameSessionProjectileSpawnTransactions.h"
#include "game/session/transaction/GameSessionBridgeLifecycleTransactions.h"
#include "game/session/frame/GameSessionDeletePostambleTransactions.h"
#include "game/session/frame/GameSessionHealthEventPublisher.h"
#include "game/session/frame/GameSessionFramePort.h"
#include "game/session/frame/GameSessionWeaponEventPublisher.h"
#include "game/object/creation/ObjectCreationListCatalog.h"
#include "game/object/simulation/structure/ObjectBridge.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/simulation/combat/ObjectCountermeasures.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/status/ObjectCheckpoint.h"
#include "game/object/simulation/status/ObjectCrateCollide.h"
#include "game/object/simulation/movement/ObjectDynamicGeometry.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/lifecycle/ObjectRebuildHole.h"
#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/lifecycle/ObjectStructureDestruction.h"
#include "game/object/simulation/lifecycle/ObjectDeathWalk.h"
#include "game/object/simulation/lifecycle/ObjectDeleteWalk.h"
#include "game/object/simulation/combat/ObjectTactical.h"
#include "game/object/simulation/structure/ObjectMinefield.h"
#include "game/object/simulation/runtime/ObjectSimulationPhysicsContracts.h"
#include "game/object/simulation/runtime/ObjectDeathEvents.h"
#include "game/object/simulation/structure/ObjectParticleUplinkCannon.h"
#include "game/object/simulation/movement/ObjectWaveGuide.h"

#include <utility>

namespace engine::detail {

constexpr size_t kMaximumSystemWeaponCommands = 65536;
constexpr size_t kMaximumSystemWeaponDamageRequests = 262144;
constexpr size_t kMaximumGameplayTransactions = 524288;
constexpr size_t kMaximumOclNuggets = 131072;
constexpr size_t kMaximumOclCreatedObjects = 131072;
constexpr size_t kMaximumObjectReplacements = 65536;

[[nodiscard]] navigation::NavigationMovementMask
objectCreationNavigationMovementMask(
    const game::ObjectArchetype& archetype,
    const GameContentSnapshot& content) noexcept;

[[nodiscard]] std::optional<ObjectFixedTransformComponent> findCratePlacement(
    const ecs::registry& registry, const PlayerRegistry& players,
    const game::terrain::TerrainLogic& terrain,
    const navigation::NavigationSystem* navigationSystem,
    const ObjectCreateCrateDieEvent& event);

struct GameSessionWeaponDrainStateSource final {
    GameSessionContentStartState& content;
    GameSessionWorldState& world;
    GameSessionAIState& ai;
    GameSessionScriptPresentationState& presentation;
    GameSessionObjectEventState& objectEvents;
};

struct GameSessionWeaponDrainTransactions final {
    GameSessionObjectLifecycleTransactions lifecycle;
    GameSessionObjectOwnershipTransactions ownership;
    GameSessionGameplayPublicationPort publication;
    GameSessionFramePort frame;
    GameSessionNavigationTransactions navigation;
    GameSessionNavigationFootprintTransactions navigationFootprints;
    GameSessionObjectTargetRemapTransactions targetRemap;
    GameSessionWeaponEventPublisher weaponEvents;
    GameSessionProjectileSpawnTransactions projectiles;
    GameSessionBridgeLifecycleTransactions bridges;
    GameSessionDeletePostambleTransactions deletePostamble;
    GameSessionHealthEventPublisher healthEvents;
};

class GameSessionWeaponEventDrain final {
public:
    explicit GameSessionWeaponEventDrain(
        GameSessionWeaponDrainStateSource source,
        GameSessionWeaponDrainTransactions transactions) noexcept
        : m_content(source.content),
          m_world(source.world),
          m_ai(source.ai),
          m_presentation(source.presentation),
          m_objectEvents(source.objectEvents),
          m_lifecycle(std::move(transactions.lifecycle)),
          m_ownership(std::move(transactions.ownership)),
          m_publication(std::move(transactions.publication)),
          m_frame(std::move(transactions.frame)),
          m_navigation(std::move(transactions.navigation)),
          m_navigationFootprints(
              std::move(transactions.navigationFootprints)),
          m_targetRemap(std::move(transactions.targetRemap)),
          m_weaponEvents(std::move(transactions.weaponEvents)),
          m_projectiles(std::move(transactions.projectiles)),
          m_bridges(std::move(transactions.bridges)),
          m_deletePostamble(std::move(transactions.deletePostamble)),
          m_healthEvents(std::move(transactions.healthEvents)) {}

    void run();
    void closeCurrentReaction();

private:
    void applyCratePickupCommands(
        container::Vector<ObjectCratePickupCommand> commands);
    [[nodiscard]] bool changeObjectOwner(
        ObjectId object, PlayerId owner, uint64_t confirmedTick);
    void publishObjectCashFloatingText(
        ObjectId object, LogicFixedVec3 position, int64_t signedAmount,
        uint32_t color, uint64_t confirmedTick);
    [[nodiscard]] bool applyProductionSpawnTransaction(
        const ObjectProductionSpawnIntent& intent, bool blockJobSuffix);
    void applyProductionUpgradeTransaction(
        const ObjectProductionUpgradeCompletionIntent& intent);
    using WorkKind = gameplay::GameplayTransactionKind;

    struct OclWorkState final {
        ObjectId firstCreatedObject = INVALID_OBJECT_ID;
        uint32_t createdObjects = 0;
        uint32_t specialPowerCreationOrdinal = 0;
        bool completionApplied = false;
    };

    struct DestroyObjectWork final {
        ObjectId object = INVALID_OBJECT_ID;
        ObjectDestroyReason reason = ObjectDestroyReason::System;
        ObjectId source = INVALID_OBJECT_ID;
        uint64_t emissionSequence = 0;
        uint64_t confirmedTick = 0;
    };

    struct SlowDeathRubbleWork final {
        ObjectId source = INVALID_OBJECT_ID;
        PlayerId owner = INVALID_PLAYER_ID;
        ObjectTeamId primaryTeam = INVALID_OBJECT_TEAM_ID;
        container::String objectTemplate;
        ObjectFixedTransformComponent transform{};
        uint32_t sourcePathfindLayer = 0;
        uint32_t authoredOrder = 0;
        uint64_t emissionSequence = 0;
        uint64_t confirmedTick = 0;
    };

    struct BridgeRepairScaffoldBatchWork final {
        container::Vector<ObjectBridgeRepairScaffoldIntent> intents;
        uint64_t submissionOrdinal = 0;
    };

    struct CratePickupBatchWork final {
        container::Vector<ObjectCratePickupCommand> commands;
        uint64_t submissionOrdinal = 0;
    };

    struct AIMovementObstructionBatchWork final {
        container::Vector<ObjectAIMovementObstructionEvent> events;
        uint64_t submissionOrdinal = 0;
    };

    struct WorkItem final {
        WorkKind kind = WorkKind::Weapon;
        // Common admission ordinal for payloads whose typed continuation
        // state does not expose one directly (Damage/DeathWalk/BodyResume).
        // It is never a producer-private source/authored sequence.
        uint64_t admissionOrdinal = 0;
        ObjectSystemWeaponFireCommand weapon;
        ObjectDamageRequest damage;
        ObjectCreationListInvocation ocl;
        ObjectCreateCrateDieEvent crate;
        ObjectReplacementInvocation replacement;
        ObjectUpgradeFxInvocation upgradeFx;
        ObjectStructureEffectEvent structureFx;
        ObjectTransitionDamageFxEvent transitionOcl;
        ObjectInstantDeathEffectEvent instantDeath;
        ObjectSlowDeathPhaseEvent slowDeath;
        SlowDeathRubbleWork slowDeathRubble;
        ObjectTopplePathfindRemovalRequest topplePathfind;
        ObjectToppleStumpSpawnRequest toppleStump;
        ObjectPhysicsCrashCommand physicsCrash;
        AIMovementObstructionBatchWork aiMovementObstructionBatch;
        DestroyObjectWork destroyObject;
        ObjectMineSpawnCommand mineSpawn;
        ObjectParticleUplinkRemnantSpawnRequest particleUplinkRemnant;
        ObjectWaveGuideBridgeImpact waveBridgeImpact;
        ObjectCheckpointNavigationEvent checkpointNavigation;
        ObjectTensileFormationEvent tensileNavigation;
        ObjectDynamicGeometryGameplayEvent dynamicGeometry;
        ObjectTransportGameplayTransaction transport;
        ObjectDeathWalkState deathWalk;
        ObjectDeleteWalkState deleteWalk;
        ObjectBodyResumeState bodyResume;
        ObjectOwnershipChangeRequest ownershipChange;
        ObjectDefectionRequest defection;
        ObjectPilotVehicleTakeoverRequest pilotVehicleTakeover;
        ObjectRailedTransportDockAttachCompletion railedTransportDockAttach;
        ObjectRailroadDisembarkRequest railroadDisembark;
        ObjectRailroadCarriageSpawnRequest railroadCarriageSpawn;
        ObjectSpawnSlaveRequest spawnSlave;
        ObjectSpecialPowerSpawnRequest specialPowerSpawn;
        ObjectBridgeStateEvent bridgeState;
        ObjectConstructionCompletionIntent constructionCompletion;
        BridgeRepairScaffoldBatchWork bridgeRepairScaffoldBatch;
        ObjectRebuildTargetRemapIntent rebuildTargetRemap;
        ObjectRebuildHoleExposeIntent rebuildHoleExpose;
        ObjectRebuildWorkerSpawnIntent rebuildWorkerSpawn;
        ObjectRebuildCompletionIntent rebuildCompletion;
        ObjectContainmentEvent containmentEvent;
        ObjectVehicleNeutralizationRequest vehicleNeutralization;
        CratePickupBatchWork cratePickupBatch;
        ObjectCountermeasureFlareSpawnCommand countermeasureFlareSpawn;
        ObjectProductionSpawnIntent productionSpawn;
        ObjectProductionUpgradeCompletionIntent productionUpgrade;
        ObjectSpecialAbilityEffectRequest specialAbilityEffect;
        ObjectSpecialPowerCompletionEvent specialPowerCompletion;
        size_t oclNuggetIndex = 0;
        container::SharedPtr<OclWorkState> oclState;
    };

    struct OclWorkPayload final {
        ObjectCreationListInvocation invocation;
        size_t nuggetIndex = 0;
        container::SharedPtr<OclWorkState> state;
    };

    struct WorkStorage final {
        container::Vector<ObjectSystemWeaponFireCommand> weapons;
        container::Vector<ObjectDamageRequest> damages;
        container::Vector<OclWorkPayload> ocls;
        container::Vector<ObjectCreateCrateDieEvent> crates;
        container::Vector<ObjectReplacementInvocation> replacements;
        container::Vector<ObjectUpgradeFxInvocation> upgradeFx;
        container::Vector<ObjectStructureEffectEvent> structureFx;
        container::Vector<ObjectTransitionDamageFxEvent> transitionOcls;
        container::Vector<ObjectInstantDeathEffectEvent> instantDeaths;
        container::Vector<ObjectSlowDeathPhaseEvent> slowDeaths;
        container::Vector<SlowDeathRubbleWork> slowDeathRubbles;
        container::Vector<ObjectTopplePathfindRemovalRequest> topplePathfind;
        container::Vector<ObjectToppleStumpSpawnRequest> toppleStumps;
        container::Vector<ObjectPhysicsCrashCommand> physicsCrashes;
        container::Vector<AIMovementObstructionBatchWork>
            aiMovementObstructionBatches;
        container::Vector<DestroyObjectWork> destroyObjects;
        container::Vector<ObjectMineSpawnCommand> mineSpawns;
        container::Vector<ObjectParticleUplinkRemnantSpawnRequest>
            particleUplinkRemnants;
        container::Vector<ObjectWaveGuideBridgeImpact> waveBridgeImpacts;
        container::Vector<ObjectCheckpointNavigationEvent>
            checkpointNavigation;
        container::Vector<ObjectTensileFormationEvent> tensileNavigation;
        container::Vector<ObjectDynamicGeometryGameplayEvent>
            dynamicGeometry;
        container::Vector<ObjectTransportGameplayTransaction> transports;
        container::Vector<ObjectDeathWalkState> deathWalks;
        container::Vector<ObjectDeleteWalkState> deleteWalks;
        container::Vector<ObjectBodyResumeState> bodyResumes;
        container::Vector<ObjectOwnershipChangeRequest> ownershipChanges;
        container::Vector<ObjectDefectionRequest> defections;
        container::Vector<ObjectPilotVehicleTakeoverRequest>
            pilotVehicleTakeovers;
        container::Vector<ObjectRailedTransportDockAttachCompletion>
            railedTransportDockAttaches;
        container::Vector<ObjectRailroadDisembarkRequest>
            railroadDisembarks;
        container::Vector<ObjectRailroadCarriageSpawnRequest>
            railroadCarriageSpawns;
        container::Vector<ObjectSpawnSlaveRequest> spawnSlaves;
        container::Vector<ObjectSpecialPowerSpawnRequest> specialPowerSpawns;
        container::Vector<ObjectBridgeStateEvent> bridgeStates;
        container::Vector<ObjectConstructionCompletionIntent>
            constructionCompletions;
        container::Vector<BridgeRepairScaffoldBatchWork>
            bridgeRepairScaffoldBatches;
        container::Vector<ObjectRebuildTargetRemapIntent>
            rebuildTargetRemaps;
        container::Vector<ObjectRebuildHoleExposeIntent> rebuildHoleExposes;
        container::Vector<ObjectRebuildWorkerSpawnIntent> rebuildWorkerSpawns;
        container::Vector<ObjectRebuildCompletionIntent> rebuildCompletions;
        container::Vector<ObjectContainmentEvent> containmentEvents;
        container::Vector<ObjectVehicleNeutralizationRequest>
            vehicleNeutralizations;
        container::Vector<CratePickupBatchWork> cratePickupBatches;
        container::Vector<ObjectCountermeasureFlareSpawnCommand>
            countermeasureFlareSpawns;
        container::Vector<ObjectProductionSpawnIntent> productionSpawns;
        container::Vector<ObjectProductionUpgradeCompletionIntent>
            productionUpgrades;
        container::Vector<ObjectSpecialAbilityEffectRequest>
            specialAbilityEffects;
        container::Vector<ObjectSpecialPowerCompletionEvent>
            specialPowerCompletions;

        [[nodiscard]] gameplay::GameplayTransactionToken store(
            gameplay::GameplayEnvelope envelope, WorkItem item);
        [[nodiscard]] WorkItem take(
            const gameplay::GameplayTransactionToken& token);
    };

    static bool commandOrder(
        const ObjectSystemWeaponFireCommand& left,
        const ObjectSystemWeaponFireCommand& right) noexcept;
    static uint64_t workSequence(const WorkItem& item) noexcept;
    static ObjectId workSource(const WorkItem& item) noexcept;
    [[nodiscard]] gameplay::GameplayEnvelope workEnvelope(
        const WorkItem& item) const noexcept;

    void appendCreateObjectWork(
        container::Vector<WorkItem>& output,
        ObjectCreateObjectDieEvent source);
    void pushCommands(
        container::Vector<ObjectSystemWeaponFireCommand> commands);
    void collectPendingWork(container::Vector<WorkItem>& output);
    void sortWork(container::Vector<WorkItem>& pending);
    void pushPendingWork(container::Vector<WorkItem> pending);
    [[nodiscard]] gameplay::GameplayTransactionToken storeWork(WorkItem item);
    void pushWork(WorkItem item);
    void drainToSize(size_t floor);
    [[nodiscard]] bool processOne(WorkItem item);
    [[nodiscard]] bool handleWeapon(WorkItem item);
    [[nodiscard]] bool handleWeaponImpact(WorkItem item);
    [[nodiscard]] bool handleDamage(WorkItem item);
    [[nodiscard]] bool handleOcl(WorkItem item);
    [[nodiscard]] bool handleCrate(WorkItem item);
    [[nodiscard]] bool handleReplacement(WorkItem item);
    [[nodiscard]] bool handleUpgradeFx(WorkItem item);
    [[nodiscard]] bool handleStructureFx(WorkItem item);
    [[nodiscard]] bool handleTransitionOcl(WorkItem item);
    [[nodiscard]] bool handleInstantDeath(WorkItem item);
    [[nodiscard]] bool handleSlowDeath(WorkItem item);
    [[nodiscard]] bool handleSlowDeathRubble(WorkItem item);
    [[nodiscard]] bool handleTopplePathfind(WorkItem item);
    [[nodiscard]] bool handleToppleStump(WorkItem item);
    [[nodiscard]] bool handlePhysicsCrash(WorkItem item);
    [[nodiscard]] bool handleAIMovementObstructionBatch(WorkItem item);
    [[nodiscard]] bool handleDestroyObject(WorkItem item);
    [[nodiscard]] bool handleMineSpawn(WorkItem item);
    [[nodiscard]] bool handleParticleUplinkRemnant(WorkItem item);
    [[nodiscard]] bool handleWaveBridgeImpact(WorkItem item);
    [[nodiscard]] bool handleCheckpointNavigation(WorkItem item);
    [[nodiscard]] bool handleTensileNavigation(WorkItem item);
    [[nodiscard]] bool handleDynamicGeometry(WorkItem item);
    [[nodiscard]] bool handleTransport(WorkItem item);
    [[nodiscard]] bool handleTransportTransaction(
        ObjectTransportPayloadStrafeTransaction event);
    [[nodiscard]] bool handleTransportTransaction(
        ObjectTransportPayloadWeaponTransaction event);
    [[nodiscard]] bool handleTransportTransaction(
        ObjectTransportPayloadFinishedTransaction event);
    [[nodiscard]] bool handleTransportTransaction(
        ObjectTransportOclTransaction event);
    [[nodiscard]] bool handleTransportTransaction(
        ObjectTransportWeaponAtPositionTransaction event);
    [[nodiscard]] bool handleTransportTransaction(
        ObjectTransportBunkerBustTransaction event);
    [[nodiscard]] bool handleTransportTransaction(
        ObjectTransportVeterancySyncTransaction event);
    [[nodiscard]] bool handleTransportTransaction(
        ObjectTransportHijackerReleaseTransaction event);
    [[nodiscard]] bool handleTransportTransaction(
        ObjectTransportBattleBusStartTransaction event);
    [[nodiscard]] bool handleTransportTransaction(
        ObjectTransportBattleBusLandedTransaction event);
    [[nodiscard]] bool handleTransportTransaction(
        ObjectTransportPayloadDropTransaction event);
    [[nodiscard]] bool handleTransportTransaction(
        ObjectTransportVisiblePayloadDropTransaction event);
    [[nodiscard]] bool handleTransportPayloadPlacement(
        ObjectTransportPayloadPlacementTransaction event,
        bool visiblePayload);
    void emitTransportObjectFx(
        ObjectId object, const container::String& fxList);
    [[nodiscard]] bool handleDeathWalk(WorkItem item);
    [[nodiscard]] bool handleDeleteWalk(WorkItem item);
    [[nodiscard]] bool handleBodyResume(WorkItem item);
    [[nodiscard]] bool handleOwnershipChange(WorkItem item);
    [[nodiscard]] bool handleDefection(WorkItem item);
    [[nodiscard]] bool handlePilotVehicleTakeover(WorkItem item);
    [[nodiscard]] bool handleRailedTransportDockAttach(WorkItem item);
    [[nodiscard]] bool handleRailroadDisembark(WorkItem item);
    [[nodiscard]] bool handleRailroadCarriageSpawn(WorkItem item);
    [[nodiscard]] bool handleSpawnSlave(WorkItem item);
    [[nodiscard]] bool handleSpecialPowerSpawn(WorkItem item);
    [[nodiscard]] bool handleBridgeState(WorkItem item);
    [[nodiscard]] bool handleConstructionCompletion(WorkItem item);
    [[nodiscard]] bool handleBridgeRepairScaffoldBatch(WorkItem item);
    [[nodiscard]] bool handleRebuildTargetRemap(WorkItem item);
    [[nodiscard]] bool handleRebuildHoleExpose(WorkItem item);
    [[nodiscard]] bool handleRebuildWorkerSpawn(WorkItem item);
    [[nodiscard]] bool handleRebuildCompletion(WorkItem item);
    [[nodiscard]] bool handleContainmentEvent(WorkItem item);
    [[nodiscard]] bool handleVehicleNeutralization(WorkItem item);
    [[nodiscard]] bool handleCratePickupBatch(WorkItem item);
    [[nodiscard]] bool handleCountermeasureFlareSpawn(WorkItem item);
    [[nodiscard]] bool handleProductionSpawn(WorkItem item);
    [[nodiscard]] bool handleProductionUpgrade(WorkItem item);
    [[nodiscard]] bool handleSpecialAbilityEffect(WorkItem item);
    [[nodiscard]] bool handleSpecialPowerCompletion(WorkItem item);
    void retargetStickyBombTargets(ObjectId from, ObjectId to);
    void discardPendingWork();
    void finalizeOcl(
        const ObjectCreationListInvocation& invocation,
        const container::SharedPtr<OclWorkState>& state);

    [[nodiscard]] bool processDamage(WorkItem item);
    void processUpgradeFx(const WorkItem& item);
    void processStructureFx(const WorkItem& item);
    [[nodiscard]] bool processTransitionOcl(const WorkItem& item);
    [[nodiscard]] bool processInstantDeath(const WorkItem& item);
    [[nodiscard]] bool processSlowDeath(const WorkItem& item);
    void processSlowDeathRubble(const WorkItem& item);
    void processTopplePathfind(const WorkItem& item);
    void processToppleStump(const WorkItem& item);
    void processPhysicsCrash(const WorkItem& item);
    void processDestroyObject(const WorkItem& item);
    void processMineSpawn(const WorkItem& item);
    void processParticleUplinkRemnant(const WorkItem& item);
    void processWaveBridgeImpact(const WorkItem& item);
    void appendDeathPayloadWork(
        container::Vector<WorkItem>& output, ObjectId object,
        const std::optional<container::String>& ocl,
        const std::optional<container::String>& weapon,
        uint32_t sourceSequence, uint32_t authoredOrder,
        uint64_t sourceEmissionSequence, uint64_t confirmedTick,
        std::optional<LogicFixedVec3> frozenPosition = std::nullopt,
        std::optional<ObjectPhysicsComponent::Scalar> frozenRotation =
            std::nullopt,
        PlayerId frozenOwner = INVALID_PLAYER_ID,
        uint32_t frozenSourcePathfindLayer = 0);
    [[nodiscard]] bool processReplacement(const WorkItem& item);
    void processCrate(const WorkItem& item);
    [[nodiscard]] bool processOcl(WorkItem item);
    void processOclApplyRandomForce(
        const WorkItem& item,
        const game::ObjectCreationApplyRandomForceNugget& nugget);
    void processOclFireWeapon(
        const WorkItem& item,
        const game::ObjectCreationListDefinition& definition,
        const game::ObjectCreationFireWeaponNugget& nugget);
    void processOclAttack(
        const WorkItem& item,
        const game::ObjectCreationListDefinition& definition,
        const game::ObjectCreationAttackNugget& nugget);
    void processOclDelivery(
        const WorkItem& item,
        const game::ObjectCreationListDefinition& definition,
        const game::ObjectCreationDeliverPayloadNugget& nugget);
    void processOclCreation(
        const WorkItem& item,
        const game::ObjectCreationNugget& nugget);
    void queueOclPhysics(
        const WorkItem& item, ObjectId target,
        ObjectPhysicsRequestKind kind, LogicFixedVec3 linear,
        uint32_t sequence,
        ObjectPhysicsComponent::Scalar yaw = {},
        ObjectPhysicsComponent::Scalar pitch = {},
        ObjectPhysicsComponent::Scalar roll = {});
    [[nodiscard]] LogicFixedVec3 randomOclForce(
        ObjectPhysicsComponent::Scalar minimumMagnitude,
        ObjectPhysicsComponent::Scalar maximumMagnitude,
        ObjectPhysicsComponent::Scalar minimumPitch,
        ObjectPhysicsComponent::Scalar maximumPitch);
    [[nodiscard]] bool processWeapon(WorkItem item);
    [[nodiscard]] bool processWeaponImpact(WorkItem item);

    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionObjectEventState& m_objectEvents;
    GameSessionObjectLifecycleTransactions m_lifecycle;
    GameSessionObjectOwnershipTransactions m_ownership;
    GameSessionGameplayPublicationPort m_publication;
    GameSessionFramePort m_frame;
    GameSessionNavigationTransactions m_navigation;
    GameSessionNavigationFootprintTransactions m_navigationFootprints;
    GameSessionObjectTargetRemapTransactions m_targetRemap;
    GameSessionWeaponEventPublisher m_weaponEvents;
    GameSessionProjectileSpawnTransactions m_projectiles;
    GameSessionBridgeLifecycleTransactions m_bridges;
    GameSessionDeletePostambleTransactions m_deletePostamble;
    GameSessionHealthEventPublisher m_healthEvents;
    container::Vector<gameplay::GameplayTransactionToken> m_work;
    WorkStorage m_storage;
    size_t m_processedTransactions = 0;
    size_t m_processedCommands = 0;
    size_t m_processedDamage = 0;
    size_t m_processedOclNuggets = 0;
    size_t m_createdOclObjects = 0;
    size_t m_processedReplacements = 0;
    size_t m_damageResolutionDepth = 0;
    size_t m_reservedStructuralTransactions = 0;
    ObjectId m_blockedProductionProducer = INVALID_OBJECT_ID;
    uint32_t m_blockedProductionId = 0;
};

} // namespace engine::detail
