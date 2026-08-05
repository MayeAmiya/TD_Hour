#include "game/object/simulation/status/ObjectCheckpoint.h"

#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/runtime/ObjectVisionRange.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>

namespace engine {
namespace {

struct Candidate final {
    ObjectId object = INVALID_OBJECT_ID;
    ecs::entity entity = ecs::null;
};

[[nodiscard]] uint64_t millisecondsToFrames(
    uint32_t milliseconds, uint32_t logicFramesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t fps = std::max<uint32_t>(1u, logicFramesPerSecond);
    return (static_cast<uint64_t>(milliseconds) * fps + 999u) / 1000u;
}

[[nodiscard]] uint64_t saturatingAdd(uint64_t left,
                                     uint64_t right) noexcept {
    if (left > std::numeric_limits<uint64_t>::max() - right) {
        return std::numeric_limits<uint64_t>::max();
    }
    return left + right;
}

[[nodiscard]] bool canParticipate(const ecs::registry& registry,
                                  const ObjectLifecycle& lifecycle,
                                  ObjectId object,
                                  ecs::entity entity) noexcept {
    if (!object || lifecycle.isPendingDestroy(object)) return false;
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    if (health && health->effectivelyDead) return false;
    const ObjectMapStatusComponent* map =
        ecs::try_get<ObjectMapStatusComponent>(registry, entity);
    if (map && map->offMap) return false;
    return !ecs::try_get<ObjectContainedByComponent>(registry, entity);
}

void scanNearby(const ecs::registry& registry,
                const ObjectLifecycle& lifecycle,
                const PlayerRegistry& players,
                const Candidate& checkpoint,
                bool& enemyNear, bool& allyNear) {
    enemyNear = false;
    allyNear = false;
    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(registry, checkpoint.entity);
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, checkpoint.entity);
    if (!owner || !owner->player || !transform) return;
    const math::q32_32 range = math::q32_32::max(
        math::q32_32{},
        effectiveObjectVisionRangeFixed(registry, checkpoint.entity));
    if (range <= math::q32_32{}) return;
    const math::q32_32 rangeSquared = range * range;
    const LogicFixedVec3 checkpointPosition =
        readAuthoritativeObjectPosition(
            registry, checkpoint.entity, *transform);

    container::Vector<Candidate> objects;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const OwnerComponent,
                                const TransformComponent>(registry);
    objects.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (identity.id == checkpoint.object ||
            !canParticipate(registry, lifecycle, identity.id, entity)) {
            continue;
        }
        objects.push_back({identity.id, entity});
    }
    std::sort(objects.begin(), objects.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.object < right.object;
              });

    for (const Candidate& candidate : objects) {
        const TransformComponent& otherTransform =
            ecs::get<TransformComponent>(registry, candidate.entity);
        const LogicFixedVec3 otherPosition =
            readAuthoritativeObjectPosition(
                registry, candidate.entity, otherTransform);
        const math::q32_32 dx =
            otherPosition.x - checkpointPosition.x;
        const math::q32_32 dy =
            otherPosition.y - checkpointPosition.y;
        if (dx * dx + dy * dy > rangeSquared) continue;
        const PlayerRelationship relationship = relationshipBetweenObjects(
            registry, players, checkpoint.entity, candidate.entity);
        enemyNear = enemyNear || relationship == PlayerRelationship::Enemies;
        allyNear = allyNear || relationship == PlayerRelationship::Allies;
        if (enemyNear && allyNear) return;
    }
}

void refreshDerivedGeometry(ObjectGeometryComponent& geometry) noexcept {
    geometry.majorRadiusFixed = math::q32_32::max(
        math::q32_32{}, geometry.majorRadiusFixed);
    geometry.minorRadiusFixed = math::q32_32::max(
        math::q32_32{}, geometry.minorRadiusFixed);
    geometry.heightFixed = math::q32_32::max(
        math::q32_32{}, geometry.heightFixed);
    switch (geometry.shape) {
    case ObjectGeometryShape::Sphere:
        geometry.minorRadiusFixed = geometry.majorRadiusFixed;
        geometry.heightFixed = geometry.majorRadiusFixed;
        geometry.boundingCircleRadiusFixed = geometry.majorRadiusFixed;
        geometry.boundingSphereRadiusFixed = geometry.majorRadiusFixed;
        break;
    case ObjectGeometryShape::Cylinder:
        geometry.boundingCircleRadiusFixed = geometry.majorRadiusFixed;
        geometry.boundingSphereRadiusFixed = math::q32_32::max(
            geometry.majorRadiusFixed,
            geometry.heightFixed / math::q32_32{int32_t{2}});
        break;
    case ObjectGeometryShape::Box:
        {
            const math::q32_32 halfHeight = geometry.heightFixed /
                math::q32_32{int32_t{2}};
            geometry.boundingCircleRadiusFixed = math::q32_32::sqrt(
                geometry.majorRadiusFixed * geometry.majorRadiusFixed +
                geometry.minorRadiusFixed * geometry.minorRadiusFixed);
            geometry.boundingSphereRadiusFixed = math::q32_32::sqrt(
                geometry.majorRadiusFixed * geometry.majorRadiusFixed +
                geometry.minorRadiusFixed * geometry.minorRadiusFixed +
                halfHeight * halfHeight);
        }
        break;
    }
}

} // namespace

void ObjectCheckpointSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    const ObjectSimulationRules& rules, SimulationRandom* random) const {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!type || !type->archetype || !type->archetype->checkpointPlan) return;
    const ObjectGeometryComponent* geometry =
        ecs::try_get<ObjectGeometryComponent>(registry, entity);
    const ObjectLifecycleComponent* lifecycle =
        ecs::try_get<ObjectLifecycleComponent>(registry, entity);
    const uint64_t createdAtTick = lifecycle ? lifecycle->createdAtTick : 0u;

    ObjectCheckpointComponent value{
        .plan = type->archetype->checkpointPlan,
    };
    value.instances.reserve(value.plan->rules.size());
    for (const game::ObjectCheckpointRule& rule : value.plan->rules) {
        const uint64_t delayFrames = millisecondsToFrames(
            rule.scanDelayMilliseconds, rules.logicFramesPerSecond);
        uint64_t initialDelay = 0;
        if (random && delayFrames != 0) {
            const int32_t maximum = static_cast<int32_t>(std::min<uint64_t>(
                delayFrames,
                static_cast<uint64_t>(std::numeric_limits<int32_t>::max())));
            initialDelay = static_cast<uint64_t>(
                random->integerInclusive(0, maximum));
        }
        value.instances.push_back({
            .nextScanTick = saturatingAdd(createdAtTick, initialDelay),
            .maximumMinorRadius = geometry
                ? math::q32_32::max(
                      math::q32_32{}, geometry->minorRadiusFixed)
                : math::q32_32{},
        });
    }
    if (ObjectCheckpointComponent* existing =
            ecs::try_get<ObjectCheckpointComponent>(registry, entity)) {
        *existing = std::move(value);
    } else {
        ecs::emplace<ObjectCheckpointComponent>(registry, entity,
                                                std::move(value));
    }
}

void ObjectCheckpointSystem::update(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, const ObjectSimulationRules& rules,
    uint64_t confirmedTick, uint64_t& nextGameplaySubmissionOrdinal,
    container::Vector<ObjectCheckpointNavigationEvent>&
        outNavigationEvents) const {
    container::Vector<Candidate> checkpoints;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectCheckpointComponent,
                                ObjectGeometryComponent,
                                RenderModelComponent>(registry);
    checkpoints.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (!canParticipate(registry, lifecycle, identity.id, entity)) continue;
        checkpoints.push_back({identity.id, entity});
    }
    std::sort(checkpoints.begin(), checkpoints.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.object < right.object;
              });

    for (const Candidate& checkpoint : checkpoints) {
        ObjectCheckpointComponent& component =
            ecs::get<ObjectCheckpointComponent>(registry, checkpoint.entity);
        if (!component.plan ||
            component.instances.size() != component.plan->rules.size()) {
            continue;
        }
        ObjectGeometryComponent& geometry =
            ecs::get<ObjectGeometryComponent>(registry, checkpoint.entity);
        bool footprintChanged = false;
        uint32_t changeAuthoredOrder = 0;
        for (size_t index = 0; index < component.instances.size(); ++index) {
            ObjectCheckpointRuntime& runtime = component.instances[index];
            if (confirmedTick >= runtime.nextScanTick) {
                const bool wasEnemyNear = runtime.enemyNear;
                const bool wasAllyNear = runtime.allyNear;
                scanNearby(registry, lifecycle, players, checkpoint,
                           runtime.enemyNear, runtime.allyNear);
                const uint64_t delayFrames = millisecondsToFrames(
                    component.plan->rules[index].scanDelayMilliseconds,
                    rules.logicFramesPerSecond);
                runtime.nextScanTick = saturatingAdd(
                    confirmedTick, std::max<uint64_t>(1u, delayFrames));
                if (wasEnemyNear != runtime.enemyNear ||
                    wasAllyNear != runtime.allyNear) {
                    publishObjectModelConditionDoor(
                        registry, checkpoint.entity,
                        ObjectModelConditionDoorSource::Checkpoint, 0,
                        (!runtime.enemyNear && runtime.allyNear)
                            ? ObjectModelConditionDoorPhase::Opening
                            : ObjectModelConditionDoorPhase::Closing,
                        confirmedTick,
                        component.plan->rules[index].authoredOrder);
                }
            }

            const bool open = !runtime.enemyNear && runtime.allyNear;
            const math::q32_32 radiusStepPerLogicFrame =
                math::q32_32::from_fraction(333, 1000);
            const int64_t previousBoundingCircleRadiusRaw =
                geometry.boundingCircleRadiusFixed.raw();
            geometry.minorRadiusFixed = open
                ? math::q32_32::max(
                      math::q32_32{},
                      geometry.minorRadiusFixed - radiusStepPerLogicFrame)
                : math::q32_32::min(
                      runtime.maximumMinorRadius,
                      geometry.minorRadiusFixed + radiusStepPerLogicFrame);
            refreshDerivedGeometry(geometry);
            if (geometry.boundingCircleRadiusFixed.raw() !=
                    previousBoundingCircleRadiusRaw) {
                footprintChanged = true;
                changeAuthoredOrder =
                    component.plan->rules[index].authoredOrder;
            }
        }
        if (footprintChanged) {
            outNavigationEvents.push_back({
                .object = checkpoint.object,
                .authoredOrder = changeAuthoredOrder,
                .confirmedTick = confirmedTick,
                .submissionOrdinal = nextGameplaySubmissionOrdinal++,
            });
            if (nextGameplaySubmissionOrdinal == 0) {
                ++nextGameplaySubmissionOrdinal;
            }
        }
    }
}

} // namespace engine
