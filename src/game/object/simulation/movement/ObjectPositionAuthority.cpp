#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "debug/debug.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"

namespace engine {

LogicFixedVec3 readAuthoritativeObjectPosition(const ecs::registry& registry,
                                               ecs::entity entity,
                                               const TransformComponent& transform) noexcept {
    // Every ObjectProjectileComponent denotes one of the typed translation
    // owners (DumbBezier, MissileAI or NeutronMissile). Placed helpers do not
    // carry this component and continue through their own Physics/Transform
    // authority after Weapon positions them at the destination.
    if (const ObjectProjectileComponent* projectile =
            ecs::try_get<ObjectProjectileComponent>(registry, entity);
        projectile) {
        return projectile->position;
    }
    if (const ObjectFixedTransformComponent* fixedTransform =
            ecs::try_get<ObjectFixedTransformComponent>(registry, entity);
        fixedTransform && fixedTransform->authoritative) {
        return fixedTransform->position;
    }
    if (const ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, entity);
        physics && physics->hasAuthoritativePosition) {
        return physics->position;
    }
    static_cast<void>(transform);
    TD_ASSERT_MSG(false,
                  "live simulation object is missing fixed position authority");
    return {};
}

math::q32_32 readAuthoritativeObjectYaw(
    const ecs::registry& registry, ecs::entity entity,
    const TransformComponent& transform) noexcept {
    if (const ObjectFixedTransformComponent* fixedTransform =
            ecs::try_get<ObjectFixedTransformComponent>(registry, entity);
        fixedTransform && fixedTransform->authoritative) {
        return fixedTransform->yawRadians;
    }
    if (const ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, entity);
        physics && physics->hasAuthoritativePosition) {
        return physics->yaw;
    }
    static_cast<void>(transform);
    TD_ASSERT_MSG(false,
                  "live simulation object is missing fixed yaw authority");
    return {};
}

void writeAuthoritativeObjectPosition(ecs::registry& registry, ecs::entity entity,
                                      const LogicFixedVec3& position) noexcept {
    bool changed = false;
    if (ObjectFixedTransformComponent* fixedTransform =
            ecs::try_get<ObjectFixedTransformComponent>(registry, entity)) {
        changed = fixedTransform->position.x != position.x ||
            fixedTransform->position.y != position.y ||
            fixedTransform->position.z != position.z ||
            !fixedTransform->authoritative;
        fixedTransform->position = position;
        fixedTransform->authoritative = true;
    }
    if (TransformComponent* transform = ecs::try_get<TransformComponent>(registry, entity)) {
        transform->x = position.x.to_float();
        transform->y = position.y.to_float();
        transform->z = position.z.to_float();
    }

    // Object::setPosition is a coherent object-level write. Existing owner
    // projections are updated together; no velocity/force is synthesized.
    if (ObjectProjectileComponent* projectile =
            ecs::try_get<ObjectProjectileComponent>(registry, entity)) {
        projectile->position = position;
    }
    if (ObjectPhysicsComponent* physics = ecs::try_get<ObjectPhysicsComponent>(registry, entity)) {
        physics->position = position;
        physics->lastPublishedPosition = position;
        physics->hasAuthoritativePosition = true;
    }
    if (changed) {
        markObjectDirty(
            registry, entity,
            objectDirtyBit(ObjectDirtyDomain::Spatial) |
                objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
    }
}

void writeAuthoritativeObjectYaw(ecs::registry& registry, ecs::entity entity,
                                 math::q32_32 yawRadians) noexcept {
    bool changed = false;
    if (ObjectFixedTransformComponent* fixedTransform =
            ecs::try_get<ObjectFixedTransformComponent>(registry, entity)) {
        changed = fixedTransform->yawRadians != yawRadians ||
            !fixedTransform->authoritative;
        fixedTransform->yawRadians = yawRadians;
        fixedTransform->authoritative = true;
    }
    if (TransformComponent* transform =
            ecs::try_get<TransformComponent>(registry, entity)) {
        transform->rotation = yawRadians.to_float();
    }
    if (ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, entity)) {
        physics->yaw = yawRadians;
        physics->lastPublishedYaw = yawRadians;
    }
    if (changed) {
        markObjectDirty(
            registry, entity, ObjectDirtyDomain::RenderExtraction);
    }
}

void writeAuthoritativeObjectTransform(ecs::registry& registry,
                                       ecs::entity entity,
                                       const LogicFixedVec3& position,
                                       math::q32_32 yawRadians) noexcept {
    writeAuthoritativeObjectPosition(registry, entity, position);
    writeAuthoritativeObjectYaw(registry, entity, yawRadians);
}

} // namespace engine
