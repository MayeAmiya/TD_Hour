#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"

#include "game/base/SimulationRandom.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/navigation/runtime/NavigationSystem.h"
#include "game/navigation/integration/NavigationTerrainLayerMapping.h"
#include "game/navigation/integration/NavigationDestinationAdjustment.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/definition/LocomotorTemplate.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/combat/ObjectCombatProfileRuntime.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/plan/movement/ObjectPhysicsPlanTypes.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"
#include "core/math/wwmath/base/wwmath.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>
#include "game/object/simulation/runtime/ObjectSimulationDetail.h"

namespace engine {

namespace {

using PhysicsScalar = math::q32_32;
using HealthScalar = ObjectHealthComponent::Scalar;

const PhysicsScalar kPhysicsZero{int32_t{0}};
const PhysicsScalar kPhysicsOne{int32_t{1}};
const PhysicsScalar kPhysicsTwo{int32_t{2}};
constexpr PhysicsScalar kMovementArrivalEpsilonFixed =
    PhysicsScalar::from_fraction(1, 1000);
constexpr PhysicsScalar kMovementPi =
    PhysicsScalar::from_raw(13'493'037'705ll);
const PhysicsScalar kMovementFullTurn = kPhysicsTwo * kMovementPi;
const PhysicsScalar kMovementHalfPi = kMovementPi / kPhysicsTwo;
const PhysicsScalar kMovementQuarterPi = kMovementPi /
    PhysicsScalar{int32_t{4}};
constexpr PhysicsScalar kPhysicsGroundEpsilon =
    PhysicsScalar::from_fraction(1, 10'000);
constexpr PhysicsScalar kPhysicsRestEpsilon =
    PhysicsScalar::from_fraction(1, 100);
constexpr PhysicsScalar kPhysicsMinimumFrictionPerFrame =
    PhysicsScalar::from_fraction(1, 100);
constexpr PhysicsScalar kPhysicsMaximumFrictionPerFrame =
    PhysicsScalar::from_fraction(99, 100);
const PhysicsScalar kPhysicsMinimumGroundStiffness{
    PhysicsSimulationRules::kMinimumGroundStiffness};
const PhysicsScalar kPhysicsMaximumGroundStiffness{
    PhysicsSimulationRules::kMaximumGroundStiffness};
const HealthScalar kHealthZero{};
const HealthScalar kHealthOne{int32_t{1}};

using container::asciiEqualIgnoreCase;

[[nodiscard]] bool hasKind(const ObjectKindOfComponent* kinds,
                           game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

[[nodiscard]] LogicFixedVec3 addFixed(const LogicFixedVec3& left,
                                       const LogicFixedVec3& right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] LogicFixedVec3 scaleFixed(const LogicFixedVec3& value,
                                         PhysicsScalar amount) noexcept {
    return {value.x * amount, value.y * amount, value.z * amount};
}

[[nodiscard]] PhysicsScalar squaredLengthFixed(const LogicFixedVec3& value) noexcept {
    return value.x * value.x + value.y * value.y + value.z * value.z;
}

[[nodiscard]] PhysicsScalar clampPhysics(PhysicsScalar value, PhysicsScalar minimum,
                                          PhysicsScalar maximum) noexcept {
    return value < minimum ? minimum : value > maximum ? maximum : value;
}
} // namespace

namespace object_simulation_detail {

void executeNeutronBlastDeath(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry* players, ecs::entity sourceEntity, ObjectId source,
    const game::ObjectNeutronBlastDieParameters& parameters,
    uint32_t authoredOrder, uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& damage,
    container::Vector<ObjectVehicleNeutralizationRequest>& events) {
    const PhysicsScalar radius = PhysicsScalar::max(
        PhysicsScalar{}, parameters.blastRadius);
    if (radius <= PhysicsScalar{}) return;
    const TransformComponent* sourceTransform =
        ecs::try_get<TransformComponent>(registry, sourceEntity);
    if (!sourceTransform) return;
    const LogicFixedVec3 center = readAuthoritativeObjectPosition(
        registry, sourceEntity, *sourceTransform);
    const ObjectMapStatusComponent* sourceMap =
        ecs::try_get<ObjectMapStatusComponent>(registry, sourceEntity);
    const OwnerComponent* sourceOwner =
        ecs::try_get<OwnerComponent>(registry, sourceEntity);
    const PhysicsScalar radiusSquared = radius * radius;

    struct Candidate final {
        ObjectId id = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const TransformComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id || identity.id == source ||
            !lifecycle.entityFromId(identity.id) ||
            lifecycle.isPendingDestroy(identity.id)) {
            continue;
        }
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, entity);
        if (health && health->effectivelyDead) continue;
        const ObjectMapStatusComponent* targetMap =
            ecs::try_get<ObjectMapStatusComponent>(registry, entity);
        if ((sourceMap ? sourceMap->offMap : false) !=
            (targetMap ? targetMap->offMap : false)) {
            continue;
        }
        const TransformComponent& transform =
            view.template get<const TransformComponent>(entity);
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            registry, entity, transform);
        const PhysicsScalar dx = position.x - center.x;
        const PhysicsScalar dy = position.y - center.y;
        if (dx * dx + dy * dy > radiusSquared) continue;
        candidates.push_back({identity.id, entity});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.id < right.id;
              });

    container::Vector<ObjectId> killed;
    const auto kill = [&](ObjectId target, ecs::entity entity) {
        if (!target ||
            std::find(killed.begin(), killed.end(), target) != killed.end()) {
            return;
        }
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, entity);
        if (!health || health->effectivelyDead) return;
        killed.push_back(target);
        damage.push_back({
            .target = target,
            .source = INVALID_OBJECT_ID,
            .sourceSequence = authoredOrder,
            .causalGroup = source,
            .amount = health->maximumFixed,
            .damageType = game::DamageType::UNRESISTABLE,
            .deathType = game::DeathType::NORMAL,
            .forceKill = true,
            .confirmedTick = confirmedTick,
        });
    };
    const auto killAllContained = [&](ecs::entity root) {
        container::Vector<ObjectId> descendants;
        if (const ObjectContainmentComponent* contents =
                ecs::try_get<ObjectContainmentComponent>(registry, root)) {
            descendants.reserve(contents->objects.size());
            for (const ObjectContainedObjectRecord& record : contents->objects) {
                if (record.object) descendants.push_back(record.object);
            }
        }
        // A malformed containment graph must not loop the death transaction.
        // Expand each stable ID at most once, then submit damage in canonical
        // ObjectId order independently of EnTT storage/roster insertion.
        container::Vector<ObjectId> visited;
        for (size_t cursor = 0; cursor < descendants.size(); ++cursor) {
            const ObjectId occupant = descendants[cursor];
            if (std::find(visited.begin(), visited.end(), occupant) !=
                visited.end()) {
                continue;
            }
            visited.push_back(occupant);
            const std::optional<ecs::entity> entity =
                lifecycle.entityFromId(occupant);
            const ObjectContainmentComponent* nested = entity
                ? ecs::try_get<ObjectContainmentComponent>(registry, *entity)
                : nullptr;
            if (!nested) continue;
            for (const ObjectContainedObjectRecord& record : nested->objects) {
                if (record.object) descendants.push_back(record.object);
            }
        }
        std::sort(visited.begin(), visited.end());
        for (const ObjectId occupant : visited) {
            const std::optional<ecs::entity> entity =
                lifecycle.entityFromId(occupant);
            if (entity) kill(occupant, *entity);
        }
    };

    for (const Candidate& candidate : candidates) {
        if (!parameters.affectAllies) {
            bool allied = false;
            if (players) {
                allied = relationshipBetweenObjects(
                    registry, *players, sourceEntity, candidate.entity) ==
                    PlayerRelationship::Allies;
            } else {
                const OwnerComponent* targetOwner =
                    ecs::try_get<OwnerComponent>(registry, candidate.entity);
                allied = sourceOwner && targetOwner &&
                    sourceOwner->player == targetOwner->player;
            }
            if (allied) continue;
        }

        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(registry, candidate.entity);
        const ObjectAirborneComponent* airborne =
            ecs::try_get<ObjectAirborneComponent>(registry, candidate.entity);
        if (!parameters.affectAirborne &&
            (hasKind(kinds, game::ObjectKindOf::Aircraft) ||
             (airborne && airborne->isAirborne))) {
            continue;
        }
        if (hasKind(kinds, game::ObjectKindOf::Infantry)) {
            kill(candidate.id, candidate.entity);
        }
        killAllContained(candidate.entity);
        if (!hasKind(kinds, game::ObjectKindOf::Vehicle) ||
            hasKind(kinds, game::ObjectKindOf::Drone)) {
            continue;
        }
        if (hasKind(kinds, game::ObjectKindOf::CliffJumper)) {
            kill(candidate.id, candidate.entity);
            continue;
        }
        if (ObjectOrderQueueComponent* queue =
                ecs::try_get<ObjectOrderQueueComponent>(
                    registry, candidate.entity)) {
            if (!queue->orders.empty()) {
                queue->orders.clear();
                ++queue->revision;
            }
        }
        static_cast<void>(ObjectDisabledSystem::setUntil(
            registry, candidate.entity, ObjectDisabledReason::Unmanned,
            OBJECT_DISABLED_FOREVER_TICK, confirmedTick));
        events.push_back({
            .source = source,
            .target = candidate.id,
            .authoredOrder = authoredOrder,
            .confirmedTick = confirmedTick,
        });
    }
}

[[nodiscard]] uint64_t millisecondsToConfirmedTicks(uint32_t milliseconds,
                                                     uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t rate = std::max<uint32_t>(1, framesPerSecond);
    return (static_cast<uint64_t>(milliseconds) * rate + 999u) / 1000u;
}

// SplitMix64 is used as a counter PRF, never as a shared mutable simulation
// RNG. Death-effect choices therefore remain reproducible when another
// system adds a random call or EnTT storage changes iteration order.
[[nodiscard]] uint64_t mixDeathRandom(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

[[nodiscard]] uint64_t makeDeathRandomKey(uint64_t sessionSeed, ObjectId object,
                                           const ObjectDamageRequest& request,
                                           uint32_t authoredOrder) noexcept {
    uint64_t value = mixDeathRandom(sessionSeed);
    value ^= mixDeathRandom(static_cast<uint64_t>(object.value));
    value ^= mixDeathRandom(static_cast<uint64_t>(request.source.value) << 32u |
                            static_cast<uint64_t>(request.sourceSequence));
    value ^= mixDeathRandom(request.confirmedTick);
    value ^= mixDeathRandom(static_cast<uint64_t>(authoredOrder));
    return mixDeathRandom(value);
}

[[nodiscard]] uint64_t deathRandom(uint64_t key, uint64_t purpose) noexcept {
    return mixDeathRandom(key ^ mixDeathRandom(purpose));
}

[[nodiscard]] uint64_t nextFxEmissionSequence(uint64_t& next) noexcept {
    const uint64_t sequence = next++;
    if (next == 0) next = 1;
    return sequence;
}

[[nodiscard]] uint64_t randomInclusive(uint64_t key, uint64_t purpose,
                                        uint64_t minimum, uint64_t maximum) noexcept {
    if (minimum >= maximum) return maximum;
    const uint64_t width = maximum - minimum + 1u;
    return minimum + deathRandom(key, purpose) % width;
}

// Preserve the legacy inclusive [0, 1] contract without taking a value from
// GameSession's shared mutable RNG stream.  Mapping one PRF word through the
// full uint32 range makes both endpoints reachable, while the Q32.32 result
// keeps all authored probability comparisons deterministic.
[[nodiscard]] math::q32_32 deathRandomUnit(uint64_t key,
                                           uint64_t purpose) noexcept {
    constexpr uint64_t kUnitRaw = uint64_t{1} << 32u;
    constexpr uint64_t kMaximumSample =
        std::numeric_limits<uint32_t>::max();
    const uint64_t sample = static_cast<uint32_t>(deathRandom(key, purpose));
    return math::q32_32::from_raw(static_cast<int64_t>(
        (sample * kUnitRaw) / kMaximumSample));
}

[[nodiscard]] uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept {
    return right > std::numeric_limits<uint64_t>::max() - left
        ? std::numeric_limits<uint64_t>::max()
        : left + right;
}

[[nodiscard]] uint64_t slowDeathWeight(const game::ObjectSlowDeathParameters& parameters,
                                       HealthScalar resolvedDamage,
                                       HealthScalar clippedDamage,
                                       HealthScalar maximumHealth) noexcept {
    int64_t weight = std::max<int32_t>(1, parameters.probabilityModifier);
    if (maximumHealth > kHealthZero && resolvedDamage > clippedDamage) {
        const HealthScalar overkill = resolvedDamage - clippedDamage;
        const HealthScalar bonus = (overkill / maximumHealth) *
            parameters.modifierBonusPerOverkillPercent;
        // RefCode converts the real expression to Int, which truncates
        // toward zero. q32_32::to_int intentionally floors negative values,
        // so spell that conversion out here; a negative authored modifier is
        // allowed to lower a spectacular-death weight, but the final weight
        // still has the original minimum of one.
        const int64_t truncated = bonus.raw() / (int64_t{1} << 32u);
        weight += truncated;
    }
    return static_cast<uint64_t>(std::max<int64_t>(1, weight));
}

[[nodiscard]] constexpr size_t slowPhaseIndex(game::ObjectSlowDeathPhase phase) noexcept {
    return static_cast<size_t>(phase);
}

[[nodiscard]] std::optional<container::String> selectDeathPayload(
    const container::Vector<container::String>& values, uint64_t key, uint64_t purpose) {
    if (values.empty()) return std::nullopt;
    const size_t index = static_cast<size_t>(randomInclusive(
        key, purpose, 0, static_cast<uint64_t>(values.size() - 1u)));
    return values[index];
}

void emitInstantDeathEffect(container::Vector<ObjectInstantDeathEffectEvent>& events,
                            ecs::registry& registry, ecs::entity entity, ObjectId object,
                            const ObjectDamageRequest& request,
                            const game::ObjectDeathReactionRule& rule,
                            uint64_t sessionSeed,
                            uint64_t& nextFxSequence) {
    if (!rule.instantDeath) return;
    const game::ObjectInstantDeathParameters& parameters = *rule.instantDeath;
    const TransformComponent* transform = ecs::try_get<TransformComponent>(registry, entity);
    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, entity);
    const ObjectTerrainLayerComponent* terrainLayer =
        ecs::try_get<ObjectTerrainLayerComponent>(registry, entity);
    const uint64_t randomKey = makeDeathRandomKey(
        sessionSeed, object, request, rule.authoredOrder);
    constexpr uint64_t kInstantDeathPurpose = 0x494e535444454154ull; // "INSTDEAT"
    events.push_back({
        .object = object,
        .source = request.source,
        .sourceSequence = request.sourceSequence,
        .damageType = request.damageType,
        .deathType = request.deathType,
        .position = transform
            ? readAuthoritativeObjectPosition(registry, entity, *transform)
            : LogicFixedVec3{},
        .rotationRadians = transform
            ? readAuthoritativeObjectYaw(registry, entity, *transform)
            : math::q32_32{},
        .owner = owner ? owner->player : INVALID_PLAYER_ID,
        .sourcePathfindLayer = terrainLayer
            ? terrainLayer->pathfindLayer
            : game::terrain::kGroundPathfindLayer,
        .authoredOrder = rule.authoredOrder,
        .fx = selectDeathPayload(parameters.fx, randomKey, kInstantDeathPurpose),
        .ocl = selectDeathPayload(parameters.ocls, randomKey, kInstantDeathPurpose + 1u),
        .weapon = selectDeathPayload(parameters.weapons, randomKey, kInstantDeathPurpose + 2u),
        .fxEmissionSequence = nextFxEmissionSequence(nextFxSequence),
        .confirmedTick = request.confirmedTick,
    });
}

[[nodiscard]] bool isFxListDieActive(const ecs::registry& registry, ecs::entity entity,
                                     uint32_t ruleIndex,
                                     const game::ObjectDeathReactionRule& rule) noexcept {
    if (!rule.fxListDie) return false;
    const ObjectFxListDieRuntimeComponent* runtime =
        ecs::try_get<ObjectFxListDieRuntimeComponent>(registry, entity);
    // Focused low-level fixtures may attach a plan directly without running
    // the normal spawn assembler.  Retain RefCode's StartsActive default in
    // that explicit fallback rather than accidentally activating a
    // TriggeredBy-only module.
    if (!runtime || ruleIndex >= runtime->rules.size()) return rule.fxListDie->startsActive;
    const ObjectFxListDieRuleRuntime& state = runtime->rules[ruleIndex];
    return state.activated && !state.playerConflict;
}

[[nodiscard]] bool allowsFxListDieUpgradeActivation(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    if (!status) return true;
    const game::ObjectStatusMask blocked =
        game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction) |
        game::objectStatusBit(game::ObjectStatusFlag::Destroyed);
    return !status->hasAny(blocked);
}

[[nodiscard]] FxInvocationAnchorSnapshot snapshotFxListDieAnchor(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    FxInvocationAnchorSnapshot snapshot;
    if (const TransformComponent* transform =
            ecs::try_get<TransformComponent>(registry, entity)) {
        snapshot.position = readAuthoritativeObjectPosition(
            registry, entity, *transform);
        snapshot.yawRadians = readAuthoritativeObjectYaw(
            registry, entity, *transform);
    }
    if (const ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, entity);
        physics && physics->ownsAttitude) {
        snapshot.rollRadians = physics->roll;
        snapshot.pitchRadians = physics->pitch;
        snapshot.yawRadians = physics->yaw;
    }
    return snapshot;
}

void emitFxListDieEffect(container::Vector<ObjectFxListDieEffectEvent>& events,
                         const ecs::registry& registry, const ObjectLifecycle& lifecycle,
                         ecs::entity entity, ObjectId object,
                         const ObjectDamageRequest& request,
                         const game::ObjectDeathReactionRule& rule,
                         uint64_t& nextFxSequence) {
    if (!rule.fxListDie || rule.fxListDie->deathFx.empty()) return;
    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, entity);

    std::optional<FxInvocationAnchorSnapshot> secondary;
    if (request.source) {
        if (const std::optional<ecs::entity> sourceEntity =
                lifecycle.entityFromIdIncludingPending(request.source)) {
            secondary = snapshotFxListDieAnchor(registry, *sourceEntity);
        }
    }

    events.push_back({
        .object = object,
        .source = request.source,
        .sourceSequence = request.sourceSequence,
        .damageType = request.damageType,
        .deathType = request.deathType,
        .primary = snapshotFxListDieAnchor(registry, entity),
        .secondary = secondary,
        .owner = owner ? owner->player : INVALID_PLAYER_ID,
        .fx = rule.fxListDie->deathFx,
        .anchor = rule.fxListDie->orientToObject
            ? FxInvocationAnchorKind::ObjectAttachment
            : FxInvocationAnchorKind::WorldPosition,
        .authoredOrder = rule.authoredOrder,
        .fxEmissionSequence = nextFxEmissionSequence(nextFxSequence),
        .confirmedTick = request.confirmedTick,
    });
}

void emitSlowDeathPhase(container::Vector<ObjectSlowDeathPhaseEvent>& events,
                        ObjectId object, const ObjectSlowDeathRuntimeComponent& runtime,
                        const game::ObjectDeathReactionRule& rule,
                        game::ObjectSlowDeathPhase phase, uint64_t confirmedTick,
                        uint32_t sourcePathfindLayer,
                        uint64_t& nextFxSequence) {
    if (!rule.slowDeath) return;
    const game::ObjectSlowDeathParameters& parameters = *rule.slowDeath;
    const uint64_t phaseBase = 0x53504c4f57444541ull +
        static_cast<uint64_t>(slowPhaseIndex(phase)) * 3u;
    events.push_back({
        .object = object,
        .source = runtime.source,
        .sourceSequence = runtime.sourceSequence,
        .authoredOrder = rule.authoredOrder,
        .sourcePathfindLayer = sourcePathfindLayer,
        .phase = phase,
        .fx = selectDeathPayload(parameters.fx[slowPhaseIndex(phase)], runtime.randomKey, phaseBase),
        .ocl = selectDeathPayload(parameters.ocls[slowPhaseIndex(phase)], runtime.randomKey,
                                  phaseBase + 1u),
        .weapon = selectDeathPayload(parameters.weapons[slowPhaseIndex(phase)], runtime.randomKey,
                                     phaseBase + 2u),
        .fxEmissionSequence = nextFxEmissionSequence(nextFxSequence),
        .confirmedTick = confirmedTick,
    });
}

[[nodiscard]] PhysicsScalar lerpDeathRandom(
    PhysicsScalar minimum, PhysicsScalar maximum,
    uint64_t randomKey, uint64_t purpose) noexcept {
    return minimum + (maximum - minimum) *
        deathRandomUnit(randomKey, purpose);
}

void releaseHeldSlavedObjectForSlowDeath(
    ecs::registry& registry, ecs::entity entity,
    uint64_t confirmedTick) {
    if (!isObjectDisabledBy(
            registry, entity, ObjectDisabledReason::Held,
            confirmedTick)) {
        return;
    }
    ObjectSpawnSlaveComponent* slaves =
        ecs::try_get<ObjectSpawnSlaveComponent>(registry, entity);
    if (!slaves || slaves->slaved.empty()) return;

    // RefCode finds SlavedUpdate and calls onSlaverDie(nullptr) before the
    // fling. The modern object may contain more than one authored occurrence;
    // release every occurrence so another controller cannot reassert HELD on
    // the same dead object after the shared object-level flag is cleared.
    for (ObjectSlaveRuntime& slave : slaves->slaved) {
        slave.master = INVALID_OBJECT_ID;
        slave.guardOffset = {};
        slave.returningToMaster = false;
        slave.repairState = ObjectSlaveRepairState::None;
        slave.repairPhaseDueTick = 0;
        slave.repairDestinationValid = false;
        slave.slavedEffectsApplied = false;
        ++slave.revision;
    }
    static_cast<void>(ObjectDisabledSystem::clear(
        registry, entity, ObjectDisabledReason::Held, confirmedTick));
    static_cast<void>(ObjectStatusSystem::apply(
        registry, entity,
        {.clearMask = game::objectStatusBit(
             game::ObjectStatusFlag::Unselectable),
         .confirmedTick = confirmedTick}));
}

void beginSlowDeathFling(
    ecs::registry& registry, ecs::entity entity,
    const game::terrain::TerrainLogic* terrain,
    const game::ObjectSlowDeathParameters& parameters,
    ObjectSlowDeathRuntimeComponent& runtime,
    uint64_t confirmedTick) {
    if (parameters.flingForce <= kPhysicsZero) return;
    ObjectPhysicsComponent* physics =
        ecs::try_get<ObjectPhysicsComponent>(registry, entity);
    TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, entity);
    if (!physics || !transform) return;

    releaseHeldSlavedObjectForSlowDeath(
        registry, entity, confirmedTick);

    LogicFixedVec3 position = readAuthoritativeObjectPosition(
        registry, entity, *transform);
    if (terrain) {
        const PhysicsScalar ground = physicsLayerHeight(
            *terrain, registry, entity, position);
        constexpr PhysicsScalar minimumAltitude{int32_t{1}};
        if (position.z - ground < minimumAltitude) {
            // RefCode adds MIN_ALTITUDE rather than snapping to ground + 1.
            position.z += minimumAltitude;
            writeAuthoritativeObjectPosition(registry, entity, position);
        }
        runtime.minimumFlingAltitudeApplied = true;
    }
    physics->position = position;
    physics->lastPublishedPosition = position;
    physics->hasAuthoritativePosition = true;

    constexpr PhysicsScalar pi = PhysicsScalar::from_raw(13493037705ll);
    const PhysicsScalar magnitude = lerpDeathRandom(
        parameters.flingForce,
        parameters.flingForce + parameters.flingForceVariance,
        runtime.randomKey, 4u);
    const PhysicsScalar pitch = lerpDeathRandom(
        parameters.flingPitchRadians,
        parameters.flingPitchRadians +
            parameters.flingPitchVarianceRadians,
        runtime.randomKey, 5u);
    const PhysicsScalar angle = lerpDeathRandom(
        -pi, pi, runtime.randomKey, 6u);
    const math::q32_32_sincos pitchTrig = math::fixed_sincos(pitch);
    const math::q32_32_sincos angleTrig = math::fixed_sincos(angle);
    const LogicFixedVec3 force{
        magnitude * pitchTrig.cosine * angleTrig.cosine,
        magnitude * pitchTrig.cosine * angleTrig.sine,
        magnitude * pitchTrig.sine,
    };
    physics->pendingForce = addFixed(physics->pendingForce, force);
    physics->allowToFall = true;
    physics->allowBouncing = true;
    physics->forceFreeBodyTranslation = true;
    physics->sleeping = false;
    physics->yaw = math::fixed_atan2(force.y, force.x);
    physics->pitch = {};
    physics->roll = {};
    rebuildPhysicsOrientation(*physics);
    physics->ownsAttitude = true;

    // setExtraFriction(-3 * SECONDS_PER_LOGICFRAME_REAL) is a -3/sec
    // modifier after conversion to the modern per-second representation.
    constexpr PhysicsScalar flingFrictionReduction{int32_t{3}};
    physics->forwardFrictionPerSecond = PhysicsScalar::max(
        kPhysicsZero,
        physics->forwardFrictionPerSecond - flingFrictionReduction);
    physics->lateralFrictionPerSecond = PhysicsScalar::max(
        kPhysicsZero,
        physics->lateralFrictionPerSecond - flingFrictionReduction);
    physics->zFrictionPerSecond = PhysicsScalar::max(
        kPhysicsZero,
        physics->zFrictionPerSecond - flingFrictionReduction);
    physics->aerodynamicFrictionPerSecond = PhysicsScalar::max(
        kPhysicsZero,
        physics->aerodynamicFrictionPerSecond - flingFrictionReduction);

    setPhysicsModelCondition(
        registry, entity, game::ModelConditionFlag::ExplodedBouncing, false);
    setPhysicsModelCondition(
        registry, entity, game::ModelConditionFlag::Parachuting, false);
    setPhysicsModelCondition(
        registry, entity, game::ModelConditionFlag::ExplodedFlailing, true);
    runtime.flungIntoAir = true;
}

void scheduleSlowDeath(ecs::registry& registry, ecs::entity entity, ObjectId object,
                       const ObjectDamageRequest& request,
                       const game::ObjectDeathReactionPlan& plan, uint32_t selectedRuleIndex,
                       const game::ObjectSlowDeathParameters& parameters,
                       HealthScalar resolvedDamage, HealthScalar clippedDamage,
                       HealthScalar maximumHealth, const ObjectSimulationRules& rules,
                       const game::terrain::TerrainLogic* terrain,
                       uint64_t sessionSeed,
                       container::Vector<ObjectDeathEvent>& deathEvents,
                       container::Vector<ObjectSlowDeathPhaseEvent>& phaseEvents,
                       uint64_t& nextFxSequence) {
    if (selectedRuleIndex >= plan.rules.size()) return;
    const game::ObjectDeathReactionRule& rule = plan.rules[selectedRuleIndex];
    const uint64_t randomKey = makeDeathRandomKey(
        sessionSeed, object, request, rule.authoredOrder);
    const uint64_t framesPerSecond = std::max<uint32_t>(1, rules.logicFramesPerSecond);
    const uint64_t sinkDelayTicks = saturatingAdd(
        millisecondsToConfirmedTicks(parameters.sinkDelayMilliseconds,
                                     static_cast<uint32_t>(framesPerSecond)),
        randomInclusive(randomKey, 1u, 0,
            millisecondsToConfirmedTicks(parameters.sinkDelayVarianceMilliseconds,
                                         static_cast<uint32_t>(framesPerSecond))));
    const uint64_t destructionDelayTicks = saturatingAdd(
        millisecondsToConfirmedTicks(parameters.destructionDelayMilliseconds,
                                     static_cast<uint32_t>(framesPerSecond)),
        randomInclusive(randomKey, 2u, 0,
            millisecondsToConfirmedTicks(parameters.destructionDelayVarianceMilliseconds,
                                         static_cast<uint32_t>(framesPerSecond))));
    // RefCode samples midpoint between floor(35%) and floor(65%) of the
    // selected destruction frame. The modern timer uses the same integer
    // interval and keeps LOD scaling fixed at one.
    const uint64_t midpointMinimum = destructionDelayTicks * 35u / 100u;
    const uint64_t midpointMaximum = destructionDelayTicks * 65u / 100u;
    const uint64_t midpointDelayTicks = randomInclusive(
        randomKey, 3u, midpointMinimum, midpointMaximum);
    ObjectSlowDeathRuntimeComponent runtime{
        .selectedRuleIndex = selectedRuleIndex,
        .source = request.source,
        .sourceSequence = request.sourceSequence,
        .randomKey = randomKey,
        .deathTick = request.confirmedTick,
        .sinkTick = saturatingAdd(request.confirmedTick, sinkDelayTicks),
        .midpointTick = saturatingAdd(request.confirmedTick, midpointDelayTicks),
        .destructionTick = saturatingAdd(request.confirmedTick, destructionDelayTicks),
        .initialEmitted = true,
    };
    if (ObjectSlowDeathRuntimeComponent* existing =
            ecs::try_get<ObjectSlowDeathRuntimeComponent>(registry, entity)) {
        *existing = runtime;
    } else {
        ecs::emplace<ObjectSlowDeathRuntimeComponent>(registry, entity, runtime);
    }
    ObjectSlowDeathRuntimeComponent& storedRuntime =
        ecs::get<ObjectSlowDeathRuntimeComponent>(registry, entity);
    beginSlowDeathFling(
        registry, entity, terrain, parameters, storedRuntime,
        request.confirmedTick);
    if (parameters.sinkRateUnitsPerSecond != kPhysicsZero &&
        hasKind(ecs::try_get<ObjectKindOfComponent>(registry, entity),
                game::ObjectKindOf::Infantry)) {
        // Drawable shadow ownership is renderer-side, but the terrain decal
        // fade is already a detached confirmed presentation intent.
        setObjectTerrainDecalFade(
            registry, entity, {}, math::q32_32::from_fraction(1, 5),
            request.confirmedTick);
    }
    const ObjectTerrainLayerComponent* terrainLayer =
        ecs::try_get<ObjectTerrainLayerComponent>(registry, entity);
    emitSlowDeathPhase(
        phaseEvents, object, runtime, rule,
        game::ObjectSlowDeathPhase::Initial, request.confirmedTick,
        terrainLayer ? terrainLayer->pathfindLayer
                     : game::terrain::kGroundPathfindLayer,
        nextFxSequence);
    deathEvents.push_back({
        .kind = ObjectDeathEventKind::ReactionApplied,
        .object = object,
        .source = request.source,
        .reaction = game::ObjectDeathReactionKind::SlowDeath,
        .authoredOrder = rule.authoredOrder,
        .damageType = request.damageType,
        .deathType = request.deathType,
        .confirmedTick = request.confirmedTick,
    });
    static_cast<void>(resolvedDamage);
    static_cast<void>(clippedDamage);
    static_cast<void>(maximumHealth);
}

void updateSlowDeaths(ecs::registry& registry, ObjectLifecycle& lifecycle,
                       const game::terrain::TerrainLogic* terrain,
                       const ObjectSimulationRules& rules,
                       uint64_t confirmedTick,
                       container::Vector<ObjectSlowDeathPhaseEvent>& phaseEvents,
                       uint64_t& nextFxSequence) {
    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectDeathReactionComponent,
                                ObjectSlowDeathRuntimeComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id || !lifecycle.entityFromId(identity.id)) continue;
        candidates.push_back({.object = identity.id, .entity = entity});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left,
                                                         const Candidate& right) {
        return left.object < right.object;
    });

    for (const Candidate& candidate : candidates) {
        ObjectDeathReactionComponent& reaction =
            ecs::get<ObjectDeathReactionComponent>(registry, candidate.entity);
        ObjectSlowDeathRuntimeComponent& runtime =
            ecs::get<ObjectSlowDeathRuntimeComponent>(registry, candidate.entity);
        if (!reaction.plan || runtime.selectedRuleIndex >= reaction.plan->rules.size()) continue;
        const game::ObjectDeathReactionRule& rule =
            reaction.plan->rules[runtime.selectedRuleIndex];
        if (rule.kind != game::ObjectDeathReactionKind::SlowDeath || !rule.slowDeath) continue;
        const game::ObjectSlowDeathParameters& parameters = *rule.slowDeath;
        const ObjectTerrainLayerComponent* terrainLayer =
            ecs::try_get<ObjectTerrainLayerComponent>(registry, candidate.entity);
        const uint32_t sourcePathfindLayer = terrainLayer
            ? terrainLayer->pathfindLayer
            : game::terrain::kGroundPathfindLayer;

        const bool firstMotionPassThisTick =
            !runtime.motionTickInitialized ||
            runtime.lastMotionTick != confirmedTick;
        if (firstMotionPassThisTick) {
            runtime.motionTickInitialized = true;
            runtime.lastMotionTick = confirmedTick;
        }

        TransformComponent* transform =
            ecs::try_get<TransformComponent>(registry, candidate.entity);
        ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, candidate.entity);
        PhysicsScalar ground{};
        bool hasGround = false;
        if (terrain && transform) {
            const LogicFixedVec3 position =
                readAuthoritativeObjectPosition(
                    registry, candidate.entity, *transform);
            ground = physicsLayerHeight(
                *terrain, registry, candidate.entity, position);
            hasGround = true;
        }

        if (runtime.flungIntoAir &&
            !runtime.minimumFlingAltitudeApplied && hasGround &&
            transform && physics) {
            constexpr PhysicsScalar minimumAltitude{int32_t{1}};
            LogicFixedVec3 position = readAuthoritativeObjectPosition(
                registry, candidate.entity, *transform);
            PhysicsScalar z = position.z;
            if (z - ground < minimumAltitude) {
                z += minimumAltitude;
                position.z = z;
                writeAuthoritativeObjectPosition(
                    registry, candidate.entity, position);
                physics->position.z = z;
                physics->lastPublishedPosition.z = z;
                physics->hasAuthoritativePosition = true;
            }
            runtime.minimumFlingAltitudeApplied = true;
        }

        if (runtime.flungIntoAir && !runtime.flingLanded &&
            transform && physics) {
            const ObjectId collided = physics->lastCollidee;
            const std::optional<ecs::entity> collidedEntity = collided
                ? lifecycle.entityFromIdIncludingPending(collided)
                : std::nullopt;
            const ObjectKindOfComponent* collidedKinds = collidedEntity
                ? ecs::try_get<ObjectKindOfComponent>(
                      registry, *collidedEntity)
                : nullptr;
            if (collidedKinds &&
                hasKind(collidedKinds, game::ObjectKindOf::Shrubbery)) {
                runtime.snaggedInShrubbery = true;
                static_cast<void>(ObjectDisabledSystem::setUntil(
                    registry, candidate.entity,
                    ObjectDisabledReason::Held,
                    OBJECT_DISABLED_FOREVER_TICK, confirmedTick));
                setPhysicsModelCondition(
                    registry, candidate.entity,
                    game::ModelConditionFlag::ExplodedFlailing, false);
                setPhysicsModelCondition(
                    registry, candidate.entity,
                    game::ModelConditionFlag::ExplodedBouncing, false);
                setPhysicsModelCondition(
                    registry, candidate.entity,
                    game::ModelConditionFlag::Parachuting, true);
            } else if (hasGround) {
                const PhysicsScalar z = readAuthoritativeObjectPosition(
                    registry, candidate.entity, *transform).z;
                if (z <= ground + kPhysicsGroundEpsilon) {
                    runtime.flingLanded = true;
                    setPhysicsModelCondition(
                        registry, candidate.entity,
                        game::ModelConditionFlag::ExplodedFlailing, false);
                    setPhysicsModelCondition(
                        registry, candidate.entity,
                        game::ModelConditionFlag::ExplodedBouncing, true);
                }
            }

            if (!runtime.flingLanded && firstMotionPassThisTick) {
                // RefCode increments every absolute frame while the flung
                // body has not touched terrain. This pauses Sink/Midpoint/
                // Final without changing their relative authored offsets.
                runtime.sinkTick = saturatingAdd(runtime.sinkTick, 1u);
                runtime.midpointTick = saturatingAdd(
                    runtime.midpointTick, 1u);
                runtime.destructionTick = saturatingAdd(
                    runtime.destructionTick, 1u);
            }
        }

        if (runtime.snaggedInShrubbery && transform &&
            firstMotionPassThisTick) {
            const PhysicsScalar fps{static_cast<int32_t>(
                std::min<uint32_t>(
                    static_cast<uint32_t>(
                        std::numeric_limits<int32_t>::max()),
                    std::max<uint32_t>(
                        1u, rules.logicFramesPerSecond)))};
            const PhysicsScalar amount =
                parameters.sinkRateUnitsPerSecond /
                fps * PhysicsScalar{int32_t{50}};
            LogicFixedVec3 position = readAuthoritativeObjectPosition(
                registry, candidate.entity, *transform);
            const PhysicsScalar nextZ = position.z - amount;
            position.z = nextZ;
            writeAuthoritativeObjectPosition(
                registry, candidate.entity, position);
            if (physics) {
                physics->position.z = nextZ;
                physics->lastPublishedPosition.z = nextZ;
                physics->hasAuthoritativePosition = true;
            }
            if (hasGround && nextZ <= ground) {
                static_cast<void>(lifecycle.requestDestroy(
                    candidate.object, ObjectDestroyReason::Combat,
                    confirmedTick));
                continue;
            }
        }

        if (!runtime.snaggedInShrubbery && transform &&
            firstMotionPassThisTick && confirmedTick >= runtime.sinkTick &&
            parameters.sinkRateUnitsPerSecond > kPhysicsZero) {
            static_cast<void>(ObjectDisabledSystem::setUntil(
                registry, candidate.entity, ObjectDisabledReason::Held,
                OBJECT_DISABLED_FOREVER_TICK, confirmedTick));
            const PhysicsScalar fps{static_cast<int32_t>(
                std::min<uint32_t>(
                    static_cast<uint32_t>(
                        std::numeric_limits<int32_t>::max()),
                    std::max<uint32_t>(
                        1u, rules.logicFramesPerSecond)))};
            LogicFixedVec3 position = readAuthoritativeObjectPosition(
                registry, candidate.entity, *transform);
            const PhysicsScalar nextZ = position.z -
                parameters.sinkRateUnitsPerSecond / fps;
            position.z = nextZ;
            writeAuthoritativeObjectPosition(
                registry, candidate.entity, position);
            if (physics) {
                physics->position.z = nextZ;
                physics->lastPublishedPosition.z = nextZ;
                physics->velocityUnitsPerSecond = {};
                physics->pendingForce = {};
                physics->hasAuthoritativePosition = true;
            }
        }
        if (!runtime.midpointEmitted && confirmedTick >= runtime.midpointTick) {
            emitSlowDeathPhase(phaseEvents, candidate.object, runtime, rule,
                               game::ObjectSlowDeathPhase::Midpoint, confirmedTick,
                               sourcePathfindLayer,
                               nextFxSequence);
            runtime.midpointEmitted = true;
        }
        if (!runtime.finalEmitted && confirmedTick >= runtime.destructionTick) {
            emitSlowDeathPhase(phaseEvents, candidate.object, runtime, rule,
                               game::ObjectSlowDeathPhase::Final, confirmedTick,
                               sourcePathfindLayer,
                               nextFxSequence);
            runtime.finalEmitted = true;
            static_cast<void>(lifecycle.requestDestroy(
                candidate.object, ObjectDestroyReason::Combat, confirmedTick));
        }
    }
}

[[nodiscard]] bool emitCrushDie(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity victimEntity, ObjectId victim,
    const ObjectDamageRequest& request,
    const game::ObjectDeathReactionRule& rule,
    uint64_t sessionSeed,
    container::Vector<ObjectCrushDieEvent>& events) {
    if (request.damageType != game::DamageType::CRUSH || !rule.crushDie) return false;

    ObjectCrushStateComponent* state =
        ecs::try_get<ObjectCrushStateComponent>(registry, victimEntity);
    if (!state) state = &ecs::emplace<ObjectCrushStateComponent>(registry, victimEntity);
    const TransformComponent* victimTransform =
        ecs::try_get<TransformComponent>(registry, victimEntity);
    const LogicFixedVec3 victimPosition = victimTransform
        ? readAuthoritativeObjectPosition(
              registry, victimEntity, *victimTransform)
        : LogicFixedVec3{};
    const ObjectGeometryComponent* geometry =
        ecs::try_get<ObjectGeometryComponent>(registry, victimEntity);

    game::ObjectCrushType type = game::ObjectCrushType::Total;
    const std::optional<ecs::entity> sourceEntity = lifecycle.entityFromId(request.source);
    const TransformComponent* sourceTransform = sourceEntity
        ? ecs::try_get<TransformComponent>(registry, *sourceEntity) : nullptr;
    if (victimTransform && sourceTransform) {
        const LogicFixedVec3 sourcePosition =
            readAuthoritativeObjectPosition(
                registry, *sourceEntity, *sourceTransform);
        const PhysicsScalar offsetDistance = geometry
            ? PhysicsScalar::max(
                  kPhysicsZero, geometry->majorRadiusFixed) /
                  PhysicsScalar{int32_t{2}}
            : PhysicsScalar::from_fraction(1, 2);
        const math::q32_32_sincos direction = math::fixed_sincos(
            readAuthoritativeObjectYaw(
                registry, victimEntity, *victimTransform));
        const PhysicsScalar victimX = victimPosition.x;
        const PhysicsScalar victimY = victimPosition.y;
        const PhysicsScalar sourceX = sourcePosition.x;
        const PhysicsScalar sourceY = sourcePosition.y;
        std::optional<PhysicsScalar> bestDistance;
        type = game::ObjectCrushType::None;
        const auto consider = [&](PhysicsScalar x, PhysicsScalar y,
                                  game::ObjectCrushType candidate,
                                  std::optional<PhysicsScalar>& best,
                                  game::ObjectCrushType& selected) {
            const PhysicsScalar dx = x - sourceX;
            const PhysicsScalar dy = y - sourceY;
            const PhysicsScalar distance = dx * dx + dy * dy;
            if (!best || distance < *best) {
                best = distance;
                selected = candidate;
            }
        };
        if (!state->frontCrushed && !state->backCrushed) {
            consider(victimX, victimY,
                     game::ObjectCrushType::Total, bestDistance, type);
        }
        if (!state->frontCrushed) {
            consider(victimX + direction.cosine * offsetDistance,
                     victimY + direction.sine * offsetDistance,
                     state->backCrushed ? game::ObjectCrushType::Total
                                        : game::ObjectCrushType::FrontEnd,
                     bestDistance, type);
        }
        if (!state->backCrushed) {
            consider(victimX - direction.cosine * offsetDistance,
                     victimY - direction.sine * offsetDistance,
                     state->frontCrushed ? game::ObjectCrushType::Total
                                         : game::ObjectCrushType::BackEnd,
                     bestDistance, type);
        }
    }
    if (type == game::ObjectCrushType::None) return false;

    state->frontCrushed = type == game::ObjectCrushType::Total ||
        type == game::ObjectCrushType::FrontEnd;
    state->backCrushed = type == game::ObjectCrushType::Total ||
        type == game::ObjectCrushType::BackEnd;
    if (RenderModelComponent* visual =
            ecs::try_get<RenderModelComponent>(registry, victimEntity)) {
        static const game::ModelConditionMask allCrush =
            game::modelConditionMaskOf(game::ModelConditionFlag::FrontCrushed, game::ModelConditionFlag::BackCrushed);
        static const game::ModelConditionMask front =
            game::modelConditionMaskOf(game::ModelConditionFlag::FrontCrushed);
        static const game::ModelConditionMask back =
            game::modelConditionMaskOf(game::ModelConditionFlag::BackCrushed);
        visual->modelConditionFlags.clear(allCrush);
        if (state->frontCrushed) {
            visual->modelConditionFlags.words[0] |= front.words[0];
            visual->modelConditionFlags.words[1] |= front.words[1];
        }
        if (state->backCrushed) {
            visual->modelConditionFlags.words[0] |= back.words[0];
            visual->modelConditionFlags.words[1] |= back.words[1];
        }
    }

    const size_t soundIndex = type == game::ObjectCrushType::Total ? 0u
        : type == game::ObjectCrushType::BackEnd ? 1u : 2u;
    std::optional<container::String> audio;
    const game::ObjectCrushDieParameters& parameters = *rule.crushDie;
    const int32_t percent = parameters.soundPercents[soundIndex];
    if (!parameters.sounds[soundIndex].empty() && percent > 0) {
        const uint64_t randomKey = makeDeathRandomKey(
            sessionSeed, victim, request, rule.authoredOrder);
        constexpr uint64_t kCrushSoundPurpose = 0x4352555348534e44ull; // "CRUSHSND"
        if (percent >= 100 ||
            randomInclusive(randomKey, kCrushSoundPurpose, 0, 99) <
                static_cast<uint64_t>(percent)) {
            audio = parameters.sounds[soundIndex];
        }
    }
    events.push_back({
        .object = victim,
        .source = request.source,
        .crushType = type,
        .authoredOrder = rule.authoredOrder,
        .frontCrushed = state->frontCrushed,
        .backCrushed = state->backCrushed,
        .audioEvent = std::move(audio),
        .position = victimPosition,
        .confirmedTick = request.confirmedTick,
    });
    return true;
}

[[nodiscard]] bool significantlyAboveTerrain(
    const ecs::registry& registry, ecs::entity entity,
    const game::terrain::TerrainLogic* terrain,
    const ObjectSimulationRules& rules) noexcept {
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, entity);
    if (transform && terrain && terrain->isLoaded()) {
        const LogicFixedVec3 position =
            readAuthoritativeObjectPosition(registry, entity, *transform);
        const ObjectTerrainLayerComponent* layer =
            ecs::try_get<ObjectTerrainLayerComponent>(registry, entity);
        const math::q32_32 ground = math::q32_32::from_raw(
            terrain->pathfindLayerHeightRawAt(
            layer ? layer->pathfindLayer
                  : game::terrain::kGroundPathfindLayer,
            position.x.raw(), position.y.raw())
            .value_or(terrain->groundHeightRaw(
                position.x.raw(), position.y.raw())));
        const math::q32_32 gravityPerFrame =
            rules.gravityUnitsPerSecondSq * rules.logicDeltaSeconds *
            rules.logicDeltaSeconds;
        const math::q32_32 threshold = math::q32_32::max(
            math::q32_32{}, -math::q32_32{int32_t{9}} * gravityPerFrame);
        return position.z - ground > threshold;
    }
    const ObjectAirborneComponent* airborne =
        ecs::try_get<ObjectAirborneComponent>(registry, entity);
    return airborne && airborne->isAirborne;
}

[[nodiscard]] bool aboveTerrainLayer(
    const ecs::registry& registry, ecs::entity entity,
    const game::terrain::TerrainLogic* terrain) noexcept {
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, entity);
    if (transform && terrain && terrain->isLoaded()) {
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            registry, entity, *transform);
        const ObjectTerrainLayerComponent* layer =
            ecs::try_get<ObjectTerrainLayerComponent>(registry, entity);
        const math::q32_32 surface = math::q32_32::from_raw(
            terrain->pathfindLayerHeightRawAt(
            layer ? layer->pathfindLayer
                  : game::terrain::kGroundPathfindLayer,
            position.x.raw(), position.y.raw())
            .value_or(terrain->groundHeightRaw(
                position.x.raw(), position.y.raw())));
        return position.z > surface;
    }
    const ObjectAirborneComponent* airborne =
        ecs::try_get<ObjectAirborneComponent>(registry, entity);
    return airborne && airborne->isAirborne;
}

void detachDeadAircraftReservations(
    ObjectAirfieldSystem& airfieldSystem, ecs::registry& registry,
    const ObjectLifecycle& lifecycle, ObjectId aircraft,
    uint64_t confirmedTick,
    container::Vector<ObjectAirfieldEvent>& events,
    std::optional<uint32_t> authoredOrder) {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(aircraft);
    ObjectAirfieldComponent* component = entity
        ? ecs::try_get<ObjectAirfieldComponent>(registry, *entity)
        : nullptr;
    if (!entity || !component || component->jetAi.empty()) return;

    const auto selected = [&](size_t index) {
        return !authoredOrder ||
            (component->plan && index < component->plan->jetAi.size() &&
             component->plan->jetAi[index].authoredOrder == *authoredOrder);
    };
    container::Vector<ObjectId> reservedAirfields;
    for (size_t index = 0; index < component->jetAi.size(); ++index) {
        if (!selected(index)) continue;
        const ObjectJetAiRuntime& jet = component->jetAi[index];
        if (jet.reservedAirfield)
            reservedAirfields.push_back(jet.reservedAirfield);
        if (jet.parkingReservation.airfield)
            reservedAirfields.push_back(jet.parkingReservation.airfield);
        if (jet.runwayReservation.airfield)
            reservedAirfields.push_back(jet.runwayReservation.airfield);
    }
    std::sort(reservedAirfields.begin(), reservedAirfields.end());
    reservedAirfields.erase(
        std::unique(reservedAirfields.begin(), reservedAirfields.end()),
        reservedAirfields.end());
    for (const ObjectId airfield : reservedAirfields) {
        static_cast<void>(airfieldSystem.releaseAircraftReservations(
            registry, lifecycle, airfield, aircraft, confirmedTick, events));
    }

    // The host may already be PendingDestroy or may have purged this dead
    // stable ID before the aircraft reaches terrain.  Clear the aircraft
    // reverse edge unconditionally after the best-effort host transaction.
    component = ecs::try_get<ObjectAirfieldComponent>(registry, *entity);
    if (component) {
        for (size_t index = 0; index < component->jetAi.size(); ++index) {
            if (!selected(index)) continue;
            ObjectJetAiRuntime& jet = component->jetAi[index];
            jet.reservedAirfield = INVALID_OBJECT_ID;
            jet.parkingReservation = {};
            jet.runwayReservation = {};
        }
    }
    ecs::remove<ObjectCarrierDeckComponent>(registry, *entity);
    static_cast<void>(ObjectStatusSystem::apply(
        registry, *entity,
        {.clearMask = game::objectStatusBit(
             game::ObjectStatusFlag::DeckHeightOffset),
         .confirmedTick = confirmedTick}));
}

[[nodiscard]] bool emitSpecialPowerCompletion(
    ecs::registry& registry, ecs::entity entity, ObjectId object,
    uint32_t ruleIndex, const game::ObjectDeathReactionRule& rule,
    uint64_t confirmedTick, uint64_t& nextGameplaySubmissionOrdinal,
    container::Vector<ObjectSpecialPowerCompletionEvent>& events) {
    if (rule.kind != game::ObjectDeathReactionKind::SpecialPowerCompletion ||
        !rule.specialPowerCompletionDie ||
        rule.specialPowerCompletionDie->specialPowerTemplate.empty()) {
        return false;
    }
    const ObjectSpecialPowerCompletionRuntimeComponent* runtime =
        ecs::try_get<ObjectSpecialPowerCompletionRuntimeComponent>(registry,
                                                                    entity);
    if (!runtime || ruleIndex >= runtime->rules.size()) return false;
    const ObjectSpecialPowerCompletionRuleRuntime& state =
        runtime->rules[ruleIndex];
    if (!state.creatorSet || !state.creator) return false;
    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(registry, entity);
    if (!owner || !owner->player) return false;
    events.push_back({
        .object = object,
        .creator = state.creator,
        .player = owner->player,
        .specialPowerTemplate =
            rule.specialPowerCompletionDie->specialPowerTemplate,
        .authoredOrder = rule.authoredOrder,
        .submissionOrdinal = nextGameplaySubmissionOrdinal++,
        .confirmedTick = confirmedTick,
    });
    if (nextGameplaySubmissionOrdinal == 0) {
        ++nextGameplaySubmissionOrdinal;
    }
    return true;
}

} // namespace object_simulation_detail

using namespace object_simulation_detail;

void ObjectSimulation::initializeDeathReactionRuntime(
    ecs::registry& registry, ecs::entity entity,
    const UpgradeMask& ownerCompletedUpgrades) const {
    const ObjectDeathReactionComponent* reaction =
        ecs::try_get<ObjectDeathReactionComponent>(registry, entity);
    if (!reaction || !reaction->plan) return;

    ObjectFxListDieRuntimeComponent runtime;
    runtime.rules.resize(reaction->plan->rules.size());
    ObjectSpecialPowerCompletionRuntimeComponent completionRuntime;
    completionRuntime.rules.resize(reaction->plan->rules.size());
    const bool allowUpgradeActivation = allowsFxListDieUpgradeActivation(registry, entity);
    const ObjectUpgradeInventoryComponent* inventory =
        ecs::try_get<ObjectUpgradeInventoryComponent>(registry, entity);
    const UpgradeMask localCompletedUpgrades = inventory
        ? inventory->completed : UpgradeMask{};
    bool hasFxListDie = false;
    bool hasSpecialPowerCompletionDie = false;
    for (size_t index = 0; index < reaction->plan->rules.size(); ++index) {
        const game::ObjectDeathReactionRule& rule = reaction->plan->rules[index];
        if (rule.kind == game::ObjectDeathReactionKind::SpecialPowerCompletion &&
            rule.specialPowerCompletionDie) {
            hasSpecialPowerCompletionDie = true;
        }
        if (rule.kind != game::ObjectDeathReactionKind::FxList || !rule.fxListDie) continue;
        hasFxListDie = true;
        const game::ObjectFxListDieParameters& parameters = *rule.fxListDie;
        ObjectFxListDieRuleRuntime& state = runtime.rules[index];
        // Structural death-reaction materialization has no content catalog yet;
        // conflicts/triggers are rechecked with catalog on upgrade fan-out.
        state.playerConflict = game::objectFxListDieHasUpgradeConflict(
            parameters, ownerCompletedUpgrades, localCompletedUpgrades, nullptr);
        // UpgradeMux's constructor calls giveSelfUpgrade when StartsActive is
        // true, even if a conflict is already present.  The conflict is
        // checked again in FXListDie::onDie, so retain activation and only
        // suppress emission through playerConflict.
        state.activated = parameters.startsActive ||
             (allowUpgradeActivation && !state.playerConflict &&
              game::objectFxListDieUpgradeTriggersSatisfied(
                 parameters, ownerCompletedUpgrades, localCompletedUpgrades,
                 nullptr));
    }
    if (hasFxListDie) {
        if (ObjectFxListDieRuntimeComponent* existing =
                ecs::try_get<ObjectFxListDieRuntimeComponent>(registry, entity)) {
            *existing = std::move(runtime);
        } else {
            ecs::emplace<ObjectFxListDieRuntimeComponent>(registry, entity,
                                                          std::move(runtime));
        }
    }
    if (hasSpecialPowerCompletionDie) {
        if (ObjectSpecialPowerCompletionRuntimeComponent* existing =
                ecs::try_get<ObjectSpecialPowerCompletionRuntimeComponent>(
                    registry, entity)) {
            *existing = std::move(completionRuntime);
        } else {
            ecs::emplace<ObjectSpecialPowerCompletionRuntimeComponent>(
                registry, entity, std::move(completionRuntime));
        }
    }
}

bool ObjectSimulation::setSpecialPowerCompletionCreator(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, ObjectId creator) const noexcept {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    if (!entity) return false;
    const ObjectDeathReactionComponent* reaction =
        ecs::try_get<ObjectDeathReactionComponent>(registry, *entity);
    ObjectSpecialPowerCompletionRuntimeComponent* runtime =
        ecs::try_get<ObjectSpecialPowerCompletionRuntimeComponent>(registry,
                                                                    *entity);
    if (!reaction || !reaction->plan || !runtime) return false;
    const size_t count = std::min(reaction->plan->rules.size(),
                                  runtime->rules.size());
    for (size_t index = 0; index < count; ++index) {
        const game::ObjectDeathReactionRule& rule =
            reaction->plan->rules[index];
        if (rule.kind != game::ObjectDeathReactionKind::SpecialPowerCompletion ||
            !rule.specialPowerCompletionDie) {
            continue;
        }
        ObjectSpecialPowerCompletionRuleRuntime& state =
            runtime->rules[index];
        if (state.creatorSet) return false;
        state.creatorSet = true;
        state.creator = creator;
        return true;
    }
    return false;
}

bool ObjectSimulation::notifySpecialPowerCompletion(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t confirmedTick) {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    if (!entity) return false;
    const ObjectDeathReactionComponent* reaction =
        ecs::try_get<ObjectDeathReactionComponent>(registry, *entity);
    if (!reaction || !reaction->plan) return false;
    for (uint32_t index = 0; index < reaction->plan->rules.size(); ++index) {
        const game::ObjectDeathReactionRule& rule =
            reaction->plan->rules[index];
        if (rule.kind != game::ObjectDeathReactionKind::SpecialPowerCompletion ||
            !rule.specialPowerCompletionDie) {
            continue;
        }
        return emitSpecialPowerCompletion(
            registry, *entity, object, index, rule, confirmedTick,
            object_simulation_detail::state(*this)
                .m_nextGameplaySubmissionOrdinal,
            object_simulation_detail::state(*this).m_specialPowerCompletionEvents);
    }
    return false;
}

} // namespace engine
