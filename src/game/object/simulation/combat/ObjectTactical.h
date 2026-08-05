#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/object/definition/CombatProfile.h"
#include "game/object/definition/ObjectKindOf.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/status/ObjectAutoHeal.h"
#include "game/object/simulation/runtime/ObjectToppleTransaction.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "core/ecs/ObjectId.h"
#include "game/data/base/SpecialPowerCatalog.h"
#include "game/data/base/SpecialPowerType.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

#include "game/object/plan/combat/ObjectTacticalPlanTypes.h"

namespace engine::navigation {
class NavigationSystem;
}

namespace game::terrain {
class TerrainLogic;
}

namespace engine {

class GameContentSnapshot;
class ObjectLifecycle;
class PlayerRegistry;
class SimulationRandom;
struct ObjectDamageRequest;
struct ObjectDefectionRequest;
struct ObjectSimulationRules;
struct SpecialPowerDefinition;

struct ObjectPropagandaTowerRuntime final {
    uint64_t nextScanTick = 0;
    // RefCode refreshes the objects inside the aura only on
    // DelayBetweenUpdates, then applies healing/weapon bonuses to that saved
    // roster every logic frame.  Keeping stable ObjectIds here preserves that
    // distinction without retaining ECS entity handles or Object pointers.
    container::Vector<ObjectId> members;
};

enum class ObjectDeployStyleState : uint8_t {
    ReadyToMove,
    Deploying,
    ReadyToAttack,
    Undeploying,
    // DeployStyleAIUpdate's ALIGNING_TURRETS. Reached only when
    // TurretsMustCenterBeforePacking is authored: the unit stays deployed and
    // able to fire while the barrel recentres, and only then starts packing.
    AligningTurrets,
};

struct ObjectDeployStyleRuntime final {
    ObjectDeployStyleState state = ObjectDeployStyleState::ReadyToMove;
    uint64_t transitionEndTick = 0;
};

// Detached presentation intent matching Drawable::setAnimationFrame.  The
// deploy state machine owns the confirmed frame number, while GameSession
// applies it to the game-owned VisualAnimationState before snapshot
// extraction.  No renderer or clip metadata crosses into simulation.
struct ObjectDeployStyleManualFrameEvent final {
    ObjectId object = INVALID_OBJECT_ID;
    uint32_t frame = 0;
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectToppleRuntime final {
    bool active = false;
    bool finished = false;
    bool startKillIssued = false;
    bool noBounce = false;
    bool noFx = false;
    math::q32_32 angularVelocity{};
    math::q32_32 angularAcceleration{};
    math::q32_32 angularAccumulation{};
    math::q32_32 yawDelta{};
    uint32_t yawStepsRemaining = 0;
    LogicFixedVec3 direction{};
};

// Detached, confirmed ToppleUpdate outputs.  The simulation owns the exact
// trigger tick; GameSession resolves presentation and object creation without
// exposing renderer/factory handles back to the ECS state machine.
struct ObjectToppleFxEvent final {
    ObjectId object = INVALID_OBJECT_ID;
    LogicFixedVec3 position{};
    math::q32_32 yawRadians{};
    container::String fxList;
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectToppleStumpSpawnRequest final {
    ObjectId source = INVALID_OBJECT_ID;
    container::String objectTemplate;
    LogicFixedVec3 position{};
    math::q32_32 yawRadians{};
    uint32_t ruleIndex = 0;
    uint32_t authoredOrder = 0;
    uint64_t emissionSequence = 0;
    uint64_t confirmedTick = 0;
    bool burned = false;
};

struct ObjectToppleStumpOwnerComponent final {
    ObjectId source = INVALID_OBJECT_ID;
    uint32_t ruleIndex = 0;
};

struct ObjectTopplePathfindRemovalRequest final {
    ObjectId object = INVALID_OBJECT_ID;
    uint64_t emissionSequence = 0;
    uint64_t confirmedTick = 0;
};

enum class ObjectBattlePlanTransition : uint8_t {
    Idle,
    Unpacking,
    Active,
    Packing,
};

struct ObjectBattlePlanRuntime final {
    // Resolved once at initializeObject; activate/admit match by id only.
    SpecialPowerContentId specialPower = INVALID_SPECIAL_POWER_CONTENT_ID;
    game::ObjectBattlePlanStatus current = game::ObjectBattlePlanStatus::None;
    game::ObjectBattlePlanStatus desired = game::ObjectBattlePlanStatus::None;
    ObjectBattlePlanTransition transition = ObjectBattlePlanTransition::Idle;
    uint64_t nextTransitionTick = 0;
};

enum class ObjectBattlePlanPresentationPhase : uint8_t {
    Unpacking,
    Active,
    Packing,
};

// Detached presentation edge emitted only after the confirmed transition.
// Strings are already resolved by the frozen BattlePlan rule; audio/UI never
// reads ModuleData or feeds a result back into the state machine.
struct ObjectBattlePlanPresentationEvent final {
    ObjectBattlePlanPresentationPhase phase =
        ObjectBattlePlanPresentationPhase::Unpacking;
    ObjectId source = INVALID_OBJECT_ID;
    PlayerId owner = INVALID_PLAYER_ID;
    game::ObjectBattlePlanStatus plan =
        game::ObjectBattlePlanStatus::None;
    container::String transitionSound;
    container::String idleLoopSound;
    container::String announcement;
    container::String messageLabel;
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

enum class ObjectSpecialAbilityPhase : uint8_t {
    Inactive,
    Facing,
    Unpacking,
    Preparing,
    Packing,
};

struct ObjectSpecialAbilityObject final {
    ObjectId object = INVALID_OBJECT_ID;
    ObjectId target = INVALID_OBJECT_ID;
    uint64_t spawnSequence = 0;
};

struct ObjectSpecialAbilityRuntime final {
    // Resolved once at initializeObject from the frozen rule template name.
    // Hot paths match SpecialPowerDefinition::id against this field only.
    SpecialPowerContentId specialPower = INVALID_SPECIAL_POWER_CONTENT_ID;
    bool active = false;
    bool effectTriggered = false;
    // SpecialPowerModule::UpdateModuleStartsAttack defers the first recharge
    // until the update has actually reached its preparation phase.  Keeping
    // this on the update runtime also makes abort-during-approach/unpack a
    // recharge-free cancellation, as in RefCode.
    bool deferredRechargePending = false;
    bool hasTargetPosition = false;
    bool noTargetCommand = false;
    bool preTriggerRevealApplied = false;
    // RefCode creates LaserUpdate-backed helper objects exactly once when
    // startPreparation() is entered.  The spawn completes later through the
    // deterministic gameplay drain, so this latch prevents the preparation
    // loop from submitting the same helper every confirmed tick.
    bool preparationObjectAttempted = false;
    bool boobyTrapTriggered = false;
    bool fleeAfterPacking = false;
    bool facingRequestQueued = false;
    bool facingStateActive = false;
    bool facingComplete = false;
    bool facingFailed = false;
    ObjectId target = INVALID_OBJECT_ID;
    LogicFixedVec3 targetPosition{};
    game::SpecialPowerType specialPowerType =
        game::SpecialPowerType::Invalid;
    ObjectSpecialAbilityPhase phase = ObjectSpecialAbilityPhase::Inactive;
    uint64_t phaseEndTick = 0;
    uint64_t triggerTick = 0;
    uint64_t finishTick = 0;
    uint64_t activationTick = 0;
    uint64_t activationSequence = 0;
    uint64_t facingRequestIssuedTick = 0;
    uint32_t facingRequestSequence = 0;
    // SpecialAbilityUpdate exits as soon as AIUpdate reports that a player or
    // script command replaced its AI-owned activity.  The confirmed command
    // queue exposes that edge as an external ingress revision; retaining the
    // activation stamp avoids inspecting transient AI implementation state.
    uint64_t observedExternalRevision = 0;
    // Fixed event scheduler for SpecialAbilityUpdate::DoCaptureFX. The
    // resulting flash remains presentation-owned.
    math::q32_32 captureFlashPhase{};
    container::Vector<ObjectSpecialAbilityObject> specialObjects;
};

enum class ObjectTacticalPresentationEventKind : uint8_t {
    CapturePulse,
    CaptureCompleted,
    DeployStarted,
    UndeployStarted,
    // A completed non-capture SpecialAbilityUpdate effect.  The audio/UI
    // consumer distinguishes its authored SpecialPowerType; simulation emits
    // no sound handles and does not consult presentation state.
    SpecialAbilityCompleted,
    AssistedTargetingLasers,
    // PropagandaTowerBehavior calls FXList::doFXObj on every authored scan.
    // This is presentation-only; the deterministic roster/healing remains
    // in ObjectTacticalSystem.
    PropagandaPulse,
};

struct ObjectTacticalPresentationEvent final {
    ObjectTacticalPresentationEventKind kind =
        ObjectTacticalPresentationEventKind::CapturePulse;
    ObjectId source = INVALID_OBJECT_ID;
    ObjectId assisted = INVALID_OBJECT_ID;
    ObjectId target = INVALID_OBJECT_ID;
    container::String primaryResource;
    container::String secondaryResource;
    container::String fxList;
    game::SpecialPowerType specialPowerType =
        game::SpecialPowerType::Invalid;
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

enum class ObjectSpecialAbilityAdmissionStatus : uint8_t {
    Ready,
    Approach,
    Rejected,
};

struct ObjectSpecialAbilityAdmission final {
    ObjectSpecialAbilityAdmissionStatus status =
        ObjectSpecialAbilityAdmissionStatus::Rejected;
    LogicFixedVec3 approachPosition{};
    uint32_t ruleIndex = 0;
    bool supportsDeferredRecharge = false;
};

struct ObjectSpecialAbilityFacingRequest final {
    ObjectId source = INVALID_OBJECT_ID;
    ObjectId target = INVALID_OBJECT_ID;
    LogicFixedVec3 targetPosition{};
    bool hasTargetPosition = false;
    uint32_t ruleIndex = 0;
    uint64_t activationSequence = 0;
    uint64_t confirmedTick = 0;
};

enum class ObjectSpecialAbilityEffectKind : uint8_t {
    SpawnSpecialObject,
    DetonateSpecialObjects,
    TriggerTargetBoobyTrap,
    DestroySpecialObjects,
    DisguiseAsTarget,
    MarkDetected,
    AwardExperience,
    RestartRecharge,
};

struct ObjectSpecialAbilityEffectRequest final {
    ObjectSpecialAbilityEffectKind kind =
        ObjectSpecialAbilityEffectKind::SpawnSpecialObject;
    ObjectId source = INVALID_OBJECT_ID;
    ObjectId target = INVALID_OBJECT_ID;
    // Content identity for simulation consumers (recharge, execution events).
    SpecialPowerContentId specialPower = INVALID_SPECIAL_POWER_CONTENT_ID;
    // Authored template name retained for diagnostics / presentation only.
    container::String specialPowerTemplate;
    game::SpecialPowerType specialPowerType =
        game::SpecialPowerType::Invalid;
    container::String objectTemplate;
    container::String attachToBone;
    LogicFixedVec3 position{};
    container::Vector<ObjectId> objects;
    bool hasPosition = false;
    // SpecialAbility helpers are not uniformly StickyBombs.  Capture/hack
    // lasers are ordinary client-backed helper objects, while TNT/C4/booby
    // trap objects must initialize StickyBombUpdate after spawning.
    bool attachStickyBomb = false;
    uint32_t ruleIndex = 0;
    uint32_t authoredOrder = 0;
    uint64_t activationSequence = 0;
    int32_t value = 0;
    bool marksSpecialPowerTriggered = false;
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectCommandButtonHuntRuntime final {
    container::String commandButton;
    uint64_t nextScanTick = 0;
    // CommandButtonHuntUpdate permanently sleeps after a player/script
    // command replaces its AI-owned command.  Keep the queue's explicit
    // ingress revision instead of trying to infer that event from an empty
    // queue after the replacement order has completed.
    uint64_t observedExternalRevision = 0;
};

struct ObjectWanderRuntime final {};

struct ObjectTacticalComponent final {
    container::SharedPtr<const game::ObjectTacticalPlan> plan;
    container::Vector<ObjectPropagandaTowerRuntime> propagandaTowers;
    container::Vector<ObjectDeployStyleRuntime> deployStyles;
    container::Vector<ObjectToppleRuntime> topple;
    container::Vector<ObjectBattlePlanRuntime> battlePlans;
    container::Vector<ObjectSpecialAbilityRuntime> specialAbilities;
    container::Vector<ObjectCommandButtonHuntRuntime> commandButtonHunts;
    container::Vector<ObjectWanderRuntime> wander;
};

// Derived per-object projection of the currently active player battle plan.
// Damage and Combat consume this value without following a strategy-center
// entity or retaining a legacy Player::BattlePlanBonuses pointer.
struct ObjectBattlePlanEffectComponent final {
    game::WeaponBonusConditionMask weaponConditions = 0;
    math::q32_32 armorDamageScalar{int32_t{1}};
    math::q32_32 sightRangeScalar{int32_t{1}};
    uint64_t paralyzedUntilTick = 0;
    uint64_t revision = 0;
};

using ObjectPropagandaBenefactorComponent =
    ObjectSoleHealingBenefactorComponent;

class ObjectTacticalSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          const GameContentSnapshot& content,
                          const ObjectSimulationRules& rules,
                          uint64_t confirmedTick) const;

    [[nodiscard]] bool activateSpecialAbility(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, const SpecialPowerDefinition& definition,
        const ObjectOrderIntent& order, const GameContentSnapshot& content,
        const ObjectSimulationRules& rules,
        uint64_t confirmedTick,
        uint64_t& nextGameplaySubmissionOrdinal,
        container::Vector<ObjectSpecialAbilityEffectRequest>& effectRequests,
        PlayerRegistry* players = nullptr,
        SimulationRandom* random = nullptr,
        bool deferRechargeUntilPreparation = false) const;

    // Admission is intentionally evaluated before SpecialPower consumes its
    // recharge.  An out-of-range command is converted into a deterministic
    // system Move followed by the original SpecialPower order; it is not a
    // failed activation and therefore must not start the timer early.
    // RefCode's ApproachRequiresLOS installs PartitionFilterLineOfSight, which
    // is isClearLineOfSightTerrain() plus
    // Pathfinder::isViewBlockedByObstacle(). Only the obstacle half exists in
    // this project; `visibility` remains the stand-in for the terrain half.
    [[nodiscard]] ObjectSpecialAbilityAdmission admitSpecialAbility(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, const SpecialPowerDefinition& definition,
        const GameContentSnapshot& content, const ObjectOrderIntent& order,
        const game::terrain::MapVisibilitySnapshot* visibility,
        const navigation::NavigationSystem* navigation = nullptr,
        bool attackUsesLineOfSight = true,
        container::Span<const uint64_t> seeThroughObstacles = {}) const;

    [[nodiscard]] bool acknowledgeSpecialObjectSpawn(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId owner, uint32_t ruleIndex, uint64_t activationSequence,
        ObjectId target, uint64_t spawnSequence, ObjectId spawned,
        bool accepted) const;

    [[nodiscard]] bool acceptsSpecialAbilityFacingRequest(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectSpecialAbilityFacingRequest& request) const;
    [[nodiscard]] bool acknowledgeSpecialAbilityFacingRequest(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectSpecialAbilityFacingRequest& request, bool accepted,
        bool terminalFailure, uint64_t requestIssuedTick,
        uint32_t requestSequence) const;
    [[nodiscard]] bool acknowledgeSpecialAbilityFacingFeedback(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId source, uint64_t requestIssuedTick,
        uint32_t requestSequence, bool completed) const;

    [[nodiscard]] bool onBattlePlanDelete(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, uint32_t authoredOrder,
        const ObjectSimulationRules& rules,
        ObjectUpgradeExecutionContext context,
        uint64_t confirmedTick) const;
    // SpecialAbilityUpdate performs onExit(true) from module destruction,
    // after authored onDelete callbacks. Keep this separate from the plan so
    // it runs at the DeleteWalk postamble rather than masquerading as one.
    void onSpecialAbilityReclaim(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, uint64_t confirmedTick,
        container::Vector<ObjectDamageRequest>& damageRequests) const;

    // PropagandaTowerBehavior::onDie removes this exact source's saved aura
    // roster at its authored DeathWalk position.  Remaining live towers are
    // folded immediately so a later Behavior never observes stale healing or
    // weapon-bonus state from the dead source.
    [[nodiscard]] bool onPropagandaTowerDie(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        PlayerRegistry* players, const GameContentSnapshot* content,
        const ObjectSimulationRules& rules, SimulationRandom* random,
        ObjectId object, uint32_t authoredOrder,
        uint64_t confirmedTick) const;

    [[nodiscard]] bool setCommandButtonHunt(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, container::String commandButton,
        uint64_t confirmedTick) const;
    [[nodiscard]] bool setWanderInPlace(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, uint64_t confirmedTick) const;

    [[nodiscard]] bool applyTopplingForce(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, const LogicFixedVec3& direction,
        math::q32_32 speed, uint64_t confirmedTick,
        uint64_t& nextGameplaySubmissionOrdinal,
        bool noBounce = false, bool noFx = false) const;

    // Commits sparse ToppleUpdate ingress. The generic Body transaction
    // barrier invokes this before damage; individual hazard producers do not
    // own or hard-code the handoff.
    void consumeToppleRequests(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        uint64_t confirmedTick,
        uint64_t& nextGameplaySubmissionOrdinal) const;

    void onHealthEvent(ecs::registry& registry,
                       const ObjectLifecycle& lifecycle,
                       ObjectId object, math::q32_32 actualDamage,
                       uint64_t confirmedTick) const;

    void update(ecs::registry& registry, const ObjectLifecycle& lifecycle,
                 PlayerRegistry& players, const GameContentSnapshot& content,
                 const game::terrain::TerrainLogic& terrain,
                 const ObjectAITargetPriorityQuery& targetPriority,
                 const game::terrain::MapVisibilitySnapshot* visibility,
                 const navigation::NavigationSystem* navigation,
                 const ObjectSimulationRules& rules, SimulationRandom* random,
                uint64_t confirmedTick, int32_t rankLevelLimit,
                uint64_t& nextGameplaySubmissionOrdinal,
                container::Vector<ObjectDamageRequest>& damageRequests,
                container::Vector<ObjectDefectionRequest>& defectionRequests,
                container::Vector<ObjectSpecialAbilityEffectRequest>&
                    effectRequests,
                container::Vector<ObjectSpecialAbilityFacingRequest>&
                    facingRequests) const;

    [[nodiscard]] container::Vector<ObjectBattlePlanPresentationEvent>
    takeBattlePlanPresentationEvents();
    [[nodiscard]] container::Vector<ObjectTacticalPresentationEvent>
    takeTacticalPresentationEvents();
    [[nodiscard]] container::Vector<ObjectDeployStyleManualFrameEvent>
    takeDeployStyleManualFrameEvents();
    [[nodiscard]] container::Vector<ObjectToppleFxEvent>
    takeToppleFxEvents();
    [[nodiscard]] container::Vector<ObjectToppleStumpSpawnRequest>
    takeToppleStumpSpawnRequests();
    void drainToppleStumpSpawnRequests(
        container::Vector<ObjectToppleStumpSpawnRequest>& out);
    [[nodiscard]] container::Vector<ObjectTopplePathfindRemovalRequest>
    takeTopplePathfindRemovalRequests();
    void drainTopplePathfindRemovalRequests(
        container::Vector<ObjectTopplePathfindRemovalRequest>& out);
    void discardToppleGameplayRequests() noexcept;
    void releaseToppleGameplayStorage() noexcept;

private:
    mutable container::Vector<ObjectBattlePlanPresentationEvent>
        m_battlePlanPresentationEvents;
    mutable container::Vector<ObjectTacticalPresentationEvent>
        m_tacticalPresentationEvents;
    mutable container::Vector<ObjectDeployStyleManualFrameEvent>
        m_deployStyleManualFrameEvents;
    mutable container::Vector<ObjectToppleFxEvent> m_toppleFxEvents;
    mutable container::Vector<ObjectToppleStumpSpawnRequest>
        m_toppleStumpSpawnRequests;
    mutable container::Vector<ObjectTopplePathfindRemovalRequest>
        m_topplePathfindRemovalRequests;
};

} // namespace engine
