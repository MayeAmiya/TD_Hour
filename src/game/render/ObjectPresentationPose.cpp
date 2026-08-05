#include "game/render/ObjectPresentationPose.h"

#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"

namespace engine {

ObjectPresentationPose projectObjectPresentationPose(
    const ecs::registry& registry,
    ecs::entity entity,
    const TransformComponent& transform) noexcept {
    ObjectPresentationPose result{
        .position = {transform.x, transform.y, transform.z},
        .yawRadians = transform.rotation,
    };

    // NeutronMissileBehavior's SpecialJitterDistance is presentation-only in
    // RefCode.  Project it once here so the model, object-attached exhaust and
    // every other detached presentation consumer share the same world pose.
    if (const ObjectNeutronMissileProjectileComponent* neutron =
            ecs::try_get<ObjectNeutronMissileProjectileComponent>(
                registry, entity)) {
        result.position += math::vec3{
            neutron->presentationOffset.x.to_float(),
            neutron->presentationOffset.y.to_float(),
            neutron->presentationOffset.z.to_float(),
        };
    }

    if (const ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, entity);
        physics && physics->ownsAttitude) {
        result.rollRadians = physics->roll.to_float();
        result.pitchRadians = physics->pitch.to_float();
        result.yawRadians = physics->yaw.to_float();
    }
    return result;
}

} // namespace engine
