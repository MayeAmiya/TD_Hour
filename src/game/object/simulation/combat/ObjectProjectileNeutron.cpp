#include "game/object/simulation/combat/ObjectProjectileSystemDetail.h"

#include "core/container/string_utils.h"
#include "debug/debug.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/combat/ObjectWeaponDamage.h"
#include "game/object/runtime/ObjectStatus.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>

namespace engine::object_projectile_detail {

namespace {

[[nodiscard]] game::WeaponFxPolicy freezeProjectileEffectFxPolicy(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content, const PlayerRegistry* players,
    ecs::entity projectileEntity,
    const ObjectProjectileComponent& projectile) {
    const game::WeaponTemplate* weapon = content.findWeapon(
        projectile.detonationWeapon);
    if (!weapon) return game::WeaponFxPolicy::Play;

    const std::optional<ecs::entity> launcher = lifecycle.entityFromId(
        projectile.launcher);
    return resolveObjectWeaponFxPolicy(
        registry, launcher.value_or(projectileEntity),
        &lifecycle, players, *weapon,
        weapon->suspendFxDelayMilliseconds != 0);
}

} // namespace

void killNeutronProjectile(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    ObjectId projectileId, ecs::entity entity,
    ObjectProjectileComponent& projectile,
    ObjectNeutronMissileProjectileComponent& neutron,
    TransformComponent& transform,
    const FixedVec3& position, ObjectId collidedTarget,
    uint64_t confirmedTick, container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectProjectileEvent>& events) {
    if (projectile.detonated) return;
    projectile.detonated = true;
    // The visible nuclear detonation is entirely a Die-side reaction of this
    // force kill (NeutronMissileSlowDeathBehavior).
    publishGuidedProjectileTransform(
        registry, entity, projectile, transform, position,
        projectile.flightPathForward);
    static_cast<void>(ObjectStatusSystem::apply(
        registry, entity,
        {
            .setMask = game::objectStatusBit(
                game::ObjectStatusFlag::NoCollisions),
            .confirmedTick = confirmedTick,
        }));
    if (RenderModelComponent* visual =
            ecs::try_get<RenderModelComponent>(registry, entity)) {
        visual->hidden = true;
    }
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    if (health) {
        outDamage.push_back({
            .target = projectileId,
            .source = INVALID_OBJECT_ID,
            .sourceSequence = projectile.sourceShotSequence,
            .causalGroup = projectileId,
            .amount = health->maximumFixed,
            .damageType = game::DamageType::UNRESISTABLE,
            .deathType = game::DeathType::NORMAL,
            .forceKill = true,
            .confirmedTick = confirmedTick,
        });
    } else {
        static_cast<void>(lifecycle.requestDestroy(
            projectileId, ObjectDestroyReason::Combat, confirmedTick));
    }
    events.push_back({
        .kind = ObjectProjectileEventKind::Collided,
        .projectile = projectileId,
        .launcher = projectile.launcher,
        .sourcePathfindLayer = snapshotPathfindLayer(
            registry, lifecycle, projectile.launcher,
            projectile.sourcePathfindLayer),
        .target = collidedTarget,
        .sourceShotSequence = projectile.sourceShotSequence,
        .detonationWeapon = projectile.detonationWeapon,
        .position = position,
        .confirmedTick = confirmedTick,
    });
    if (neutron.deliveryDecalActive) {
        events.push_back({
            .kind = ObjectProjectileEventKind::GroundDecalEnd,
            .projectile = projectileId,
            .launcher = projectile.launcher,
            .sourceShotSequence = projectile.sourceShotSequence,
            .position = neutron.target,
            .authoredOrder = neutron.deliveryDecalAuthoredOrder,
            .confirmedTick = confirmedTick,
        });
        neutron.deliveryDecalActive = false;
    }
}

[[nodiscard]] bool updateNeutronProjectile(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content, const ObjectSpatialIndex* spatialIndex,
    const PlayerRegistry* players, const game::terrain::TerrainLogic& terrain,
    uint64_t confirmedTick, ObjectId projectileId, ecs::entity entity,
    ObjectProjectileComponent& projectile,
    ObjectNeutronMissileProjectileComponent& neutron,
    TransformComponent& transform,
    container::Vector<ObjectId>& collisionCandidateScratch,
    container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectProjectileEvent>& events) {
    if (projectile.detonated || confirmedTick <= projectile.spawnedTick) return true;

    if (!neutron.armed) {
        // RefCode resolves the launch vehicle again in doLaunch(); a missile
        // whose launcher disappeared between Weapon::fireProjectile() and
        // its first update destroys itself without ignition or detonation.
        // The detached spawn request still freezes launch geometry for the
        // normal path, but it must not make a missing launcher look valid.
        if (!lifecycle.entityFromId(projectile.launcher)) {
            if (neutron.deliveryDecalActive) {
                events.push_back({
                    .kind = ObjectProjectileEventKind::GroundDecalEnd,
                    .projectile = projectileId,
                    .launcher = projectile.launcher,
                    .sourceShotSequence = projectile.sourceShotSequence,
                    .position = neutron.target,
                    .authoredOrder = neutron.deliveryDecalAuthoredOrder,
                    .confirmedTick = confirmedTick,
                });
                neutron.deliveryDecalActive = false;
            }
            projectile.launcher = INVALID_OBJECT_ID;
            TD_LOG_ERROR(
                "[ObjectProjectile] Neutron projectile={} destroyed before ignition at tick={}: launcher is gone",
                projectileId.value, confirmedTick);
            static_cast<void>(lifecycle.requestDestroy(
                projectileId, ObjectDestroyReason::System, confirmedTick));
            return true;
        }
        neutron.armed = true;
        const FixedVec3 destination = add(
            projectile.position, neutron.velocityPerFrame);
        const game::WeaponFxPolicy effectFxPolicy =
            freezeProjectileEffectFxPolicy(
                registry, lifecycle, content, players, entity, projectile);
        if (!neutron.launchFx.empty()) {
            events.push_back({
                .kind = ObjectProjectileEventKind::Effect,
                .projectile = projectileId,
                .launcher = projectile.launcher,
                .target = projectile.intendedTarget,
                .sourceShotSequence = projectile.sourceShotSequence,
                .detonationWeapon = projectile.detonationWeapon,
                .position = projectile.position,
                .weaponFxPolicy = effectFxPolicy,
                .fxListName = neutron.launchFx,
                .confirmedTick = confirmedTick,
            });
        }
        if (!neutron.ignitionFx.empty() ||
            !neutron.exhaustParticleSystem.empty()) {
            events.push_back({
                .kind = ObjectProjectileEventKind::Effect,
                .projectile = projectileId,
                .launcher = projectile.launcher,
                .target = projectile.intendedTarget,
                .sourceShotSequence = projectile.sourceShotSequence,
                .detonationWeapon = projectile.detonationWeapon,
                .position = destination,
                .weaponFxPolicy = effectFxPolicy,
                .fxListName = neutron.ignitionFx,
                .particleSystemName = neutron.exhaustParticleSystem,
                .confirmedTick = confirmedTick,
            });
        }
#if TD_DEBUG_ENABLED
        TD_LOG_DEBUG(
            "[ObjectProjectile] Neutron launch effects produced: projectile={} tick={} launchFx='{}' ignitionFx='{}' exhaust='{}' start=({}, {}, {}) destination=({}, {}, {})",
            projectileId.value, confirmedTick, neutron.launchFx,
            neutron.ignitionFx, neutron.exhaustParticleSystem,
            projectile.position.x.to_float(),
            projectile.position.y.to_float(),
            projectile.position.z.to_float(), destination.x.to_float(),
            destination.y.to_float(), destination.z.to_float());
#endif
        publishGuidedProjectileTransform(
            registry, entity, projectile, transform, destination, neutron.forward);
        return true;
    }

    const FixedVec3 target = neutron.reachedIntermediateTarget
        ? neutron.target : neutron.intermediateTarget;
    const FixedVec3 toTarget = subtract(target, projectile.position);
    const ObjectGeometryComponent* projectileGeometry =
        ecs::try_get<ObjectGeometryComponent>(registry, entity);
    const Fixed projectileRadius = projectileGeometry
        ? Fixed::max(kFixedZero,
              projectileGeometry->boundingSphereRadiusFixed)
        : kFixedZero;
    if (!neutron.reachedIntermediateTarget &&
        squaredLength(toTarget) <= projectileRadius * projectileRadius) {
        neutron.reachedIntermediateTarget = true;
        projectile.position = neutron.intermediateTarget;
        const Fixed speed = length(neutron.velocityPerFrame);
        neutron.velocityPerFrame = {kFixedZero, kFixedZero, -speed * kFixedHalf};
    }

    const FixedVec3 activeTarget = neutron.reachedIntermediateTarget
        ? neutron.target : neutron.intermediateTarget;
    const bool retainLaunchAttitude =
        neutron.noTurnDistanceRemaining > kFixedZero;
    if (!retainLaunchAttitude) {
        // The preceding confirmed frame has already published the last
        // no-turn pose. From this update onward calcTransform owns attitude,
        // matching NeutronMissileUpdate's switch from copying the current
        // Matrix3D to rebuilding it toward the target. Clearing this only at
        // the beginning of the turning frame avoids changing presentation one
        // frame early when the previous movement merely crossed distance zero.
        projectile.hasLaunchOrientation = false;
        neutron.forward = turnToward(
            neutron.forward, subtract(activeTarget, projectile.position),
            neutron.maximumTurnRateRadiansPerFrame);
    }
    Fixed relativeSpeed = neutron.relativeSpeedPerFrame;
    if (neutron.reachedIntermediateTarget &&
        neutron.intermediateTarget.z != neutron.target.z) {
        relativeSpeed *= kFixedHalf;
    }
    const FixedVec3 acceleration = subtract(
        scale(neutron.forward, relativeSpeed),
        scale(neutron.velocityPerFrame, neutron.forwardDamping));
    neutron.velocityPerFrame = add(neutron.velocityPerFrame, acceleration);

    const uint64_t elapsed = confirmedTick - neutron.launchTick;
    neutron.presentationOffset = {};
    if (neutron.specialSpeedFrames > 0 &&
        elapsed < neutron.specialSpeedFrames) {
        const Fixed time = fraction(
            static_cast<uint32_t>(elapsed), neutron.specialSpeedFrames);
        const Fixed newHeight = neutron.launchHeight +
            neutron.specialAccelerationFactor * time * time *
                neutron.specialSpeedHeight;
        neutron.velocityPerFrame.x = kFixedZero;
        neutron.velocityPerFrame.y = kFixedZero;
        neutron.velocityPerFrame.z = newHeight - projectile.position.z;
        if (neutron.specialJitterDistance > kFixedZero) {
            const Fixed amplitude = (kFixedOne - time) *
                neutron.specialJitterDistance;
            const FixedVec3 localY = deterministicPerpendicular(neutron.forward);
            const FixedVec3 localZ = normalizedOr({
                neutron.forward.y * localY.z - neutron.forward.z * localY.y,
                neutron.forward.z * localY.x - neutron.forward.x * localY.z,
                neutron.forward.x * localY.y - neutron.forward.y * localY.x,
            }, {kFixedZero, kFixedZero, kFixedOne});
            neutron.presentationOffset = add(
                scale(localY, deterministicSignedUnit(
                    projectileId, confirmedTick, 0) * amplitude),
                scale(localZ, deterministicSignedUnit(
                    projectileId, confirmedTick, 1) * amplitude));
        }
    }

    const FixedVec3 destination = add(
        projectile.position, neutron.velocityPerFrame);
    const game::WeaponTemplate* weapon = content.findWeapon(projectile.detonationWeapon);
    const std::optional<ProjectileCollision> collision = findProjectileCollision(
        registry, lifecycle, spatialIndex, players, entity, projectileId,
        projectile, projectile.position, destination, projectileRadius,
        weapon, false, {}, collisionCandidateScratch, confirmedTick);
    if (collision) {
        const FixedVec3 impact = add(
            projectile.position,
            scale(subtract(destination, projectile.position), collision->time));
        killNeutronProjectile(
            registry, lifecycle, projectileId, entity, projectile, neutron,
            transform, impact, collision->target, confirmedTick, outDamage,
            events);
        return true;
    }

    if (terrain.isLoaded()) {
        Fixed collisionHeight = Fixed::from_raw(
            terrain.groundHeightRaw(destination.x.raw(), destination.y.raw()));
        const game::terrain::TerrainPathfindLayerId layer =
            terrain.highestPathfindLayerAtXYRaw(
                destination.x.raw(), destination.y.raw());
        if (const std::optional<int64_t> layerHeight =
                terrain.pathfindLayerHeightRawAt(
                    layer, destination.x.raw(), destination.y.raw())) {
            collisionHeight = maxFixed(
                collisionHeight, Fixed::from_raw(*layerHeight));
        }
        if (destination.z <= collisionHeight) {
            FixedVec3 impact = destination;
            impact.z = collisionHeight;
            killNeutronProjectile(
                registry, lifecycle, projectileId, entity, projectile, neutron,
                transform, impact, INVALID_OBJECT_ID, confirmedTick,
                outDamage, events);
            return true;
        }
    }

    const Fixed travelled = length(subtract(destination, projectile.position));
    neutron.noTurnDistanceRemaining -= travelled;
    neutron.forward = normalizedOr(neutron.velocityPerFrame, neutron.forward);
    publishGuidedProjectileTransform(
        registry, entity, projectile, transform, destination, neutron.forward);
    return true;
}

} // namespace engine::object_projectile_detail
