#pragma once

// Private storage for ObjectSimulation.  Public users intentionally do not
// include this file: concrete systems and per-frame journals stay owned by the
// domain that produces them instead of leaking through ObjectSimulation.h.

#include "core/container/hash_containers.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/ai/contracts/AIStateServices.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/movement/ObjectAnimationSteering.h"
#include "game/object/simulation/economy/ObjectAutoDeposit.h"
#include "game/object/simulation/status/ObjectAutoHeal.h"
#include "game/object/simulation/status/ObjectBaseRegenerate.h"
#include "game/object/simulation/combat/ObjectBoneFx.h"
#include "game/object/simulation/structure/ObjectBridge.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/simulation/status/ObjectCheckpoint.h"
#include "game/object/simulation/lifecycle/ObjectCleanupHazard.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/combat/ObjectCountermeasures.h"
#include "game/object/simulation/status/ObjectCrateCollide.h"
#include "game/object/simulation/runtime/ObjectDeathEvents.h"
#include "game/object/simulation/lifecycle/ObjectDeleteWalk.h"
#include "game/object/simulation/movement/ObjectDynamicGeometry.h"
#include "game/object/simulation/world/ObjectDynamicShroud.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/simulation/status/ObjectEmpUpdate.h"
#include "game/object/simulation/status/ObjectEnemyNear.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/combat/ObjectFireUpdates.h"
#include "game/object/simulation/combat/ObjectFireWeaponBehavior.h"
#include "game/object/simulation/combat/ObjectFireWeaponCollide.h"
#include "game/object/simulation/combat/ObjectFireWeaponUpdate.h"
#include "game/object/simulation/movement/ObjectFloat.h"
#include "game/object/simulation/runtime/ObjectHealthEvents.h"
#include "game/object/simulation/lifecycle/ObjectHeightDie.h"
#include "game/object/simulation/combat/ObjectLeafletDrop.h"
#include "game/object/simulation/lifecycle/ObjectLifetime.h"
#include "game/object/simulation/structure/ObjectMinefield.h"
#include "game/object/simulation/structure/ObjectMissileLauncherBuilding.h"
#include "game/object/simulation/runtime/ObjectMovementEvents.h"
#include "game/object/simulation/combat/ObjectNeutronMissileSlowDeath.h"
#include "game/object/simulation/special/ObjectOclUpdate.h"
#include "game/object/simulation/structure/ObjectOvercharge.h"
#include "game/object/simulation/structure/ObjectParticleUplinkCannon.h"
#include "game/object/simulation/status/ObjectPoisoned.h"
#include "game/object/simulation/world/ObjectRadiusDecal.h"
#include "game/object/simulation/lifecycle/ObjectRebuildHole.h"
#include "game/object/simulation/runtime/ObjectSimulationPhysicsContracts.h"
#include "game/object/simulation/combat/ObjectSmartBomb.h"
#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/world/ObjectSpyVision.h"
#include "game/object/simulation/movement/ObjectSquishCollide.h"
#include "game/object/simulation/status/ObjectStealth.h"
#include "game/object/simulation/combat/ObjectStickyBomb.h"
#include "game/object/simulation/lifecycle/ObjectStructureDestruction.h"
#include "game/object/simulation/structure/ObjectSupplyWarehouseCrippling.h"
#include "game/object/simulation/combat/ObjectTactical.h"
#include "game/object/simulation/structure/ObjectTechBuilding.h"
#include "game/object/simulation/combat/ObjectTransitionDamageFx.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/object/simulation/movement/ObjectWaveGuide.h"
#include "game/object/simulation/combat/ObjectWeaponBonusUpdate.h"
#include "game/object/spatial/ObjectSpatialIndex.h"

#include <cstdint>
#include <optional>

namespace engine::object_simulation_detail {

struct QueuedDamageRequest final {
    ObjectDamageRequest request;
    ObjectHealthComponent::Scalar amount{};
    uint64_t submissionOrdinal = 0;
};

struct QueuedPhysicsRequest final {
    ObjectPhysicsRequest request;
    uint64_t submissionOrdinal = 0;
};

// Non-authoritative owner-thread scratch for the two serialized Physics
// passes. No reference or iterator may escape either pass; a worker-based
// implementation must replace this with one instance per worker.
struct ObjectPhysicsScratch final {
    struct Candidate final {
        ObjectId id = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };

    struct SweptContact final {
        Candidate left;
        Candidate right;
        math::q32_32 timeOfImpact{};
        ObjectCollisionContact response;
    };

    // One frame of accumulated overlap separation for objects whose
    // translation is owned by locomotion, which never consumes pendingForce.
    // Entries are kept sorted by ObjectId while the contact loop fills them so
    // the single apply pass writes in ObjectId order on every peer.
    struct Separation final {
        ObjectId id = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
        LogicFixedVec3 displacement{};
    };

    container::Vector<Candidate> candidates;
    container::Vector<SweptContact> sweptContacts;
    container::Vector<Separation> separations;
    container::HashSet<uint64_t> consideredPairs;
    container::Vector<ecs::entity> expiredCollisionIgnores;
    container::Vector<ObjectId> nearby;
    container::Vector<QueuedPhysicsRequest> readyRequests;

    void release() noexcept {
        container::Vector<Candidate>{}.swap(candidates);
        container::Vector<SweptContact>{}.swap(sweptContacts);
        container::Vector<Separation>{}.swap(separations);
        container::HashSet<uint64_t>{}.swap(consideredPairs);
        container::Vector<ecs::entity>{}.swap(expiredCollisionIgnores);
        container::Vector<ObjectId>{}.swap(nearby);
        container::Vector<QueuedPhysicsRequest>{}.swap(readyRequests);
    }
};

struct DamageState {
    container::Vector<QueuedDamageRequest> m_damageRequests;
    // Confirmed-barrier staging retains its high-water allocations across
    // re-entrant drains. Deferred requests are compacted in m_damageRequests
    // itself, so the producer queue never hands its allocation to a temporary.
    container::Vector<QueuedDamageRequest> m_readyDamageScratch;
    container::Vector<ObjectDamageTransactionIngress>
        m_readyDamageIngressScratch;
    // Non-authoritative typed scratch shared only by serialized producer
    // phases. Every user clears, fills and drains it before returning to the
    // GameSession damage barrier; it is never snapshotted or exposed.
    container::Vector<ObjectDamageRequest> m_damageScratch;
    bool m_resolvingSingleDamageTransaction = false;
    uint64_t m_nextBodyTransactionOrdinal = 1;
    container::Vector<ObjectHealthEvent> m_healthEvents;
    container::Vector<ObjectVehicleNeutralizationRequest>
        m_vehicleNeutralizationRequests;
    container::Vector<ObjectTransitionDamageFxEvent> m_transitionDamageFxEvents;
    container::Vector<ObjectTransitionDamageFxEvent>
        m_transitionDamageGameplayScratch;
    ObjectTransitionDamageFxSystem m_transitionDamageFx;
    ObjectSupplyWarehouseCripplingSystem m_supplyWarehouseCrippling;
};

struct MotionState {
    container::Vector<QueuedPhysicsRequest> m_physicsRequests;
    ObjectPhysicsScratch m_physicsScratch;
    container::Vector<ObjectMovementEvent> m_movementEvents;
    container::Vector<ai::MovementFeedback> m_aiMovementFeedback;
    container::Vector<ai::AIFacingFeedback> m_aiFacingFeedback;
    container::Vector<ObjectPhysicsEvent> m_physicsEvents;
    container::Vector<ObjectAIMovementObstructionEvent>
        m_aiMovementObstructionEvents;
    container::Vector<ObjectPhysicsCrashCommand> m_physicsCrashCommands;
    container::Vector<ObjectToppleStumpSpawnRequest>
        m_toppleStumpGameplayScratch;
    container::Vector<ObjectTopplePathfindRemovalRequest>
        m_topplePathfindGameplayScratch;
    // Navigation changes are gameplay and close immediately after the
    // SpawnSlave phase. Crack audio remains in the presentation stream.
    container::Vector<ObjectTensileFormationEvent>
        m_tensileNavigationEvents;
    container::Vector<ObjectTensileFormationEvent> m_tensileFormationEvents;
    ObjectSquishCollideSystem m_squishCollide;
    // One current-pose broad phase is shared by every post-kinematics contact
    // module. This is the ECS counterpart of RefCode PartitionManager
    // producing a contact list once before dispatching Object::onCollide().
    ObjectSpatialIndex m_contactIndex;
    ObjectAnimationSteeringSystem m_animationSteering;
    ObjectTacticalSystem m_tactical;
    ObjectFloatSystem m_float;
};

struct LifecycleState {
    container::Vector<ObjectDeleteWalkState> m_deleteWalks;
    container::Vector<ObjectDeletePostambleEvent> m_deletePostambleEvents;
    container::Vector<ObjectDeleteDestroyRequest> m_deleteDestroyRequests;
    container::Vector<ObjectDeathEvent> m_deathEvents;
    container::Vector<ObjectSpecialPowerCompletionEvent> m_specialPowerCompletionEvents;
    container::Vector<ObjectCrushDieEvent> m_crushDieEvents;
    container::Vector<ObjectInstantDeathEffectEvent> m_instantDeathEffectEvents;
    container::Vector<ObjectInstantDeathEffectEvent>
        m_instantDeathGameplayScratch;
    container::Vector<ObjectCreateObjectDieEvent> m_createObjectDieEvents;
    container::Vector<ObjectCreateCrateDieEvent> m_createCrateDieEvents;
    container::Vector<ObjectFxListDieEffectEvent> m_fxListDieEffectEvents;
    container::Vector<ObjectStructureEffectEvent> m_structureEffectEvents;
    container::Vector<ObjectBridgeDeathEffectRuntime> m_bridgeDeathEffects;
    container::Vector<ObjectHeightDiePresentationEvent> m_heightDiePresentationEvents;
    container::Vector<ObjectSlowDeathPhaseEvent> m_slowDeathPhaseEvents;
    container::Vector<ObjectSlowDeathPhaseEvent> m_slowDeathGameplayScratch;
    container::Vector<ObjectCreationListInvocation> m_objectCreationListInvocations;
    container::Vector<ObjectReplacementInvocation> m_objectReplacementInvocations;
    container::Vector<ObjectUpgradeFxInvocation> m_objectUpgradeFxInvocations;
    ObjectHeightDieSystem m_heightDie;
    ObjectLifetimeSystem m_lifetime;
    ObjectStructureDestructionSystem m_structureDestruction;
    ObjectOclUpdateSystem m_oclUpdate;
    std::optional<uint32_t> m_hulkLifetimeOverrideFrames;
    uint64_t m_sessionSeed = 0;
};

struct ProgressionState {
    container::Vector<ObjectExperienceEvent> m_experienceEvents;
    container::Vector<ObjectAutoDepositEvent> m_autoDepositEvents;
    container::Vector<ObjectSupplyEvent> m_supplyEvents;
    container::Vector<ObjectCratePickupCommand> m_cratePickupCommands;
    container::Vector<ObjectWeaponBonusUpdateEvent> m_weaponBonusUpdateEvents;
    ObjectAutoDepositSystem m_autoDeposit;
    ObjectCrateCollideSystem m_crateCollide;
    ObjectEconomySystem m_economy;
    ObjectExperienceSystem m_experience;
    ObjectUpgradeSystem m_upgrades;
    ObjectWeaponBonusUpdateSystem m_weaponBonusUpdate;
};

struct ConstructionState {
    container::Vector<ObjectBridgeStateEvent> m_bridgeStateEvents;
    container::Vector<ObjectRailedTransportDockAttachCompletion>
        m_railedTransportDockAttachCompletions;
    container::Vector<ObjectRailroadCarriageSpawnRequest> m_railroadCarriageSpawnRequests;
    container::Vector<ObjectRailroadDisembarkRequest>
        m_railroadDisembarkRequests;
    container::Vector<ObjectRailroadPresentationEvent>
        m_railroadPresentationEvents;
    container::Vector<ObjectSpawnSlaveRequest> m_spawnSlaveRequests;
    container::Vector<ObjectSlaveRepairPresentationEvent> m_slaveRepairPresentationEvents;
    container::Vector<ObjectConstructionCompletionIntent>
        m_completedObjectConstructions;
    container::Vector<ObjectBridgeRepairScaffoldIntent> m_bridgeRepairScaffoldIntents;
    container::Vector<ObjectRebuildHoleExposeIntent> m_rebuildExposeIntents;
    container::Vector<ObjectRebuildWorkerSpawnIntent> m_rebuildWorkerIntents;
    container::Vector<ObjectRebuildCompletionIntent> m_rebuildCompletionIntents;
    container::Vector<ObjectRebuildTargetRemapIntent> m_rebuildTargetRemapIntents;
    ObjectBuilderSystem m_builder;
    ObjectRebuildHoleSystem m_rebuildHole;
    ObjectBridgeSystem m_bridge;
    ObjectSpawnSlaveSystem m_spawnSlave;
};

struct AbilityState {
    container::Vector<ObjectSystemWeaponFireCommand> m_systemWeaponFireCommands;
    container::Vector<ObjectMineSpawnCommand> m_mineSpawnCommands;
    container::Vector<ObjectMinefieldFxEvent> m_minefieldFxEvents;
    container::Vector<ObjectStickyBombPresentationEvent> m_stickyBombPresentationEvents;
    container::Vector<ObjectNeutronMissilePresentationEvent> m_neutronMissilePresentationEvents;
    container::Vector<ObjectWaveGuideEvent> m_waveGuideEvents;
    container::Vector<ObjectWaveGuideBridgeImpact> m_waveGuideBridgeImpacts;
    container::Vector<ObjectMissileLauncherFxEvent> m_missileLauncherFxEvents;
    container::Vector<ObjectParticleUplinkPhaseEvent> m_particleUplinkPhaseEvents;
    container::Vector<ObjectParticleUplinkBeamEvent> m_particleUplinkBeamEvents;
    container::Vector<ObjectParticleUplinkScorchEvent> m_particleUplinkScorchEvents;
    container::Vector<ObjectParticleUplinkFxEvent> m_particleUplinkFxEvents;
    container::Vector<ObjectParticleUplinkRemnantSpawnRequest> m_particleUplinkRemnantSpawnRequests;
    container::Vector<ObjectFireAudioCommand> m_objectFireAudioCommands;
    container::Vector<ObjectEmpParticleEvent> m_empParticleEvents;
    container::Vector<ObjectAutoHealParticleEvent> m_autoHealParticleEvents;
    container::Vector<ObjectLeafletParticleEvent> m_leafletParticleEvents;
    container::Vector<ObjectStealthDetectorPulseEvent> m_stealthDetectorPulseEvents;
    container::Vector<ObjectGrantStealthPulseEvent> m_grantStealthPulseEvents;
    container::Vector<ObjectDynamicShroudDecalEvent> m_dynamicShroudDecalEvents;
    container::Vector<ObjectRadiusDecalEvent> m_radiusDecalEvents;
    container::Vector<ObjectTechBuildingEvent> m_techBuildingEvents;
    container::Vector<ObjectBeaconClientEvent> m_beaconClientEvents;
    container::Vector<ObjectCheckpointNavigationEvent> m_checkpointNavigationEvents;
    container::Vector<ObjectDynamicGeometryGameplayEvent> m_dynamicGeometryGameplayEvents;
    container::Vector<ObjectDynamicGeometryPresentationEvent> m_dynamicGeometryPresentationEvents;
    container::Vector<ObjectSpecialPowerExecutionEvent> m_specialPowerExecutionEvents;
    container::Vector<ObjectSpecialAbilityEffectRequest>
        m_specialAbilityEffectRequests;
    container::Vector<ObjectSpecialAbilityFacingRequest>
        m_specialAbilityFacingRequests;
    container::Vector<ObjectDefectionRequest> m_objectDefectionRequests;
    container::Vector<ObjectOwnershipChangeRequest> m_ownershipChangeRequests;
    container::Vector<ObjectPilotVehicleTakeoverRequest> m_pilotVehicleTakeoverRequests;
    container::Vector<ObjectSpecialPowerSpawnRequest> m_specialPowerSpawnRequests;
    container::Vector<ObjectCountermeasureFlareSpawnCommand>
        m_countermeasureFlareGameplayScratch;
    ObjectDynamicGeometrySystem m_dynamicGeometry;
    ObjectDynamicShroudSystem m_dynamicShroud;
    ObjectEnemyNearSystem m_enemyNear;
    ObjectCheckpointSystem m_checkpoint;
    ObjectCleanupHazardSystem m_cleanupHazard;
    ObjectMinefieldSystem m_minefield;
    ObjectNeutronMissileSlowDeathSystem m_neutronMissileSlowDeath;
    ObjectMissileLauncherBuildingSystem m_missileLauncherBuilding;
    ObjectParticleUplinkCannonSystem m_particleUplinkCannon;
    ObjectCountermeasuresSystem m_countermeasures;
    ObjectSmartBombSystem m_smartBomb;
    ObjectStickyBombSystem m_stickyBomb;
    ObjectWaveGuideSystem m_waveGuide;
    ObjectSpyVisionSystem m_spyVision;
    ObjectSpecialPowerSystem m_specialPower;
    ObjectAutoHealSystem m_autoHeal;
    ObjectBaseRegenerateSystem m_baseRegenerate;
    ObjectBoneFxSystem m_boneFx;
    ObjectFireWeaponBehaviorSystem m_fireWeaponBehaviors;
    ObjectFireWeaponCollideSystem m_fireWeaponCollide;
    ObjectFireWeaponUpdateSystem m_fireWeaponUpdate;
    ObjectFireUpdateSystem m_fireUpdates;
    ObjectEmpUpdateSystem m_empUpdate;
    ObjectLeafletDropSystem m_leafletDrop;
    ObjectOverchargeSystem m_overcharge;
    ObjectPoisonedSystem m_poisoned;
    ObjectRadiusDecalSystem m_radiusDecal;
    ObjectStealthSystem m_stealth;
    ObjectTechBuildingSystem m_techBuilding;
};

struct ContainmentState {
    container::Vector<ObjectContainmentEvent> m_containmentEvents;
    ObjectTransportEventStream m_transportEvents;
    ObjectContainmentSystem m_containment;
};

struct AirOperationsState {
    container::Vector<ObjectAirfieldEvent> m_airfieldEvents;
    container::Vector<ObjectAirfieldAutomaticProductionRequest>
        m_airfieldAutomaticProductionRequests;
    container::Vector<ObjectChinookRopePresentationEvent> m_chinookRopePresentationEvents;
    ObjectAirfieldSystem m_airfield;
};

struct ObjectSimulationState final : DamageState, MotionState, LifecycleState,
                                     ProgressionState, ConstructionState,
                                     AbilityState, ContainmentState,
                                     AirOperationsState {
    // One owner-thread admission clock for every authoritative object
    // reaction. Presentation entries may share the ordinal of their source
    // operation or consume gaps, but gameplay producers never compare
    // private Damage/Weapon/Physics/FX counters again.
    uint64_t m_nextGameplaySubmissionOrdinal = 1;
    ObjectSimulationRules m_rules;
};

} // namespace engine::object_simulation_detail
