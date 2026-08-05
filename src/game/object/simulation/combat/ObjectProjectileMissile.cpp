#include "game/object/simulation/combat/ObjectProjectileSystemDetail.h"

#include "core/container/string_utils.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
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

    // Match the firing policy while the launcher remains available. A
    // missile can outlive its launcher, in which case its own frozen object
    // state is the closest authoritative policy source still available.
    const std::optional<ecs::entity> launcher = lifecycle.entityFromId(
        projectile.launcher);
    return resolveObjectWeaponFxPolicy(
        registry, launcher.value_or(projectileEntity),
        &lifecycle, players, *weapon,
        weapon->suspendFxDelayMilliseconds != 0);
}

} // namespace

[[nodiscard]] uint64_t mixProjectileRandom(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

[[nodiscard]] FixedVec3 missileWaypointTarget(
    const game::terrain::WaypointRecord& waypoint) noexcept {
    return {
        Fixed::from_raw(waypoint.positionRaw[0]),
        Fixed::from_raw(waypoint.positionRaw[1]),
        Fixed::from_raw(waypoint.positionRaw[2]),
    };
}

[[nodiscard]] MissileWaypointAdvance advanceMissileWaypoint(
    const game::terrain::TerrainLogic& terrain, ObjectId projectileId,
    const ObjectProjectileComponent& projectile,
    ObjectProjectileWaypointPathComponent& route,
    FixedVec3& nextTarget) noexcept {
    if (route.graphRevision == 0 ||
        route.graphRevision != terrain.waypointGraphRevision()) {
        return MissileWaypointAdvance::Invalid;
    }
    const game::terrain::WaypointRecord* current =
        terrain.waypointById(route.currentWaypointId);
    if (!current) return MissileWaypointAdvance::Invalid;
    if (current->links.empty()) return MissileWaypointAdvance::Terminal;

    uint64_t branchKey = projectileId.value;
    branchKey ^= projectile.launcher.value + 0x9e3779b97f4a7c15ull +
        (branchKey << 6u) + (branchKey >> 2u);
    branchKey ^= static_cast<uint64_t>(projectile.sourceShotSequence) << 32u;
    branchKey ^= static_cast<uint64_t>(route.currentWaypointId) +
        (static_cast<uint64_t>(route.hopGeneration) << 32u);
    const size_t linkIndex = static_cast<size_t>(
        mixProjectileRandom(branchKey) % current->links.size());
    const uint32_t nextId = current->links[linkIndex];
    const game::terrain::WaypointRecord* next = terrain.waypointById(nextId);
    if (!next) return MissileWaypointAdvance::Invalid;

    route.currentWaypointId = nextId;
    ++route.hopGeneration;
    if (route.hopGeneration == 0) ++route.hopGeneration;
    nextTarget = missileWaypointTarget(*next);
    return MissileWaypointAdvance::Advanced;
}

[[nodiscard]] Fixed jamScatter(uint64_t key, Fixed radius) noexcept {
    const uint32_t sample = static_cast<uint32_t>(mixProjectileRandom(key));
    const Fixed unit = Fixed::from_raw(static_cast<int64_t>(sample));
    return (unit * kFixedTwo - kFixedOne) * radius;
}
void publishGuidedProjectileTransform(
    ecs::registry& registry, ecs::entity entity,
    ObjectProjectileComponent& projectile, TransformComponent& transform,
    const FixedVec3& position, const FixedVec3& forward) noexcept {
    projectile.position = position;
    projectile.flightPathForward = normalizedOr(
        forward, {kFixedOne, kFixedZero, kFixedZero});
    projectile.hasFlightPathForward = true;
    writeAuthoritativeObjectPosition(registry, entity, position);
    projectFlightPathYaw(registry, entity, transform, projectile);
    if (ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, entity)) {
        const Fixed planar = planarLength(projectile.flightPathForward);
        physics->pitch = math::fixed_atan2(
            projectile.flightPathForward.z, planar);
        physics->roll = kFixedZero;
        physics->ownsAttitude = true;
        physics->position = position;
        physics->lastPublishedPosition = position;
        physics->hasAuthoritativePosition = true;
    }
}

void finishMissileSelfKill(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    ObjectId projectileId, ecs::entity entity,
    const ObjectProjectileComponent& projectile, uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& outDamage) {
    if (!projectile.detonateCallsKill) {
        static_cast<void>(lifecycle.requestDestroy(
            projectileId, ObjectDestroyReason::Combat, confirmedTick));
        return;
    }
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    if (!health) {
        static_cast<void>(lifecycle.requestDestroy(
            projectileId, ObjectDestroyReason::Combat, confirmedTick));
        return;
    }
    outDamage.push_back({
        .target = projectileId,
        .source = INVALID_OBJECT_ID,
        .sourceSequence = projectile.sourceShotSequence,
        .causalGroup = projectileId,
        .amount = health->maximumFixed,
        .damageType = game::DamageType::UNRESISTABLE,
        .deathType = game::DeathType::NORMAL,
        .resolutionPhase = ObjectDamageResolutionPhase::PostDetonationSelfKill,
        .forceKill = true,
        .confirmedTick = confirmedTick,
    });
}

void beginMissileDetonation(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content, const ObjectSpatialIndex* spatialIndex,
    const PlayerRegistry* players, ObjectId projectileId, ecs::entity entity,
    ObjectProjectileComponent& projectile,
    ObjectMissileProjectileComponent& missile, TransformComponent& transform,
    const FixedVec3& position, ObjectProjectileEventKind kind,
    ObjectId target, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick,
    container::Vector<ObjectId>& damageVictimScratch,
    container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectHistoricBonusWeaponFire>& outHistoricBonusWeapons,
    container::Vector<ObjectProjectileEvent>& events) {
    // MissileAIUpdate::detonate marks this before attempting its self-damage.
    // Authored Die callbacks use it to distinguish an intentional terminal
    // detonation from external PDL or script damage.
    static_cast<void>(ObjectStatusSystem::apply(
        registry, entity,
        {.setMask = game::objectStatusBit(
             game::ObjectStatusFlag::MissileKillingSelf),
         .confirmedTick = confirmedTick}));
    detonate(registry, lifecycle, content, spatialIndex, players,
             projectileId, entity, projectile, transform, position, kind,
             target, logicFramesPerSecond, confirmedTick,
             damageVictimScratch, outDamage,
             outHistoricBonusWeapons, events,
             !missile.suppressDetonationDamage, true);
    const game::WeaponTemplate* weapon = content.findWeapon(
        projectile.detonationWeapon);
    if (weapon && weapon->missileCallsOnDie) {
        if (const ObjectHealthComponent* health =
                ecs::try_get<ObjectHealthComponent>(registry, entity)) {
            outDamage.push_back({
                .target = projectileId,
                .source = INVALID_OBJECT_ID,
                .sourceSequence = projectile.sourceShotSequence,
                .causalGroup = projectileId,
                .amount = health->maximumFixed,
                .damageType = game::DamageType::UNRESISTABLE,
                .deathType = game::DeathType::DETONATED,
                .resolutionPhase =
                    ObjectDamageResolutionPhase::PostDetonationSelfKill,
                .forceKill = false,
                .confirmedTick = confirmedTick,
            });
        }
    }
    missile.state = ObjectMissileProjectileState::KillSelf;
    missile.killSelfTick = confirmedTick + missile.killSelfDelayFrames;
}

[[nodiscard]] bool updateMissileProjectile(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content, const ObjectSpatialIndex* spatialIndex,
    const PlayerRegistry* players, const game::terrain::TerrainLogic& terrain,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick,
    ObjectId projectileId, ecs::entity entity,
    ObjectProjectileComponent& projectile,
    ObjectMissileProjectileComponent& missile, TransformComponent& transform,
    container::Vector<ObjectId>& collisionCandidateScratch,
    container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectHistoricBonusWeaponFire>& outHistoricBonusWeapons,
    container::Vector<ObjectProjectileEvent>& events) {
    if (missile.state == ObjectMissileProjectileState::KillSelf) {
        if (confirmedTick >= missile.killSelfTick) {
            finishMissileSelfKill(registry, lifecycle, projectileId, entity,
                                  projectile, confirmedTick, outDamage);
        }
        return true;
    }
    if (confirmedTick <= projectile.spawnedTick) return true;

    if (missile.jamPending && !missile.jammed) {
        FixedVec3 scatteredTarget = projectile.target;
        if (missile.trackingTarget && projectile.intendedTarget) {
            const std::optional<ecs::entity> victim =
                lifecycle.entityFromId(projectile.intendedTarget);
            const TransformComponent* victimTransform = victim
                ? ecs::try_get<TransformComponent>(registry, *victim)
                : nullptr;
            if (victimTransform) {
                scatteredTarget = readAuthoritativeObjectPosition(
                    registry, *victim, *victimTransform);
            }
        }
        const uint64_t key = mixProjectileRandom(
            projectileId.value ^ (confirmedTick * 0x6a09e667f3bcc909ull));
        scatteredTarget.x += jamScatter(key, missile.distanceScatterWhenJammed);
        scatteredTarget.y += jamScatter(
            key ^ 0xbb67ae8584caa73bull,
            missile.distanceScatterWhenJammed);
        if (terrain.isLoaded()) {
            const game::terrain::TerrainPathfindLayerId layer =
                terrain.highestPathfindLayerAtXYRaw(
                    scatteredTarget.x.raw(), scatteredTarget.y.raw());
            const std::optional<int64_t> layerHeight =
                terrain.pathfindLayerHeightRawAt(
                    layer, scatteredTarget.x.raw(), scatteredTarget.y.raw());
            scatteredTarget.z = Fixed::from_raw(layerHeight.value_or(
                terrain.groundHeightRaw(
                    scatteredTarget.x.raw(), scatteredTarget.y.raw())));
        }
        projectile.intendedTarget = INVALID_OBJECT_ID;
        projectile.target = scatteredTarget;
        missile.originalTarget = scatteredTarget;
        missile.trackingTarget = false;
        missile.jammed = true;
        missile.jamPending = false;
        // MissileAIUpdate::projectileNowJammed replaces its AI path with the
        // sampled scatter destination. Retaining the script route would
        // overwrite that diversion on the next confirmed tick.
        ecs::remove<ObjectProjectileWaypointPathComponent>(registry, entity);
        if (RenderModelComponent* visual =
                ecs::try_get<RenderModelComponent>(registry, entity)) {
            static const game::ModelConditionMask jammed =
                game::modelConditionMaskOf(game::ModelConditionFlag::Jammed);
            for (size_t index = 0;
                 index < visual->modelConditionFlags.words.size(); ++index) {
                visual->modelConditionFlags.words[index] |=
                    jammed.words[index];
            }
        }
    }

    const Fixed framesPerSecond{static_cast<int32_t>(std::min<uint32_t>(
        std::max<uint32_t>(1, logicFramesPerSecond), kMaximumPathSegments))};
    if (missile.state == ObjectMissileProjectileState::Launch) {
        if (confirmedTick < missile.ignitionTick) {
            if (missile.hasLocomotor) return true;
            const Fixed retained = clampUnit(
                kFixedOne - missile.aerodynamicFrictionPerSecond / framesPerSecond);
            missile.velocityUnitsPerSecond = scale(
                missile.velocityUnitsPerSecond, retained);
            missile.velocityUnitsPerSecond.z +=
                missile.gravityUnitsPerSecondSq / framesPerSecond;
            const FixedVec3 destination = add(
                projectile.position,
                scale(missile.velocityUnitsPerSecond,
                      kFixedOne / framesPerSecond));
            if (destination.z < kFixedZero) {
                static_cast<void>(lifecycle.requestDestroy(
                    projectileId, ObjectDestroyReason::Combat, confirmedTick));
                return true;
            }
            missile.forward = normalizedOr(
                missile.velocityUnitsPerSecond, missile.forward);
            projectile.pathfindLayer = terrain.highestPathfindLayerAtRaw(
                destination.x.raw(), destination.y.raw(),
                destination.z.raw());
            publishGuidedProjectileTransform(
                registry, entity, projectile, transform, destination,
                missile.forward);
            return true;
        }
        missile.armed = true;
        missile.state = ObjectMissileProjectileState::AttackNoTurn;
        if (!missile.ignitionFx.empty() ||
            !missile.exhaustParticleSystem.empty()) {
            const game::WeaponFxPolicy effectFxPolicy =
                freezeProjectileEffectFxPolicy(
                    registry, lifecycle, content, players, entity, projectile);
            events.push_back({
                .kind = ObjectProjectileEventKind::Effect,
                .projectile = projectileId,
                .launcher = projectile.launcher,
                .target = projectile.intendedTarget,
                .sourceShotSequence = projectile.sourceShotSequence,
                .detonationWeapon = projectile.detonationWeapon,
                .position = projectile.position,
                .weaponFxPolicy = effectFxPolicy,
                .fxListName = missile.ignitionFx,
                .particleSystemName = missile.exhaustParticleSystem,
                .particleSystemLifetimeFrames = missile.fuelExpiryTick == UINT64_MAX
                    ? 0u : static_cast<uint32_t>(std::min<uint64_t>(
                        missile.fuelExpiryTick > confirmedTick
                            ? missile.fuelExpiryTick - confirmedTick : 1u,
                        UINT32_MAX)),
                .confirmedTick = confirmedTick,
            });
        }
    }

    if (missile.trackingTarget && projectile.intendedTarget) {
        const std::optional<ecs::entity> target =
            lifecycle.entityFromId(projectile.intendedTarget);
        const TransformComponent* targetTransform = target
            ? ecs::try_get<TransformComponent>(registry, *target) : nullptr;
        if (!targetTransform) {
            missile.state = ObjectMissileProjectileState::KillSelf;
            missile.killSelfTick = confirmedTick + missile.killSelfDelayFrames;
            return true;
        }
        projectile.target = readAuthoritativeObjectPosition(
            registry, *target, *targetTransform);
    }

    if (!missile.fuelExpired && confirmedTick >= missile.fuelExpiryTick) {
        missile.fuelExpired = true;
        if (missile.detonateOnNoFuel) {
            beginMissileDetonation(
                registry, lifecycle, content, spatialIndex, players,
                projectileId, entity, projectile, missile, transform,
                projectile.position, ObjectProjectileEventKind::Expired,
                INVALID_OBJECT_ID, logicFramesPerSecond, confirmedTick,
                collisionCandidateScratch, outDamage,
                outHistoricBonusWeapons, events);
            return true;
        }
    }

    ObjectProjectileWaypointPathComponent* waypointRoute =
        ecs::try_get<ObjectProjectileWaypointPathComponent>(registry, entity);
    const game::terrain::WaypointRecord* routeWaypoint = nullptr;
    if (waypointRoute) {
        if (waypointRoute->graphRevision == 0 ||
            waypointRoute->graphRevision != terrain.waypointGraphRevision() ||
            !(routeWaypoint = terrain.waypointById(
                  waypointRoute->currentWaypointId))) {
            beginMissileDetonation(
                registry, lifecycle, content, spatialIndex, players,
                projectileId, entity, projectile, missile, transform,
                projectile.position, ObjectProjectileEventKind::PathInvalid,
                INVALID_OBJECT_ID, logicFramesPerSecond, confirmedTick,
                collisionCandidateScratch, outDamage,
                outHistoricBonusWeapons, events);
            return true;
        }
    }
    const bool routeIntermediate =
        routeWaypoint && !routeWaypoint->links.empty();

    FixedVec3 desiredTarget = projectile.target;
    const FixedVec3 toTarget = subtract(projectile.target, projectile.position);
    const Fixed planarDistance = planarLength(toTarget);
    Fixed lockDistance = missile.lockDistance;
    if (!missile.trackingTarget) lockDistance *= kFixedHalf;
    if (routeIntermediate) {
        // Intermediate route nodes are locomotor goals, not attack targets.
        // Do not enter the terminal straight-line lock/detonation state.
        if (missile.state == ObjectMissileProjectileState::Locked)
            missile.state = ObjectMissileProjectileState::Attack;
    } else if (lockDistance > kFixedZero && planarDistance < lockDistance) {
        missile.state = ObjectMissileProjectileState::Locked;
        // RefCode freezes only a position-target shot. Object-target missiles
        // keep following the live victim after entering LOCKED; otherwise a
        // fast aircraft can escape toward the stale launch-time coordinate.
        if (!missile.trackingTarget) desiredTarget = missile.originalTarget;
    } else if (!missile.trackingTarget && missile.lockDistance > kFixedZero) {
        desiredTarget.z += Fixed{int32_t{10}};
    }

    if (missile.state != ObjectMissileProjectileState::Locked &&
        missile.preferredHeight > kFixedZero &&
        (routeIntermediate || missile.diveDistance <= kFixedZero ||
         planarDistance >= missile.diveDistance) &&
        terrain.isLoaded()) {
        const Fixed preferred = Fixed::from_raw(terrain.groundHeightRaw(
            projectile.position.x.raw(), projectile.position.y.raw())) +
            missile.preferredHeight;
        desiredTarget.z = maxFixed(desiredTarget.z, preferred);
    }

    if (missile.hasLocomotor && !missile.fuelExpired) {
        missile.currentSpeedUnitsPerSecond = minFixed(
            missile.maximumSpeedUnitsPerSecond,
            missile.currentSpeedUnitsPerSecond +
                missile.accelerationUnitsPerSecondSq / framesPerSecond);
    }
    FixedVec3 desiredForward = normalizedOr(
        subtract(desiredTarget, projectile.position), missile.forward);
    if (missile.hasLocomotor) {
        if (missile.state == ObjectMissileProjectileState::Locked) {
            missile.forward = desiredForward;
            missile.currentSpeedUnitsPerSecond = maxFixed(
                missile.currentSpeedUnitsPerSecond,
                missile.maximumSpeedUnitsPerSecond);
        } else if (!missile.fuelExpired &&
                   missile.state == ObjectMissileProjectileState::Attack) {
            missile.forward = turnToward(
                missile.forward, desiredForward,
                missile.maximumTurnRateRadiansPerSecond / framesPerSecond);
        }
        missile.velocityUnitsPerSecond = scale(
            missile.forward, missile.currentSpeedUnitsPerSecond);
    } else {
        const Fixed retained = clampUnit(
            kFixedOne - missile.aerodynamicFrictionPerSecond / framesPerSecond);
        missile.velocityUnitsPerSecond = scale(
            missile.velocityUnitsPerSecond, retained);
        missile.velocityUnitsPerSecond.z +=
            missile.gravityUnitsPerSecondSq / framesPerSecond;
        missile.forward = normalizedOr(
            missile.velocityUnitsPerSecond, missile.forward);
    }
    FixedVec3 destination = add(
        projectile.position,
        scale(missile.velocityUnitsPerSecond, kFixedOne / framesPerSecond));
    const Fixed travelled = length(subtract(destination, projectile.position));
    if (!missile.fuelExpired &&
        missile.state == ObjectMissileProjectileState::AttackNoTurn) {
        missile.noTurnDistanceRemaining -= travelled;
        if (missile.noTurnDistanceRemaining <= kFixedZero) {
            missile.state = ObjectMissileProjectileState::Attack;
        }
    }

    const ObjectGeometryComponent* projectileGeometry =
        ecs::try_get<ObjectGeometryComponent>(registry, entity);
    const Fixed projectileRadius = projectileGeometry
        ? Fixed::max(kFixedZero,
              projectileGeometry->boundingSphereRadiusFixed)
        : kFixedZero;
    const game::WeaponTemplate* weapon = content.findWeapon(projectile.detonationWeapon);
    if (missile.armed) {
        const std::optional<ProjectileCollision> collision = findProjectileCollision(
            registry, lifecycle, spatialIndex, players, entity, projectileId,
            projectile, projectile.position, destination, projectileRadius,
            weapon, true, {}, collisionCandidateScratch, confirmedTick);
        if (collision) {
            const FixedVec3 impact = add(
                projectile.position,
                scale(subtract(destination, projectile.position), collision->time));
            if (clearGarrisonOnImpact(
                    registry, lifecycle, projectileId, entity, projectile,
                    collision->target, impact, confirmedTick, outDamage, events)) {
                return true;
            }
            beginMissileDetonation(
                registry, lifecycle, content, spatialIndex, players,
                projectileId, entity, projectile, missile, transform, impact,
                ObjectProjectileEventKind::Collided, collision->target,
                logicFramesPerSecond, confirmedTick,
                collisionCandidateScratch, outDamage,
                outHistoricBonusWeapons, events);
            return true;
        }
        if (const std::optional<FixedVec3> bridgeImpact =
                bridgeLayerImpact(projectile, terrain, destination)) {
            beginMissileDetonation(
                registry, lifecycle, content, spatialIndex, players,
                projectileId, entity, projectile, missile, transform,
                *bridgeImpact, ObjectProjectileEventKind::Collided,
                INVALID_OBJECT_ID, logicFramesPerSecond, confirmedTick,
                collisionCandidateScratch, outDamage,
                outHistoricBonusWeapons, events);
            return true;
        }
        if (terrain.isLoaded()) {
            const Fixed ground = Fixed::from_raw(
                terrain.groundHeightRaw(destination.x.raw(), destination.y.raw()));
            if (destination.z <= ground) {
                FixedVec3 impact = destination;
                impact.z = ground;
                const FixedVec3 goalDelta = subtract(
                    impact, projectile.target);
                const Fixed pathfindCell{int32_t{10}};
                if (goalDelta.z > pathfindCell &&
                    length(goalDelta) > pathfindCell * Fixed{int32_t{3}}) {
                    // RefCode treats a remote hillside hit as unexpected and
                    // leaves the armed missile alive to keep navigating.
                    destination = impact;
                    destination.z += Fixed::from_fraction(1, 100);
                    publishGuidedProjectileTransform(
                        registry, entity, projectile, transform, destination,
                        missile.forward);
                    return true;
                }
                beginMissileDetonation(
                    registry, lifecycle, content, spatialIndex, players,
                    projectileId, entity, projectile, missile, transform,
                    impact, ObjectProjectileEventKind::Collided,
                    INVALID_OBJECT_ID, logicFramesPerSecond, confirmedTick,
                    collisionCandidateScratch, outDamage,
                    outHistoricBonusWeapons, events);
                return true;
            }
        }
    }

    const Fixed routeDistance = routeIntermediate
        ? planarLength(subtract(projectile.target, projectile.position))
        : length(subtract(projectile.target, projectile.position));
    const Fixed routeTravelled = routeIntermediate
        ? planarLength(subtract(destination, projectile.position))
        : travelled;
    if (waypointRoute && routeDistance <= routeTravelled) {
        FixedVec3 nextTarget{};
        switch (advanceMissileWaypoint(
            terrain, projectileId, projectile, *waypointRoute,
            nextTarget)) {
        case MissileWaypointAdvance::Advanced:
            // Snap to the completed route node before beginning the next
            // segment so tick-rate changes cannot accumulate planar
            // overshoot. Intermediate waypoints do not force a ground-Z
            // snap; the Missile locomotor retains PreferredHeight.
            {
                FixedVec3 completedNode = projectile.position;
                completedNode.x = projectile.target.x;
                completedNode.y = projectile.target.y;
                publishGuidedProjectileTransform(
                    registry, entity, projectile, transform, completedNode,
                    missile.forward);
            }
            projectile.target = nextTarget;
            missile.originalTarget = nextTarget;
            missile.state = ObjectMissileProjectileState::Attack;
            return true;
        case MissileWaypointAdvance::Terminal:
            beginMissileDetonation(
                registry, lifecycle, content, spatialIndex, players,
                projectileId, entity, projectile, missile, transform,
                projectile.target,
                ObjectProjectileEventKind::ReachedDestination,
                INVALID_OBJECT_ID, logicFramesPerSecond, confirmedTick,
                collisionCandidateScratch, outDamage,
                outHistoricBonusWeapons, events);
            return true;
        case MissileWaypointAdvance::Invalid:
            beginMissileDetonation(
                registry, lifecycle, content, spatialIndex, players,
                projectileId, entity, projectile, missile, transform,
                projectile.position, ObjectProjectileEventKind::PathInvalid,
                INVALID_OBJECT_ID, logicFramesPerSecond, confirmedTick,
                collisionCandidateScratch, outDamage,
                outHistoricBonusWeapons, events);
            return true;
        }
    }
    if (!waypointRoute &&
        missile.state == ObjectMissileProjectileState::Locked &&
        length(subtract(projectile.target, projectile.position)) <= travelled) {
        beginMissileDetonation(
            registry, lifecycle, content, spatialIndex, players,
            projectileId, entity, projectile, missile, transform,
            projectile.target, ObjectProjectileEventKind::ReachedDestination,
            projectile.intendedTarget, logicFramesPerSecond, confirmedTick,
            collisionCandidateScratch, outDamage,
            outHistoricBonusWeapons, events);
        return true;
    }
    publishGuidedProjectileTransform(
        registry, entity, projectile, transform, destination, missile.forward);
    return true;
}

} // namespace engine::object_projectile_detail
