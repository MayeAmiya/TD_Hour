#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/combat/ObjectFireWeaponCollide.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
namespace engine {
namespace {

struct Candidate final {
    ObjectId object = INVALID_OBJECT_ID;
    ecs::entity entity = ecs::null;
};

[[nodiscard]] bool noCollisions(const ecs::registry& registry,
                                ecs::entity entity) noexcept {
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    return status && status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::NoCollisions));
}

[[nodiscard]] bool isTerminallyDead(const ecs::registry& registry,
                                    ecs::entity entity) noexcept {
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    return health && health->terminalDeathIssued;
}

[[nodiscard]] bool excludedFromPhysicalContact(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    if (const ObjectContainedByComponent* contained =
            ecs::try_get<ObjectContainedByComponent>(registry, entity);
        contained && contained->enclosing) {
        return true;
    }
    const ObjectMapStatusComponent* map =
        ecs::try_get<ObjectMapStatusComponent>(registry, entity);
    return map && map->offMap;
}

struct VerticalInterval final {
    math::q32_32 minimum{};
    math::q32_32 maximum{};
};

[[nodiscard]] VerticalInterval verticalInterval(
    const LogicFixedVec3& position,
    const ObjectGeometryComponent* geometry) noexcept {
    if (!geometry) {
        return {position.z, position.z + math::q32_32{int32_t{2}}};
    }
    if (geometry->shape == ObjectGeometryShape::Sphere) {
        const math::q32_32 radius = math::q32_32::max(
            math::q32_32{}, geometry->boundingSphereRadiusFixed);
        return {position.z - radius, position.z + radius};
    }
    return {position.z, position.z + math::q32_32::max(
        math::q32_32{}, geometry->heightFixed)};
}

[[nodiscard]] bool overlaps(
    const ecs::registry& registry,
    ecs::entity leftEntity,
    const ObjectFixedTransformComponent& leftTransform,
    const ObjectGeometryComponent* leftGeometry,
    ecs::entity rightEntity,
    const ObjectFixedTransformComponent& rightTransform,
    const ObjectGeometryComponent* rightGeometry,
    uint64_t confirmedTick) noexcept {
    const LogicFixedVec3 leftEnd = leftTransform.position;
    const LogicFixedVec3 rightEnd = rightTransform.position;
    if (leftGeometry && rightGeometry) {
        LogicFixedVec3 leftStart = leftEnd;
        LogicFixedVec3 rightStart = rightEnd;
        math::q32_32 leftEndYaw = leftTransform.yawRadians;
        math::q32_32 rightEndYaw = rightTransform.yawRadians;
        math::q32_32 leftStartYaw = leftEndYaw;
        math::q32_32 rightStartYaw = rightEndYaw;
        if (const ObjectPhysicsComponent* physics =
                ecs::try_get<ObjectPhysicsComponent>(registry, leftEntity)) {
            if (physics->ownsAttitude) leftEndYaw = physics->yaw;
            if (physics->collisionStartTick == confirmedTick) {
                leftStart = physics->collisionStartPosition;
                leftStartYaw = physics->collisionStartYaw;
            }
        }
        if (const ObjectPhysicsComponent* physics =
                ecs::try_get<ObjectPhysicsComponent>(registry, rightEntity)) {
            if (physics->ownsAttitude) rightEndYaw = physics->yaw;
            if (physics->collisionStartTick == confirmedTick) {
                rightStart = physics->collisionStartPosition;
                rightStartYaw = physics->collisionStartYaw;
            }
        }
        ObjectCollisionContact contact;
        math::q32_32 timeOfImpact{};
        return computeObjectSweptCollisionContact(
            leftStart, leftEnd, leftStartYaw, leftEndYaw, *leftGeometry,
            rightStart, rightEnd, rightStartYaw, rightEndYaw,
            *rightGeometry, timeOfImpact, contact);
    }

    // Generated/tool fixtures without typed Geometry retain the historical
    // conservative fallback. Production objects use the shared fixed-point
    // Sphere/Cylinder/OBB contact path above.
    const math::q32_32 leftRadius = leftGeometry
        ? math::q32_32::max(
              math::q32_32{}, leftGeometry->boundingCircleRadiusFixed)
        : math::q32_32{int32_t{1}};
    const math::q32_32 rightRadius = rightGeometry
        ? math::q32_32::max(
              math::q32_32{}, rightGeometry->boundingCircleRadiusFixed)
        : math::q32_32{int32_t{1}};
    const math::q32_32 dx = leftEnd.x - rightEnd.x;
    const math::q32_32 dy = leftEnd.y - rightEnd.y;
    const math::q32_32 combinedRadius = leftRadius + rightRadius;
    if (dx * dx + dy * dy > combinedRadius * combinedRadius) return false;
    const VerticalInterval left = verticalInterval(leftEnd, leftGeometry);
    const VerticalInterval right = verticalInterval(rightEnd, rightGeometry);
    return left.maximum >= right.minimum && right.maximum >= left.minimum;
}

void advanceSequence(uint32_t& sequence) noexcept {
    ++sequence;
    if (sequence == 0) ++sequence;
}

void advanceSequence(uint64_t& sequence) noexcept {
    ++sequence;
    if (sequence == 0) ++sequence;
}

} // namespace

void ObjectFireWeaponCollideSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content) const {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!type || !type->archetype ||
        !type->archetype->fireWeaponCollidePlan) return;
    ObjectFireWeaponCollideComponent component;
    component.plan = type->archetype->fireWeaponCollidePlan;
    component.instances.resize(component.plan->rules.size());
    for (size_t index = 0; index < component.plan->rules.size(); ++index) {
        component.instances[index].content = content.findWeaponId(
            component.plan->rules[index].collideWeapon);
    }
    if (ObjectFireWeaponCollideComponent* existing =
            ecs::try_get<ObjectFireWeaponCollideComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectFireWeaponCollideComponent>(registry, entity,
                                                        std::move(component));
    }
}

void ObjectFireWeaponCollideSystem::update(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectSpatialIndex& spatialIndex,
    const GameContentSnapshot& content, SimulationRandom& random,
    uint64_t confirmedTick, uint64_t& nextEmissionSequence,
    container::Vector<ObjectSystemWeaponFireCommand>& outCommands) {
    container::Vector<Candidate> sources;
    const auto sourceView = ecs::view<const ObjectIdentityComponent,
                                      ObjectFireWeaponCollideComponent,
                                      const ObjectFixedTransformComponent>(registry);
    sources.reserve(sourceView.size_hint());
    for (const ecs::entity entity : sourceView) {
        const ObjectIdentityComponent& identity =
            sourceView.template get<const ObjectIdentityComponent>(entity);
        const ObjectFixedTransformComponent& transform =
            sourceView.template get<
                const ObjectFixedTransformComponent>(entity);
        if (!identity.id || lifecycle.isPendingDestroy(identity.id) ||
            !transform.authoritative ||
            noCollisions(registry, entity) ||
            excludedFromPhysicalContact(registry, entity) ||
            isTerminallyDead(registry, entity)) continue;
        sources.push_back({identity.id, entity});
    }
    std::sort(sources.begin(), sources.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
    });
    if (sources.empty()) return;

    // One subsystem-owned ordered result buffer serves every source and keeps
    // its capacity across updates. It is consumed completely before the next
    // query clears it, so no pointer/span escapes this loop.
    auto& nearby = m_nearbyScratch;
    for (const Candidate& source : sources) {
        ObjectFireWeaponCollideComponent& component =
            ecs::get<ObjectFireWeaponCollideComponent>(registry,
                                                        source.entity);
        if (!component.plan) continue;
        const ObjectFixedTransformComponent& sourceTransform =
            ecs::get<const ObjectFixedTransformComponent>(registry,
                                                           source.entity);
        const ObjectGeometryComponent* sourceGeometry =
            ecs::try_get<ObjectGeometryComponent>(registry, source.entity);
        const LogicFixedVec3 sourcePosition = sourceTransform.position;
        math::q32_32 sourceRadius = sourceGeometry
            ? math::q32_32::max(math::q32_32{},
                  sourceGeometry->boundingCircleRadiusFixed)
            : math::q32_32{int32_t{1}};
        if (const ObjectPhysicsComponent* physics =
                ecs::try_get<ObjectPhysicsComponent>(registry,
                                                     source.entity);
            physics && physics->collisionStartTick == confirmedTick) {
            const math::q32_32 dx = sourcePosition.x -
                physics->collisionStartPosition.x;
            const math::q32_32 dy = sourcePosition.y -
                physics->collisionStartPosition.y;
            const math::q32_32 dz = sourcePosition.z -
                physics->collisionStartPosition.z;
            sourceRadius += math::q32_32::sqrt(
                dx * dx + dy * dy + dz * dz);
        }
        spatialIndex.queryRadiusFixed(
            sourcePosition, sourceRadius, nearby);
        // A fast target can cross the source and finish outside the final-
        // position query. Physics keeps the exact start pose, so include the
        // small current-tick moving set and let the shared swept narrow phase
        // reject non-contacts deterministically.
        const auto movingPhysics = ecs::view<
            const ObjectIdentityComponent,
            const ObjectPhysicsComponent>(registry);
        for (const ecs::entity entity : movingPhysics) {
            const ObjectPhysicsComponent& physics =
                movingPhysics.template get<const ObjectPhysicsComponent>(
                    entity);
            if (physics.collisionStartTick != confirmedTick) continue;
            const ObjectId object =
                movingPhysics.template get<const ObjectIdentityComponent>(
                    entity).id;
            if (object) nearby.push_back(object);
        }
        std::sort(nearby.begin(), nearby.end());
        nearby.erase(std::unique(nearby.begin(), nearby.end()), nearby.end());
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, source.entity);
        const game::ObjectStatusMask sourceStatus = status ? status->flags : 0;
        const size_t count = std::min(component.plan->rules.size(),
                                      component.instances.size());
        for (size_t ruleIndex = 0; ruleIndex < count; ++ruleIndex) {
            const game::ObjectFireWeaponCollideRule& rule =
                component.plan->rules[ruleIndex];
            ObjectFireWeaponCollideRuleRuntime& runtime =
                component.instances[ruleIndex];
            if (!runtime.content ||
                (sourceStatus & rule.requiredStatus) != rule.requiredStatus ||
                (sourceStatus & rule.forbiddenStatus) != 0 ||
                (rule.fireOnce && runtime.everFired)) continue;

            for (const ObjectId targetObject : nearby) {
                if (targetObject == source.object) continue;
                const std::optional<ecs::entity> targetEntity =
                    lifecycle.entityFromId(targetObject);
                if (!targetEntity || noCollisions(registry, *targetEntity) ||
                    isTerminallyDead(registry, *targetEntity)) continue;
                const ObjectFixedTransformComponent* targetTransform =
                    ecs::try_get<ObjectFixedTransformComponent>(
                        registry, *targetEntity);
                if (!targetTransform || !targetTransform->authoritative)
                    continue;
                if (!overlaps(
                        registry, source.entity, sourceTransform,
                        sourceGeometry, *targetEntity, *targetTransform,
                        ecs::try_get<ObjectGeometryComponent>(
                            registry, *targetEntity),
                        confirmedTick)) continue;
                const uint32_t shotSequence = runtime.nextShotSequence;
                if (!queueObjectTargetedTransientWeaponFire(
                        runtime.content, registry, source.entity,
                        source.object, *targetEntity, targetObject,
                        content, random, shotSequence, rule.authoredOrder,
                        nextEmissionSequence, confirmedTick, outCommands)) {
                    continue;
                }
                advanceSequence(runtime.nextShotSequence);
                advanceSequence(nextEmissionSequence);
                runtime.everFired = true;
                if (rule.fireOnce) break;
            }
        }
    }
}

} // namespace engine
