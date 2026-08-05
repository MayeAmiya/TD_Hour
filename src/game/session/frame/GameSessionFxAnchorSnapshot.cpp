#include "game/session/frame/GameSessionFxAnchorSnapshot.h"

#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/render/ObjectPresentationPose.h"
#include "game/object/simulation/combat/ObjectTransitionDamageFx.h"
#include "game/object/simulation/runtime/ObjectDeathEvents.h"
#include "game/object/contracts/ObjectLifecycle.h"

namespace engine::session_fx {

std::optional<game::FxInvocationAnchor> snapshotAnchor(
    const ecs::registry& registry,
    const ObjectLifecycle& lifecycle,
    ObjectId object) noexcept {
    if (!object) return std::nullopt;
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    if (!entity) return std::nullopt;
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, *entity);
    if (!transform) return std::nullopt;

    const ObjectPresentationPose pose = projectObjectPresentationPose(
        registry, *entity, *transform);
    game::FxInvocationAnchor anchor{
        .object = object,
        .position = pose.position,
        .rollRadians = pose.rollRadians,
        .pitchRadians = pose.pitchRadians,
        .yawRadians = pose.yawRadians,
    };
    if (const ThingTemplateComponent* source =
            ecs::try_get<ThingTemplateComponent>(registry, *entity);
        source && source->archetype) {
        const math::q32_32 radius = source->archetype->templateData.geometry
            .boundingCircleRadiusFixed;
        if (radius > math::q32_32{}) {
            anchor.objectBoundingCircleRadius = radius.to_float();
        }
    }
    return anchor;
}

game::FxInvocationAnchor snapshotAnchor(
    const FxInvocationAnchorSnapshot& source,
    ObjectId object) noexcept {
    return {
        .object = object,
        .position = {
            source.position.x.to_float(),
            source.position.y.to_float(),
            source.position.z.to_float(),
        },
        .rollRadians = source.rollRadians.to_float(),
        .pitchRadians = source.pitchRadians.to_float(),
        .yawRadians = source.yawRadians.to_float(),
    };
}

game::FxInvocationAnchor snapshotAnchor(
    const ObjectTransitionDamageFxAnchor& source,
    ObjectId object) noexcept {
    return {
        .object = object,
        .position = {
            source.position.x.to_float(),
            source.position.y.to_float(),
            source.position.z.to_float(),
        },
        .rollRadians = source.rollRadians.to_float(),
        .pitchRadians = source.pitchRadians.to_float(),
        .yawRadians = source.yawRadians.to_float(),
    };
}

game::FxInvocationAnchor worldAnchor(
    math::vec3 position,
    ObjectId object) noexcept {
    return {
        .object = object,
        .position = position,
    };
}

} // namespace engine::session_fx
