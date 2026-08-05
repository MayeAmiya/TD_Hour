#pragma once

#include "core/container/container_types.h"

#include "core/ecs/registry.h"
#include "game/base/DamageTypes.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/combat/ObjectTactical.h"
#include "game/object/simulation/combat/ObjectBoneFx.h"
#include "game/object/simulation/lifecycle/ObjectCreate.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/simulation/lifecycle/ObjectDeathWalk.h"
#include "game/object/simulation/lifecycle/ObjectDeleteWalk.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/lifecycle/ObjectLifetime.h"
#include "game/object/simulation/world/ObjectRadiusDecal.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/combat/ObjectStickyBomb.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/object/simulation/runtime/ObjectSimulationPhysicsContracts.h"
#include "game/data/base/ObjectSimulationRules.h"
#include "game/object/simulation/runtime/ObjectSimulationDomains.h"

#include "game/object/contracts/ObjectLifecycle.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
namespace engine {

struct ObjectKinematicsPhaseState final {
    size_t physicsEventBegin = 0;
};

class ObjectSimulation;
namespace object_simulation_detail {
struct ObjectSimulationState;
class ObjectBodyReactionExecutor;
[[nodiscard]] ObjectSimulationState& state(ObjectSimulation&) noexcept;
[[nodiscard]] const ObjectSimulationState& state(const ObjectSimulation&) noexcept;
}

class GameContentSnapshot;
class SimulationRandom;
class ObjectOwnershipIndex;
class PlayerRegistry;
}

namespace game {
struct ThingTemplate;
namespace terrain {
class TerrainLogic;
}
} // namespace game

namespace engine {

// Deterministic, ObjectId-sorted ECS replacement for per-object Body and
// Locomotor virtual calls. Locomotor profiles are frozen into each object and
// selected by the current terrain surface; no confirmed tick follows mutable
// content-store pointers.
class ObjectSimulation final : public ObjectSimulationDamageDomain,
                               public ObjectSimulationMotionDomain,
                               public ObjectSimulationLifecycleDomain,
                               public ObjectSimulationProgressionDomain,
                               public ObjectSimulationConstructionDomain,
                               public ObjectSimulationAbilityDomain,
                               public ObjectSimulationContainmentDomain,
                               public ObjectSimulationAirOperationsDomain,
                               public ObjectUpgradeEffectSink {
public:
    ObjectSimulation();
    ~ObjectSimulation();
    ObjectSimulation(const ObjectSimulation&) = delete;
    ObjectSimulation& operator=(const ObjectSimulation&) = delete;
    ObjectSimulation(ObjectSimulation&&) noexcept;
    ObjectSimulation& operator=(ObjectSimulation&&) noexcept;

    void reset() noexcept;
    // Applied only when GameSession starts/replaces a match. Systems never
    // consult mutable GlobalData during a confirmed frame.
    void setRules(ObjectSimulationRules rules) noexcept;
    [[nodiscard]] const ObjectSimulationRules& rules() const noexcept;
    // A counter-based death PRF derives every choice from this frozen session
    // seed plus ObjectId/context; unlike a private mutable RNG it cannot be
    // perturbed by an unrelated ECS iteration or presentation effect.
    void setSessionSeed(uint64_t seed) noexcept;
    // Script-facing code may alter this session-owned value, but its effect
    // is deliberately sampled only when a HULK's LifetimeUpdate is spawned.
    // Values are already logic frames; nullopt is the legacy "-1/off" state.
    void setHulkLifetimeOverrideFrames(std::optional<uint32_t> frames) noexcept;
    [[nodiscard]] std::optional<uint32_t> hulkLifetimeOverrideFrames() const noexcept;

    // Called by GameSession's single spawn path before lifecycle events are
    // published. It copies immutable locomotor rules into the component, so a
    // running session does not retain a mutable content-store pointer. The
    // content argument is the session's sealed startup snapshot, never the
    // reloadable GameDataRegistry facade.
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          const game::ThingTemplate& templateData,
                          const GameContentSnapshot& content,
                          const game::terrain::TerrainLogic& terrain,
                          SimulationRandom* random = nullptr) const;
    // ExperienceTracker exists on every source Object even when it is not
    // trainable, because a victim's per-level ExperienceValue remains useful
    // to its killer. This runs before UpgradeMux so ExperienceScalarUpgrade
    // always sees a materialized deterministic component.
    void initializeExperience(ecs::registry& registry, ecs::entity entity,
                              const game::ThingTemplate& templateData,
                              uint64_t confirmedTick) const;

    // AutoHeal is assembled only after the immutable ObjectArchetype and
    // OwnerComponent exist, but before the Created lifecycle event is
    // published. Passing a value-only completed-upgrade span keeps the object
    // system independent of PlayerRegistry ownership and prevents any
    // per-tick technology polling.
    void initializeAutoHeal(ecs::registry& registry, ecs::entity entity,
                            const UpgradeMask& ownerCompletedUpgrades,
                            SimulationRandom& random, uint64_t confirmedTick) const;
    // FXListDie has sticky UpgradeMux activation state distinct from its
    // immutable DieMux recipe.  Assemble it after OwnerComponent exists so
    // an object spawned after a completed player upgrade gets the same death
    // behavior as an existing object receiving that upgrade transition.
    void initializeDeathReactionRuntime(
        ecs::registry& registry, ecs::entity entity,
        const UpgradeMask& ownerCompletedUpgrades) const;
    [[nodiscard]] bool hasSpecialPowerCompletionDie(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object) const noexcept;
    // Mirrors SpecialPowerCompletionDie::setCreator: only the first call on
    // the first matching module is accepted, including an explicit INVALID
    // creator used to suppress duplicate projectile/OCL completion.
    [[nodiscard]] bool setSpecialPowerCompletionCreator(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, ObjectId creator) const noexcept;
    // Weapon creation invokes notifyScriptEngine directly on a source which
    // already owns this Die module; death invokes the same value-event path.
    [[nodiscard]] bool notifySpecialPowerCompletion(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, uint64_t confirmedTick);
    // UpgradeMux storage is materialized before Create callbacks, but its
    // first general owner-fact evaluation is deliberately delayed until after
    // onCreate, matching RefCode Object::initObject().
    void materializeObjectUpgrades(
        ecs::registry& registry, ecs::entity entity) const;
    void activateInitialObjectUpgrades(
        ecs::registry& registry, ObjectLifecycle& lifecycle, ObjectId object,
        const UpgradeMask& ownerCompletedUpgrades,
        uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context = {}) const;
    void queueDamage(ObjectDamageRequest request);
    void queueCheckpointNavigationChange(
        ObjectId object, uint32_t authoredOrder,
        uint64_t confirmedTick);
    [[nodiscard]] bool resetObjectOclTimers(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, SimulationRandom& random,
        uint64_t confirmedTick) const;
    [[nodiscard]] bool setPlayerSpyVisionDisabledUntil(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        PlayerId player, uint64_t untilTick,
        uint64_t confirmedTick) const;
    [[nodiscard]] bool restartAllSpecialPowerRecharge(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, const GameContentSnapshot& content,
        uint64_t confirmedTick) const;
    void queuePhysicsRequest(ObjectPhysicsRequest request);
    [[nodiscard]] bool setPhysicsIgnoreCollisionWith(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, ObjectId ignored) const noexcept;
    // Resolves the currently admitted health transactions without advancing
    // locomotion.  GameSession uses this as a deterministic script-effect
    // barrier: a DAMAGE action must become visible before a later stamped
    // DELETE / Team expansion is applied, while ObjectSimulation remains the
    // sole Body/Health writer.
    void resolveQueuedDamage(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        uint64_t confirmedTick, ObjectUpgradeExecutionContext context = {},
        ObjectDamageTransactionResult* transactionResult = nullptr);
    [[nodiscard]] container::Vector<ObjectDamageTransactionIngress>
    takeReadyDamageTransactions(uint64_t confirmedTick);
    [[nodiscard]] ObjectSimulationEventLease<ObjectDamageTransactionIngress>
    leaseReadyDamageTransactions(uint64_t confirmedTick);
    [[nodiscard]] ObjectDamageTransactionResult executeDamageTransaction(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        ObjectDamageRequest request, uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context = {});
    [[nodiscard]] ObjectDeathWalkAdvance advanceDeathWalk(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        ObjectDeathWalkState& deathWalk,
        ObjectUpgradeExecutionContext context = {});
    [[nodiscard]] bool completeDeathWalk(ObjectDeathWalkState deathWalk);
    [[nodiscard]] bool resumeDamageTransaction(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const ObjectBodyResumeState& bodyResume,
        ObjectUpgradeExecutionContext context = {});
    [[nodiscard]] container::Vector<ObjectOwnershipChangeRequest>
    takeOwnershipChangeRequests();
    [[nodiscard]] ObjectSimulationEventLease<ObjectOwnershipChangeRequest>
    leaseOwnershipChangeRequests();
    void discardQueuedDamageTransactions() noexcept;
    // Clears every confirmed-gameplay producer queue without transferring its
    // backing storage. Split gameplay/presentation records retain only their
    // presentation payloads.
    void discardConfirmedGameplayEvents() noexcept;
    // WeaponBonusUpdate is sampled before CombatSystem so a pulse affects
    // this confirmed frame's shots. ObjectSimulation::update calls the same
    // idempotent boundary for focused users that do not own a combat phase.
    void updateWeaponBonuses(ecs::registry& registry,
                             ObjectLifecycle& lifecycle,
                             const PlayerRegistry& players,
                             const GameContentSnapshot& content,
                             SimulationRandom& random,
                             uint64_t confirmedTick);
    void updateRadarProviders(
        const ecs::registry& registry,
        const ObjectLifecycle& lifecycle,
        PlayerRegistry& players,
        uint64_t confirmedTick) const;
    // GameSession invokes this before Combat. The phase only produces typed
    // damage/status work; the Session transaction owner must drain it before
    // Combat or any later phase samples Disabled/effectivelyDead state.
    void updatePreCombatStatusEffects(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const PlayerRegistry& players, SimulationRandom& random,
        uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context = {});
    // Session-driven prelude of the object frame. Each damage-producing
    // method only appends typed gameplay work; Session must drain immediately
    // before invoking the next method. Poison finish deliberately follows its
    // drain because a lethal final pulse retains the authored poison state.
    void updateFrameAdmissionPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        uint64_t confirmedTick, ObjectUpgradeExecutionContext context = {});
    [[nodiscard]] bool updateNeutronSlowDeathPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context,
        container::Vector<ObjectDamageRequest>& outDamage);
    void updateMinefieldHazardPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context = {});
    void updateDynamicGeometryHazardPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain, uint64_t confirmedTick);
    void updateFlammableHazardPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        uint64_t confirmedTick);
    void updatePoisonHazardPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        uint64_t confirmedTick);
    void finishPoisonHazardPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        uint64_t confirmedTick);
    void updateOverchargeHazardPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        uint64_t confirmedTick);
    void updateOrdersAndTacticalPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context = {});
    [[nodiscard]] bool executeSpecialAbilityEffect(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        ObjectSpecialAbilityEffectRequest effect,
        ObjectUpgradeExecutionContext context = {});
    [[nodiscard]] bool acceptsSpecialAbilityFacingRequest(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectSpecialAbilityFacingRequest& request) const;
    [[nodiscard]] bool acknowledgeSpecialAbilityFacingRequest(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectSpecialAbilityFacingRequest& request, bool accepted,
        bool terminalFailure, uint64_t requestIssuedTick,
        uint32_t requestSequence);
    [[nodiscard]] bool acknowledgeSpecialAbilityFacingFeedback(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId source, uint64_t requestIssuedTick,
        uint32_t requestSequence, bool completed);
    [[nodiscard]] bool commitRailedTransportDockAttachCompletion(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const ObjectRailedTransportDockAttachCompletion& completion);
    void acknowledgeRailedTransportDockAttachCompletion(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const ObjectRailedTransportDockAttachCompletion& completion,
        bool accepted);
    [[nodiscard]] bool executeRailroadDisembark(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const ObjectRailroadDisembarkRequest& request);
    void updateParticleUplinkPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context = {});
    void updateDynamicSightPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        uint64_t confirmedTick, ObjectUpgradeExecutionContext context = {});
    void updateAirOperationsPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context = {});
    void updateSupplyEconomyPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        uint64_t confirmedTick, ObjectUpgradeExecutionContext context = {});
    void updateBaseRegenerationPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        uint64_t confirmedTick);
    void updateAutoHealPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        uint64_t confirmedTick, ObjectUpgradeExecutionContext context = {});
    void updateConstructionRepairPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        uint64_t confirmedTick, ObjectUpgradeExecutionContext context = {});
    void updateRebuildRecoveryPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        uint64_t confirmedTick);
    void updateWarehouseRecoveryPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        uint64_t confirmedTick);
    void prepareLifecyclePhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context,
        container::Vector<ObjectLifetimeCommand>& commands);
    [[nodiscard]] bool applyLifetimeCommand(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const ObjectLifetimeCommand& command, uint64_t confirmedTick);
    void finishLifecyclePhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain, uint64_t confirmedTick);
    [[nodiscard]] bool reportIncomingSmallMissile(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId victim, ObjectId projectile, SimulationRandom& random,
        uint64_t confirmedTick);
    void updateCountermeasures(ecs::registry& registry,
                               const ObjectLifecycle& lifecycle,
                               uint64_t confirmedTick);
    void acknowledgeCountermeasureFlareSpawn(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId source, uint32_t ruleIndex, ObjectId flare, bool created,
        uint64_t confirmedTick);
    void resolveCountermeasureDiversions(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        uint64_t confirmedTick);
    [[nodiscard]] bool reloadObjectCountermeasures(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, uint64_t confirmedTick);
    [[nodiscard]] bool setSmartBombTarget(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, const LogicFixedVec3& target) const;
    // SpecialAbility/script adapters attach and detonate by stable ObjectId;
    // no caller receives an EnTT entity or mutable module pointer.
    [[nodiscard]] bool attachStickyBomb(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const game::terrain::TerrainLogic& terrain,
        const ObjectStickyBombAttachRequest& request);
    [[nodiscard]] bool acknowledgeSpecialPowerSpawn(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectSpecialPowerSpawnRequest& request, ObjectId spawned,
        bool accepted);
    [[nodiscard]] bool retargetStickyBomb(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId bomb, ObjectId target) const;
    [[nodiscard]] bool detonateStickyBomb(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const GameContentSnapshot& content, ObjectId bomb,
        sticky_bomb::DetonationTrigger trigger, uint64_t confirmedTick);
    [[nodiscard]] std::optional<ObjectStickyBombState> stickyBombState(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId bomb) const noexcept;
    // Value-only bridge for legacy producers that alter a spawned helper's
    // LifetimeUpdate/DeletionUpdate timer. Inputs are logic frames, not
    // milliseconds; the immutable template plan remains untouched.
    [[nodiscard]] bool rescheduleLifetime(ecs::registry& registry,
                                          const ObjectLifecycle& lifecycle,
                                          const ObjectLifetimeRescheduleRequest& request) const;
    // Future OCL Disposition=FLOATING and object-creation paths use this
    // value-only boundary instead of finding a mutable legacy UpdateModule.
    [[nodiscard]] bool setFloatEnabled(ecs::registry& registry,
                                       const ObjectLifecycle& lifecycle,
                                       const ObjectFloatEnableRequest& request) const;
    // Terminal structure motion uses the original stopAllBoneFX semantic
    // without finding or retaining a mutable legacy UpdateModule.
    [[nodiscard]] bool stopAllBoneFx(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectBoneFxStopRequest& request);
    [[nodiscard]] bool markObjectDetected(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, uint32_t frames, uint64_t confirmedTick) const;
    [[nodiscard]] bool grantObjectStealth(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, bool active, uint32_t frames,
        uint64_t confirmedTick) const;
    // BattlePlanUpdate and scripted ability adapters toggle a detector by
    // stable ID; callers never find a mutable legacy UpdateModule.
    [[nodiscard]] bool setStealthDetectorEnabled(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, bool enabled, uint64_t confirmedTick) const;
    [[nodiscard]] bool setCommandButtonHunt(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, container::String commandButton,
        uint64_t confirmedTick) const;
    [[nodiscard]] bool setWanderInPlace(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, uint64_t confirmedTick) const;
    [[nodiscard]] bool setOverchargeActive(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, bool active, uint64_t confirmedTick) const;
    [[nodiscard]] bool toggleOvercharge(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, uint64_t confirmedTick) const;
    [[nodiscard]] bool isOverchargeActive(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object) const noexcept;
    [[nodiscard]] bool createRadiusDecal(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectRadiusDecalRequest& request);
    [[nodiscard]] bool killRadiusDecal(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, uint64_t confirmedTick);
    void onObjectDestroyRequested(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        ObjectId object, uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context = {});
    [[nodiscard]] ObjectDeleteWalkAdvance advanceDeleteWalk(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        ObjectDeleteWalkState& deleteWalk,
        ObjectUpgradeExecutionContext context = {});
    [[nodiscard]] bool completeDeleteWalk(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        ObjectDeleteWalkState deleteWalk,
        ObjectUpgradeExecutionContext context = {});
    // Read the fixed deadline of an armed LifetimeUpdate/DeletionUpdate
    // timer without exposing an ECS entity. This is the future StickyBomb
    // dependency point corresponding to legacy getDieFrame().
    [[nodiscard]] std::optional<uint64_t> lifetimeDueTick(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, game::ObjectLifetimeAction action,
        std::optional<uint32_t> authoredOrder = std::nullopt) const;
    void updateKinematicsPreludePhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context = {});
    [[nodiscard]] ObjectKinematicsPhaseState updateKinematicsMotionPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context = {},
        container::Span<const ObjectId> aiMoveStopOwners = {},
        container::Span<const ObjectId> aiAttackOwners = {},
        container::Span<const ObjectAIMovementCommand>
            aiMovementCommands = {},
        container::Span<const ai::AIStateCommand>
            aiFacingCommands = {});
    void finishKinematicsPostDamagePhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
        ObjectKinematicsPhaseState phase);
    void updateBridgeRailPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain, uint64_t confirmedTick);
    void updateSpawnSlavePhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context = {});
    void prepareKinematicsCollisionPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        ObjectUpgradeExecutionContext context = {});
    void updateSquishCollisionPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context = {});
    void finishKinematicsCollisionPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context = {});

    void queueSystemWeaponFireCommand(ObjectSystemWeaponFireCommand command);
    void queueObjectCreationListInvocation(
        ObjectCreationListInvocation invocation) override;
    void queueObjectReplacementInvocation(
        ObjectReplacementInvocation invocation) override;
    void queueObjectUpgradeFxInvocation(
        ObjectUpgradeFxInvocation invocation) override;
    [[nodiscard]] uint64_t
    reserveGameplaySubmissionOrdinal() noexcept override;

private:
    friend class ObjectSimulationProgressionDomain;
    friend class object_simulation_detail::ObjectBodyReactionExecutor;
    [[nodiscard]] bool applyBodyStateProjection(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectBodyStateProjection& projection);
    [[nodiscard]] bool applyBodyHealthProjection(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectBodyHealthProjection& projection);
    void resolveQueuedPhysics(ecs::registry& registry, ObjectLifecycle& lifecycle,
                              uint64_t confirmedTick);
    void updateKinematicsFloats(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain, uint64_t confirmedTick);
    void updateKinematicsContainment(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context);
    void queueKinematicsHeightDeaths(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain, uint64_t confirmedTick);
    void finishKinematicsCrateCollisions(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context);
    void finishKinematicsFireWeaponCollisions(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        uint64_t confirmedTick, ObjectUpgradeExecutionContext context);
    void updateKinematicsAnimationSteering(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        uint64_t confirmedTick);
    void updateKinematicsSmartBombs(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain, uint64_t confirmedTick);
    void updateKinematicsStickyBombs(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain, uint64_t confirmedTick);
    void updateWaveGuideKinematicsPhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context);
    void finalizeExperienceMutation(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const ObjectExperienceMutation& mutation, ObjectId source,
        const UpgradeMask& ownerCompletedUpgrades,
        uint64_t confirmedTick, ObjectUpgradeExecutionContext context,
        bool provideFeedback = true);
    [[nodiscard]] ObjectCreateExecutionReport executeObjectCreatePhase(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const ObjectOwnershipIndex& ownership, ObjectId object,
        ObjectCreatePhase phase,
        const UpgradeMask& ownerCompletedUpgrades,
        uint64_t confirmedTick,
        ObjectUpgradeExecutionContext context);

    friend object_simulation_detail::ObjectSimulationState&
        object_simulation_detail::state(ObjectSimulation&) noexcept;
    friend const object_simulation_detail::ObjectSimulationState&
        object_simulation_detail::state(const ObjectSimulation&) noexcept;
    std::unique_ptr<object_simulation_detail::ObjectSimulationState> m_state;
};

} // namespace engine
