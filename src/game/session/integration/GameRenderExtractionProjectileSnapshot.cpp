#include "GameRenderExtractionDetail.h"

#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/render/ObjectPresentationPose.h"
#include "presentation/render/ProjectileStreamJoinPresentation.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace engine::render_extraction_detail {

void appendProjectilePresentation(
    const ProjectilePresentationSource& source,
    ecs::entity entity,
    const ObjectIdentityComponent& identity,
    const TransformComponent& transform,
    const ObjectGeometryComponent* geometry,
    const ObjectProjectileComponent* projectile,
    bool hiddenByLocalVisibility,
    bool alliedToObserver,
    render::WorldRenderSnapshot& snapshot) {
    if (projectile && !projectile->detonated && hiddenByLocalVisibility) {
        ++snapshot.localVisibility.hiddenProjectileCount;
    }
    const game::WeaponTemplate* projectileWeapon = projectile
        ? source.content.findWeapon(projectile->detonationWeapon)
        : nullptr;
    const game::ProjectileStreamRenderDescriptor* projectileStream =
        projectileWeapon && projectileWeapon->projectileStream.enabled
        ? &projectileWeapon->projectileStream : nullptr;
    const render::RenderVector projectileStreamAnchor = projectile
        ? render::RenderVector{
              projectile->projectileStreamOwnerAnchorPosition.x.to_float(),
              projectile->projectileStreamOwnerAnchorPosition.y.to_float(),
              projectile->projectileStreamOwnerAnchorPosition.z.to_float(),
          }
        : render::RenderVector{};
    // Weapon::createProjectileStream places the independent stream Object at
    // the source position. Its Drawable shroud state gates the complete line.
    const bool projectileStreamAnchorVisible = projectileStream &&
        source.localVisibility.isInsidePlayableBounds(
            projectileStreamAnchor) &&
        render::projectileStreamOwnerVisible(
            source.localVisibility.worldState(projectileStreamAnchor),
            source.localVisibility.isValid(), alliedToObserver);
    const render::ProjectileStreamExtractionPolicy streamPolicy =
        render::projectileStreamExtractionPolicy(
            !hiddenByLocalVisibility, projectileStream != nullptr,
            projectileStreamAnchorVisible);
    if (!projectile || projectile->detonated ||
        !streamPolicy.retainProjectileSnapshot) {
        return;
    }
    const ObjectPresentationPose presentationPose =
        projectObjectPresentationPose(source.registry, entity, transform);

    render::ProjectileRenderSnapshot output{
        .objectId = identity.id.value,
        .launcherId = projectile->launcher ? projectile->launcher.value : 0,
        .intendedTargetId = projectile->intendedTarget
            ? projectile->intendedTarget.value : 0,
        .intendedTargetPosition = {
            projectile->target.x.to_float(),
            projectile->target.y.to_float(),
            projectile->target.z.to_float(),
        },
        .trailChainIdentity = projectile->projectileStreamChainIdentity,
        .trailOwnerGeneration = projectile->projectileStreamOwnerGeneration,
        .sourceShotSequence = projectile->sourceShotSequence,
        .sourceBarrelSequenceOrdinal =
            projectile->sourceBarrelSequenceOrdinal,
        .spawnedTick = projectile->spawnedTick,
        .position = presentationPose.position,
        .trailOwnerAnchorPosition = projectileStreamAnchor,
        .launchSlot = std::min<uint8_t>(
            projectile->launchSlot,
            static_cast<uint8_t>(render::kRenderWeaponSlotCount - 1u)),
        .hasForward = projectile->hasFlightPathForward,
        .trailOwnerAnchorVisible = projectileStreamAnchorVisible,
        .visibilityExempt = alliedToObserver,
    };
    if (streamPolicy.shadowEnabled) {
        const ThingTemplateComponent* projectileTemplate =
            ecs::try_get<ThingTemplateComponent>(source.registry, entity);
        if (projectileTemplate && projectileTemplate->archetype) {
            const game::ThingShadowTemplate& authoredShadow =
                projectileTemplate->archetype->templateData.shadow;
            output.shadow = {
                .typeMask = render::filterRenderShadowTypeMask(
                    authoredShadow.typeMask,
                    source.featureQuality.useShadowVolumes,
                    source.featureQuality.useShadowDecals),
                .textureName = authoredShadow.texture,
                .sizeX = authoredShadow.sizeX,
                .sizeY = authoredShadow.sizeY,
                .offsetX = authoredShadow.offsetX,
                .offsetY = authoredShadow.offsetY,
            };
        }
    }
    if (geometry) {
        output.boundingRadius =
            geometry->boundingSphereRadiusFixed.to_float();
    }
    if (const std::optional<ecs::entity> launcherEntity =
            source.objects.entityFromId(projectile->launcher)) {
        const ObjectKindOfComponent* launcherKinds =
            ecs::try_get<ObjectKindOfComponent>(
                source.registry, *launcherEntity);
        const TransformComponent* launcherTransform =
            ecs::try_get<TransformComponent>(
                source.registry, *launcherEntity);
        const ObjectGeometryComponent* launcherGeometry =
            ecs::try_get<ObjectGeometryComponent>(
                source.registry, *launcherEntity);
        if (hasObjectKind(launcherKinds, game::ObjectKindOf::Vehicle) &&
            launcherTransform && launcherGeometry) {
            const float deltaX = output.position.x() - launcherTransform->x;
            const float deltaY = output.position.y() - launcherTransform->y;
            const float liftRadius = std::max(
                0.0f, launcherGeometry->majorRadiusFixed.to_float()) * 1.5f;
            if (deltaX * deltaX + deltaY * deltaY <=
                liftRadius * liftRadius) {
                const float heightAbovePosition =
                    launcherGeometry->shape == ObjectGeometryShape::Sphere
                    ? launcherGeometry->majorRadiusFixed.to_float()
                    : launcherGeometry->heightFixed.to_float();
                output.position[2] = std::max(
                    output.position.z(),
                    launcherTransform->z + heightAbovePosition + 0.5f);
            }
        }
    }
    if (projectileStream) {
        const game::ProjectileStreamRenderDescriptor& stream =
            *projectileStream;
        output.trailStreamName = projectileWeapon->projectileStreamName;
        output.trailTexture = stream.texture;
        output.trailStreamInstance = projectile->detonationWeapon.value;
        output.trailColor = {
            stream.color[0], stream.color[1], stream.color[2]};
        output.trailAlpha = stream.color[3];
        output.trailWidth = stream.width;
        output.trailTileFactor = stream.tileFactor;
        output.trailScrollRate = stream.scrollRate;
        output.trailLogicFramesPerSecond =
            render::projectileStreamLogicFramesPerSecond(
                source.logicFramesPerSecond);
        output.trailLifetimeSeconds = stream.segmentLifetimeSeconds;
        output.trailMaximumSegments = stream.maximumSegments;
        output.trailBlend = projectileTrailBlend(stream.blend);
        output.trailDepth = projectileTrailDepth(stream.depth);
        output.trailEnabled = streamPolicy.trailEnabled;
    }
    if (projectile->hasFlightPathForward) {
        output.forward = {
            projectile->flightPathForward.x.to_float(),
            projectile->flightPathForward.y.to_float(),
            projectile->flightPathForward.z.to_float(),
        };
    }
    snapshot.projectiles.push_back(std::move(output));
}

} // namespace engine::render_extraction_detail
