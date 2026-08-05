#include "game/object/simulation/status/ObjectEnemyNear.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/runtime/ObjectVisionRange.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/MapVisibilityAuthority.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace engine {
namespace {

[[nodiscard]] uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept {
    if (left > std::numeric_limits<uint64_t>::max() - right) {
        return std::numeric_limits<uint64_t>::max();
    }
    return left + right;
}

[[nodiscard]] uint64_t millisecondsToFrames(
    uint32_t milliseconds, uint32_t logicFramesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t fps = std::max<uint32_t>(1u, logicFramesPerSecond);
    return (static_cast<uint64_t>(milliseconds) * fps + 999u) / 1000u;
}

[[nodiscard]] bool hasStatus(
    const ecs::registry& registry, ecs::entity entity,
    game::ObjectStatusMask mask) noexcept {
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    return status && status->hasAny(mask);
}

[[nodiscard]] bool canParticipate(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    constexpr game::ObjectStatusMask kRejected =
        game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction) |
        game::objectStatusBit(game::ObjectStatusFlag::Sold) |
        game::objectStatusBit(game::ObjectStatusFlag::Destroyed);
    if (hasStatus(registry, entity, kRejected)) return false;

    const ObjectLifecycleComponent* lifecycle =
        ecs::try_get<ObjectLifecycleComponent>(registry, entity);
    if (lifecycle && lifecycle->phase != ObjectLifecyclePhase::Alive) {
        return false;
    }
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    if (health && health->effectivelyDead) return false;
    const ObjectMapStatusComponent* map =
        ecs::try_get<ObjectMapStatusComponent>(registry, entity);
    return !map || !map->offMap;
}

[[nodiscard]] math::q32_32 visionRangeFor(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    return effectiveObjectVisionRangeFixed(registry, entity);
}

// RefCode's findClosestEnemy(..., AI::CAN_SEE) applies a gameplay perception
// filter before EnemyNear accepts a candidate.  Read only the completed
// visibility snapshot and authoritative object state here: renderer-local
// observer visibility must never feed deterministic simulation.
[[nodiscard]] bool isPerceivedBy(
    const ecs::registry& registry, PlayerId viewer,
    ecs::entity targetEntity,
    const game::terrain::MapVisibilitySnapshot* visibility) noexcept {
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, targetEntity);
    if (status &&
        status->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::Stealthed)) &&
        !status->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::Detected)) &&
        !status->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::Disguised))) {
        return false;
    }

    // Disabled shroud intentionally means every object is visible.  A null
    // snapshot is also the non-map/focused-simulation fallback; do not make
    // EnemyNear permanently false merely because no terrain authority exists.
    if (!visibility || !visibility->renderingActive) return true;

    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, targetEntity);
    if (!viewer || !transform) return false;
    const ObjectGeometryComponent* geometry =
        ecs::try_get<ObjectGeometryComponent>(registry, targetEntity);
    const engine::LogicFixedVec3 position =
        engine::readAuthoritativeObjectPosition(
            registry, targetEntity, *transform);
    const math::q32_32 radius = geometry
        ? math::q32_32::max(
              math::q32_32{}, geometry->boundingCircleRadiusFixed)
        : math::q32_32{};
    return visibility->footprintHasClearCellRaw(
        viewer, position.x.raw(), position.y.raw(), radius.raw());
}

[[nodiscard]] bool hasEnemyInRange(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, ecs::entity sourceEntity,
    ObjectId sourceObject,
    const game::terrain::MapVisibilitySnapshot* visibility) {
    if (!sourceObject || !canParticipate(registry, sourceEntity)) return false;
    const OwnerComponent* sourceOwner =
        ecs::try_get<OwnerComponent>(registry, sourceEntity);
    const TransformComponent* sourceTransform =
        ecs::try_get<TransformComponent>(registry, sourceEntity);
    if (!sourceOwner || !sourceOwner->player || !sourceTransform) return false;

    const math::q32_32 range = visionRangeFor(registry, sourceEntity);
    if (range <= math::q32_32{}) return false;
    const math::q32_32 rangeSquared = range * range;
    const LogicFixedVec3 sourcePosition = readAuthoritativeObjectPosition(
        registry, sourceEntity, *sourceTransform);

    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const OwnerComponent,
                                const TransformComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id || identity.id == sourceObject ||
            lifecycle.isPendingDestroy(identity.id)) {
            continue;
        }
        candidates.push_back({identity.id, entity});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.object < right.object;
              });

    for (const Candidate& candidate : candidates) {
        if (!canParticipate(registry, candidate.entity)) continue;
        const OwnerComponent& targetOwner =
            ecs::get<OwnerComponent>(registry, candidate.entity);
        if (!targetOwner.player ||
            relationshipBetweenObjects(
                registry, players, sourceEntity, candidate.entity) !=
                PlayerRelationship::Enemies) {
            continue;
        }
        if (!isPerceivedBy(registry, sourceOwner->player, candidate.entity,
                           visibility)) {
            continue;
        }
        const TransformComponent& target =
            ecs::get<TransformComponent>(registry, candidate.entity);
        const LogicFixedVec3 targetPosition = readAuthoritativeObjectPosition(
            registry, candidate.entity, target);
        const math::q32_32 dx = targetPosition.x - sourcePosition.x;
        const math::q32_32 dy = targetPosition.y - sourcePosition.y;
        const math::q32_32 dz = targetPosition.z - sourcePosition.z;
        if (dx * dx + dy * dy + dz * dz <= rangeSquared) return true;
    }
    return false;
}

void setEnemyNearModelCondition(ecs::registry& registry, ecs::entity entity,
                                bool enabled) {
    RenderModelComponent* visual =
        ecs::try_get<RenderModelComponent>(registry, entity);
    if (!visual) return;
    static const game::ModelConditionMask enemyNear =
        game::modelConditionMaskOf(game::ModelConditionFlag::EnemyNear);
    if (!enabled) {
        visual->modelConditionFlags.clear(enemyNear);
        return;
    }
    for (size_t index = 0; index < visual->modelConditionFlags.words.size();
         ++index) {
        visual->modelConditionFlags.words[index] |= enemyNear.words[index];
    }
}

} // namespace

void ObjectEnemyNearSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    const ObjectSimulationRules& rules, SimulationRandom* random) const {
    const ThingTemplateComponent* templateComponent =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const container::SharedPtr<const game::ObjectEnemyNearPlan> plan =
        templateComponent && templateComponent->archetype
            ? templateComponent->archetype->enemyNearPlan
            : nullptr;
    if (!plan || plan->rules.empty()) return;

    const ObjectLifecycleComponent* lifecycle =
        ecs::try_get<ObjectLifecycleComponent>(registry, entity);
    const uint64_t createdAtTick = lifecycle ? lifecycle->createdAtTick : 0u;
    ObjectEnemyNearComponent component;
    component.plan = plan;
    component.instances.resize(plan->rules.size());
    for (size_t index = 0; index < plan->rules.size(); ++index) {
        const uint64_t delayFrames = millisecondsToFrames(
            plan->rules[index].scanDelayMilliseconds,
            rules.logicFramesPerSecond);
        const int32_t jitterMaximum =
            delayFrames > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())
                ? std::numeric_limits<int32_t>::max()
                : static_cast<int32_t>(delayFrames);
        const uint64_t jitter = random && jitterMaximum > 0
            ? static_cast<uint64_t>(
                  random->integerInclusive(0, jitterMaximum))
            : 0u;
        component.instances[index].nextScanTick =
            saturatingAdd(createdAtTick, jitter);
    }

    if (ObjectEnemyNearComponent* existing =
            ecs::try_get<ObjectEnemyNearComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectEnemyNearComponent>(registry, entity,
                                               std::move(component));
    }
}

void ObjectEnemyNearSystem::update(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, const ObjectSimulationRules& rules,
    const game::terrain::MapVisibilitySnapshot* visibility,
    uint64_t confirmedTick) const {
    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectEnemyNearComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (identity.id && !lifecycle.isPendingDestroy(identity.id)) {
            candidates.push_back({identity.id, entity});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.object < right.object;
              });

    for (const Candidate& candidate : candidates) {
        ObjectEnemyNearComponent& component =
            ecs::get<ObjectEnemyNearComponent>(registry, candidate.entity);
        if (!component.plan ||
            component.instances.size() != component.plan->rules.size()) {
            continue;
        }
        bool anyEnemyNear = false;
        bool scanned = false;
        for (size_t index = 0; index < component.instances.size(); ++index) {
            ObjectEnemyNearRuntime& runtime = component.instances[index];
            anyEnemyNear = anyEnemyNear || runtime.enemyNear;
            if (confirmedTick < runtime.nextScanTick) continue;

            scanned = true;
            const uint64_t interval = millisecondsToFrames(
                component.plan->rules[index].scanDelayMilliseconds,
                rules.logicFramesPerSecond);
            runtime.nextScanTick =
                saturatingAdd(confirmedTick, std::max<uint64_t>(1u, interval));
            runtime.enemyNear = hasEnemyInRange(
                registry, lifecycle, players, candidate.entity,
                candidate.object, visibility);
        }

        bool nextAnyEnemyNear = false;
        for (const ObjectEnemyNearRuntime& runtime : component.instances) {
            nextAnyEnemyNear = nextAnyEnemyNear || runtime.enemyNear;
        }
        if (scanned && nextAnyEnemyNear != anyEnemyNear) {
            setEnemyNearModelCondition(registry, candidate.entity,
                                       nextAnyEnemyNear);
        }
    }
}

} // namespace engine
