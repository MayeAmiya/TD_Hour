#include "game/object/simulation/combat/ObjectProjectileSystemDetail.h"

#include "core/container/string_utils.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/combat/ObjectWeaponDamage.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>

namespace engine {

using namespace object_projectile_detail;
using container::asciiEqualIgnoreCase;

namespace {

[[nodiscard]] LogicFixedQuaternion normalizedProjectileQuaternion(
    LogicFixedQuaternion value) noexcept {
    const Fixed lengthSquared = value.x * value.x + value.y * value.y +
        value.z * value.z + value.w * value.w;
    if (lengthSquared <= kFixedZero) return {};
    const Fixed length = Fixed::sqrt(lengthSquared);
    if (length <= kFixedZero) return {};
    value.x /= length;
    value.y /= length;
    value.z /= length;
    value.w /= length;
    return value;
}

[[nodiscard]] LogicFixedQuaternion multiplyProjectileQuaternion(
    const LogicFixedQuaternion& left,
    const LogicFixedQuaternion& right) noexcept {
    return normalizedProjectileQuaternion({
        .x = left.w * right.x + left.x * right.w +
            left.y * right.z - left.z * right.y,
        .y = left.w * right.y - left.x * right.z +
            left.y * right.w + left.z * right.x,
        .z = left.w * right.z + left.x * right.y -
            left.y * right.x + left.z * right.w,
        .w = left.w * right.w - left.x * right.x -
            left.y * right.y - left.z * right.z,
    });
}

[[nodiscard]] LogicFixedQuaternion neutronLaunchOrientation(
    const LogicFixedQuaternion& launchBoneOrientation) noexcept {
    // NeutronMissileUpdate::doLaunch() installs the complete launch-bone
    // Matrix3D, then post-multiplies a local +X PI/2 correction before the
    // missile leaves the rack.  Keep that authored model-space correction in
    // the deterministic launch value; rotating around local X deliberately
    // leaves the controller's forward direction unchanged.
    const Fixed pi = Fixed::from_raw(13493037704ll);
    const math::q32_32_sincos halfAngle = math::fixed_sincos(
        pi / Fixed{int32_t{4}});
    return multiplyProjectileQuaternion(
        launchBoneOrientation,
        LogicFixedQuaternion{
            .x = halfAngle.sine,
            .w = halfAngle.cosine,
        });
}

} // namespace

bool ObjectProjectileSystem::initializeObject(ecs::registry& registry, ecs::entity entity,
                                              ObjectId projectileId,
                                              const game::ObjectArchetype& archetype,
                                              const GameContentSnapshot& content,
                                              const ObjectProjectileSpawnRequest& request,
                                              const game::terrain::TerrainLogic& terrain,
                                              uint32_t logicFramesPerSecond,
                                              math::q32_32 gravityUnitsPerSecondSq) {
    const game::ThingTemplate& templateData = archetype.templateData;
    const container::SharedPtr<const ObjectProjectilePlan>& planHandle =
        archetype.projectilePlan;
    const ObjectProjectilePlan* plan = planHandle.get();
    const ObjectProjectileBehaviorKind behaviorKind = plan
        ? plan->behaviorKind : ObjectProjectileBehaviorKind::Unsupported;
    const bool placedHelper = behaviorKind == ObjectProjectileBehaviorKind::PlacedHelper;
    const game::WeaponTemplate* weapon = content.findWeapon(request.detonationWeapon);
    TransformComponent* transform = ecs::try_get<TransformComponent>(registry, entity);
    if (behaviorKind == ObjectProjectileBehaviorKind::Unsupported ||
        !weapon || !transform || !projectileId || !request.launcher) {
        m_events.push_back({
            .kind = ObjectProjectileEventKind::UnsupportedTemplate,
            .projectile = projectileId,
            .launcher = request.launcher,
            .target = request.intendedTarget,
            .sourceShotSequence = request.sourceShotSequence,
            .detonationWeapon = request.detonationWeapon,
            .position = request.launchPosition,
            .confirmedTick = request.confirmedTick,
        });
        return false;
    }

    FixedVec3 resolvedTarget = request.targetPosition;
    if (request.targetWasScattered) {
        resolvedTarget.z = Fixed::from_raw(
            terrain.pathfindLayerHeightRawAt(
                request.scatteredTargetPathfindLayer,
                resolvedTarget.x.raw(), resolvedTarget.y.raw())
                .value_or(terrain.groundHeightRaw(
                    resolvedTarget.x.raw(), resolvedTarget.y.raw())));
    }

    const game::terrain::WaypointRecord* waypointPathStart = nullptr;
    if (behaviorKind == ObjectProjectileBehaviorKind::MissileAI &&
        request.waypointPathStartId != UINT32_MAX) {
        if (request.waypointGraphRevision == 0 ||
            request.waypointGraphRevision != terrain.waypointGraphRevision() ||
            !(waypointPathStart =
                  terrain.waypointById(request.waypointPathStartId))) {
            m_events.push_back({
                .kind = ObjectProjectileEventKind::PathInvalid,
                .projectile = projectileId,
                .launcher = request.launcher,
                .sourcePathfindLayer = request.sourcePathfindLayer,
                .target = request.intendedTarget,
                .sourceShotSequence = request.sourceShotSequence,
                .detonationWeapon = request.detonationWeapon,
                .position = request.launchPosition,
                .weaponFxPolicy = weapon->suspendFxDelayMilliseconds != 0
                    ? game::WeaponFxPolicy::SuppressedBySuspendDelay
                    : game::WeaponFxPolicy::Play,
                .confirmedTick = request.confirmedTick,
            });
            return false;
        }
        resolvedTarget = {
            math::q32_32::from_raw(waypointPathStart->positionRaw[0]),
            math::q32_32::from_raw(waypointPathStart->positionRaw[1]),
            math::q32_32::from_raw(waypointPathStart->positionRaw[2]),
        };
    }

    if (placedHelper) {
        writeAuthoritativeObjectPosition(
            registry, entity, resolvedTarget);
        ObjectPlacedProjectileHelperComponent helper{
            .launcher = request.launcher,
            .intendedTarget = request.intendedTarget,
            .sourceWeapon = request.detonationWeapon,
            .sourceShotSequence = request.sourceShotSequence,
            .spawnedTick = request.confirmedTick,
        };
        ecs::emplace<ObjectPlacedProjectileHelperComponent>(
            registry, entity, std::move(helper));
        m_events.push_back({
            .kind = ObjectProjectileEventKind::Spawned,
            .projectile = projectileId,
            .launcher = request.launcher,
            .target = request.intendedTarget,
            .sourceShotSequence = request.sourceShotSequence,
            .detonationWeapon = request.detonationWeapon,
            .position = resolvedTarget,
            .confirmedTick = request.confirmedTick,
        });
        return true;
    }

    ObjectProjectileComponent projectile;
    projectile.launcher = request.launcher;
    projectile.sourcePathfindLayer = request.sourcePathfindLayer;
    projectile.intendedTarget = request.intendedTarget;
    projectile.detonationWeapon = request.detonationWeapon;
    projectile.launcherWeaponBonusConditions =
        request.launcherWeaponBonusConditions;
    projectile.projectileStreamOwnerGeneration =
        request.projectileStreamOwnerGeneration;
    projectile.launchSlot = request.launchSlot;
    projectile.projectileStreamChainIdentity = resolveProjectileStreamChain(
        request, weapon->projectileStream.enabled);
    projectile.sourceShotSequence = request.sourceShotSequence;
    projectile.sourceBarrelSequenceOrdinal =
        request.sourceBarrelSequenceOrdinal;
    projectile.spawnedTick = request.confirmedTick;
    projectile.projectileStreamOwnerAnchorPosition =
        request.projectileStreamOwnerAnchorPosition;
    projectile.start = request.launchPosition;
    projectile.launchOrientation = request.launchOrientation;
    projectile.hasLaunchOrientation = request.hasLaunchOrientation;
    if (behaviorKind == ObjectProjectileBehaviorKind::NeutronMissile &&
        projectile.hasLaunchOrientation) {
        projectile.launchOrientation = neutronLaunchOrientation(
            projectile.launchOrientation);
    }
    projectile.target = resolvedTarget;
    const bool missileTryToFollowTarget =
        behaviorKind == ObjectProjectileBehaviorKind::MissileAI &&
        plan->tryToFollowTarget;
    if (behaviorKind == ObjectProjectileBehaviorKind::MissileAI &&
        missileTryToFollowTarget && request.intendedTarget &&
        request.hasIntendedTargetBasePosition) {
        projectile.target = request.intendedTargetBasePosition;
    }
    projectile.position = projectile.start;
    projectile.pathfindLayer = terrain.highestPathfindLayerAtRaw(
        projectile.start.x.raw(), projectile.start.y.raw(),
        projectile.start.z.raw());
    projectile.garrisonHitRequiredKindMask =
        plan->garrisonHitRequiredKindMask;
    projectile.garrisonHitForbiddenKindMask =
        plan->garrisonHitForbiddenKindMask;
    projectile.garrisonHitFx = plan->garrisonHitFx;
    projectile.garrisonHitKillCount = plan->garrisonHitKillCount;
    projectile.detonateCallsKill = plan->detonateCallsKill;

    const uint32_t rate = std::max<uint32_t>(1, logicFramesPerSecond);
    const Fixed framesPerSecond{
        static_cast<int32_t>(std::min<uint32_t>(rate, kMaximumPathSegments))};
    const FixedVec3 initialForward = normalizedOr(
        request.hasLaunchOrientation
            ? quaternionForward(request.launchOrientation)
            : subtract(projectile.target, projectile.start),
        {kFixedOne, kFixedZero, kFixedZero});
    // Weapon::positionProjectileForLaunch transfers the launcher's Physics
    // velocity to every ordinary projectile before its update module starts.
    // Preserve the value on the projectile Physics lane even when a guided
    // controller later owns position integration; tumble, Die/OCL and
    // unpowered launch phases still observe the inherited momentum.
    if (ObjectPhysicsComponent* launchPhysics =
            ecs::try_get<ObjectPhysicsComponent>(registry, entity)) {
        launchPhysics->velocityUnitsPerSecond.x +=
            request.launcherVelocityUnitsPerSecond.x;
        launchPhysics->velocityUnitsPerSecond.y +=
            request.launcherVelocityUnitsPerSecond.y;
        launchPhysics->velocityUnitsPerSecond.z +=
            request.launcherVelocityUnitsPerSecond.z;
    }

    if (behaviorKind == ObjectProjectileBehaviorKind::MissileAI) {
        FixedVec3 missileForward = initialForward;
        const Fixed uphill = projectile.target.z - projectile.start.z;
        if (request.hasLaunchOrientation && uphill > kFixedZero) {
            const Fixed planar = maxFixed(
                planarLength(subtract(projectile.target, projectile.start)),
                kFixedOne);
            missileForward.z += kFixedTwo * uphill / planar;
            missileForward = normalizedOr(missileForward, initialForward);
        }
        projectile.motion = ObjectProjectileMotion::MissileAI;
        projectile.flightPathForward = missileForward;
        projectile.hasFlightPathForward = true;
        ObjectMissileProjectileComponent missile;
        missile.forward = missileForward;
        missile.originalTarget = projectile.target;
        if (!request.projectileExhaust.empty() &&
            !asciiEqualIgnoreCase(request.projectileExhaust, "NONE")) {
            missile.exhaustParticleSystem = request.projectileExhaust;
        }
        missile.ignitionFx = plan->ignitionFx;
        missile.tryToFollowTarget = missileTryToFollowTarget;
        missile.trackingTarget = missile.tryToFollowTarget && request.intendedTarget;
        missile.useWeaponSpeed = plan->useWeaponSpeed;
        missile.detonateOnNoFuel = plan->detonateOnNoFuel;
        missile.noTurnDistanceRemaining = plan->noTurnDistance;
        missile.diveDistance = plan->diveDistance;
        missile.lockDistance = plan->lockDistance;
        missile.distanceScatterWhenJammed = plan->distanceScatterWhenJammed;
        const uint32_t ignitionMilliseconds = plan->ignitionDelayMilliseconds;
        missile.ignitionTick = request.confirmedTick +
            millisecondsToFrames(ignitionMilliseconds, rate);
        const uint32_t fuelMilliseconds = plan->fuelLifetimeMilliseconds;
        const uint64_t fuelFrames = millisecondsToFrames(fuelMilliseconds, rate);
        missile.fuelExpiryTick = fuelFrames == 0 ? UINT64_MAX
                                                 : missile.ignitionTick + fuelFrames;
        if (plan->killSelfDelayMilliseconds) {
            missile.killSelfDelayFrames = static_cast<uint32_t>(std::min<uint64_t>(
                millisecondsToFrames(*plan->killSelfDelayMilliseconds, rate),
                UINT32_MAX));
        } else {
            missile.killSelfDelayFrames = 3;
        }

        const game::FrozenLocomotorTemplate* locomotor = nullptr;
        for (const game::LocomotorSetDefinition& set : templateData.locomotorSets) {
            if (set.slot != game::LocomotorSetSlot::Normal) continue;
            for (const container::String& name : set.templates) {
                locomotor = content.findLocomotor(name);
                if (locomotor && locomotor->supportsRuntimeLocomotion()) break;
                locomotor = nullptr;
            }
            if (locomotor) break;
        }
        const Fixed authoredInitialVelocity = plan->initialVelocity;
        const Fixed maximumSpeed = missile.useWeaponSpeed
            ? weapon->fixed.weaponSpeed
            : locomotor
                ? locomotor->fixed.maximumSpeed
                : authoredInitialVelocity;
        const Fixed acceleration = missile.useWeaponSpeed
            ? weapon->fixed.weaponSpeed
            : locomotor
                ? locomotor->fixed.acceleration
                : kFixedZero;
        missile.currentSpeedUnitsPerSecond = missile.useWeaponSpeed
            ? weapon->fixed.weaponSpeed : authoredInitialVelocity;
        missile.maximumSpeedUnitsPerSecond = Fixed::max(
            kFixedZero, maximumSpeed);
        missile.accelerationUnitsPerSecondSq = Fixed::max(
            kFixedZero, acceleration);
        missile.maximumTurnRateRadiansPerSecond = locomotor
            ? Fixed::max(kFixedZero, locomotor->fixed.maximumTurnRate)
            : kFixedZero;
        missile.preferredHeight = locomotor
            ? Fixed::max(kFixedZero, locomotor->fixed.preferredHeight)
            : kFixedZero;
        missile.hasLocomotor = locomotor != nullptr;
        if (waypointPathStart) {
            projectile.intendedTarget = INVALID_OBJECT_ID;
            projectile.target = missileWaypointTarget(*waypointPathStart);
            missile.originalTarget = projectile.target;
            missile.trackingTarget = false;
            ecs::emplace<ObjectProjectileWaypointPathComponent>(
                registry, entity,
                ObjectProjectileWaypointPathComponent{
                    .currentWaypointId = waypointPathStart->id,
                    .graphRevision = request.waypointGraphRevision,
                });
        }
        missile.gravityUnitsPerSecondSq = gravityUnitsPerSecondSq;
        if (const ObjectPhysicsComponent* physics =
                ecs::try_get<ObjectPhysicsComponent>(registry, entity)) {
            missile.aerodynamicFrictionPerSecond =
                physics->aerodynamicFrictionPerSecond;
        }
        missile.velocityUnitsPerSecond = add(
            scale(missileForward, missile.currentSpeedUnitsPerSecond),
            request.launcherVelocityUnitsPerSecond);
        ecs::emplace<ObjectMissileProjectileComponent>(registry, entity,
                                                        std::move(missile));
    } else if (behaviorKind == ObjectProjectileBehaviorKind::NeutronMissile) {
        projectile.motion = ObjectProjectileMotion::NeutronMissile;
        projectile.flightPathForward = initialForward;
        projectile.hasFlightPathForward = true;
        ObjectNeutronMissileProjectileComponent neutron;
        neutron.forward = initialForward;
        neutron.target = projectile.target;
        if (!request.projectileExhaust.empty() &&
            !asciiEqualIgnoreCase(request.projectileExhaust, "NONE")) {
            neutron.exhaustParticleSystem = request.projectileExhaust;
        }
        neutron.launchFx = plan->launchFx;
        neutron.ignitionFx = plan->ignitionFx;
        neutron.intermediateTarget = neutron.target;
        neutron.intermediateTarget.z += plan->targetFromDirectlyAbove;
        neutron.reachedIntermediateTarget =
            plan->targetFromDirectlyAbove == kFixedZero;
        neutron.noTurnDistanceRemaining = plan->noTurnDistance;
        neutron.maximumTurnRateRadiansPerFrame =
            plan->maximumTurnRateRadiansPerSecond / framesPerSecond;
        neutron.forwardDamping = plan->forwardDamping;
        neutron.relativeSpeedPerFrame = plan->relativeSpeed;
        neutron.specialSpeedFrames = static_cast<uint32_t>(std::min<uint64_t>(
            millisecondsToFrames(plan->specialSpeedMilliseconds, rate),
            UINT32_MAX));
        neutron.specialSpeedHeight = plan->specialSpeedHeight;
        neutron.specialAccelerationFactor = plan->specialAccelerationFactor;
        neutron.specialJitterDistance = plan->specialJitterDistance;
        if (const std::optional<ObjectProjectileDeliveryDecalPlan>& delivery =
                plan->deliveryDecal) {
            neutron.deliveryDecalTexture = delivery->texture;
            neutron.deliveryDecalRadius = delivery->radius;
            neutron.deliveryDecalShadowTypeMask = delivery->shadowTypeMask;
            neutron.deliveryDecalMinimumOpacity = delivery->minimumOpacity;
            neutron.deliveryDecalMaximumOpacity = delivery->maximumOpacity;
            neutron.deliveryDecalOpacityThrobFrames = millisecondsToFrames(
                delivery->opacityThrobMilliseconds, rate);
            neutron.deliveryDecalColor = delivery->color;
            neutron.deliveryDecalUsesPlayerColor = delivery->usesPlayerColor;
            neutron.deliveryDecalOnlyVisibleToOwningPlayer =
                delivery->onlyVisibleToOwningPlayer;
            neutron.deliveryDecalAuthoredOrder = delivery->authoredOrder;
            neutron.deliveryDecalActive = true;
        }
        neutron.launchTick = request.confirmedTick;
        neutron.launchHeight = projectile.start.z;
        neutron.velocityPerFrame = scale(
            request.launcherVelocityUnitsPerSecond, kFixedOne / framesPerSecond);
        if (neutron.deliveryDecalActive) {
            m_events.push_back({
                .kind = ObjectProjectileEventKind::GroundDecalBegin,
                .projectile = projectileId,
                .launcher = projectile.launcher,
                .sourceShotSequence = projectile.sourceShotSequence,
                .position = neutron.target,
                .authoredOrder = neutron.deliveryDecalAuthoredOrder,
                .decalTexture = neutron.deliveryDecalTexture,
                .decalRadius = neutron.deliveryDecalRadius,
                .decalShadowTypeMask =
                    neutron.deliveryDecalShadowTypeMask,
                .decalMinimumOpacity =
                    neutron.deliveryDecalMinimumOpacity,
                .decalMaximumOpacity =
                    neutron.deliveryDecalMaximumOpacity,
                .decalOpacityThrobFrames =
                    neutron.deliveryDecalOpacityThrobFrames,
                .decalColor = neutron.deliveryDecalColor,
                .decalUsesPlayerColor =
                    neutron.deliveryDecalUsesPlayerColor,
                .decalOnlyVisibleToOwningPlayer =
                    neutron.deliveryDecalOnlyVisibleToOwningPlayer,
                .confirmedTick = request.confirmedTick,
            });
        }
        ecs::emplace<ObjectNeutronMissileProjectileComponent>(
            registry, entity, std::move(neutron));
    }

    if (behaviorKind == ObjectProjectileBehaviorKind::MissileAI ||
        behaviorKind == ObjectProjectileBehaviorKind::NeutronMissile) {
        writeAuthoritativeObjectPosition(
            registry, entity, projectile.start);
        projectFlightPathYaw(
            registry, entity, *transform, projectile);
        ecs::emplace<ObjectProjectileComponent>(registry, entity, projectile);
        synchronizeProjectileTerrainLayer(
            registry, entity, projectile, request.confirmedTick);
        m_events.push_back({
            .kind = ObjectProjectileEventKind::Spawned,
            .projectile = projectileId,
            .launcher = projectile.launcher,
            .target = projectile.intendedTarget,
            .sourceShotSequence = projectile.sourceShotSequence,
            .detonationWeapon = projectile.detonationWeapon,
            .position = projectile.start,
            .confirmedTick = request.confirmedTick,
        });
        trackActiveProjectile(projectileId);
        return true;
    }

    projectile.firstHeight = plan->firstHeight;
    projectile.secondHeight = plan->secondHeight;
    projectile.firstPercentIndent = plan->firstPercentIndent;
    projectile.secondPercentIndent = plan->secondPercentIndent;
    projectile.targetAdjustDistancePerSecond =
        plan->targetAdjustDistancePerSecond;
    projectile.orientToFlightPath = plan->orientToFlightPath;
    projectile.tumbleRandomly = plan->tumbleRandomly;

    const Fixed weaponSpeed = weapon->fixed.weaponSpeed;
    const Fixed minimumWeaponSpeed = weapon->fixed.minimumWeaponSpeed;
    const Fixed minimumAttackRange = weapon->fixed.minimumAttackRange;
    const Fixed attackRange = weapon->fixed.attackRange;
    Fixed speed = weaponSpeed;
    if (weapon->scaleWeaponSpeed && attackRange > minimumAttackRange) {
        const Fixed range = planarLength(subtract(projectile.target, projectile.start));
        // RefCode intentionally does not clamp this ratio. Normal Weapon
        // range admission keeps ordinary shots inside the interval, while a
        // scripted/forced out-of-range launch retains the authored extrapolated
        // speed instead of silently changing its flight time.
        const Fixed ratio = (range - minimumAttackRange) /
            (attackRange - minimumAttackRange);
        speed = minimumWeaponSpeed + ratio * (weaponSpeed - minimumWeaponSpeed);
    }
    if (speed <= kFixedZero) {
        m_events.push_back({
            .kind = ObjectProjectileEventKind::PathInvalid,
            .projectile = projectileId,
            .launcher = request.launcher,
            .sourcePathfindLayer = request.sourcePathfindLayer,
            .target = request.intendedTarget,
            .sourceShotSequence = request.sourceShotSequence,
            .detonationWeapon = request.detonationWeapon,
            .position = request.launchPosition,
            .weaponFxPolicy = weapon->suspendFxDelayMilliseconds != 0
                ? game::WeaponFxPolicy::SuppressedBySuspendDelay
                : game::WeaponFxPolicy::Play,
            .confirmedTick = request.confirmedTick,
        });
        return false;
    }
    // The legacy parser accepts arbitrarily small positive authored velocity.
    // We retain every value representable by Q32.32 instead of silently
    // speeding it up to an arbitrary minimum; the compact sampled-path state
    // has no allocation cost for a large segment count.
    projectile.speedUnitsPerSecond = speed;
    if (!rebuildDumbControls(projectile, terrain)) {
        m_events.push_back({
            .kind = ObjectProjectileEventKind::PathInvalid,
            .projectile = projectileId,
            .launcher = request.launcher,
            .sourcePathfindLayer = request.sourcePathfindLayer,
            .target = request.intendedTarget,
            .sourceShotSequence = request.sourceShotSequence,
            .detonationWeapon = request.detonationWeapon,
            .position = request.launchPosition,
            .weaponFxPolicy = weapon->suspendFxDelayMilliseconds != 0
                ? game::WeaponFxPolicy::SuppressedBySuspendDelay
                : game::WeaponFxPolicy::Play,
            .confirmedTick = request.confirmedTick,
        });
        return false;
    }

    const Fixed unitsPerFrame = projectile.speedUnitsPerSecond /
        Fixed{static_cast<int32_t>(std::min<uint32_t>(rate, kMaximumPathSegments))};
    projectile.pathSegments = ceilPositiveRatio(approximateBezierLength(projectile), unitsPerFrame);
    // Seed presentation attitude at structural creation instead of waiting
    // for the first movement tick. The fixed tangent is already authoritative
    // path data and is also used by directional detonation, while extraction
    // turns it into the complete pitch/yaw quaternion for the W3D model.
    refreshFlightPathForward(projectile, 0u);
    projectFlightPathYaw(registry, entity, *transform, projectile);
    const uint32_t lifespanMilliseconds =
        plan->maximumLifespanMilliseconds;
    const uint64_t lifespanFrames = millisecondsToFrames(lifespanMilliseconds, rate);
    // An explicit zero MaxLifespan remains a real zero-frame lifetime rather
    // than being reinterpreted as "live forever" by a zero sentinel.
    projectile.hasExpiryTick = true;
    projectile.expiryTick = request.confirmedTick + lifespanFrames;

    writeAuthoritativeObjectPosition(
        registry, entity, projectile.start);
    if (ObjectProjectileComponent* existing = ecs::try_get<ObjectProjectileComponent>(registry, entity)) {
        *existing = projectile;
    } else {
        ecs::emplace<ObjectProjectileComponent>(registry, entity, projectile);
        synchronizeProjectileTerrainLayer(
            registry, entity, projectile, request.confirmedTick);
    }
    if (projectile.tumbleRandomly && request.hasTumbleAngularRates) {
        if (ObjectPhysicsComponent* physics = ecs::try_get<ObjectPhysicsComponent>(registry, entity)) {
            if (request.hasLaunchOrientation) {
                physics->orientationX = quaternionForward(
                    request.launchOrientation);
                physics->orientationY = quaternionLocalY(
                    request.launchOrientation);
                physics->orientationZ = quaternionLocalZ(
                    request.launchOrientation);
                physics->orientationProjectionYaw = physics->yaw;
                physics->orientationProjectionPitch = physics->pitch;
                physics->orientationProjectionRoll = physics->roll;
                physics->orientationBasisValid = true;
            }
            physics->yawRate = request.tumbleYawRate;
            physics->pitchRate = request.tumblePitchRate;
            physics->rollRate = request.tumbleRollRate;
            physics->ownsAttitude = true;
        }
    }
    m_events.push_back({
        .kind = ObjectProjectileEventKind::Spawned,
        .projectile = projectileId,
        .launcher = projectile.launcher,
        .target = projectile.intendedTarget,
        .sourceShotSequence = projectile.sourceShotSequence,
        .detonationWeapon = projectile.detonationWeapon,
        .position = projectile.start,
        .confirmedTick = request.confirmedTick,
    });
    trackActiveProjectile(projectileId);
    return true;
}

} // namespace engine
