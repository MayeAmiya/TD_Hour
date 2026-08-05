#include "game/object/simulation/combat/ObjectLeafletDrop.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>

namespace engine {
namespace {

[[nodiscard]] uint64_t saturatingAdd(uint64_t value,
                                     uint64_t delta) noexcept {
    return delta > std::numeric_limits<uint64_t>::max() - value
        ? std::numeric_limits<uint64_t>::max() : value + delta;
}

[[nodiscard]] uint64_t millisecondsToTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t fps = std::max<uint32_t>(1, framesPerSecond);
    const uint64_t product = static_cast<uint64_t>(milliseconds) * fps;
    return product / 1000u + (product % 1000u != 0 ? 1u : 0u);
}

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] bool hasKind(const ObjectKindOfComponent* kinds,
                           game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

[[nodiscard]] bool withinBoundingSphere(
    const LogicFixedVec3& source, LogicFixedVec3 target,
    const ObjectGeometryComponent* geometry,
    math::q32_32 radius) noexcept {
    const math::q32_32 combined = radius + (geometry
        ? math::q32_32::max(math::q32_32{},
              geometry->boundingSphereRadiusFixed)
        : math::q32_32{});
    // PartitionManager's FROM_BOUNDINGSPHERE_3D uses the geometry centre for
    // Box/Cylinder and the authored transform origin for Sphere.
    if (geometry && geometry->shape != ObjectGeometryShape::Sphere) {
        target.z += math::q32_32::max(math::q32_32{},
            geometry->heightFixed) /
            math::q32_32{int32_t{2}};
    }
    const math::q32_32 dx = target.x - source.x;
    const math::q32_32 dy = target.y - source.y;
    const math::q32_32 dz = target.z - source.z;
    return dx * dx + dy * dy + dz * dz < combined * combined;
}

} // namespace

void ObjectLeafletDropSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    uint32_t logicFramesPerSecond, uint64_t createdAtTick) const {
    const ThingTemplateComponent* objectTemplate =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const container::SharedPtr<const game::ObjectLeafletDropPlan> plan =
        objectTemplate && objectTemplate->archetype
            ? objectTemplate->archetype->leafletDropPlan : nullptr;
    if (!plan || plan->rules.empty()) return;

    ObjectLeafletDropComponent component;
    component.plan = plan;
    component.instances.resize(plan->rules.size());
    for (size_t index = 0; index < plan->rules.size(); ++index) {
        const game::ObjectLeafletDropParameters& rule = plan->rules[index];
        const uint64_t delay = rule.delayAuthored
            ? millisecondsToTicks(rule.delayMilliseconds,
                                  logicFramesPerSecond)
            : 1u;
        component.instances[index].startTick =
            saturatingAdd(createdAtTick, delay);
    }
    ecs::emplace<ObjectLeafletDropComponent>(registry, entity,
                                              std::move(component));
}

void ObjectLeafletDropSystem::disableAttack(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, ecs::entity sourceEntity,
    ObjectId source, const game::ObjectLeafletDropParameters& rule,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick) {
    const TransformComponent* sourceTransform =
        ecs::try_get<TransformComponent>(registry, sourceEntity);
    const OwnerComponent* sourceOwner =
        ecs::try_get<OwnerComponent>(registry, sourceEntity);
    const math::q32_32 radius = math::q32_32::max(
        math::q32_32{}, rule.radius);
    if (!sourceTransform || !sourceOwner ||
        radius <= math::q32_32{}) return;
    const LogicFixedVec3 sourcePosition = readAuthoritativeObjectPosition(
        registry, sourceEntity, *sourceTransform);

    m_currentPositionIndex.querySphereRadiusFixed(
        sourcePosition, radius, m_nearbyScratch);
    const container::Vector<ObjectId>& nearby = m_nearbyScratch;
    for (const ObjectId targetId : nearby) {
        if (!targetId || targetId == source ||
            lifecycle.isPendingDestroy(targetId)) continue;
        const std::optional<ecs::entity> target =
            lifecycle.entityFromId(targetId);
        if (!target) continue;
        const ObjectMapStatusComponent* mapStatus =
            ecs::try_get<ObjectMapStatusComponent>(registry, *target);
        if (mapStatus && mapStatus->offMap) continue;
        const TransformComponent* targetTransform =
            ecs::try_get<TransformComponent>(registry, *target);
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(registry, *target);
        if (!targetTransform || !withinBoundingSphere(
                sourcePosition,
                readAuthoritativeObjectPosition(
                    registry, *target, *targetTransform),
                geometry, radius)) {
            continue;
        }
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, *target);
        if (health && health->effectivelyDead) continue;
        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(registry, *target);
        if (!hasKind(kinds, game::ObjectKindOf::Infantry) &&
            !hasKind(kinds, game::ObjectKindOf::Vehicle)) {
            continue;
        }
        const OwnerComponent* targetOwner =
            ecs::try_get<OwnerComponent>(registry, *target);
        if (!targetOwner || relationshipBetweenObjects(
                registry, players, *target, sourceEntity) !=
                PlayerRelationship::Enemies) {
            continue;
        }
        const uint64_t duration = millisecondsToTicks(
            rule.disabledDurationMilliseconds, logicFramesPerSecond);
        static_cast<void>(ObjectDisabledSystem::setUntil(
            registry, *target, ObjectDisabledReason::Emp,
            saturatingAdd(confirmedTick, duration), confirmedTick));
    }
}

void ObjectLeafletDropSystem::update(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick,
    container::Vector<ObjectLeafletParticleEvent>& outParticles) {
    struct Candidate final { ObjectId object; ecs::entity entity; };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectLeafletDropComponent>(registry);
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
    if (!candidates.empty()) {
        m_currentPositionIndex.rebuild(registry, lifecycle);
    }

    for (const Candidate& candidate : candidates) {
        if (isObjectDisabled(registry, candidate.entity, confirmedTick)) continue;
        ObjectLeafletDropComponent& component =
            ecs::get<ObjectLeafletDropComponent>(registry, candidate.entity);
        if (!component.plan || component.lastUpdatedTick == confirmedTick) continue;
        component.lastUpdatedTick = confirmedTick;
        const size_t count = std::min(component.plan->rules.size(),
                                      component.instances.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectLeafletDropParameters& rule =
                component.plan->rules[index];
            ObjectLeafletDropRuntime& runtime = component.instances[index];
            if (!runtime.particleEmitted) {
                runtime.particleEmitted = true;
                if (!rule.particleSystem.empty()) {
                    outParticles.push_back({
                        .source = candidate.object,
                        .particleSystem = rule.particleSystem,
                        .authoredOrder = rule.authoredOrder,
                        .confirmedTick = confirmedTick,
                    });
                }
            }
            if (confirmedTick < runtime.startTick) continue;
            disableAttack(registry, lifecycle, players, candidate.entity,
                          candidate.object, rule, logicFramesPerSecond,
                          confirmedTick);
        }
    }
}

bool ObjectLeafletDropSystem::onDie(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, ObjectId object,
    uint32_t authoredOrder, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick) {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    if (!entity) return false;
    ObjectLeafletDropComponent* component =
        ecs::try_get<ObjectLeafletDropComponent>(registry, *entity);
    if (!component || !component->plan) return false;
    const size_t count = std::min(component->plan->rules.size(),
                                  component->instances.size());
    for (size_t index = 0; index < count; ++index) {
        const game::ObjectLeafletDropParameters& rule =
            component->plan->rules[index];
        if (rule.authoredOrder != authoredOrder) continue;
        m_currentPositionIndex.rebuild(registry, lifecycle);
        disableAttack(registry, lifecycle, players, *entity, object, rule,
                      logicFramesPerSecond, confirmedTick);
        return true;
    }
    return false;
}

} // namespace engine
