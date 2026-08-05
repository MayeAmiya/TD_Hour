#pragma once

#include "game/object/simulation/combat/ObjectCombatSystem.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/content/runtime/GameContentSnapshot.h"

#include <optional>

namespace engine::object_combat_detail {

using Fixed = math::q32_32;
using container::asciiEqualIgnoreCase;

inline const Fixed kFixedZero{};
inline const Fixed kFixedOne{int32_t{1}};
inline const Fixed kFixedHalf = Fixed::from_fraction(1, 2);
inline const Fixed kFixedPi = Fixed::from_raw(13493037704ll);
inline const Fixed kFixedFullTurn = Fixed{int32_t{2}} * kFixedPi;
inline const Fixed kFixedPitchAlwaysAcceptDeltaZ{int32_t{10}};
inline constexpr Fixed kFixedRationalizedRangeUndersize =
    Fixed::from_fraction(5, 2);
inline const Fixed kFixedHorizontalEpsilon = Fixed::from_fraction(1, 10000);
inline const Fixed kFixedYawAlignmentTolerance = Fixed::from_fraction(7, 200);
inline const Fixed kFixedPitchAlignmentTolerance = Fixed::from_fraction(1, 1000000);
inline const Fixed kFixedLegacyLogicFramesPerSecond{int32_t{30}};
inline const Fixed kFixedTumbleRateBoundPerLegacyFrame = kFixedOne / kFixedPi;

struct PristineWeaponPresentation final {
    game::W3dWeaponBarrelTable barrels;
    container::Array<game::ModelTurretBoneDefinition, 2> turrets;
    container::Array<std::optional<data::w3d::FixedRigidTransform>, 2>
        yawPivots;
    container::Array<std::optional<data::w3d::FixedRigidTransform>, 2>
        pitchPivots;
    LogicFixedVec3 attachOffset{};
};

struct WeaponLaunchTransform final {
    LogicFixedVec3 position{};
    LogicFixedQuaternion orientation{};
    bool hasOrientation = false;
};

[[nodiscard]] std::optional<PristineWeaponPresentation>
pristineWeaponPresentation(
    const ecs::registry& registry, ecs::entity sourceEntity,
    const GameContentSnapshot& content, game::WeaponSlot slot,
    const game::ModelConditionMask* presentationConditions = nullptr);
[[nodiscard]] std::optional<game::ModelConditionMask>
firingPresentationConditions(
    const ecs::registry& registry, ecs::entity sourceEntity,
    game::WeaponSlot slot) noexcept;
[[nodiscard]] std::optional<LogicFixedVec3>
containmentFirePointWorldPosition(
    const ecs::registry& registry, ecs::entity hostEntity,
    const GameContentSnapshot& content, container::StringView boneName,
    bool passengersInTurret, const LogicFixedVec3& hostPosition,
    const TransformComponent& hostTransform);
void releaseGarrisonFirePoint(
    ecs::registry& registry, ecs::entity hostEntity, ObjectId occupant);
[[nodiscard]] std::optional<size_t> assignGarrisonFirePoint(
    ObjectGarrisonFirePointComponent& state, ObjectId occupant,
    ObjectId target, const LogicFixedVec3& targetPosition,
    const container::Vector<LogicFixedVec3>& points);
[[nodiscard]] WeaponLaunchTransform pristineWeaponLaunchTransform(
    const ecs::registry& registry, ecs::entity sourceEntity,
    const GameContentSnapshot& content, game::WeaponSlot slot,
    uint32_t barrelSequenceOrdinal, const LogicFixedVec3& fallback,
    const game::ModelConditionMask* presentationConditions = nullptr);

void initializeTurretRuntime(
    const ObjectCombatInitializationPlan* plan,
    ObjectWeaponComponent& weapons);
void advanceTurretsTowardTarget(
    ObjectWeaponComponent& weapons,
    const LogicFixedVec3& source, Fixed sourceYaw,
    const LogicFixedVec3& target,
    const ObjectGeometryComponent* sourceGeometry,
    const ObjectGeometryComponent* targetGeometry,
    game::WeaponSlot selectedSlot, Fixed selectedAttackRange,
    bool selectedTargetUsesGroundPitch, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick);
void advanceTurretsTowardNatural(
    ObjectWeaponComponent& weapons, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick, SimulationRandom& random) noexcept;
void notifyTurretWeaponFired(
    ObjectWeaponComponent& weapons, game::WeaponSlot slot,
    uint64_t confirmedTick) noexcept;
[[nodiscard]] bool turretAlignedForWeaponSlot(
    const ObjectWeaponComponent& weapons, game::WeaponSlot slot,
    const LogicFixedVec3& source, Fixed sourceYaw,
    const LogicFixedVec3& target,
    const ObjectGeometryComponent* sourceGeometry,
    const ObjectGeometryComponent* targetGeometry,
    Fixed attackRange, Fixed acceptableAimDelta,
    bool targetUsesGroundPitch) noexcept;

void populateTumbleLaunchRates(
    ObjectProjectileSpawnRequest& request, const GameContentSnapshot& content,
    SimulationRandom& random);
[[nodiscard]] bool containsKind(
    const ObjectKindOfComponent* kinds,
    game::ObjectKindOf sought) noexcept;
void applyProjectileScatter(
    ObjectProjectileSpawnRequest& request,
    const game::WeaponTemplate& weapon,
    const ObjectKindOfComponent* targetKinds,
    uint32_t targetPathfindLayer, SimulationRandom& random,
    container::Vector<uint32_t>* scatterTargetsUnused = nullptr);
void rebuildScatterTargets(
    ObjectWeaponSlotRuntime& slot,
    const game::WeaponTemplate& definition);
[[nodiscard]] bool matchesPreferredAgainst(
    const game::WeaponSlotProfile& slot,
    const ObjectKindOfComponent* kinds) noexcept;
[[nodiscard]] uint64_t millisecondsToFrames(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept;
[[nodiscard]] uint64_t saturatingTickAdd(
    uint64_t tick, uint64_t delay) noexcept;
[[nodiscard]] uint64_t multiplyFramesByPreAttack(
    uint64_t frames, const game::WeaponBonus& bonus) noexcept;
[[nodiscard]] uint64_t clipReloadFrames(
    const game::WeaponTemplate& definition, const game::WeaponBonus& bonus,
    uint32_t framesPerSecond) noexcept;
[[nodiscard]] uint64_t chooseShotDelayFrames(
    const game::WeaponTemplate& definition, uint32_t framesPerSecond,
    SimulationRandom& random, const game::WeaponBonus& bonus) noexcept;
[[nodiscard]] bool isReloading(
    const ObjectWeaponSlotRuntime& slot, uint64_t tick) noexcept;
[[nodiscard]] bool hasFiniteEmptyClip(
    const ObjectWeaponSlotRuntime& slot,
    const game::WeaponTemplate& definition) noexcept;
void advanceWeaponSet(
    ObjectWeaponSetRuntime& set, const GameContentSnapshot& content,
    uint64_t tick) noexcept;
[[nodiscard]] bool releaseWeaponLock(
    ObjectWeaponComponent& weapons, ObjectWeaponLockType type) noexcept;

void updateObjectFiringTracker(
    ecs::registry& registry, ecs::entity entity, ObjectId object,
    ObjectWeaponComponent& weapons, const GameContentSnapshot& content,
    SimulationRandom& random, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick, container::Vector<ObjectWeaponEvent>& events);
void notifyObjectFiringTrackerShot(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity entity, ObjectId object, ObjectId victim,
    ObjectWeaponComponent& weapons, const game::WeaponTemplate& weapon,
    game::WeaponContentId weaponContent, game::WeaponSlot weaponSlot,
    uint64_t possibleNextShotTick,
    const GameContentSnapshot& content, SimulationRandom& random,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick,
    container::Vector<ObjectWeaponEvent>& events);
[[nodiscard]] bool activateWeaponSetRuntime(
    ObjectWeaponComponent& weapons, size_t setIndex,
    const GameContentSnapshot& content, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick) noexcept;

[[nodiscard]] bool targetMatchesAntiMask(
    const game::WeaponTemplate& weapon, const ObjectKindOfComponent* kinds,
    const ObjectAirborneComponent* airborne,
    const ObjectStatusComponent* status) noexcept;
[[nodiscard]] Fixed combatDistance(
    const LogicFixedVec3& source,
    const ObjectGeometryComponent* sourceGeometry,
    const LogicFixedVec3& target,
    const ObjectGeometryComponent* targetGeometry) noexcept;
[[nodiscard]] bool pitchMatches(
    const game::WeaponTemplate& weapon, const LogicFixedVec3& source,
    const LogicFixedVec3& target) noexcept;
[[nodiscard]] Fixed resolvedAttackRange(
    const game::WeaponTemplate& weapon,
    const game::WeaponBonus& bonus) noexcept;
[[nodiscard]] bool isWithinRange(
    const game::WeaponTemplate& weapon, const game::WeaponBonus& bonus,
    Fixed distance) noexcept;
[[nodiscard]] Fixed estimatedDamage(
    const game::WeaponTemplate& weapon, const game::WeaponBonus& bonus,
    const ObjectArmorComponent* armor, const ObjectHealthComponent* health,
    const ObjectKindOfComponent* kinds, const ObjectStatusComponent* status,
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    std::optional<ecs::entity> targetEntity) noexcept;
[[nodiscard]] bool sameActiveAttack(
    const ObjectWeaponComponent& weapons,
    const ObjectOrderIntent& order) noexcept;
void resetAttackLock(
    ObjectWeaponComponent& weapons,
    bool releaseTemporary = true) noexcept;
[[nodiscard]] game::WeaponCommandSource toWeaponCommandSource(
    ObjectOrderSource source) noexcept;
[[nodiscard]] bool requiresPreAttack(
    const ObjectWeaponSlotRuntime& slot,
    const game::WeaponTemplate& definition,
    const ObjectOrderIntent& order) noexcept;
[[nodiscard]] bool beginPreAttack(
    ObjectWeaponSlotRuntime& slot, const game::WeaponTemplate& definition,
    const game::WeaponBonus& bonus, const ObjectOrderIntent& order,
    uint32_t framesPerSecond, uint64_t tick) noexcept;
void consumeAttackOrder(ObjectOrderQueueComponent* queue);

[[nodiscard]] bool isOrdinaryAIAttackOrder(
    const ObjectOrderIntent* order) noexcept;
[[nodiscard]] bool isAIAttackMoveOrder(
    const ObjectOrderIntent* order) noexcept;
[[nodiscard]] bool isAITacticalAttackOrder(
    const ObjectOrderIntent* order) noexcept;
[[nodiscard]] ai::AIAsyncOrderIdentity attackOrderIdentity(
    ObjectId subject, const ObjectOrderQueueComponent& queue,
    const ObjectOrderIntent& order) noexcept;
[[nodiscard]] bool isActiveAICombatPhase(
    ai::AIAttackPhase phase) noexcept;
[[nodiscard]] bool commandPhaseMatchesKind(
    const ai::AIAttackCommand& command) noexcept;
void clearAICombatOperation(
    ecs::registry& registry, ecs::entity entity,
    ObjectWeaponComponent& weapons);
void appendAIAttackFeedbackOnce(
    container::Vector<ai::AIAttackFeedback>& output,
    const ai::AIAttackFeedback& feedback);

class ScopedAIAttackSnapshot final {
public:
    ScopedAIAttackSnapshot(
        container::Vector<ai::AIAttackFeedback>& output,
        ai::AIAttackFeedback& feedback, bool enabled) noexcept;
    ScopedAIAttackSnapshot(const ScopedAIAttackSnapshot&) = delete;
    ScopedAIAttackSnapshot& operator=(const ScopedAIAttackSnapshot&) = delete;
    ~ScopedAIAttackSnapshot() noexcept;
    void cancel() noexcept;

private:
    container::Vector<ai::AIAttackFeedback>& m_output;
    ai::AIAttackFeedback& m_feedback;
    bool m_enabled = false;
};

void appendWeaponDamage(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSpatialIndex* spatialIndex, const PlayerRegistry* players,
    ObjectId sourceId, ecs::entity sourceEntity, ObjectId primaryTarget,
    const game::WeaponTemplate& weapon, const game::WeaponBonus& bonus,
    uint32_t shotSequence, uint64_t tick,
    container::Vector<ObjectDamageRequest>& outDamage);

} // namespace engine::object_combat_detail
