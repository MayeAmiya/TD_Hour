#include "core/container/container_types.h"
#include "core/container/hash_containers.h"
#include "debug/debug.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/ai/runtime/AIRecipeOwnerRoute.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"

#include "game/base/SimulationRandom.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/navigation/runtime/NavigationSystem.h"
#include "game/navigation/integration/NavigationTerrainLayerMapping.h"
#include "game/navigation/integration/NavigationDestinationAdjustment.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/definition/LocomotorTemplate.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/combat/ObjectCombatProfileRuntime.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/plan/movement/ObjectPhysicsPlanTypes.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"
#include "game/object/simulation/runtime/ObjectToppleTransaction.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"
#include "core/math/wwmath/base/wwmath.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>
#include "game/object/simulation/runtime/ObjectSimulationDetail.h"
#include "game/object/component/ObjectDirty.h"

namespace engine {

namespace {

using PhysicsScalar = math::q32_32;
using HealthScalar = ObjectHealthComponent::Scalar;

const PhysicsScalar kPhysicsZero{int32_t{0}};
const PhysicsScalar kPhysicsOne{int32_t{1}};
const PhysicsScalar kPhysicsTwo{int32_t{2}};
constexpr PhysicsScalar kMovementArrivalEpsilonFixed =
    PhysicsScalar::from_fraction(1, 1000);
constexpr PhysicsScalar kMovementPi =
    PhysicsScalar::from_raw(13'493'037'705ll);
const PhysicsScalar kMovementFullTurn = kPhysicsTwo * kMovementPi;
const PhysicsScalar kMovementHalfPi = kMovementPi / kPhysicsTwo;
const PhysicsScalar kMovementQuarterPi = kMovementPi /
    PhysicsScalar{int32_t{4}};
constexpr PhysicsScalar kPhysicsGroundEpsilon =
    PhysicsScalar::from_fraction(1, 10'000);
constexpr PhysicsScalar kPhysicsRestEpsilon =
    PhysicsScalar::from_fraction(1, 100);
constexpr PhysicsScalar kPhysicsMinimumFrictionPerFrame =
    PhysicsScalar::from_fraction(1, 100);
constexpr PhysicsScalar kPhysicsMaximumFrictionPerFrame =
    PhysicsScalar::from_fraction(99, 100);
const PhysicsScalar kPhysicsMinimumGroundStiffness{
    PhysicsSimulationRules::kMinimumGroundStiffness};
const PhysicsScalar kPhysicsMaximumGroundStiffness{
    PhysicsSimulationRules::kMaximumGroundStiffness};
const HealthScalar kHealthZero{};
const HealthScalar kHealthOne{int32_t{1}};

[[nodiscard]] bool hasKind(const ObjectKindOfComponent* kinds,
                           game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

[[nodiscard]] PhysicsScalar normalizeMovementAngle(
    PhysicsScalar angle) noexcept {
    int64_t raw = angle.raw() % kMovementFullTurn.raw();
    if (raw > kMovementPi.raw()) raw -= kMovementFullTurn.raw();
    if (raw < -kMovementPi.raw()) raw += kMovementFullTurn.raw();
    return PhysicsScalar::from_raw(raw);
}

[[nodiscard]] PhysicsScalar moveTowardsFixed(
    PhysicsScalar current, PhysicsScalar target,
    PhysicsScalar maximumDelta) noexcept {
    if (maximumDelta <= kPhysicsZero) return current;
    if (current < target)
        return PhysicsScalar::min(target, current + maximumDelta);
    return PhysicsScalar::max(target, current - maximumDelta);
}

[[nodiscard]] PhysicsScalar length2D(PhysicsScalar x,
                                     PhysicsScalar y) noexcept {
    return PhysicsScalar::sqrt(x * x + y * y);
}

[[nodiscard]] PhysicsScalar length3D(
    PhysicsScalar x, PhysicsScalar y, PhysicsScalar z) noexcept {
    return PhysicsScalar::sqrt(x * x + y * y + z * z);
}

[[nodiscard]] uint32_t ceilPositiveMovementRatio(
    PhysicsScalar numerator, PhysicsScalar denominator) noexcept {
    if (numerator <= kPhysicsZero || denominator <= kPhysicsZero) return 0;
    const uint64_t top = static_cast<uint64_t>(numerator.raw());
    const uint64_t bottom = static_cast<uint64_t>(denominator.raw());
    uint64_t result = top / bottom;
    if (top % bottom != 0) ++result;
    return static_cast<uint32_t>(std::min<uint64_t>(
        result, std::numeric_limits<uint32_t>::max()));
}

[[nodiscard]] LogicFixedVec3 scaleOrientationAxis(
    const LogicFixedVec3& axis, PhysicsScalar scale) noexcept {
    return {axis.x * scale, axis.y * scale, axis.z * scale};
}

[[nodiscard]] LogicFixedVec3 addOrientationAxes(
    const LogicFixedVec3& left, const LogicFixedVec3& right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] PhysicsScalar dotOrientationAxes(
    const LogicFixedVec3& left, const LogicFixedVec3& right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

[[nodiscard]] LogicFixedVec3 crossOrientationAxes(
    const LogicFixedVec3& left, const LogicFixedVec3& right) noexcept {
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

[[nodiscard]] bool normalizeOrientationAxis(
    LogicFixedVec3& axis) noexcept {
    const PhysicsScalar lengthSquared = dotOrientationAxes(axis, axis);
    if (lengthSquared <= kPhysicsZero) return false;
    const PhysicsScalar length = PhysicsScalar::sqrt(lengthSquared);
    if (length <= kPhysicsZero) return false;
    axis = scaleOrientationAxis(axis, kPhysicsOne / length);
    return true;
}

void projectPhysicsOrientation(ObjectPhysicsComponent& physics) noexcept {
    const PhysicsScalar horizontal = PhysicsScalar::sqrt(
        physics.orientationX.x * physics.orientationX.x +
        physics.orientationX.y * physics.orientationX.y);
    physics.yaw = math::fixed_atan2(
        physics.orientationX.y, physics.orientationX.x);
    physics.pitch = math::fixed_atan2(
        -physics.orientationX.z, horizontal);
    physics.roll = math::fixed_atan2(
        -physics.orientationY.z, physics.orientationZ.z);
    physics.orientationProjectionYaw = physics.yaw;
    physics.orientationProjectionPitch = physics.pitch;
    physics.orientationProjectionRoll = physics.roll;
}

} // namespace

namespace object_simulation_detail {

void rebuildPhysicsOrientation(ObjectPhysicsComponent& physics) noexcept {
    const math::q32_32_sincos yaw = math::fixed_sincos(physics.yaw);
    const math::q32_32_sincos pitch = math::fixed_sincos(physics.pitch);
    // PhysicsBehavior::setAngles() uses pre-X(-roll), pre-Y(pitch),
    // pre-Z(yaw).  These columns are the exact Z-up basis produced by that
    // order and therefore also match Get_Z_Rotation's atan2(X.y, X.x).
    const math::q32_32_sincos negativeRoll =
        math::fixed_sincos(-physics.roll);
    physics.orientationX = {
        yaw.cosine * pitch.cosine,
        yaw.sine * pitch.cosine,
        -pitch.sine,
    };
    physics.orientationY = {
        yaw.cosine * pitch.sine * negativeRoll.sine -
            yaw.sine * negativeRoll.cosine,
        yaw.sine * pitch.sine * negativeRoll.sine +
            yaw.cosine * negativeRoll.cosine,
        pitch.cosine * negativeRoll.sine,
    };
    physics.orientationZ = {
        yaw.cosine * pitch.sine * negativeRoll.cosine +
            yaw.sine * negativeRoll.sine,
        yaw.sine * pitch.sine * negativeRoll.cosine -
            yaw.cosine * negativeRoll.sine,
        pitch.cosine * negativeRoll.cosine,
    };
    physics.orientationBasisValid = true;
    physics.orientationProjectionYaw = physics.yaw;
    physics.orientationProjectionPitch = physics.pitch;
    physics.orientationProjectionRoll = physics.roll;
}

[[nodiscard]] bool physicsOrientationProjectionChanged(
    const ObjectPhysicsComponent& physics) noexcept {
    return !physics.orientationBasisValid ||
        physics.yaw.raw() != physics.orientationProjectionYaw.raw() ||
        physics.pitch.raw() != physics.orientationProjectionPitch.raw() ||
        physics.roll.raw() != physics.orientationProjectionRoll.raw();
}

void postRotatePhysicsOrientation(
    ObjectPhysicsComponent& physics, PhysicsScalar roll,
    PhysicsScalar pitch, PhysicsScalar yaw) noexcept {
    if (physicsOrientationProjectionChanged(physics))
        rebuildPhysicsOrientation(physics);

    const math::q32_32_sincos xRotation = math::fixed_sincos(roll);
    LogicFixedVec3 oldY = physics.orientationY;
    LogicFixedVec3 oldZ = physics.orientationZ;
    physics.orientationY = addOrientationAxes(
        scaleOrientationAxis(oldY, xRotation.cosine),
        scaleOrientationAxis(oldZ, xRotation.sine));
    physics.orientationZ = addOrientationAxes(
        scaleOrientationAxis(oldY, -xRotation.sine),
        scaleOrientationAxis(oldZ, xRotation.cosine));

    const math::q32_32_sincos yRotation = math::fixed_sincos(pitch);
    LogicFixedVec3 oldX = physics.orientationX;
    oldZ = physics.orientationZ;
    physics.orientationX = addOrientationAxes(
        scaleOrientationAxis(oldX, yRotation.cosine),
        scaleOrientationAxis(oldZ, -yRotation.sine));
    physics.orientationZ = addOrientationAxes(
        scaleOrientationAxis(oldX, yRotation.sine),
        scaleOrientationAxis(oldZ, yRotation.cosine));

    const math::q32_32_sincos zRotation = math::fixed_sincos(yaw);
    oldX = physics.orientationX;
    oldY = physics.orientationY;
    physics.orientationX = addOrientationAxes(
        scaleOrientationAxis(oldX, zRotation.cosine),
        scaleOrientationAxis(oldY, zRotation.sine));
    physics.orientationY = addOrientationAxes(
        scaleOrientationAxis(oldX, -zRotation.sine),
        scaleOrientationAxis(oldY, zRotation.cosine));

    // Fixed polynomial/truncation error otherwise accumulates forever.  A
    // deterministic Gram-Schmidt pass preserves a right-handed orthonormal
    // basis without rebuilding it from lossy Euler projections.
    if (!normalizeOrientationAxis(physics.orientationX)) {
        rebuildPhysicsOrientation(physics);
        return;
    }
    physics.orientationY = addOrientationAxes(
        physics.orientationY,
        scaleOrientationAxis(
            physics.orientationX,
            -dotOrientationAxes(physics.orientationX,
                                physics.orientationY)));
    if (!normalizeOrientationAxis(physics.orientationY)) {
        rebuildPhysicsOrientation(physics);
        return;
    }
    physics.orientationZ = crossOrientationAxes(
        physics.orientationX, physics.orientationY);
    if (!normalizeOrientationAxis(physics.orientationZ)) {
        rebuildPhysicsOrientation(physics);
        return;
    }
    projectPhysicsOrientation(physics);
}

void integratePhysicsOrientation(
    ObjectPhysicsComponent& physics, PhysicsScalar frameSeconds) noexcept {
    if (physicsOrientationProjectionChanged(physics))
        rebuildPhysicsOrientation(physics);
    PhysicsScalar pitchDelta =
        physics.pitchRate * physics.pitchRollYawFactor * frameSeconds;
    if (physics.centerOfMassOffset.raw() != 0) {
        const PhysicsScalar horizontal = PhysicsScalar::sqrt(
            physics.orientationX.x * physics.orientationX.x +
            physics.orientationX.y * physics.orientationX.y);
        const PhysicsScalar pitchAngle = math::fixed_atan2(
            physics.orientationX.z, horizontal);
        constexpr PhysicsScalar kHalfPi =
            PhysicsScalar::from_raw(6746518852ll);
        const PhysicsScalar remaining = physics.centerOfMassOffset > kPhysicsZero
            ? kHalfPi - pitchAngle
            : -kHalfPi + pitchAngle;
        pitchDelta *= math::fixed_sin(remaining);
    }
    postRotatePhysicsOrientation(
        physics,
        physics.rollRate * physics.pitchRollYawFactor * frameSeconds,
        pitchDelta,
        physics.yawRate * physics.pitchRollYawFactor * frameSeconds);
}

void setPhysicsModelCondition(ecs::registry& registry, ecs::entity entity,
                              game::ModelConditionFlag flag, bool enabled) {
    RenderModelComponent* render =
        ecs::try_get<RenderModelComponent>(registry, entity);
    if (!render) return;
    const uint32_t flagIndex = game::modelConditionFlagIndex(flag);
    const bool wasEnabled = flagIndex < game::kModelConditionFlagCount &&
        (render->modelConditionFlags.words[flagIndex / 64u] &
         (uint64_t{1} << (flagIndex % 64u))) != 0;
    if (wasEnabled == enabled) return;
    if (enabled) {
        render->modelConditionFlags.set(flag);
    } else {
        render->modelConditionFlags.clear(game::modelConditionMaskOf(flag));
    }
    markObjectDirty(
        registry, entity,
        objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
            objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
}

void synchronizePhysicsFreeFall(ecs::registry& registry, ecs::entity entity,
                                ObjectPhysicsComponent& physics,
                                bool airborne, uint64_t tick) {
    if (physics.inFreeFall && airborne) {
        static_cast<void>(ObjectDisabledSystem::setUntil(
            registry, entity, ObjectDisabledReason::Freefall,
            OBJECT_DISABLED_FOREVER_TICK, tick));
        setPhysicsModelCondition(
            registry, entity, game::ModelConditionFlag::FreeFall, true);
    } else if (!airborne) {
        physics.inFreeFall = false;
        static_cast<void>(ObjectDisabledSystem::clear(
            registry, entity, ObjectDisabledReason::Freefall, tick));
        setPhysicsModelCondition(
            registry, entity, game::ModelConditionFlag::FreeFall, false);
    }
}

[[nodiscard]] LogicFixedVec3 addFixed(const LogicFixedVec3& left,
                                       const LogicFixedVec3& right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] LogicFixedVec3 scaleFixed(const LogicFixedVec3& value,
                                         PhysicsScalar amount) noexcept {
    return {value.x * amount, value.y * amount, value.z * amount};
}

[[nodiscard]] PhysicsScalar squaredLengthFixed(const LogicFixedVec3& value) noexcept {
    return value.x * value.x + value.y * value.y + value.z * value.z;
}

[[nodiscard]] PhysicsScalar clampPhysics(PhysicsScalar value, PhysicsScalar minimum,
                                          PhysicsScalar maximum) noexcept {
    return value < minimum ? minimum : value > maximum ? maximum : value;
}
} // namespace object_simulation_detail

namespace object_simulation_detail {

[[nodiscard]] ObjectPhysicsComponent compilePhysicsComponent(
    const game::ObjectPhysicsPlan& plan,
    const ObjectFixedTransformComponent& transform,
    const GameContentSnapshot& content,
    const game::terrain::TerrainLogic& terrain,
    uint32_t pathfindLayer,
    const ObjectSimulationRules& rules) {
    ObjectPhysicsComponent physics;
    physics.position = transform.position;
    physics.lastPublishedPosition = physics.position;
    physics.collisionStartPosition = physics.position;
    physics.yaw = transform.yawRadians;
    physics.collisionStartYaw = physics.yaw;
    physics.lastPublishedYaw = physics.yaw;
    rebuildPhysicsOrientation(physics);
    physics.mass = plan.mass;
    physics.shockResistance = plan.shockResistance;
    physics.shockMaxYaw = plan.shockMaxYaw;
    physics.shockMaxPitch = plan.shockMaxPitch;
    physics.shockMaxRoll = plan.shockMaxRoll;
    physics.forwardFrictionPerSecond = plan.forwardFrictionPerSecond;
    physics.lateralFrictionPerSecond = plan.lateralFrictionPerSecond;
    physics.zFrictionPerSecond = plan.zFrictionPerSecond;
    physics.aerodynamicFrictionPerSecond = plan.aerodynamicFrictionPerSecond;
    physics.centerOfMassOffset = plan.centerOfMassOffset;
    physics.fallHeightDamageFactor = plan.fallHeightDamageFactor;
    physics.pitchRollYawFactor = plan.pitchRollYawFactor;
    physics.allowBouncing = plan.allowBouncing;
    physics.originalAllowBouncing = physics.allowBouncing;
    physics.allowCollideForce = plan.allowCollideForce;
    physics.killWhenRestingOnGround = plan.killWhenRestingOnGround;
    physics.crashIntoBuildingWeapon = plan.crashIntoBuildingWeapon;
    physics.crashIntoNonBuildingWeapon = plan.crashIntoNonBuildingWeapon;
    physics.crashIntoBuildingWeaponContent =
        content.findWeaponId(plan.crashIntoBuildingWeapon);
    physics.crashIntoNonBuildingWeaponContent =
        content.findWeaponId(plan.crashIntoNonBuildingWeapon);

    const PhysicsScalar gravityMagnitude = PhysicsScalar::abs(
        rules.gravityUnitsPerSecondSq);
    physics.minFallSpeedUnitsPerSecond = PhysicsScalar::sqrt(
        kPhysicsTwo * gravityMagnitude * PhysicsScalar::max(
            kPhysicsZero, plan.minimumFallHeight));

    const PhysicsScalar ground = terrain.isLoaded()
        ? PhysicsScalar::from_raw(terrain.pathfindLayerHeightRawAt(
              pathfindLayer, transform.position.x.raw(),
              transform.position.y.raw())
              .value_or(terrain.groundHeightRaw(
                  transform.position.x.raw(), transform.position.y.raw())))
        : PhysicsScalar{};
    physics.wasAirborneLastFrame = transform.position.z >
        ground + kPhysicsGroundEpsilon;
    physics.state = physics.wasAirborneLastFrame ? ObjectPhysicsMotionState::Airborne
                                                  : ObjectPhysicsMotionState::Grounded;
    physics.hasAuthoritativePosition = true;
    return physics;
}

[[nodiscard]] PhysicsScalar physicsLayerHeight(
    const game::terrain::TerrainLogic& terrain,
    const ecs::registry& registry, ecs::entity entity,
    const LogicFixedVec3& position) noexcept {
    if (!terrain.isLoaded()) return kPhysicsZero;
    const ObjectTerrainLayerComponent* layer =
        ecs::try_get<ObjectTerrainLayerComponent>(registry, entity);
    const ObjectProjectileComponent* projectile =
        ecs::try_get<ObjectProjectileComponent>(registry, entity);
    const int64_t heightRaw = terrain.pathfindLayerHeightRawAt(
        projectile ? projectile->pathfindLayer
                   : layer ? layer->pathfindLayer
                           : game::terrain::kGroundPathfindLayer,
        position.x.raw(), position.y.raw()).value_or(
            terrain.groundHeightRaw(position.x.raw(), position.y.raw()));
    return PhysicsScalar::from_raw(heightRaw);
}

[[nodiscard]] PhysicsScalar physicsCollisionSupportHeight(
    const game::terrain::TerrainLogic& terrain,
    const ecs::registry& registry, ecs::entity entity,
    const LogicFixedVec3& position) noexcept {
    PhysicsScalar height = physicsLayerHeight(
        terrain, registry, entity, position);
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    const ObjectCarrierDeckComponent* deck =
        ecs::try_get<ObjectCarrierDeckComponent>(registry, entity);
    if (status && deck && status->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::DeckHeightOffset))) {
        height += deck->heightOffset;
    }
    return height;
}

[[nodiscard]] bool physicsDeckTaxiing(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    if (!status || !status->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::DeckHeightOffset))) {
        return false;
    }
    const ObjectAirfieldComponent* airfield =
        ecs::try_get<ObjectAirfieldComponent>(registry, entity);
    return airfield && std::any_of(
        airfield->jetAi.begin(), airfield->jetAi.end(),
        [](const ObjectJetAiRuntime& runtime) {
            return runtime.state == ObjectAircraftRuntimeState::Taxiing;
        });
}

void publishPhysicsTransform(ObjectPhysicsComponent& physics,
                             ObjectFixedTransformComponent& fixedTransform,
                             TransformComponent& transform) noexcept {
    fixedTransform.position = physics.position;
    fixedTransform.yawRadians = physics.yaw;
    fixedTransform.authoritative = true;
    transform.x = physics.position.x.to_float();
    transform.y = physics.position.y.to_float();
    transform.z = physics.position.z.to_float();
    transform.rotation = physics.yaw.to_float();
    physics.lastPublishedPosition = physics.position;
    physics.lastPublishedYaw = physics.yaw;
}

[[nodiscard]] bool battleBusOwnsFreeBodyTranslation(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, entity);
    if (!runtime || !runtime->plan) return false;
    const size_t count = std::min(runtime->plan->behaviorRules.size(),
                                  runtime->behaviorStates.size());
    for (size_t index = 0; index < count; ++index) {
        if (runtime->plan->behaviorRules[index].kind ==
                ObjectTransportBehaviorKind::BattleBusSlowDeath &&
            runtime->behaviorStates[index].phase ==
                ObjectTransportBehaviorPhase::BattleBusUndeath) {
            return true;
        }
    }
    return false;
}

void updatePhysics(ecs::registry& registry, ObjectLifecycle& lifecycle,
                   const game::terrain::TerrainLogic& terrain,
                   const ObjectSimulationRules& rules, uint64_t tick,
                   container::Vector<ObjectPhysicsEvent>& events,
                   container::Vector<ObjectDamageRequest>& outDamage,
                   ObjectPhysicsScratch& scratch) {
    auto& candidates = scratch.candidates;
    candidates.clear();
    const PhysicsScalar frameSeconds = rules.logicDeltaSeconds;
    if (frameSeconds <= kPhysicsZero) return;

    using Candidate = ObjectPhysicsScratch::Candidate;
    const auto view = ecs::view<ObjectIdentityComponent, ObjectPhysicsComponent, TransformComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity = view.template get<ObjectIdentityComponent>(entity);
        if (!identity.id || !lifecycle.entityFromId(identity.id) ||
            lifecycle.isPendingDestroy(identity.id) ||
            ecs::try_get<ObjectContainedByComponent>(registry, entity)) continue;
        candidates.push_back({.id = identity.id, .entity = entity});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        return left.id < right.id;
    });

    const PhysicsScalar gravity = rules.gravityUnitsPerSecondSq;
    const PhysicsScalar logicFrameRate{static_cast<int32_t>(
        std::min<uint32_t>(std::max<uint32_t>(
            1u, rules.logicFramesPerSecond),
            static_cast<uint32_t>(std::numeric_limits<int32_t>::max())))};
    // Thing::isSignificantlyAboveTerrain uses the height an object falls in
    // three legacy frames: -(3*3)*gravityPerFrame. This boundary controls
    // both ground-vs-aerodynamic friction and stun relief; treating every
    // positive epsilon as flight changes low hops and slope motion.
    const PhysicsScalar gravityPerFrame =
        gravity / logicFrameRate / logicFrameRate;
    const PhysicsScalar significantAirborneHeight =
        gravityPerFrame < kPhysicsZero
            ? -gravityPerFrame * PhysicsScalar{int32_t{9}}
            : kPhysicsZero;
    const PhysicsScalar groundStiffness = clampPhysics(
        rules.groundStiffness,
        kPhysicsMinimumGroundStiffness, kPhysicsMaximumGroundStiffness);
    for (const Candidate& candidate : candidates) {
        ObjectPhysicsComponent& physics = ecs::get<ObjectPhysicsComponent>(registry, candidate.entity);
        TransformComponent& transform = ecs::get<TransformComponent>(registry, candidate.entity);
        ObjectFixedTransformComponent* fixedTransform =
            ecs::try_get<ObjectFixedTransformComponent>(
                registry, candidate.entity);
        if (!fixedTransform || !fixedTransform->authoritative) continue;
        physics.collisionStartPosition = physics.hasAuthoritativePosition
            ? physics.position
            : fixedTransform->position;
        physics.collisionStartYaw = physics.lastPublishedYaw;
        physics.collisionStartTick = tick;

        if (physics.overlapLedgerTick != tick) {
            physics.previousOverlap = physics.currentOverlap;
            physics.currentOverlap = INVALID_OBJECT_ID;
            physics.overlapLedgerTick = tick;
        }

        // PhysicsBehavior is the unusual update module that processes every
        // disabled reason except HELD. RefCode preserves velocity while held,
        // discards this frame's accumulated acceleration and does not apply
        // gravity, friction, attitude or Transform integration.
        if (isObjectDisabledBy(
                registry, candidate.entity, ObjectDisabledReason::Held,
                tick)) {
            physics.previousAcceleration = {
                physics.pendingForce.x / physics.mass,
                physics.pendingForce.y / physics.mass,
                physics.pendingForce.z / physics.mass,
            };
            physics.pendingForce = {};
            continue;
        }

        // Shared locomotion owns ordinary unit translation. Every typed
        // projectile controller likewise owns XYZ; Dumb may delegate tumble
        // attitude to PhysicsBehavior, while Missile/Neutron publish their
        // complete 3D attitude from the guided trajectory.
        const ObjectProjectileComponent* projectile =
            ecs::try_get<ObjectProjectileComponent>(registry, candidate.entity);
        const ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(registry, candidate.entity);
        const bool locomotionOwnsTranslation =
            locomotion != nullptr &&
            !physics.forceFreeBodyTranslation &&
            !battleBusOwnsFreeBodyTranslation(registry, candidate.entity);
        const bool projectileOwnsTranslation = projectile != nullptr;
        if (locomotionOwnsTranslation || projectileOwnsTranslation) {
            physics.position = projectile
                ? projectile->position
                : fixedTransform->position;
            physics.lastPublishedPosition = physics.position;
            if (projectile && projectile->motion != ObjectProjectileMotion::DumbBezier) {
                // Guided projectile update already published deterministic
                // pitch/yaw/roll from its fixed flight vector this tick.
                // Physics owns no integration here and must not erase that
                // 3D attitude before render extraction observes it.
                physics.ownsAttitude = true;
            } else if (projectile && projectile->motion == ObjectProjectileMotion::DumbBezier &&
                projectile->tumbleRandomly && physics.ownsAttitude) {
                // DumbProjectileBehavior continues to force Bezier XYZ.  A
                // TumbleRandomly PhysicsBehavior owns only attitude and does
                // not advance on the launch frame, matching the source
                // module boundary before its first regular update.
                if (tick > projectile->spawnedTick) {
                    integratePhysicsOrientation(physics, frameSeconds);
                } else if (physicsOrientationProjectionChanged(physics)) {
                    rebuildPhysicsOrientation(physics);
                }
                writeAuthoritativeObjectYaw(
                    registry, candidate.entity, physics.yaw);
            } else {
                // This pass intentionally does not integrate free-body
                // attitude for a locomotor or ordinary path-owned projectile.
                // Mirror the authoritative legacy yaw so a future Physics
                // handoff starts from the current direction.
                physics.yaw = fixedTransform->yawRadians;
                // A ground locomotor's pitch/roll are no longer erased here:
                // updateLocomotorAttitude below owns that pair for terrain
                // conformance and republishes the basis itself. Everything
                // else keeps the historical flat projection.
                if (!locomotionOwnsTranslation ||
                    !physics.conformsToTerrain) {
                    physics.pitch = {};
                    physics.roll = {};
                    rebuildPhysicsOrientation(physics);
                }
                physics.ownsAttitude = false;
            }
            if (projectile && projectile->motion !=
                    ObjectProjectileMotion::DumbBezier &&
                physicsOrientationProjectionChanged(physics)) {
                rebuildPhysicsOrientation(physics);
            }
            physics.lastPublishedYaw = physics.yaw;
            physics.hasAuthoritativePosition = true;
            const PhysicsScalar ownedGround =
                physicsLayerHeight(terrain, registry, candidate.entity,
                                   physics.position);
            const bool ownedAirborne =
                physics.position.z > ownedGround + kPhysicsGroundEpsilon;
            if (locomotionOwnsTranslation && !projectile) {
                // Movement runs before Physics in the confirmed tick and
                // sleeps every settled object, so the chassis spring is driven
                // from here: a parked vehicle on a slope must stay conformed,
                // and this pass visits every live physics object every tick.
                // RefCode's wheeled chassis suspends conforming on
                // isSignificantlyAboveTerrain, not on any positive epsilon.
                // Using the same three-frame fall height here also keeps a
                // locomotor whose authored resting groundOffset is slightly
                // above its layer from being treated as permanently airborne.
                const bool attitudeChanged = updateLocomotorAttitude(
                    *locomotion, physics, *fixedTransform, terrain, rules,
                    physics.position.z >
                        ownedGround + significantAirborneHeight,
                    hasKind(ecs::try_get<ObjectKindOfComponent>(
                                registry, candidate.entity),
                            game::ObjectKindOf::Immobile));
                if (attitudeChanged) {
                    // Movement only marks objects it admitted this tick. A
                    // parked vehicle settling onto a slope is invisible to
                    // that active set, so the conform has to announce itself
                    // or incremental extraction would reuse a flat snapshot.
                    markObjectDirty(
                        registry, candidate.entity,
                        objectDirtyBit(
                            ObjectDirtyDomain::RenderExtraction));
                }
            }
            synchronizePhysicsFreeFall(
                registry, candidate.entity, physics, ownedAirborne, tick);
            physics.physicsEverUpdated = true;
            physics.sleeping = false;
            // This path never reaches the free-body integrator, so nothing
            // below converts pendingForce into acceleration. Discard it at the
            // same explicit boundary the HELD path uses instead of letting it
            // accumulate until the object changes controller.
            physics.pendingForce = {};
            continue;
        }

        // Reaching the free-body integrator means the chassis suspension no
        // longer owns this object's attitude (a flung wreck, a BattleBus
        // undeath, an explicit force handoff). Retire the latch so the
        // terrain conform can never be attributed to a tumbling body.
        if (physics.conformsToTerrain) {
            physics.conformsToTerrain = false;
            physics.chassisPitch = kPhysicsZero;
            physics.chassisPitchRate = kPhysicsZero;
            physics.chassisRoll = kPhysicsZero;
            physics.chassisRollRate = kPhysicsZero;
            physics.chassisAccelerationPitch = kPhysicsZero;
            physics.chassisAccelerationPitchRate = kPhysicsZero;
            physics.chassisPreviousForwardSpeed = kPhysicsZero;
        }
        if (!physics.hasAuthoritativePosition) {
            physics.position = fixedTransform->position;
            physics.lastPublishedPosition = physics.position;
            physics.hasAuthoritativePosition = true;
            physics.sleeping = false;
        }
        if (physics.sleeping) continue;

        const bool wasAirborneLastFrame = physics.wasAirborneLastFrame;
        const PhysicsScalar groundBefore = physicsLayerHeight(
            terrain, registry, candidate.entity, physics.position);
        const bool airborneBefore = physics.position.z > groundBefore + kPhysicsGroundEpsilon;
        const bool significantlyAirborneBefore =
            physics.position.z > groundBefore + significantAirborneHeight;
        if (physics.stickToGround && !physics.allowToFall && airborneBefore) {
            physics.position.z = groundBefore;
            physics.velocityUnitsPerSecond.z = kPhysicsZero;
        }

        const LogicFixedVec3 acceleration = {
            physics.pendingForce.x / physics.mass,
            physics.pendingForce.y / physics.mass,
            physics.pendingForce.z / physics.mass + gravity,
        };
        physics.previousAcceleration = acceleration;
        physics.pendingForce = {};

        // RefCode decomposes the ground velocity into the object's forward
        // and lateral axes before applying the independently authored
        // friction coefficients. Keep that decomposition in Q32.32; using a
        // single world-space lateral coefficient silently ignored stock
        // ForwardFriction values on free bodies.
        const bool useAerodynamicFriction =
            significantlyAirborneBefore &&
            !physics.applyFriction2DWhenAirborne &&
            !physicsDeckTaxiing(registry, candidate.entity);
        if (useAerodynamicFriction) {
            const PhysicsScalar friction = clampPhysics(
                physics.aerodynamicFrictionPerSecond * frameSeconds,
                kPhysicsZero, kPhysicsMaximumFrictionPerFrame);
            const PhysicsScalar retainedVelocity = kPhysicsOne - friction;
            physics.velocityUnitsPerSecond.x *= retainedVelocity;
            physics.velocityUnitsPerSecond.y *= retainedVelocity;
            physics.velocityUnitsPerSecond.z *= retainedVelocity;
            physics.yawRate *= retainedVelocity;
            physics.pitchRate *= retainedVelocity;
            physics.rollRate *= retainedVelocity;
        } else {
            const math::q32_32_sincos direction =
                math::fixed_sincos(physics.yaw);
            const PhysicsScalar forwardSpeed =
                physics.velocityUnitsPerSecond.x * direction.cosine +
                physics.velocityUnitsPerSecond.y * direction.sine;
            const PhysicsScalar lateralSpeed =
                physics.velocityUnitsPerSecond.x * -direction.sine +
                physics.velocityUnitsPerSecond.y * direction.cosine;
            const PhysicsScalar forwardFriction = clampPhysics(
                physics.forwardFrictionPerSecond * frameSeconds,
                kPhysicsMinimumFrictionPerFrame,
                kPhysicsMaximumFrictionPerFrame);
            const PhysicsScalar lateralFriction = clampPhysics(
                physics.lateralFrictionPerSecond * frameSeconds,
                kPhysicsMinimumFrictionPerFrame,
                kPhysicsMaximumFrictionPerFrame);
            const PhysicsScalar retainedForward =
                physics.motiveForceExpiresTick > tick
                    ? kPhysicsOne : kPhysicsOne - forwardFriction;
            const PhysicsScalar retainedLateral =
                kPhysicsOne - lateralFriction;
            const PhysicsScalar dampedForward =
                forwardSpeed * retainedForward;
            const PhysicsScalar dampedLateral =
                lateralSpeed * retainedLateral;
            physics.velocityUnitsPerSecond.x =
                dampedForward * direction.cosine -
                dampedLateral * direction.sine;
            physics.velocityUnitsPerSecond.y =
                dampedForward * direction.sine +
                dampedLateral * direction.cosine;

            // PhysicsBehavior::applyFrictionalForces always damps angular
            // rates by the legacy default lateral coefficient on the ground,
            // independently of an authored LateralFriction override.
            const PhysicsScalar angularFriction = clampPhysics(
                PhysicsScalar::from_fraction(9, 2) * frameSeconds,
                kPhysicsMinimumFrictionPerFrame,
                kPhysicsMaximumFrictionPerFrame);
            const PhysicsScalar retainedAngular =
                kPhysicsOne - angularFriction;
            physics.yawRate *= retainedAngular;
            physics.pitchRate *= retainedAngular;
            physics.rollRate *= retainedAngular;
        }

        physics.velocityUnitsPerSecond = addFixed(
            physics.velocityUnitsPerSecond, scaleFixed(acceleration, frameSeconds));
        constexpr PhysicsScalar kVelocityClamp =
            PhysicsScalar::from_fraction(1, 1000);
        if (PhysicsScalar::abs(physics.velocityUnitsPerSecond.x) < kVelocityClamp)
            physics.velocityUnitsPerSecond.x = kPhysicsZero;
        if (PhysicsScalar::abs(physics.velocityUnitsPerSecond.y) < kVelocityClamp)
            physics.velocityUnitsPerSecond.y = kPhysicsZero;
        if (PhysicsScalar::abs(physics.velocityUnitsPerSecond.z) < kVelocityClamp)
            physics.velocityUnitsPerSecond.z = kPhysicsZero;
        if (PhysicsScalar::abs(physics.yawRate) < kVelocityClamp)
            physics.yawRate = kPhysicsZero;
        if (PhysicsScalar::abs(physics.pitchRate) < kVelocityClamp)
            physics.pitchRate = kPhysicsZero;
        if (PhysicsScalar::abs(physics.rollRate) < kVelocityClamp)
            physics.rollRate = kPhysicsZero;
        // Keep the impact speed before ground clamping/bounce response.  The
        // original PhysicsBehavior derives falling damage from this active
        // vertical velocity rather than from accumulated fall distance.
        const PhysicsScalar activeVerticalSpeed = physics.velocityUnitsPerSecond.z;
        const LogicFixedVec3 previousPosition = physics.position;
        physics.position = addFixed(physics.position,
                                    scaleFixed(physics.velocityUnitsPerSecond, frameSeconds));

        const PhysicsScalar groundAfter = physicsCollisionSupportHeight(
            terrain, registry, candidate.entity, physics.position);
        if (physics.position.z <= groundAfter) {
            const bool descendingIntoGround = physics.velocityUnitsPerSecond.z < kPhysicsZero;
            const bool canBounce = physics.allowBouncing && descendingIntoGround &&
                previousPosition.z > groundAfter + kPhysicsGroundEpsilon;
            physics.position.z = groundAfter;
            physics.allowToFall = false;
            if (canBounce) {
                physics.velocityUnitsPerSecond.z = -physics.velocityUnitsPerSecond.z * groundStiffness;
                physics.state = ObjectPhysicsMotionState::Bounced;
                events.push_back({
                    .kind = ObjectPhysicsEventKind::Bounced,
                    .object = candidate.id,
                    .position = physics.position,
                    .verticalSpeedUnitsPerSecond =
                        physics.velocityUnitsPerSecond.z,
                    .confirmedTick = tick,
                });
            } else {
                if (descendingIntoGround) physics.velocityUnitsPerSecond.z = kPhysicsZero;
                physics.state = ObjectPhysicsMotionState::Grounded;
            }
        } else {
            physics.state = ObjectPhysicsMotionState::Airborne;
        }

        integratePhysicsOrientation(physics, frameSeconds);
        physics.ownsAttitude = true;
        const PhysicsScalar groundForState = physicsLayerHeight(
            terrain, registry, candidate.entity, physics.position);
        const bool airborneAtEnd = physics.position.z > groundForState + kPhysicsGroundEpsilon;
        constexpr PhysicsScalar kStunReliefEpsilon =
            PhysicsScalar::from_fraction(1, 2);
        if (physics.stunned &&
            ((PhysicsScalar::abs(physics.velocityUnitsPerSecond.x) < kStunReliefEpsilon &&
              PhysicsScalar::abs(physics.velocityUnitsPerSecond.y) < kStunReliefEpsilon &&
              PhysicsScalar::abs(physics.velocityUnitsPerSecond.z) < kStunReliefEpsilon) ||
             !significantlyAirborneBefore)) {
            physics.stunned = false;
            setPhysicsModelCondition(registry, candidate.entity,
                                     game::ModelConditionFlag::StunnedFlailing,
                                     false);
            setPhysicsModelCondition(registry, candidate.entity,
                                     game::ModelConditionFlag::Stunned, false);
        } else if (physics.stunned && !airborneAtEnd) {
            setPhysicsModelCondition(registry, candidate.entity,
                                     game::ModelConditionFlag::StunnedFlailing,
                                     false);
            setPhysicsModelCondition(registry, candidate.entity,
                                     game::ModelConditionFlag::Stunned, true);
        }
        synchronizePhysicsFreeFall(
            registry, candidate.entity, physics, airborneAtEnd, tick);
        const bool landed = wasAirborneLastFrame && !airborneAtEnd;
        physics.wasAirborneLastFrame = airborneAtEnd;
        publishPhysicsTransform(physics, *fixedTransform, transform);
        markObjectDirty(
            registry, candidate.entity,
            objectDirtyBit(ObjectDirtyDomain::Spatial) |
                objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
        if (landed) {
            events.push_back({
                .kind = ObjectPhysicsEventKind::Landed,
                .object = candidate.id,
                .position = physics.position,
                .verticalSpeedUnitsPerSecond = activeVerticalSpeed,
                .confirmedTick = tick,
            });

            // RefCode skips ordinary falling damage for projectile PUI
            // owners.  Every current ECS projectile carries this component,
            // while its DumbBezier controller owns translation entirely.
            const bool projectileImmune = projectile != nullptr;
            const PhysicsScalar excessSpeed = -activeVerticalSpeed -
                physics.minFallSpeedUnitsPerSecond;
            constexpr PhysicsScalar kMinimumFallAngleTangent{int32_t{3}};
            const PhysicsScalar horizontalX = PhysicsScalar::abs(physics.velocityUnitsPerSecond.x);
            const PhysicsScalar horizontalY = PhysicsScalar::abs(physics.velocityUnitsPerSecond.y);
            const PhysicsScalar verticalMagnitude = PhysicsScalar::abs(activeVerticalSpeed);
            const bool steepAlongX = horizontalX <= kPhysicsRestEpsilon ||
                verticalMagnitude >= horizontalX * kMinimumFallAngleTangent;
            const bool steepAlongY = horizontalY <= kPhysicsRestEpsilon ||
                verticalMagnitude >= horizontalY * kMinimumFallAngleTangent;
            const PhysicsScalar fallDamage = excessSpeed > kPhysicsZero && steepAlongX && steepAlongY
                ? excessSpeed * physics.mass * physics.fallHeightDamageFactor
                : kPhysicsZero;
            if (!projectileImmune && fallDamage > kPhysicsZero) {
                outDamage.push_back({
                    .target = candidate.id,
                    .source = candidate.id,
                    .causalGroup = candidate.id,
                    .amount = fallDamage,
                    .damageType = game::DamageType::FALLING,
                    .deathType = game::DeathType::SPLATTED,
                    .confirmedTick = tick,
                });
            }
        }

        const bool isResting = PhysicsScalar::abs(physics.velocityUnitsPerSecond.x) < kPhysicsRestEpsilon &&
            PhysicsScalar::abs(physics.velocityUnitsPerSecond.y) < kPhysicsRestEpsilon &&
            PhysicsScalar::abs(physics.velocityUnitsPerSecond.z) < kPhysicsRestEpsilon;
        if (physics.killWhenRestingOnGround && !airborneAtEnd && isResting) {
            // Object::kill() is an UNRESISTABLE, forced Body transaction; it
            // must not bypass the shared Death/Die path with a raw destroy.
            // Drone/unmanned exceptions depend on the future AI/contain
            // components and remain explicitly outside this Stage-1 slice.
            outDamage.push_back({
                .target = candidate.id,
                .causalGroup = candidate.id,
                .damageType = game::DamageType::UNRESISTABLE,
                .deathType = game::DeathType::NORMAL,
                .resolutionPhase = ObjectDamageResolutionPhase::PostPhysicsRestKill,
                .forceKill = true,
                .confirmedTick = tick,
            });
            events.push_back({
                .kind = ObjectPhysicsEventKind::RestingDestroyed,
                .object = candidate.id,
                .position = physics.position,
                .verticalSpeedUnitsPerSecond =
                    physics.velocityUnitsPerSecond.z,
                .confirmedTick = tick,
            });
        }
        physics.physicsEverUpdated = true;
        const bool exactlyStopped =
            physics.velocityUnitsPerSecond.x.raw() == 0 &&
            physics.velocityUnitsPerSecond.y.raw() == 0 &&
            physics.velocityUnitsPerSecond.z.raw() == 0;
        const ObjectTerrainLayerComponent* sleepLayer =
            ecs::try_get<ObjectTerrainLayerComponent>(registry,
                                                       candidate.entity);
        physics.sleeping = exactlyStopped && !airborneAtEnd &&
            (!sleepLayer || sleepLayer->pathfindLayer ==
                 game::terrain::kGroundPathfindLayer) &&
            physics.yawRate.raw() == 0 && physics.pitchRate.raw() == 0 &&
            physics.rollRate.raw() == 0 &&
            physics.motiveForceExpiresTick <= tick &&
            !physics.currentOverlap && !physics.previousOverlap &&
            !physics.stunned && !physics.inFreeFall;
    }
    candidates.clear();
}

void resolvePhysicsCollisions(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain,
    const PlayerRegistry* players, navigation::NavigationSystem* navigation,
    const ObjectSimulationRules& rules,
    uint64_t confirmedTick, uint64_t& nextGameplaySubmissionOrdinal,
    container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectPilotVehicleTakeoverRequest>& takeoverRequests,
    container::Vector<ObjectPhysicsCrashCommand>& crashCommands,
    container::Vector<ObjectAIMovementObstructionEvent>&
        obstructionEvents,
    ObjectPhysicsScratch& scratch,
    ObjectSpatialIndex& broadPhase) {
    const size_t obstructionEventBegin = obstructionEvents.size();
    auto& expiredCollisionIgnores = scratch.expiredCollisionIgnores;
    expiredCollisionIgnores.clear();
    const auto collisionIgnoreView =
        ecs::view<const ObjectTemporaryCollisionIgnoreComponent>(registry);
    for (const ecs::entity entity : collisionIgnoreView) {
        if (collisionIgnoreView
                .template get<const ObjectTemporaryCollisionIgnoreComponent>(
                    entity)
                .untilTick <= confirmedTick) {
            expiredCollisionIgnores.push_back(entity);
        }
    }
    for (const ecs::entity entity : expiredCollisionIgnores) {
        ecs::remove<ObjectTemporaryCollisionIgnoreComponent>(registry,
                                                               entity);
    }

    using Candidate = ObjectPhysicsScratch::Candidate;
    using SweptContact = ObjectPhysicsScratch::SweptContact;
    using Separation = ObjectPhysicsScratch::Separation;
    auto& candidates = scratch.candidates;
    candidates.clear();
    const auto view = ecs::view<ObjectIdentityComponent,
                                ObjectFixedTransformComponent,
                                ObjectGeometryComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<ObjectIdentityComponent>(entity);
        const ObjectMapStatusComponent* map =
            ecs::try_get<ObjectMapStatusComponent>(registry, entity);
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, entity);
        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(registry, entity);
        if (!identity.id || !lifecycle.entityFromId(identity.id) ||
            lifecycle.isPendingDestroy(identity.id) ||
            (map && map->offMap) ||
            (status && status->hasAny(game::objectStatusBit(
                game::ObjectStatusFlag::NoCollisions))) ||
            hasKind(kinds, game::ObjectKindOf::NoCollide) ||
            ecs::try_get<ObjectContainedByComponent>(registry, entity)) {
            continue;
        }
        candidates.push_back({identity.id, entity});
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.id < right.id;
        });
    auto& sweptContacts = scratch.sweptContacts;
    sweptContacts.clear();
    sweptContacts.reserve(candidates.size());

    // Positional separation for locomotion-owned objects, which never consume
    // pendingForce. Accumulate every contact's share first and apply once after
    // the contact loop: applying inside the loop would make the outcome depend
    // on visit order. Insertion keeps the vector sorted by ObjectId, which is
    // therefore also the deterministic apply order.
    auto& separations = scratch.separations;
    separations.clear();
    const auto accumulateSeparation = [&separations](
        const Candidate& source, const LogicFixedVec3& displacement) {
        const auto slot = std::lower_bound(
            separations.begin(), separations.end(), source.id,
            [](const Separation& entry, ObjectId id) {
                return entry.id < id;
            });
        if (slot != separations.end() && slot->id == source.id) {
            slot->displacement.x += displacement.x;
            slot->displacement.y += displacement.y;
            slot->displacement.z += displacement.z;
            return;
        }
        separations.insert(slot, Separation{
            .id = source.id,
            .entity = source.entity,
            .displacement = displacement,
        });
    };

    // Rebuild once from this tick's published transforms, then use the
    // deterministic gameplay grid as the broad phase. Exact Q32.32 overlap
    // remains below; this avoids an all-pairs scan on maps with thousands of
    // ordinary objects.
    broadPhase.rebuild(registry, lifecycle);

    uint32_t crashSequence = 1;
    const auto reserveGameplayOrdinal = [&]() {
        const uint64_t ordinal = nextGameplaySubmissionOrdinal++;
        if (nextGameplaySubmissionOrdinal == 0) {
            ++nextGameplaySubmissionOrdinal;
        }
        return ordinal;
    };
    const auto positionOf = [&registry](ecs::entity entity) {
        if (const ObjectPhysicsComponent* physics =
                ecs::try_get<ObjectPhysicsComponent>(registry, entity);
            physics && physics->hasAuthoritativePosition) {
            return physics->position;
        }
        return ecs::get<ObjectFixedTransformComponent>(registry, entity)
            .position;
    };
    PhysicsScalar maximumFrameTravel{};
    for (const Candidate& candidate : candidates) {
        const ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry,
                                                  candidate.entity);
        if (!physics || physics->collisionStartTick != confirmedTick)
            continue;
        const LogicFixedVec3 end = positionOf(candidate.entity);
        const LogicFixedVec3 delta{
            end.x - physics->collisionStartPosition.x,
            end.y - physics->collisionStartPosition.y,
            end.z - physics->collisionStartPosition.z};
        maximumFrameTravel = PhysicsScalar::max(
            maximumFrameTravel,
            PhysicsScalar::sqrt(delta.x * delta.x + delta.y * delta.y +
                                delta.z * delta.z));
    }
    auto& consideredPairs = scratch.consideredPairs;
    consideredPairs.clear();
    consideredPairs.reserve(candidates.size());
    // Reuse one ordered broad-phase result for every candidate in this
    // collision pass. Each result is consumed before the next query clears
    // the buffer, preserving the existing ObjectId order and pair tie-break.
    auto& nearby = scratch.nearby;
    nearby.clear();
    for (const Candidate& query : candidates) {
        const ObjectPhysicsComponent* queryPhysics =
            ecs::try_get<ObjectPhysicsComponent>(registry, query.entity);
        if (!queryPhysics) continue;
        const ObjectGeometryComponent& queryGeometry =
            ecs::get<ObjectGeometryComponent>(registry, query.entity);
        const LogicFixedVec3 queryEnd = positionOf(query.entity);
        const LogicFixedVec3 queryStart =
            queryPhysics->collisionStartTick == confirmedTick
                ? queryPhysics->collisionStartPosition : queryEnd;
        const LogicFixedVec3 queryTravel{
            queryEnd.x - queryStart.x,
            queryEnd.y - queryStart.y,
            queryEnd.z - queryStart.z,
        };
        const PhysicsScalar queryTravelLength = PhysicsScalar::sqrt(
            queryTravel.x * queryTravel.x + queryTravel.y * queryTravel.y +
            queryTravel.z * queryTravel.z);
        const LogicFixedVec3 broadCenter{
            (queryStart.x + queryEnd.x) / kPhysicsTwo,
            (queryStart.y + queryEnd.y) / kPhysicsTwo,
            (queryStart.z + queryEnd.z) / kPhysicsTwo,
        };
        const PhysicsScalar broadRadius = PhysicsScalar::max(
            kPhysicsZero, queryGeometry.boundingSphereRadiusFixed) +
            queryTravelLength / kPhysicsTwo + maximumFrameTravel;
        broadPhase.querySphereRadiusFixed(
            broadCenter, broadRadius, nearby);
        for (const ObjectId nearbyId : nearby) {
            if (!nearbyId || nearbyId == query.id) continue;
            const ObjectId leftId = std::min(query.id, nearbyId);
            const ObjectId rightId = std::max(query.id, nearbyId);
            const uint64_t pairKey =
                (static_cast<uint64_t>(leftId.value) << 32u) |
                static_cast<uint64_t>(rightId.value);
            if (!consideredPairs.insert(pairKey).second) continue;

            const std::optional<ecs::entity> leftEntity =
                lifecycle.entityFromId(leftId);
            const std::optional<ecs::entity> rightEntity =
                lifecycle.entityFromId(rightId);
            if (!leftEntity || !rightEntity ||
                lifecycle.isPendingDestroy(leftId) ||
                lifecycle.isPendingDestroy(rightId) ||
                !ecs::try_get<ObjectFixedTransformComponent>(
                    registry, *leftEntity) ||
                !ecs::try_get<ObjectGeometryComponent>(registry, *leftEntity) ||
                !ecs::try_get<ObjectFixedTransformComponent>(
                    registry, *rightEntity) ||
                !ecs::try_get<ObjectGeometryComponent>(registry, *rightEntity) ||
                ecs::try_get<ObjectContainedByComponent>(registry, *leftEntity) ||
                ecs::try_get<ObjectContainedByComponent>(registry, *rightEntity)) {
                continue;
            }
            const ObjectMapStatusComponent* leftMap =
                ecs::try_get<ObjectMapStatusComponent>(registry, *leftEntity);
            const ObjectMapStatusComponent* rightMap =
                ecs::try_get<ObjectMapStatusComponent>(registry, *rightEntity);
            if ((leftMap && leftMap->offMap) ||
                (rightMap && rightMap->offMap)) continue;
            const Candidate left{leftId, *leftEntity};
            const Candidate right{rightId, *rightEntity};
            ObjectPhysicsComponent* leftPhysics =
                ecs::try_get<ObjectPhysicsComponent>(registry, left.entity);
            ObjectPhysicsComponent* rightPhysics =
                ecs::try_get<ObjectPhysicsComponent>(registry, right.entity);
            if (!leftPhysics && !rightPhysics) continue;

            const ObjectProjectileComponent* leftProjectile =
                ecs::try_get<ObjectProjectileComponent>(registry, left.entity);
            const ObjectProjectileComponent* rightProjectile =
                ecs::try_get<ObjectProjectileComponent>(registry, right.entity);
            const bool ignoredByStableId =
                (leftPhysics && leftPhysics->ignoreCollisionWith == right.id) ||
                (rightPhysics && rightPhysics->ignoreCollisionWith == left.id) ||
                (leftProjectile && leftProjectile->launcher == right.id) ||
                (rightProjectile && rightProjectile->launcher == left.id);
            const ObjectTemporaryCollisionIgnoreComponent* leftIgnore =
                ecs::try_get<ObjectTemporaryCollisionIgnoreComponent>(
                    registry, left.entity);
            const ObjectTemporaryCollisionIgnoreComponent* rightIgnore =
                ecs::try_get<ObjectTemporaryCollisionIgnoreComponent>(
                    registry, right.entity);
            const ObjectAIPathMovementComponent* leftAIMovement =
                ecs::try_get<ObjectAIPathMovementComponent>(
                    registry, left.entity);
            const ObjectAIPathMovementComponent* rightAIMovement =
                ecs::try_get<ObjectAIPathMovementComponent>(
                    registry, right.entity);
            const ObjectSystemPathSequenceComponent* leftSystemPath =
                ecs::try_get<ObjectSystemPathSequenceComponent>(
                    registry, left.entity);
            const ObjectSystemPathSequenceComponent* rightSystemPath =
                ecs::try_get<ObjectSystemPathSequenceComponent>(
                    registry, right.entity);
            const bool ignoredByAIMovement =
                (leftAIMovement &&
                 leftAIMovement->ignoredObstacle == right.id) ||
                (rightAIMovement &&
                 rightAIMovement->ignoredObstacle == left.id) ||
                (leftAIMovement && leftAIMovement->allowPathThroughUnits &&
                 ecs::try_get<ObjectLocomotionComponent>(registry,
                                                          right.entity)) ||
                (rightAIMovement && rightAIMovement->allowPathThroughUnits &&
                 ecs::try_get<ObjectLocomotionComponent>(registry,
                                                          left.entity));
            // RefCode installs FollowExitProductionPath/FollowPath
            // synchronously and carries the producer/container as the
            // ignored obstacle immediately.  Our AI path solve is
            // asynchronous, so the confirmed system-path transaction must
            // bridge that interval; otherwise collision separation can push
            // a freshly produced or unloaded unit through the structure
            // boundary before its ObjectAIPathMovementComponent exists.
            const bool ignoredBySystemPath =
                (leftSystemPath &&
                 leftSystemPath->ignoredObstacle == right.id) ||
                (rightSystemPath &&
                 rightSystemPath->ignoredObstacle == left.id);
            const auto queuePilotTakeover =
                [&](const Candidate& pilot, const Candidate& target) {
                    const ObjectKindOfComponent* pilotKinds =
                        ecs::try_get<ObjectKindOfComponent>(
                            registry, pilot.entity);
                    const ObjectKindOfComponent* targetKinds =
                        ecs::try_get<ObjectKindOfComponent>(
                            registry, target.entity);
                    if (!hasKind(pilotKinds, game::ObjectKindOf::Infantry) ||
                        !hasKind(targetKinds, game::ObjectKindOf::Vehicle) ||
                        !isObjectDisabledBy(registry, target.entity,
                                           ObjectDisabledReason::Unmanned,
                                           confirmedTick)) {
                        return;
                    }
                    const OwnerComponent* pilotOwner =
                        ecs::try_get<OwnerComponent>(
                            registry, pilot.entity);
                    if (!pilotOwner || !pilotOwner->player ||
                        lifecycle.isPendingDestroy(pilot.id) ||
                        lifecycle.isPendingDestroy(target.id)) {
                        return;
                    }
                    takeoverRequests.push_back({
                        .pilot = pilot.id,
                        .vehicle = target.id,
                        .newOwner = pilotOwner->player,
                        .submissionOrdinal = reserveGameplayOrdinal(),
                        .confirmedTick = confirmedTick,
                    });
                };
            if (leftAIMovement &&
                leftAIMovement->ignoredObstacle == right.id) {
                queuePilotTakeover(left, right);
            }
            if (rightAIMovement &&
                rightAIMovement->ignoredObstacle == left.id) {
                queuePilotTakeover(right, left);
            }
            const bool ignoredByTemporaryPair =
                (leftIgnore && leftIgnore->untilTick > confirmedTick &&
                 (!leftIgnore->other || leftIgnore->other == right.id)) ||
                (rightIgnore && rightIgnore->untilTick > confirmedTick &&
                 (!rightIgnore->other || rightIgnore->other == left.id));
            if (ignoredByStableId || ignoredByAIMovement ||
                ignoredBySystemPath ||
                ignoredByTemporaryPair) {
                continue;
            }

            const ObjectKindOfComponent* leftKinds =
                ecs::try_get<ObjectKindOfComponent>(registry, left.entity);
            const ObjectKindOfComponent* rightKinds =
                ecs::try_get<ObjectKindOfComponent>(registry, right.entity);
            const ObjectStatusComponent* leftStatus =
                ecs::try_get<ObjectStatusComponent>(registry, left.entity);
            const ObjectStatusComponent* rightStatus =
                ecs::try_get<ObjectStatusComponent>(registry, right.entity);
            const bool bothParachuting = leftStatus && rightStatus &&
                leftStatus->hasAny(game::objectStatusBit(
                    game::ObjectStatusFlag::Parachuting)) &&
                rightStatus->hasAny(game::objectStatusBit(
                    game::ObjectStatusFlag::Parachuting));
            if (bothParachuting ||
                hasKind(leftKinds, game::ObjectKindOf::NoCollide) ||
                hasKind(rightKinds, game::ObjectKindOf::NoCollide)) continue;
            const bool leftImmobile =
                hasKind(leftKinds, game::ObjectKindOf::Immobile);
            const bool rightImmobile =
                hasKind(rightKinds, game::ObjectKindOf::Immobile);
            if ((!leftPhysics && !leftImmobile) ||
                (!rightPhysics && !rightImmobile)) {
                continue;
            }

            const ObjectGeometryComponent& leftGeometry =
                ecs::get<ObjectGeometryComponent>(registry, left.entity);
            const ObjectGeometryComponent& rightGeometry =
                ecs::get<ObjectGeometryComponent>(registry, right.entity);
            const LogicFixedVec3 leftPosition = positionOf(left.entity);
            const LogicFixedVec3 rightPosition = positionOf(right.entity);
            const LogicFixedVec3 leftStart = leftPhysics &&
                    leftPhysics->collisionStartTick == confirmedTick
                ? leftPhysics->collisionStartPosition : leftPosition;
            const LogicFixedVec3 leftTravel{
                leftPosition.x - leftStart.x,
                leftPosition.y - leftStart.y,
                leftPosition.z - leftStart.z,
            };
            const ObjectFixedTransformComponent& leftTransform =
                ecs::get<ObjectFixedTransformComponent>(registry,
                                                        left.entity);
            const ObjectFixedTransformComponent& rightTransform =
                ecs::get<ObjectFixedTransformComponent>(registry,
                                                        right.entity);
            const PhysicsScalar leftYaw = leftPhysics &&
                    leftPhysics->ownsAttitude
                ? leftPhysics->yaw : leftTransform.yawRadians;
            const PhysicsScalar rightYaw = rightPhysics &&
                    rightPhysics->ownsAttitude
                ? rightPhysics->yaw : rightTransform.yawRadians;
            const PhysicsScalar leftStartYaw = leftPhysics &&
                    leftPhysics->collisionStartTick == confirmedTick
                ? leftPhysics->collisionStartYaw : leftYaw;
            const PhysicsScalar rightStartYaw = rightPhysics &&
                    rightPhysics->collisionStartTick == confirmedTick
                ? rightPhysics->collisionStartYaw : rightYaw;
            const LogicFixedVec3 rightStart = rightPhysics &&
                    rightPhysics->collisionStartTick == confirmedTick
                ? rightPhysics->collisionStartPosition : rightPosition;
            ObjectCollisionContact contact;
            PhysicsScalar contactTime = kPhysicsOne;
            const bool collided = computeObjectSweptCollisionContact(
                leftStart, leftPosition, leftStartYaw, leftYaw, leftGeometry,
                rightStart, rightPosition, rightStartYaw, rightYaw,
                rightGeometry,
                contactTime, contact);
            if (!collided) continue;
            // TOI contact is normally just touching and therefore has zero
            // penetration. Find the opposite boundary by running the same
            // deterministic sweep backwards, then sample the middle of the
            // contact interval for the legacy overlap-response magnitude.
            // Ordering still uses the exact entry time below.
            PhysicsScalar reverseContactTime = kPhysicsZero;
            ObjectCollisionContact reverseContact;
            if (computeObjectSweptCollisionContact(
                    leftPosition, leftStart, leftYaw, leftStartYaw,
                    leftGeometry,
                    rightPosition, rightStart, rightYaw, rightStartYaw,
                    rightGeometry,
                    reverseContactTime, reverseContact)) {
                const PhysicsScalar exitTime =
                    kPhysicsOne - reverseContactTime;
                const PhysicsScalar responseTime =
                    (contactTime + PhysicsScalar::max(
                        contactTime, exitTime)) /
                    PhysicsScalar{int32_t{2}};
                const LogicFixedVec3 rightTravel{
                    rightPosition.x - rightStart.x,
                    rightPosition.y - rightStart.y,
                    rightPosition.z - rightStart.z,
                };
                const LogicFixedVec3 responseLeft = addFixed(
                    leftStart, scaleFixed(leftTravel, responseTime));
                const LogicFixedVec3 responseRight = addFixed(
                    rightStart, scaleFixed(rightTravel, responseTime));
                const PhysicsScalar responseLeftYaw = leftStartYaw +
                    collision_detail::shortestAngleDelta(
                        leftStartYaw, leftYaw) * responseTime;
                const PhysicsScalar responseRightYaw = rightStartYaw +
                    collision_detail::shortestAngleDelta(
                        rightStartYaw, rightYaw) * responseTime;
                ObjectCollisionContact responseContact;
                if (computeObjectCollisionContact(
                        responseLeft, responseLeftYaw, leftGeometry,
                        responseRight, responseRightYaw, rightGeometry,
                        responseContact)) {
                    contact = responseContact;
                }
            }
            sweptContacts.push_back({
                .left = left,
                .right = right,
                .timeOfImpact = contactTime,
                .response = contact,
            });
        }
    }

    std::sort(sweptContacts.begin(), sweptContacts.end(),
        [](const SweptContact& left, const SweptContact& right) {
            if (left.timeOfImpact.raw() != right.timeOfImpact.raw()) {
                return left.timeOfImpact.raw() < right.timeOfImpact.raw();
            }
            if (left.left.id != right.left.id) {
                return left.left.id < right.left.id;
            }
            return left.right.id < right.right.id;
        });

    for (const SweptContact& swept : sweptContacts) {
            const Candidate& left = swept.left;
            const Candidate& right = swept.right;
            ObjectPhysicsComponent* leftPhysics =
                ecs::try_get<ObjectPhysicsComponent>(registry, left.entity);
            ObjectPhysicsComponent* rightPhysics =
                ecs::try_get<ObjectPhysicsComponent>(registry, right.entity);
            const ObjectKindOfComponent* leftKinds =
                ecs::try_get<ObjectKindOfComponent>(registry, left.entity);
            const ObjectKindOfComponent* rightKinds =
                ecs::try_get<ObjectKindOfComponent>(registry, right.entity);
            const bool leftImmobile =
                hasKind(leftKinds, game::ObjectKindOf::Immobile);
            const bool rightImmobile =
                hasKind(rightKinds, game::ObjectKindOf::Immobile);
            const LogicFixedVec3 normal = swept.response.normal;
            const PhysicsScalar overlap = PhysicsScalar::min(
                swept.response.penetration, PhysicsScalar{int32_t{5}});
            const auto tryVehicleTopple = [&](const Candidate& victim,
                                               const Candidate& vehicle,
                                               ObjectPhysicsComponent* physics) {
                const ObjectTacticalComponent* tactical =
                    ecs::try_get<ObjectTacticalComponent>(registry,
                                                          victim.entity);
                if (!tactical || !tactical->plan ||
                    tactical->topple.empty() || !physics) {
                    return;
                }
                const ThingTemplateComponent* vehicleType =
                    ecs::try_get<ThingTemplateComponent>(registry,
                                                         vehicle.entity);
                if (!vehicleType || !vehicleType->archetype ||
                    vehicleType->archetype->templateData.crusherLevel <= 1) {
                    return;
                }

                LogicFixedVec3 velocity = physics->velocityUnitsPerSecond;
                if (const ObjectLocomotionComponent* locomotion =
                        ecs::try_get<ObjectLocomotionComponent>(
                            registry, vehicle.entity);
                    locomotion && locomotion->state ==
                        ObjectLocomotionState::Moving) {
                    const PhysicsScalar yaw =
                        ecs::get<ObjectFixedTransformComponent>(
                            registry, vehicle.entity).yawRadians;
                    velocity.x = math::fixed_cos(yaw) *
                        locomotion->forwardSpeed;
                    velocity.y = math::fixed_sin(yaw) *
                        locomotion->forwardSpeed;
                }
                const PhysicsScalar framesPerSecond{
                    static_cast<int32_t>(std::max<uint32_t>(
                        1u, rules.logicFramesPerSecond))};
                const PhysicsScalar speedPerFrame = PhysicsScalar::sqrt(
                    velocity.x * velocity.x + velocity.y * velocity.y +
                    velocity.z * velocity.z) / framesPerSecond;
                const LogicFixedVec3 victimPosition =
                    positionOf(victim.entity);
                const LogicFixedVec3 vehiclePosition =
                    positionOf(vehicle.entity);
                queueObjectToppleRequest(registry, {
                    .object = victim.id,
                    .source = vehicle.id,
                    .direction = {
                        victimPosition.x - vehiclePosition.x,
                        victimPosition.y - vehiclePosition.y,
                        {},
                    },
                    .speed = speedPerFrame,
                    .confirmedTick = confirmedTick,
                    .noBounce = false,
                    .noFx = false,
                });
            };
            tryVehicleTopple(left, right, rightPhysics);
            tryVehicleTopple(right, left, leftPhysics);
            const auto tryCrush = [&](const Candidate& crusher,
                                      const Candidate& victim,
                                      ObjectPhysicsComponent* physics) {
                if (!physics || ecs::try_get<ObjectSquishCollideComponent>(
                        registry, victim.entity) ||
                    isObjectDisabledBy(registry, crusher.entity,
                                       ObjectDisabledReason::Unmanned,
                                       confirmedTick)) {
                    return false;
                }
                const ThingTemplateComponent* crusherType =
                    ecs::try_get<ThingTemplateComponent>(registry,
                                                         crusher.entity);
                const ThingTemplateComponent* victimType =
                    ecs::try_get<ThingTemplateComponent>(registry,
                                                         victim.entity);
                if (!crusherType || !crusherType->archetype || !victimType ||
                    !victimType->archetype ||
                    crusherType->archetype->templateData.crusherLevel <=
                        victimType->archetype->templateData.crushableLevel) {
                    return false;
                }
                if (players && relationshipBetweenObjects(
                        registry, *players, crusher.entity,
                        victim.entity) == PlayerRelationship::Allies) {
                    return false;
                }
                // RefCode allows an overlap to suppress ordinary collision
                // response only while the crusher is actually moving. The
                // front/centre/back point test below, rather than a generic
                // centre-to-centre dot product, decides when the lethal
                // transaction is allowed.
                LogicFixedVec3 velocity = physics->velocityUnitsPerSecond;
                if (const ObjectLocomotionComponent* locomotion =
                        ecs::try_get<ObjectLocomotionComponent>(
                            registry, crusher.entity);
                    locomotion && locomotion->state ==
                        ObjectLocomotionState::Moving) {
                    const PhysicsScalar yaw =
                        ecs::get<ObjectFixedTransformComponent>(
                            registry, crusher.entity).yawRadians;
                    const PhysicsScalar speed = locomotion->forwardSpeed;
                    velocity.x = math::fixed_cos(yaw) * speed;
                    velocity.y = math::fixed_sin(yaw) * speed;
                }
                const PhysicsScalar motionEpsilon =
                    PhysicsScalar::from_fraction(1, 1000);
                if (squaredLengthFixed(velocity) <=
                    motionEpsilon * motionEpsilon) {
                    return false;
                }
                if (physics->overlapLedgerTick != confirmedTick) {
                    physics->previousOverlap = physics->currentOverlap;
                    physics->currentOverlap = INVALID_OBJECT_ID;
                    physics->overlapLedgerTick = confirmedTick;
                }
                const bool firstOverlap =
                    physics->currentOverlap != victim.id &&
                    physics->previousOverlap != victim.id;
                physics->currentOverlap = victim.id;
                if (firstOverlap) {
                    outDamage.push_back({
                        .target = victim.id,
                        .source = crusher.id,
                        .sourceSequence = crashSequence++,
                        .submissionOrdinal = reserveGameplayOrdinal(),
                        .causalGroup = crusher.id,
                        .amount = PhysicsScalar{},
                        .damageType = game::DamageType::CRUSH,
                        .deathType = game::DeathType::CRUSHED,
                        .emitZeroDamageFeedback = true,
                        .confirmedTick = confirmedTick,
                    });
                }

                const ObjectCrushStateComponent* crushState =
                    ecs::try_get<ObjectCrushStateComponent>(
                        registry, victim.entity);
                const bool frontCrushed = crushState &&
                    crushState->frontCrushed;
                const bool backCrushed = crushState &&
                    crushState->backCrushed;
                if (frontCrushed && backCrushed) return true;

                enum class CrushPoint : uint8_t {
                    Centre,
                    Front,
                    Back,
                };
                const ObjectFixedTransformComponent& crusherTransform =
                    ecs::get<ObjectFixedTransformComponent>(
                        registry, crusher.entity);
                const ObjectFixedTransformComponent& victimTransform =
                    ecs::get<ObjectFixedTransformComponent>(
                        registry, victim.entity);
                const PhysicsScalar crusherYaw =
                    crusherTransform.yawRadians;
                const PhysicsScalar victimYaw = victimTransform.yawRadians;
                const collision_detail::Axis2 crusherDirection{
                    math::fixed_cos(crusherYaw),
                    math::fixed_sin(crusherYaw)};
                const collision_detail::Axis2 victimDirection{
                    math::fixed_cos(victimYaw),
                    math::fixed_sin(victimYaw)};
                const PhysicsScalar pointOffset = PhysicsScalar::max(
                    kPhysicsZero,
                    ecs::get<ObjectGeometryComponent>(
                        registry, victim.entity).majorRadiusFixed) /
                    PhysicsScalar{int32_t{2}};
                const LogicFixedVec3 crusherPosition =
                    positionOf(crusher.entity);
                const LogicFixedVec3 victimPosition =
                    positionOf(victim.entity);
                const auto pointPosition = [&](CrushPoint point) {
                    PhysicsScalar sign{};
                    if (point == CrushPoint::Front) sign = kPhysicsOne;
                    else if (point == CrushPoint::Back)
                        sign = PhysicsScalar{int32_t{-1}};
                    return LogicFixedVec3{
                        victimPosition.x +
                            victimDirection.x * pointOffset * sign,
                        victimPosition.y +
                            victimDirection.y * pointOffset * sign,
                        victimPosition.z,
                    };
                };
                struct CrushPointMetric final {
                    PhysicsScalar perpendicular{};
                    PhysicsScalar distance{};
                };
                const auto metric = [&](CrushPoint point) {
                    const LogicFixedVec3 target = pointPosition(point);
                    const PhysicsScalar dx =
                        target.x - crusherPosition.x;
                    const PhysicsScalar dy =
                        target.y - crusherPosition.y;
                    const PhysicsScalar ray =
                        dx * crusherDirection.x +
                        dy * crusherDirection.y;
                    const PhysicsScalar perpendicularX =
                        crusherDirection.x * ray - dx;
                    const PhysicsScalar perpendicularY =
                        crusherDirection.y * ray - dy;
                    return CrushPointMetric{
                        .perpendicular = PhysicsScalar::sqrt(
                            perpendicularX * perpendicularX +
                            perpendicularY * perpendicularY),
                        .distance = PhysicsScalar::sqrt(dx * dx + dy * dy),
                    };
                };

                CrushPoint selected = CrushPoint::Centre;
                if (frontCrushed) {
                    selected = CrushPoint::Back;
                } else if (backCrushed) {
                    selected = CrushPoint::Front;
                } else {
                    const CrushPointMetric front = metric(CrushPoint::Front);
                    const CrushPointMetric back = metric(CrushPoint::Back);
                    const CrushPointMetric centre = metric(CrushPoint::Centre);
                    const PhysicsScalar equalRange =
                        PhysicsScalar::from_fraction(3, 20);
                    const auto equalPerpendicular = [&](PhysicsScalar left,
                                                        PhysicsScalar right) {
                        return PhysicsScalar::abs(left - right) <= equalRange;
                    };
                    if (front.perpendicular <= centre.perpendicular &&
                        front.perpendicular <= back.perpendicular) {
                        if (equalPerpendicular(front.perpendicular,
                                               centre.perpendicular)) {
                            selected = front.distance < centre.distance
                                ? CrushPoint::Front : CrushPoint::Centre;
                        } else if (equalPerpendicular(front.perpendicular,
                                                      back.perpendicular)) {
                            selected = front.distance < back.distance
                                ? CrushPoint::Front : CrushPoint::Back;
                        } else {
                            selected = CrushPoint::Front;
                        }
                    } else if (back.perpendicular <= centre.perpendicular &&
                               back.perpendicular <= front.perpendicular) {
                        if (equalPerpendicular(back.perpendicular,
                                               centre.perpendicular)) {
                            selected = back.distance < centre.distance
                                ? CrushPoint::Back : CrushPoint::Centre;
                        } else if (equalPerpendicular(back.perpendicular,
                                                      front.perpendicular)) {
                            selected = back.distance < front.distance
                                ? CrushPoint::Back : CrushPoint::Front;
                        } else {
                            selected = CrushPoint::Back;
                        }
                    } else if (equalPerpendicular(centre.perpendicular,
                                                  back.perpendicular)) {
                        selected = centre.distance < back.distance
                            ? CrushPoint::Centre : CrushPoint::Back;
                    } else if (equalPerpendicular(centre.perpendicular,
                                                  front.perpendicular)) {
                        selected = centre.distance < front.distance
                            ? CrushPoint::Centre : CrushPoint::Front;
                    }
                }

                const LogicFixedVec3 selectedPosition =
                    pointPosition(selected);
                const PhysicsScalar pointX =
                    selectedPosition.x - crusherPosition.x;
                const PhysicsScalar pointY =
                    selectedPosition.y - crusherPosition.y;
                const PhysicsScalar passed =
                    crusherDirection.x * pointX +
                    crusherDirection.y * pointY;
                const PhysicsScalar maximumDistanceSquared =
                    PhysicsScalar::from_fraction(9, 4) *
                        pointOffset * pointOffset;
                const PhysicsScalar pointDistanceSquared =
                    pointX * pointX + pointY * pointY;
                if (passed < kPhysicsZero &&
                    pointDistanceSquared < maximumDistanceSquared) {
                    outDamage.push_back({
                        .target = victim.id,
                        .source = crusher.id,
                        .sourceSequence = crashSequence++,
                        .submissionOrdinal = reserveGameplayOrdinal(),
                        .causalGroup = crusher.id,
                        .damageType = game::DamageType::CRUSH,
                        .deathType = game::DeathType::CRUSHED,
                        .forceKill = true,
                        .confirmedTick = confirmedTick,
                    });
                    ecs::remove<ObjectUndetectedDefectorComponent>(
                        registry, victim.entity);
                }
                return true;
            };
            const bool leftCrushedRight = tryCrush(
                left, right, leftPhysics);
            const bool rightCrushedLeft = !leftCrushedRight && tryCrush(
                right, left, rightPhysics);
            if (leftCrushedRight || rightCrushedLeft) continue;
            // Unit-vs-unit obstruction is an AI movement concern, not an
            // alliance-only collision concern. RefCode's AIUpdate checks the
            // two locomotors' movement state and path priority for every
            // ground-object contact; restricting this event to Allies made
            // neutral units act like immovable walls and prevented the
            // moving unit / MoveAside unit contract from running.
            {
                const PhysicsScalar pathfindCellSize =
                    navigation && navigation->isInitialized() &&
                            navigation->grid().transform().cellSizeRaw > 0
                        ? PhysicsScalar::from_raw(
                              navigation->grid().transform().cellSizeRaw)
                        : PhysicsScalar{int32_t{10}};
                const auto participant = [&](const Candidate& candidate) {
                    ObjectAIMovementObstructionParticipant result;
                    result.object = candidate.id;
                    result.position = positionOf(candidate.entity);
                    const ObjectFixedTransformComponent& fixedTransform =
                        ecs::get<ObjectFixedTransformComponent>(
                            registry, candidate.entity);
                    const math::q32_32_sincos facing =
                        math::fixed_sincos(fixedTransform.yawRadians);
                    result.direction = {
                        facing.cosine, facing.sine, kPhysicsZero};

                    const ObjectLocomotionComponent* locomotion =
                        ecs::try_get<ObjectLocomotionComponent>(
                            registry, candidate.entity);
                    const ObjectAIPathMovementComponent* movement =
                        ecs::try_get<ObjectAIPathMovementComponent>(
                            registry, candidate.entity);
                    result.hasPath = movement && movement->path;
                    result.blockedTicks = movement
                        ? movement->blockedTicks : 0;
                    result.moving = locomotion &&
                        locomotion->hasActiveMove &&
                        locomotion->state != ObjectLocomotionState::Idle;
                    result.movingBackward = locomotion &&
                        locomotion->movingBackward;
                    const game::LocomotorSurfaceMask groundSurfaces =
                        game::locomotorSurfaceBit(
                            game::LocomotorSurface::Ground) |
                        game::locomotorSurfaceBit(
                            game::LocomotorSurface::Water) |
                        game::locomotorSurfaceBit(
                            game::LocomotorSurface::Cliff) |
                        game::locomotorSurfaceBit(
                            game::LocomotorSurface::Rubble);
                    const ObjectAirborneComponent* airborne =
                        ecs::try_get<ObjectAirborneComponent>(
                            registry, candidate.entity);
                    const ThingTemplateComponent* type =
                        ecs::try_get<ThingTemplateComponent>(
                            registry, candidate.entity);
                    const bool jetAi = type && type->archetype &&
                        type->archetype->aiRecipe ==
                            ai::AIRecipeId::JetAIUpdate;
                    // JetAIUpdate::isDoingGroundMovement is always false,
                    // even while the taxi locomotor uses a ground surface.
                    // Taxi/runway routing remains active; only the generic
                    // ground obstruction and MoveAside contract is excluded.
                    result.doingGroundMovement = locomotion &&
                        (locomotion->surfaces & groundSurfaces) != 0 &&
                        !(airborne && airborne->isAirborne) && !jetAi;

                    // ZH needToRotate(): unresolved paths need rotation,
                    // wander locomotors do not, and installed paths use the
                    // current path point with the exact PI/30 threshold.
                    result.needsRotation = !result.hasPath;
                    if (locomotion &&
                        locomotion->wanderWidthFactor > kPhysicsZero) {
                        result.needsRotation = false;
                    } else if (locomotion && result.hasPath) {
                        const PhysicsScalar goalDx =
                            locomotion->goal.x - result.position.x;
                        const PhysicsScalar goalDy =
                            locomotion->goal.y - result.position.y;
                        if (goalDx.raw() == 0 && goalDy.raw() == 0) {
                            result.needsRotation = false;
                        } else {
                            const PhysicsScalar relative =
                                normalizeMovementAngle(
                                    math::fixed_atan2(goalDy, goalDx) -
                                    fixedTransform.yawRadians);
                            const PhysicsScalar threshold =
                                kMovementPi / PhysicsScalar{int32_t{30}};
                            result.needsRotation =
                                PhysicsScalar::abs(relative) > threshold;
                        }
                    }

                    const ObjectOrderQueueComponent* queue =
                        ecs::try_get<ObjectOrderQueueComponent>(
                            registry, candidate.entity);
                    if (queue && !queue->orders.empty()) {
                        const ObjectOrderIntent& order =
                            queue->orders.front();
                        LogicFixedVec3 finalGoal{
                            order.targetX, order.targetY, order.targetZ};
                        bool hasFinalGoal = order.hasTargetPosition;
                        if (order.targetObject) {
                            const std::optional<ecs::entity> target =
                                lifecycle.entityFromId(order.targetObject);
                            if (target && ecs::try_get<
                                    ObjectFixedTransformComponent>(
                                        registry, *target)) {
                                finalGoal = positionOf(*target);
                                hasFinalGoal = true;
                            }
                        }
                        if (hasFinalGoal) {
                            result.nearFinalGoal =
                                PhysicsScalar::abs(
                                    finalGoal.x - result.position.x) <
                                    pathfindCellSize &&
                                PhysicsScalar::abs(
                                    finalGoal.y - result.position.y) <
                                    pathfindCellSize;
                        }
                    }
                    const ObjectHealthComponent* health =
                        ecs::try_get<ObjectHealthComponent>(
                            registry, candidate.entity);
                    result.effectivelyDead = health &&
                        health->effectivelyDead;
                    const ObjectKindOfComponent* kinds =
                        ecs::try_get<ObjectKindOfComponent>(
                            registry, candidate.entity);
                    result.infantry = hasKind(
                        kinds, game::ObjectKindOf::Infantry);
                    result.vehicle = hasKind(
                        kinds, game::ObjectKindOf::Vehicle);
                    result.dozer = hasKind(
                        kinds, game::ObjectKindOf::Dozer);
                    return result;
                };
                const ObjectAIMovementObstructionParticipant leftFacts =
                    participant(left);
                const ObjectAIMovementObstructionParticipant rightFacts =
                    participant(right);
                const auto canCrush = [&](const Candidate& crusher,
                                          const Candidate& victim) {
                    // Keep this frozen fact identical to
                    // Object::canCrushOrSquish(TEST_CRUSH_OR_SQUISH).  The
                    // later blockedBy transaction must not decide that a
                    // friendly unit can be driven through merely because its
                    // authored CrusherLevel exceeds the peer's CrushableLevel.
                    if (isObjectDisabledBy(
                            registry, crusher.entity,
                            ObjectDisabledReason::Unmanned,
                            confirmedTick) ||
                        (players && relationshipBetweenObjects(
                            registry, *players, crusher.entity,
                            victim.entity) == PlayerRelationship::Allies)) {
                        return false;
                    }
                    const ThingTemplateComponent* crusherType =
                        ecs::try_get<ThingTemplateComponent>(
                            registry, crusher.entity);
                    const ThingTemplateComponent* victimType =
                        ecs::try_get<ThingTemplateComponent>(
                            registry, victim.entity);
                    if (!crusherType || !crusherType->archetype ||
                        !victimType || !victimType->archetype ||
                        crusherType->archetype->templateData.crusherLevel ==
                            0) {
                        return false;
                    }
                    if (ecs::try_get<ObjectSquishCollideComponent>(
                            registry, victim.entity)) {
                        return true;
                    }
                    return crusherType->archetype->templateData.crusherLevel >
                        victimType->archetype->templateData.crushableLevel;
                };
                const auto appendObstruction = [&](const Candidate& mover,
                                                   const Candidate& blocker,
                                                   const ObjectAIMovementObstructionParticipant& moverFacts,
                                                   const ObjectAIMovementObstructionParticipant& blockerFacts) {
                    const ObjectAIPathMovementComponent* movement =
                        ecs::try_get<ObjectAIPathMovementComponent>(
                            registry, mover.entity);
                    const ObjectLocomotionComponent* moverLocomotion =
                        ecs::try_get<ObjectLocomotionComponent>(
                            registry, mover.entity);
                    const ObjectLocomotionComponent* blockerLocomotion =
                        ecs::try_get<ObjectLocomotionComponent>(
                            registry, blocker.entity);
                    if (!movement || !moverLocomotion ||
                        !blockerLocomotion ||
                        moverLocomotion->state ==
                            ObjectLocomotionState::Idle) {
                        return;
                    }
                    const ObjectPhysicsComponent* moverPhysics =
                        ecs::try_get<ObjectPhysicsComponent>(
                            registry, mover.entity);
                    const LogicFixedVec3 moverEnd =
                        positionOf(mover.entity);
                    const LogicFixedVec3 moverStart = moverPhysics &&
                            moverPhysics->collisionStartTick == confirmedTick
                        ? moverPhysics->collisionStartPosition
                        : moverEnd;
                    const LogicFixedVec3 moverTravel{
                        moverEnd.x - moverStart.x,
                        moverEnd.y - moverStart.y,
                        moverEnd.z - moverStart.z,
                    };
                    obstructionEvents.push_back({
                        .mover = moverFacts,
                        .blocker = blockerFacts,
                        .moverContactPosition = addFixed(
                            moverStart,
                            scaleFixed(moverTravel, swept.timeOfImpact)),
                        .pathfindCellSize = pathfindCellSize,
                        .moverCanCrushBlocker = canCrush(mover, blocker),
                        .confirmedTick = confirmedTick,
                        .submissionOrdinal =
                            nextGameplaySubmissionOrdinal++,
                    });
                    if (nextGameplaySubmissionOrdinal == 0) {
                        ++nextGameplaySubmissionOrdinal;
                    }
                };
                appendObstruction(left, right, leftFacts, rightFacts);
                appendObstruction(right, left, rightFacts, leftFacts);
            }
            if (leftPhysics) leftPhysics->lastCollidee = right.id;
            if (rightPhysics) rightPhysics->lastCollidee = left.id;

            const auto respond = [&](const Candidate& source,
                                     const Candidate& target,
                                     ObjectPhysicsComponent* physics,
                                     const ObjectPhysicsComponent* targetPhysics,
                                     const ObjectKindOfComponent* sourceKinds,
                                     const ObjectKindOfComponent* targetKinds,
                                     bool targetImmobile,
                                     const LogicFixedVec3& outward) {
                if (!physics || !physics->allowCollideForce ||
                    hasKind(sourceKinds, game::ObjectKindOf::Immobile) ||
                    ecs::try_get<ObjectProjectileComponent>(
                        registry, source.entity)) {
                    return;
                }
                physics->sleeping = false;

                // Locomotion owns ordinary unit translation, so updatePhysics
                // leaves that path before the free-body integrator and never
                // turns pendingForce into acceleration. Separate those objects
                // positionally instead. RefCode's PhysicsBehavior::onCollide
                // likewise resolves the overlap exactly once per frame: this
                // only accumulates the share, and the single ordered writer
                // after the contact loop performs the move.
                if (ecs::try_get<ObjectLocomotionComponent>(
                        registry, source.entity) != nullptr &&
                    !physics->forceFreeBodyTranslation &&
                    !battleBusOwnsFreeBodyTranslation(registry,
                                                      source.entity)) {
                    const ObjectLocomotionComponent* sourceLocomotion =
                        ecs::try_get<ObjectLocomotionComponent>(
                            registry, source.entity);
                    const ObjectLocomotionComponent* targetLocomotion =
                        targetPhysics
                            ? ecs::try_get<ObjectLocomotionComponent>(
                                  registry, target.entity)
                            : nullptr;
                    const bool bothLocomotionOwned = sourceLocomotion &&
                        targetLocomotion && targetPhysics &&
                        !targetPhysics->forceFreeBodyTranslation &&
                        !battleBusOwnsFreeBodyTranslation(registry,
                                                          target.entity);
                    if (bothLocomotionOwned) {
                        // Locomotion/AI owns ordinary unit translation even
                        // while the blocker is idle and therefore has no
                        // active path component. Friendly idle actors may
                        // receive MoveAside; busy, disabled, enemy, or neutral
                        // blockers make the mover stop or re-admit its path.
                        // Symmetric mass separation here would instead push
                        // the blocker directly and can also remove authored
                        // SquishCollide contact before its consumer runs.
                        return;
                    }
                    if (overlap.raw() <= 0) return;
                    // An Immobile partner never moves, so the mobile side owns
                    // the whole depth. Otherwise each side takes the fraction
                    // of the depth carried by the *other* object's mass, so the
                    // two opposite pushes sum to exactly one overlap.
                    PhysicsScalar share = kPhysicsOne;
                    if (!targetImmobile) {
                        const PhysicsScalar sourceMass = PhysicsScalar::max(
                            physics->mass, kPhysicsZero);
                        const PhysicsScalar targetMass = targetPhysics
                            ? PhysicsScalar::max(targetPhysics->mass,
                                                 kPhysicsZero)
                            : kPhysicsZero;
                        const PhysicsScalar total = sourceMass + targetMass;
                        share = total.raw() > 0
                            ? targetMass / total
                            : PhysicsScalar::from_fraction(1, 2);
                    }
                    if (share.raw() <= 0) return;
                    const PhysicsScalar depth = overlap * share;
                    accumulateSeparation(source, {
                        outward.x * depth,
                        outward.y * depth,
                        outward.z * depth,
                    });
                    return;
                }

                if (targetImmobile) {
                    const bool sourceIsVehicle =
                        hasKind(sourceKinds, game::ObjectKindOf::Vehicle);
                    const bool targetIsBuilding =
                        hasKind(targetKinds, game::ObjectKindOf::Structure);
                    const LogicFixedVec3 sourcePosition =
                        positionOf(source.entity);
                    const bool descendingOntoTarget =
                        physics->velocityUnitsPerSecond.z < kPhysicsZero &&
                        sourcePosition.z > positionOf(target.entity).z &&
                        sourcePosition.z >=
                            rules.defaultStructureRubbleHeight;
                    if (descendingOntoTarget &&
                        (targetIsBuilding || sourceIsVehicle)) {
                        const game::WeaponContentId weapon = sourceIsVehicle
                            ? (targetIsBuilding
                                ? physics->crashIntoBuildingWeaponContent
                                : physics->crashIntoNonBuildingWeaponContent)
                            : game::WeaponContentId{};
                        crashCommands.push_back({
                            .source = source.id,
                            .target = target.id,
                            .weapon = weapon,
                            .impactPosition = sourcePosition,
                            .sourceSequence = crashSequence++,
                            .submissionOrdinal = reserveGameplayOrdinal(),
                            .confirmedTick = confirmedTick,
                            .targetIsBuilding = targetIsBuilding,
                            .destroySource = targetIsBuilding,
                        });
                    }

                    const PhysicsScalar speed = PhysicsScalar::sqrt(
                        physics->velocityUnitsPerSecond.x *
                            physics->velocityUnitsPerSecond.x +
                        physics->velocityUnitsPerSecond.y *
                            physics->velocityUnitsPerSecond.y +
                        physics->velocityUnitsPerSecond.z *
                            physics->velocityUnitsPerSecond.z);
                    const PhysicsScalar bounceSpeed = PhysicsScalar::max(
                        speed, PhysicsScalar::from_fraction(1, 5));
                    const PhysicsScalar structureStiffness = clampPhysics(
                        rules.structureStiffness,
                        kPhysicsMinimumGroundStiffness,
                        kPhysicsMaximumGroundStiffness);
                    const PhysicsScalar framesPerSecond{static_cast<int32_t>(
                        std::max<uint32_t>(1u, rules.logicFramesPerSecond))};
                    const PhysicsScalar forceScale = bounceSpeed *
                        physics->mass * structureStiffness * framesPerSecond;
                    physics->velocityUnitsPerSecond = {};
                    physics->pendingForce.x += outward.x * forceScale;
                    physics->pendingForce.y += outward.y * forceScale;
                    physics->pendingForce.z += outward.z * forceScale;
                    return;
                }

                // Match PhysicsBehavior's movable overlap response: cap the
                // penetration force and let mass division occur in the next
                // fixed physics barrier. Each side is processed once in
                // stable ObjectId order with opposite normals.
                physics->pendingForce.x += outward.x * overlap;
                physics->pendingForce.y += outward.y * overlap;
                physics->pendingForce.z += outward.z * overlap;
            };

            respond(left, right, leftPhysics, rightPhysics, leftKinds,
                    rightKinds, rightImmobile,
                    {-normal.x, -normal.y, -normal.z});
            respond(right, left, rightPhysics, leftPhysics, rightKinds,
                    leftKinds, leftImmobile, normal);
        }

    if (!sweptContacts.empty() &&
        confirmedTick % std::max<uint32_t>(
            1u, rules.logicFramesPerSecond) == 0u) {
        TD_LOG_INFO(
            "[MovementCollision] tick={} candidates={} swept={} obstructions={} separations={}",
            confirmedTick, candidates.size(), sweptContacts.size(),
            obstructionEvents.size() - obstructionEventBegin,
            separations.size());
    }

    // Single ordered writer for this frame's one relaxation pass. `separations`
    // is kept sorted by ObjectId as it is filled, so every peer applies the
    // same displacements in the same sequence, and each move goes through the
    // position authority so ObjectFixedTransformComponent stays the only
    // translation owner (it also republishes physics->position and
    // lastPublishedPosition, which positionOf above prefers).
    for (const Separation& separation : separations) {
        if (separation.displacement.x.raw() == 0 &&
            separation.displacement.y.raw() == 0 &&
            separation.displacement.z.raw() == 0) {
            continue;
        }
        if (!lifecycle.entityFromId(separation.id) ||
            lifecycle.isPendingDestroy(separation.id)) {
            continue;
        }
        const ObjectFixedTransformComponent* fixedTransform =
            ecs::try_get<ObjectFixedTransformComponent>(
                registry, separation.entity);
        if (!fixedTransform || !fixedTransform->authoritative) continue;
        LogicFixedVec3 separated = addFixed(positionOf(separation.entity),
                                            separation.displacement);
        // The push is derived from a 3D contact normal, so re-establish the
        // object's own pathfind layer as the floor at the destination column
        // instead of letting a downward component sink it into terrain.
        const PhysicsScalar ground = physicsLayerHeight(
            terrain, registry, separation.entity, separated);
        if (separated.z < ground) separated.z = ground;
        writeAuthoritativeObjectPosition(registry, separation.entity,
                                         separated);
    }
    separations.clear();
    expiredCollisionIgnores.clear();
    candidates.clear();
    sweptContacts.clear();
    consideredPairs.clear();
    nearby.clear();
}

} // namespace object_simulation_detail

using namespace object_simulation_detail;

void ObjectSimulation::resolveQueuedPhysics(ecs::registry& registry, ObjectLifecycle& lifecycle,
                                            uint64_t confirmedTick) {
    auto& simulationState = object_simulation_detail::state(*this);
    auto& pending = simulationState.m_physicsRequests;
    auto& ready = simulationState.m_physicsScratch.readyRequests;
    ready.clear();
    ready.reserve(pending.size());
    size_t deferredCount = 0;
    for (size_t readIndex = 0; readIndex < pending.size(); ++readIndex) {
        object_simulation_detail::QueuedPhysicsRequest& queued =
            pending[readIndex];
        if (queued.request.confirmedTick == 0) queued.request.confirmedTick = confirmedTick;
        if (queued.request.confirmedTick > confirmedTick) {
            if (deferredCount != readIndex) {
                pending[deferredCount] = std::move(queued);
            }
            ++deferredCount;
        } else {
            ready.push_back(std::move(queued));
        }
    }
    pending.resize(deferredCount);
    std::sort(ready.begin(), ready.end(), [](const object_simulation_detail::QueuedPhysicsRequest& left,
                                             const object_simulation_detail::QueuedPhysicsRequest& right) {
        const ObjectPhysicsRequest& a = left.request;
        const ObjectPhysicsRequest& b = right.request;
        if (a.confirmedTick != b.confirmedTick) return a.confirmedTick < b.confirmedTick;
        if (a.target != b.target) return a.target < b.target;
        if (a.source != b.source) return a.source < b.source;
        if (a.sourceSequence != b.sourceSequence) return a.sourceSequence < b.sourceSequence;
        if (a.kind != b.kind) return static_cast<uint8_t>(a.kind) < static_cast<uint8_t>(b.kind);
        return left.submissionOrdinal < right.submissionOrdinal;
    });

    for (const object_simulation_detail::QueuedPhysicsRequest& queued : ready) {
        const ObjectPhysicsRequest& request = queued.request;
        const std::optional<ecs::entity> entity = lifecycle.entityFromId(request.target);
        if (!entity || lifecycle.isPendingDestroy(request.target)) continue;
        ObjectPhysicsComponent* physics = ecs::try_get<ObjectPhysicsComponent>(registry, *entity);
        if (!physics) continue;

        const ObjectProjectileComponent* projectile =
            ecs::try_get<ObjectProjectileComponent>(registry, *entity);
        const bool translationOwnedExternally =
            (ecs::try_get<ObjectLocomotionComponent>(registry, *entity) &&
             !physics->forceFreeBodyTranslation &&
             !battleBusOwnsFreeBodyTranslation(registry, *entity)) ||
            projectile;
        physics->sleeping = false;
        switch (request.kind) {
        case ObjectPhysicsRequestKind::ApplyForce:
            // Stage-1 has no locomotor force bridge yet. Dropping a force at
            // this explicit boundary is safer than secretly accumulating it
            // until an object changes controller later in the match.
            if (!translationOwnedExternally) {
                physics->pendingForce = addFixed(physics->pendingForce, request.linear);
            }
            break;
        case ObjectPhysicsRequestKind::ApplyMotiveForce:
            if (!translationOwnedExternally) {
                physics->pendingForce = addFixed(
                    physics->pendingForce, request.linear);
            }
            {
                const uint64_t motiveFrames = std::max<uint64_t>(
                    1u, simulationState.m_rules.logicFramesPerSecond / 3u);
                physics->motiveForceExpiresTick =
                    request.confirmedTick >
                            std::numeric_limits<uint64_t>::max() - motiveFrames
                        ? std::numeric_limits<uint64_t>::max()
                        : request.confirmedTick + motiveFrames;
            }
            break;
        case ObjectPhysicsRequestKind::ApplyShock: {
            if (translationOwnedExternally) break;
            const PhysicsScalar resistance = clampPhysics(
                physics->shockResistance, kPhysicsZero, kPhysicsOne);
            physics->pendingForce = addFixed(physics->pendingForce,
                scaleFixed(request.linear, kPhysicsOne - resistance));
            break;
        }
        case ObjectPhysicsRequestKind::AddVelocity:
            if (!translationOwnedExternally) {
                physics->velocityUnitsPerSecond = addFixed(
                    physics->velocityUnitsPerSecond, request.linear);
            }
            break;
        case ObjectPhysicsRequestKind::SetAngularRates:
            physics->yawRate = request.yawRate;
            physics->pitchRate = request.pitchRate;
            physics->rollRate = request.rollRate;
            // A locomotor remains yaw-authoritative until its dedicated
            // force/attitude bridge arrives. A free body can immediately
            // expose the full 3D pose through the render extraction boundary.
            if (!ecs::try_get<ObjectLocomotionComponent>(registry, *entity)) {
                physics->ownsAttitude = true;
            }
            break;
        case ObjectPhysicsRequestKind::AddAngularRates:
            if (!physics->stickToGround) {
                physics->yawRate += request.yawRate;
                physics->pitchRate += request.pitchRate;
                physics->rollRate += request.rollRate;
                physics->allowBouncing = true;
                physics->ownsAttitude = true;
            }
            break;
        case ObjectPhysicsRequestKind::ScrubVelocity2D: {
            const PhysicsScalar limit = PhysicsScalar::max(
                kPhysicsZero, request.magnitudeLimit);
            constexpr PhysicsScalar epsilon =
                PhysicsScalar::from_fraction(1, 1000);
            if (limit < epsilon) {
                physics->velocityUnitsPerSecond.x = kPhysicsZero;
                physics->velocityUnitsPerSecond.y = kPhysicsZero;
                break;
            }
            const PhysicsScalar speedSquared =
                physics->velocityUnitsPerSecond.x *
                    physics->velocityUnitsPerSecond.x +
                physics->velocityUnitsPerSecond.y *
                    physics->velocityUnitsPerSecond.y;
            if (speedSquared > limit * limit) {
                const PhysicsScalar speed =
                    PhysicsScalar::sqrt(speedSquared);
                if (speed > kPhysicsZero) {
                    const PhysicsScalar scale = limit / speed;
                    physics->velocityUnitsPerSecond.x *= scale;
                    physics->velocityUnitsPerSecond.y *= scale;
                }
            }
            break;
        }
        case ObjectPhysicsRequestKind::ScrubVelocityZ: {
            constexpr PhysicsScalar epsilon =
                PhysicsScalar::from_fraction(1, 1000);
            const PhysicsScalar limit = request.magnitudeLimit;
            if (PhysicsScalar::abs(limit) < epsilon) {
                physics->velocityUnitsPerSecond.z = kPhysicsZero;
            } else if (limit < kPhysicsZero &&
                       physics->velocityUnitsPerSecond.z < limit) {
                physics->velocityUnitsPerSecond.z = limit;
            } else if (limit > kPhysicsZero &&
                       physics->velocityUnitsPerSecond.z > limit) {
                physics->velocityUnitsPerSecond.z = limit;
            }
            break;
        }
        case ObjectPhysicsRequestKind::SetStunned:
            physics->stunned = request.enabled;
            setPhysicsModelCondition(
                registry, *entity, game::ModelConditionFlag::StunnedFlailing,
                request.enabled);
            if (!request.enabled) {
                setPhysicsModelCondition(
                    registry, *entity, game::ModelConditionFlag::Stunned,
                    false);
            }
            break;
        case ObjectPhysicsRequestKind::SetFreeFall:
            physics->inFreeFall = request.enabled;
            if (!request.enabled) {
                static_cast<void>(ObjectDisabledSystem::clear(
                    registry, *entity, ObjectDisabledReason::Freefall,
                    confirmedTick));
                setPhysicsModelCondition(
                    registry, *entity, game::ModelConditionFlag::FreeFall,
                    false);
            }
            break;
        }
    }
    ready.clear();
}

} // namespace engine
