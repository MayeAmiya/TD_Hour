#pragma once

#include "core/container/container_types.h"

#include "core/ecs/registry.h"
#include "game/object/ai/contracts/AIAttackServices.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/definition/ObjectKindOf.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/simulation/runtime/ObjectAIAttackTargetPolicy.h"
#include "game/object/simulation/combat/ObjectProjectileSystem.h"
#include "game/object/simulation/combat/ObjectHistoricWeaponTypes.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/plan/combat/ObjectCombatInitializationPlanTypes.h"

#include <cstdint>
#include <utility>
namespace game {
struct ObjectArchetype;
struct ThingTemplate;
}

namespace engine::navigation {
class NavigationSystem;
}

namespace engine {

class GameContentSnapshot;
class SimulationRandom;
class ObjectSpatialIndex;
class PlayerRegistry;

void setObjectTurretsEnabled(ObjectWeaponComponent& weapons,
                             bool enabled) noexcept;
void requestObjectTurretRecentering(ObjectWeaponComponent& weapons) noexcept;
[[nodiscard]] bool objectTurretsInNaturalPosition(
    const ObjectWeaponComponent& weapons) noexcept;
[[nodiscard]] bool objectTurretsAreForcedRecentering(
    const ObjectWeaponComponent& weapons) noexcept;

// Immediately materializes the best WeaponSet for the object's current
// condition mask. RefCode's set/clearWeaponSetFlag calls updateWeaponSet at
// the mutation boundary; delaying this until the next attack would make a
// subsequent WeaponBonus timer refresh operate on the obsolete set.
[[nodiscard]] bool refreshObjectWeaponSet(
    ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content,
    uint32_t logicFramesPerSecond = 30,
    uint64_t confirmedTick = 0) noexcept;

// FireWeaponPower's reloadAllAmmo(TRUE) boundary. Only the currently
// materialized WeaponSet exists in RefCode, so inactive modern set snapshots
// remain untouched. Full non-shared clips retain their current cooldown just
// like Weapon::reloadWithBonus's early return.
[[nodiscard]] bool reloadAllObjectWeaponsNow(
    ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content, uint64_t confirmedTick,
    uint32_t logicFramesPerSecond = 30) noexcept;

// RefCode Object::isOutOfAmmo / WeaponSet::isOutOfAmmo query. Only the
// currently materialized WeaponSet participates. A clipless weapon, a weapon
// with remaining rounds, or an automatically reloading weapon keeps the set
// usable; an empty non-auto-reloading weapon is OUT_OF_AMMO. An absent or
// empty active set therefore reports out of ammo, matching the legacy loop.
[[nodiscard]] bool objectIsOutOfAmmo(
    const ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content) noexcept;

// JetAIUpdate's RETURN_TO_BASE_TO_RELOAD policy.  The query follows the
// currently materialized WeaponSet exactly: at least one ReturnToBase weapon
// must exist and every such finite clip must be empty.
[[nodiscard]] bool objectIsOutOfReturnToBaseAmmo(
    const ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content) noexcept;

// Computes the stock airfield service duration (largest proportional missing
// clip time across the active set) and applies one deterministic progress
// sample.  This services all weapons once parked, matching ReloadAmmoState;
// ReturnToBase only controls when the aircraft decides to come home.
[[nodiscard]] uint64_t objectAirfieldReloadDurationFrames(
    const ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content,
    uint32_t logicFramesPerSecond) noexcept;
[[nodiscard]] bool applyObjectAirfieldReloadProgress(
    ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content, uint64_t elapsedFrames,
    uint64_t totalFrames, uint64_t confirmedTick) noexcept;

// Typed replacement for WeaponSet::setWeaponLock/releaseWeaponLock. Setting a
// lock verifies that the current materialized set actually contains the slot;
// a temporary request cannot override an existing permanent lock. Releasing
// Permanent clears either lock kind, while releasing Temporary clears only a
// temporary lock.
[[nodiscard]] bool setObjectWeaponLock(
    ecs::registry& registry, ecs::entity entity, game::WeaponSlot slot,
    ObjectWeaponLockType type) noexcept;
[[nodiscard]] bool releaseObjectWeaponLock(
    ecs::registry& registry, ecs::entity entity,
    ObjectWeaponLockType type) noexcept;

// One authoritative mutation boundary for all weapon-bonus producers
// (upgrades today; veterancy/horde/frenzy systems later).  A real mask change
// immediately restarts active reload/between-shot timers with the new
// rate-of-fire bonus, matching Weapon::onWeaponBonusChange.  Callers may omit
// content/random only during isolated assembly where no live timer exists.
[[nodiscard]] bool setObjectWeaponBonusCondition(
    ecs::registry& registry, ecs::entity entity,
    game::WeaponBonusCondition condition, bool enabled,
    const GameContentSnapshot* content, SimulationRandom* random,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick);

enum class ObjectWeaponEventKind : uint8_t {
    Fired,
    FireSoundLoopStarted,
    FireSoundLoopStopped,
    RapidFireVoice,
    TargetLost,
    WeaponUnavailable,
};

// PathfindCell::setTypeAsObstacle caches KINDOF_CAN_SEE_THROUGH_STRUCTURE per
// cell as m_obstacleIsTransparent. This project keeps gameplay kinds out of the
// navigation overlay, so the same fact is reconstructed from one ObjectId-sorted
// roster gathered once per confirmed tick and compared against the overlay's
// exact per-cell owner counts. Stock content authors the kind on civilian
// fences only, so the roster stays very small.
void gatherObjectSeeThroughObstacles(
    const ecs::registry& registry, container::Vector<uint64_t>& output);

// Pathfinder::isAttackViewBlockedByObstacle together with both authored gates
// it consults: AIData.AttackUsesLineOfSight and KINDOF_ATTACK_NEEDS_LINE_OF_SIGHT
// on the attacker. RefCode additionally skips the test for a non-ground
// attacker (AIStates::outOfWeaponRangeObject's "onGround"), for a victim
// significantly above terrain, and for contact weapons. This is the single
// canonical entry point so the firing path and SpecialAbilityUpdate's
// ApproachRequiresLOS cannot drift apart.
[[nodiscard]] bool objectAttackViewBlockedByObstacle(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const navigation::NavigationSystem* navigation,
    bool attackUsesLineOfSight,
    container::Span<const uint64_t> seeThroughObstacles,
    ecs::entity attackerEntity, ObjectId attacker,
    const LogicFixedVec3& attackerPosition,
    ObjectId victim, const LogicFixedVec3& victimPosition,
    bool contactWeapon) noexcept;

[[nodiscard]] game::WeaponFxPolicy resolveObjectWeaponFxPolicy(
    const ecs::registry& registry, ecs::entity sourceEntity,
    const ObjectLifecycle* lifecycle,
    const PlayerRegistry* players, const game::WeaponTemplate& weapon,
    bool suspendedByDelay) noexcept;

// Presentation and diagnostics consume this value stream after the confirmed
// frame.  It contains no renderer/audio handles and has no authority to
// mutate health; ObjectDamageRequest remains the sole combat consequence.
struct ObjectWeaponEvent final {
    ObjectWeaponEventKind kind = ObjectWeaponEventKind::Fired;
    ObjectId source = INVALID_OBJECT_ID;
    // Frozen at the confirmed fire edge. Temporary/death weapons can retire
    // their source before FXList Sound nuggets reach presentation, but the
    // original AudioEventRTS still carries the firing object's player index.
    PlayerId sourcePlayer = INVALID_PLAYER_ID;
    ObjectId target = INVALID_OBJECT_ID;
    uint32_t sourceShotSequence = 0;
    uint32_t sourceBarrelSequenceOrdinal = 0;
    game::WeaponSlot slot = game::WeaponSlot::Primary;
    game::WeaponContentId content;
    game::ObjectVeterancyLevel veterancy =
        game::ObjectVeterancyLevel::Regular;
    game::WeaponFxPolicy fxPolicy = game::WeaponFxPolicy::Play;
    container::String weaponName;
    // Frozen optional AudioEvent name for tracker-only presentation events.
    // Ordinary Fired events still resolve FireSound from immutable content.
    container::String audioEventName;
    // Frozen world anchors make a confirmed fire presentation independent of
    // the source/target lifecycle. Temporary weapons may destroy their source
    // in the same transaction that publishes Fired.
    LogicFixedVec3 sourcePosition{};
    LogicFixedVec3 impactPosition{};
    bool hasFrozenPositions = false;
    // A real WeaponSet slot selects FIRING before W3D resolves FireFXBone.
    // Transient/death/collision weapons keep the source's current pose.
    bool usesFiringPresentation = false;
    // Absolute XY shot direction captured at the fire transaction. Weapon
    // recoil presentation converts it to the source's local chassis axes;
    // it must not reconstruct a later target position after either object
    // has moved.
    math::q32_32 recoilDirectionRadians{};
    uint64_t confirmedTick = 0;
};

// Sparse typed replacement for PointDefenseLaserUpdate. Each authored module
// owns an independent private weapon cadence/scan target, matching Avenger's
// two laser modules rather than collapsing them into its normal WeaponSet.
struct ObjectPointDefenseLaserRuleRuntime final {
    game::WeaponContentId weapon;
    game::ObjectKindOfMask primaryTargetKindMask{};
    game::ObjectKindOfMask secondaryTargetKindMask{};
    uint32_t scanRateMilliseconds = 0;
    math::q32_32 scanRange{};
    math::q32_32 predictTargetVelocityFactor{};
    ObjectId trackedTarget = INVALID_OBJECT_ID;
    uint64_t nextScanTick = 0;
    uint64_t nextShotTick = 0;
    uint32_t nextShotSequence = 1;
    uint32_t authoredOrder = 0;
    bool targetWasInRange = false;
};

struct ObjectPointDefenseLaserComponent final {
    container::Vector<ObjectPointDefenseLaserRuleRuntime> rules;
};

// Borrowed for one Combat update. owners is the ObjectId-sorted ownership
// projection published by ObjectAIRuntime; commands retain their original
// emission order so phase cleanup and replacement cannot be reordered.
struct ObjectCombatAIInput final {
    container::Span<const ObjectId> owners{};
    container::Span<const ObjectId> autonomousOwners{};
    container::Span<const ai::AIAttackCommand> commands{};
    // Dynamic obstacle field consulted for KINDOF_ATTACK_NEEDS_LINE_OF_SIGHT
    // attackers. Only AIAttackFeedback::viewBlocked depends on it, which is
    // why it rides the AI input rather than the general update signature; a
    // null value keeps every actor's line of sight unobstructed.
    const navigation::NavigationSystem* navigation = nullptr;
    // AIData.AttackUsesLineOfSight. False disables the obstruction test for
    // every unit, exactly as TAiData's global switch does.
    bool attackUsesLineOfSight = true;
    // AIData.AICrushesInfantry. This affects only computer-controlled
    // object-AI pursuit/squish decisions; collision damage remains owned by
    // ObjectSquishCollide and is never enabled for human orders here.
    bool aiCrushesInfantry = true;
};


// Sparse cross-tick handoff state owned exclusively by Combat. The AI
// runtime owns the state-machine columns and value commands; it never writes
// Weapon/Turret runtime or this ECS sidecar.
struct ObjectAICombatOperationComponent final {
    ai::AIAttackCorrelation correlation{};
    ObjectId target = INVALID_OBJECT_ID;
    ai::AIFixedPosition targetPosition{};
    ai::AIAttackPhase phase = ai::AIAttackPhase::Inactive;
    bool attacksObject = false;
    bool forceAttack = false;
    bool fireRequested = false;
    // AttackMove carries MaxShotsToFire on its parent Move order rather than
    // on a queue-backed Attack child. Combat therefore owns this small
    // confirmed-tick counter while the AI child is active.
    uint32_t maximumShots = 0;
    uint32_t shotsFired = 0;
    bool hasMaximumShots = false;
    bool attackLimitReached = false;
};

// A private Weapon instance owned by a behavior module, without restoring the
// legacy heap object. Reaction/continuous weapons keep ammo and cooldown here;
// one-shot death weapons bypass this runtime and enqueue a transient command.
struct ObjectSystemWeaponRuntime final {
    game::WeaponContentId content;
    uint32_t ammoInClip = 0;
    uint64_t nextReadyTick = 0;
    uint64_t reloadCompleteTick = 0;
    uint64_t suspendFxUntilTick = 0;
    uint32_t nextShotSequence = 1;
    uint32_t currentBarrel = 0;
    uint32_t shotsRemainingForCurrentBarrel = 0;
    container::Vector<uint32_t> scatterTargetsUnused;
};

enum class ObjectSystemWeaponInitialLoad : uint8_t {
    Reload,
    Immediate,
};

struct ObjectSystemWeaponFireCommand final {
    ObjectId source = INVALID_OBJECT_ID;
    ObjectId target = INVALID_OBJECT_ID;
    game::WeaponContentId content;
    game::WeaponBonusConditionMask bonusConditions = 0;
    LogicFixedVec3 sourcePosition;
    LogicFixedVec3 impactPosition;
    LogicFixedVec3 intendedTargetBasePosition;
    uint32_t scatteredTargetPathfindLayer = 0;
    uint32_t sourceShotSequence = 0;
    uint32_t sourceBarrelSequenceOrdinal = 0;
    uint32_t projectileStreamOwnerGeneration = 0;
    // Optional script-only handoff for a newly spawned ProjectileObject.
    // UINT32_MAX is absent; Terrain waypoint ID zero remains valid.
    uint32_t waypointPathStartId = UINT32_MAX;
    uint64_t waypointGraphRevision = 0;
    game::WeaponSlot launchSlot = game::WeaponSlot::Primary;
    // True only when this command was admitted through a real WeaponSet slot.
    // Transient/death/collision weapons still execute their projectile, FX and
    // damage chain, but must not invent a FIRING pose or a model launch bone.
    bool usesFiringPresentation = false;
    uint32_t authoredOrder = 0;
    uint64_t emissionSequence = 0;
    bool weaponFxSuspendedByDelay = false;
    bool targetWasScattered = false;
    bool hasIntendedTargetBasePosition = false;
    bool hasTumbleAngularRates = false;
    ObjectPhysicsComponent::Scalar tumbleYawRate{};
    ObjectPhysicsComponent::Scalar tumblePitchRate{};
    ObjectPhysicsComponent::Scalar tumbleRollRate{};
    uint64_t confirmedTick = 0;
};

// Combat owns actor traversal and weapon-state mutation; Session owns the
// gameplay transaction stack. This narrow value sink closes one actor's
// emitted Weapon commands only after Combat has released all component
// references, so nested Spawn/OCL may safely grow EnTT storage before the
// next actor is inspected.
struct ObjectCombatWeaponFireSink final {
    using Submit = bool (*)(void*, ObjectSystemWeaponFireCommand&&);

    void* context = nullptr;
    Submit submit = nullptr;

    [[nodiscard]] bool operator()(
        ObjectSystemWeaponFireCommand command) const {
        return submit && submit(context, std::move(command));
    }
};

// Allocates a behavior-owned value runtime. Reaction/continuous private
// weapons use RefCode reloadAmmo(source); FireWeaponUpdate explicitly selects
// loadAmmoNow(source). Both snapshot the source's then-current ROF bonus.
[[nodiscard]] bool initializeObjectSystemWeaponRuntime(
    ObjectSystemWeaponRuntime& runtime, container::StringView weaponName,
    ecs::registry& registry, ecs::entity sourceEntity,
    const GameContentSnapshot& content, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick,
    ObjectSystemWeaponInitialLoad initialLoad =
        ObjectSystemWeaponInitialLoad::Reload) noexcept;

// READY_TO_FIRE + forceFireWeapon for a persistent behavior-owned weapon.
// State advances before the detached command is published, so a reaction
// chain in the same confirmed tick observes the new cooldown immediately.
[[nodiscard]] bool tryQueueObjectSystemWeaponFire(
    ObjectSystemWeaponRuntime& runtime,
    ecs::registry& registry, ecs::entity sourceEntity, ObjectId source,
    const GameContentSnapshot& content, SimulationRandom& random,
    uint32_t logicFramesPerSecond, uint32_t authoredOrder,
    uint64_t emissionSequence, uint64_t confirmedTick,
    container::Vector<ObjectSystemWeaponFireCommand>& outCommands,
    bool selectAsCurrentWeapon = true);

[[nodiscard]] bool queueObjectTransientWeaponFire(
    game::WeaponContentId contentId,
    ecs::registry& registry, ecs::entity sourceEntity, ObjectId source,
    const GameContentSnapshot& content, SimulationRandom& random,
    uint32_t sourceShotSequence, uint32_t authoredOrder,
    uint64_t emissionSequence, uint64_t confirmedTick,
    container::Vector<ObjectSystemWeaponFireCommand>& outCommands);

// StructureTopple and other world-impact producers fire a temporary weapon
// at an immutable point rather than at the source origin or a target Object.
[[nodiscard]] bool queueObjectTransientWeaponFireAtPosition(
    game::WeaponContentId contentId,
    ecs::registry& registry, ecs::entity sourceEntity, ObjectId source,
    const LogicFixedVec3& impactPosition,
    const GameContentSnapshot& content, SimulationRandom& random,
    uint32_t sourceShotSequence, uint32_t authoredOrder,
    uint64_t emissionSequence, uint64_t confirmedTick,
    container::Vector<ObjectSystemWeaponFireCommand>& outCommands);

// DeliverPayload strafing is an auxiliary fire request and must not replace
// the aircraft's primary Move order.  This path consumes the selected live
// WeaponSet slot (clip, cooldown, reload and barrel state included) while
// publishing the same detached command as ordinary combat.
[[nodiscard]] bool tryQueueObjectSlotWeaponFireAtPosition(
    ecs::registry& registry, ecs::entity sourceEntity, ObjectId source,
    game::WeaponSlot slot, const LogicFixedVec3& impactPosition,
    const GameContentSnapshot& content, SimulationRandom& random,
    uint32_t logicFramesPerSecond, uint32_t authoredOrder,
    uint64_t emissionSequence, uint64_t confirmedTick,
    container::Vector<ObjectSystemWeaponFireCommand>& outCommands,
    bool selectAsCurrentWeapon = true);

// FireWeaponCollide's loadAmmoNow + fireWeapon(source, target) path. Target
// identity and both authoritative positions are snapshotted at admission so
// the detached command remains valid across the deferred lifecycle boundary.
[[nodiscard]] bool queueObjectTargetedTransientWeaponFire(
    game::WeaponContentId contentId,
    ecs::registry& registry, ecs::entity sourceEntity, ObjectId source,
    ecs::entity targetEntity, ObjectId target,
    const GameContentSnapshot& content, SimulationRandom& random,
    uint32_t sourceShotSequence, uint32_t authoredOrder,
    uint64_t emissionSequence, uint64_t confirmedTick,
    container::Vector<ObjectSystemWeaponFireCommand>& outCommands);

// Consumes a queued direct-object Attack intent and produces ordered damage
// values.  This is intentionally separate from ObjectSimulation: it never
// changes HealthComponent, destroys entities, reaches global WeaponStore, or
// knows about rendering.  Projectile, terrain LOS and area-damage expansion
// are separate future stages; the current slice is an explicit-object
// direct-hit weapon pipeline with original WeaponSet selection semantics.
class ObjectCombatSystem final {
public:
    void reset() noexcept;

    // Called by GameSession's one spawn assembler after CombatProfile is
    // attached, before lifecycle observers receive Created.  It parses
    // KindOf once and resolves each symbolic weapon name to a compact stable
    // session-content ID.
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          const game::ObjectArchetype& archetype,
                          const GameContentSnapshot& content,
                          uint32_t logicFramesPerSecond = 30,
                          uint64_t confirmedTick = 0) const;

    // Attack actors are gathered and sorted by ObjectId before they advance
    // state or consume SimulationRandom. Every accepted shot emits one full
    // Weapon transaction; FireOCL/projectile/direct Damage ordering belongs
    // to that transaction rather than to parallel output vectors.
    [[nodiscard]] bool update(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const GameContentSnapshot& content,
        const ObjectSpatialIndex* spatialIndex,
        const PlayerRegistry* players, SimulationRandom& random,
        uint32_t logicFramesPerSecond, uint64_t confirmedTick,
        ObjectCombatWeaponFireSink weaponFireSink,
        ObjectCombatAIInput aiInput = {});

    // Executes already-admitted behavior-owned fire commands without command
    // targeting/range selection. Damage remains value-only and projectile
    // creation remains a GameSession transaction after this call.
    void executeSystemWeaponFires(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const GameContentSnapshot& content,
        const ObjectSpatialIndex* spatialIndex,
        const PlayerRegistry* players,
        container::Span<const ObjectSystemWeaponFireCommand> commands,
        uint32_t logicFramesPerSecond,
        container::Vector<ObjectDamageRequest>& outDamage,
        container::Vector<ObjectProjectileSpawnRequest>* outProjectiles = nullptr);

    [[nodiscard]] container::Vector<ObjectWeaponEvent> takeEvents();
    [[nodiscard]] container::Vector<ObjectHistoricBonusWeaponFire>
    takeHistoricBonusWeaponFires();
    [[nodiscard]] container::Vector<ai::AIAttackFeedback>
    takeAIAttackFeedback();

private:
    container::Vector<ObjectWeaponEvent> m_events;
    container::Vector<ai::AIAttackFeedback> m_aiAttackFeedback;
    container::Vector<ObjectHistoricBonusWeaponFire>
        m_historicBonusWeaponFires;
    // Serialized executeSystemWeaponFires() scratch. Damage expansion fully
    // consumes it before the next command and preserves ObjectId ordering.
    container::Vector<ObjectId> m_weaponDamageVictimScratch;
};

} // namespace engine
