#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"
#include "game/object/simulation/runtime/ObjectOrderOwnership.h"

#include "game/base/SimulationRandom.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/navigation/runtime/NavigationSystem.h"
#include "game/navigation/integration/NavigationTerrainLayerMapping.h"
#include "game/navigation/integration/NavigationDestinationAdjustment.h"
#include "game/navigation/integration/NavigationPathSmoothing.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/definition/LocomotorTemplate.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/simulation/combat/ObjectCombatProfileRuntime.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/plan/movement/ObjectPhysicsPlanTypes.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"
#include "game/object/contracts/ObjectOrderClassification.h"
#include "game/object/ai/runtime/ObjectAIOrderAdmission.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"
#include "core/math/wwmath/base/wwmath.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>
#include "game/object/simulation/runtime/ObjectSimulationDetail.h"
#include "game/object/component/ObjectDirty.h"

namespace engine {

using container::asciiEqualIgnoreCase;

namespace {

[[nodiscard]] bool isCombatDropMovementOrder(
    const ObjectOrderIntent* order) noexcept {
    return order && order->kind == ObjectOrderKind::CommandButton &&
        order->combatDrop && order->hasTargetPosition;
}

[[nodiscard]] ObjectMoveOrderConsumer resolveMoveOrderConsumer(
    const ObjectOrderIntent* order, bool aiMoveStopOwner,
    bool aiAttackOwner) noexcept {
    if (!order)
        return ObjectMoveOrderConsumer::None;

    // ChinookAIUpdate's MOVE_TO_COMBAT_DROP inherits the ordinary MoveTo
    // implementation while the queue head remains Command_CombatDrop.  The
    // typed insertion state owns only this approach leg; every other command
    // button remains outside generic movement ownership.
    if (isCombatDropMovementOrder(order))
        return aiMoveStopOwner
            ? ObjectMoveOrderConsumer::ObjectAIRuntime
            : ObjectMoveOrderConsumer::None;

    ai::ObjectAIOrderCapability capabilities =
        ai::ObjectAIOrderCapability::None;
    if (aiMoveStopOwner)
        capabilities |= ai::ObjectAIOrderCapability::MoveStop;
    if (aiAttackOwner)
        capabilities |= ai::ObjectAIOrderCapability::Attack;
    const ai::ObjectAIOrderOwner owner = ai::objectAIOrderOwner(
        static_cast<ai::ObjectAIOrderKind>(order->kind),
        static_cast<ai::ObjectAIOrderSource>(order->source),
        static_cast<ai::ObjectAIOrderSystemPurpose>(order->systemPurpose),
        capabilities, order->attackMove,
        static_cast<ai::ObjectAIMoveRouteSubtype>(order->moveRouteSubtype),
        static_cast<ai::ObjectAITacticalAttackSubtype>(
            order->tacticalAttackSubtype));
    // Attack wrappers are only observed here when ObjectAI owns the combat
    // state.  Build and other non-movement intents must never be reclassified
    // as a specialized locomotion order merely because they have a legacy
    // owner in the common table.
    if (order->kind != ObjectOrderKind::Move) {
        return owner == ai::ObjectAIOrderOwner::ObjectAIRuntime
            ? ObjectMoveOrderConsumer::ObjectAIRuntime
            : ObjectMoveOrderConsumer::None;
    }
    if (owner == ai::ObjectAIOrderOwner::ObjectAIRuntime)
        return ObjectMoveOrderConsumer::ObjectAIRuntime;
    if (owner == ai::ObjectAIOrderOwner::LegacyMovement)
        return ObjectMoveOrderConsumer::LegacyMovement;
    if (owner == ai::ObjectAIOrderOwner::LegacyCombat ||
        owner == ai::ObjectAIOrderOwner::CommandIngress ||
        owner == ai::ObjectAIOrderOwner::Unsupported ||
        owner == ai::ObjectAIOrderOwner::None)
        return ObjectMoveOrderConsumer::None;
    if (order->systemPurpose ==
            ObjectOrderSystemPurpose::IntentionalContact) {
        // ZH routes these through AIUpdate::aiEnter, but their terminal effect
        // is CrateCollide rather than containment.  LegacyMovement owns only
        // the dynamic-target approach; CrateCollide consumes the retained
        // head later in the same confirmed frame.
        return ObjectMoveOrderConsumer::LegacyMovement;
    }
    if (owner == ai::ObjectAIOrderOwner::LegacySpecialized ||
        owner == ai::ObjectAIOrderOwner::LegacyBuilderProduction)
        return ObjectMoveOrderConsumer::SpecializedSystem;
    return ObjectMoveOrderConsumer::None;
}

[[nodiscard]] constexpr bool adjustsSystemDestination(
    ObjectOrderSystemPurpose purpose) noexcept {
    return purpose == ObjectOrderSystemPurpose::ProductionExit ||
        purpose == ObjectOrderSystemPurpose::ContainmentExit ||
        purpose == ObjectOrderSystemPurpose::ScenarioReinforcementDeliver ||
        purpose == ObjectOrderSystemPurpose::ScenarioReinforcementExit ||
        purpose == ObjectOrderSystemPurpose::RailedTransport;
}

} // namespace

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

[[nodiscard]] PhysicsScalar moveTowardsFixed(
    PhysicsScalar current, PhysicsScalar target,
    PhysicsScalar maximumDelta) noexcept;

void projectFixedMovementState(
    const ObjectFixedTransformComponent& fixedTransform,
    ObjectLocomotionComponent& locomotion,
    TransformComponent& transform) noexcept {
    transform.x = fixedTransform.position.x.to_float();
    transform.y = fixedTransform.position.y.to_float();
    transform.z = fixedTransform.position.z.to_float();
    transform.rotation = fixedTransform.yawRadians.to_float();
    static_cast<void>(locomotion);
}

void ensureFixedMovementState(ObjectFixedTransformComponent& fixedTransform,
                              ObjectLocomotionComponent& locomotion,
                              const TransformComponent& transform) noexcept {
    static_cast<void>(transform);
    TD_ASSERT_MSG(fixedTransform.authoritative,
                  "locomotion object is missing fixed transform authority");
    TD_ASSERT_MSG(locomotion.fixedRuntimeInitialized,
                  "locomotion profile was not quantized at ingress");
}

void brakeFixedMovement(ObjectLocomotionComponent& locomotion,
                        PhysicsScalar logicDelta) noexcept {
    locomotion.forwardSpeed = moveTowardsFixed(
        locomotion.forwardSpeed, kPhysicsZero,
        PhysicsScalar::abs(locomotion.braking) * logicDelta);
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

[[nodiscard]] bool isMovementPenaltyState(
    ObjectBodyDamageState state,
    const ObjectSimulationRules& rules) noexcept {
    // RefCode uses IS_CONDITION_BETTER(condition, MovementPenaltyDamageState)
    // (i.e. enum ordering), not a hard-coded REALLYDAMAGED comparison.
    return static_cast<uint8_t>(state) >=
        static_cast<uint8_t>(rules.movementPenaltyDamageState);
}

} // namespace

namespace object_simulation_detail {

[[nodiscard]] container::Vector<game::FrozenLocomotorTemplate> collectRuntimeLocomotors(
    const game::ThingTemplate& objectTemplate, const GameContentSnapshot& content,
    game::LocomotorSetSlot slot) {
    container::Vector<game::FrozenLocomotorTemplate> result;
    const auto append = [&content, &result](const container::Vector<container::String>& names) {
        for (const container::String& name : names) {
            const game::FrozenLocomotorTemplate* candidate = content.findLocomotor(name);
            if (candidate && candidate->supportsRuntimeLocomotion())
                result.push_back(*candidate);
        }
    };

    for (const game::LocomotorSetDefinition& set : objectTemplate.locomotorSets) {
        if (set.slot != slot) continue;
        append(set.templates);
        return result;
    }
    // Old/generated recipes may expose only the flattened compatibility list.
    // It represents SET_NORMAL. Once any typed sets exist, however, that list
    // contains every slot; falling back through it could silently select an
    // upgraded/panic/freefall locomotor as the normal set.
    if (objectTemplate.locomotorSets.empty() &&
        slot == game::LocomotorSetSlot::Normal) {
        append(objectTemplate.locomotors);
        if (result.empty() && !objectTemplate.locomotor.empty()) {
            if (const game::FrozenLocomotorTemplate* candidate =
                    content.findLocomotor(objectTemplate.locomotor);
                candidate && candidate->supportsRuntimeLocomotion()) {
                result.push_back(*candidate);
            }
        }
    }
    return result;
}

[[nodiscard]] std::optional<game::LocomotorSetSlot>
parseRiderLocomotorSet(container::StringView value) noexcept {
    if (asciiEqualIgnoreCase(value, "SET_NORMAL"))
        return game::LocomotorSetSlot::Normal;
    if (asciiEqualIgnoreCase(value, "SET_SLUGGISH"))
        return game::LocomotorSetSlot::Sluggish;
    if (asciiEqualIgnoreCase(value, "SET_PANIC"))
        return game::LocomotorSetSlot::Panic;
    if (asciiEqualIgnoreCase(value, "SET_WANDER"))
        return game::LocomotorSetSlot::Wander;
    return std::nullopt;
}

void applyLocomotorTemplate(ObjectLocomotionComponent& runtime,
                            const game::FrozenLocomotorTemplate& locomotor) {
    runtime.templateName = locomotor.name;
    runtime.surfaces = locomotor.surfaces;
    runtime.appearance = locomotor.appearance;
    runtime.zAxisBehavior = locomotor.zAxisBehavior;
    runtime.groupPriority = locomotor.groupPriority;
    runtime.circlingRadius = locomotor.fixed.circlingRadius;
    runtime.extra2DFrictionPerSecond =
        locomotor.fixed.extra2DFrictionPerSecond;
    runtime.maximumThrustAngleRadians =
        locomotor.fixed.maximumThrustAngleRadians;
    runtime.accelerationPitchLimitRadians =
        locomotor.fixed.accelerationPitchLimitRadians;
    runtime.decelerationPitchLimitRadians =
        locomotor.fixed.decelerationPitchLimitRadians;
    runtime.bounceAngularVelocityRadiansPerSecond =
        locomotor.fixed.bounceAngularVelocityRadiansPerSecond;
    runtime.pitchStiffness = locomotor.fixed.pitchStiffness;
    runtime.rollStiffness = locomotor.fixed.rollStiffness;
    runtime.pitchDamping = locomotor.fixed.pitchDamping;
    runtime.rollDamping = locomotor.fixed.rollDamping;
    runtime.thrustRoll = locomotor.fixed.thrustRoll;
    runtime.thrustWobbleRate = locomotor.fixed.thrustWobbleRate;
    runtime.thrustMinimumWobble = locomotor.fixed.thrustMinimumWobble;
    runtime.thrustMaximumWobble = locomotor.fixed.thrustMaximumWobble;
    runtime.pitchByZVelocityFactor =
        locomotor.fixed.pitchByZVelocityFactor;
    runtime.forwardVelocityPitchFactor =
        locomotor.fixed.forwardVelocityPitchFactor;
    runtime.lateralVelocityRollFactor =
        locomotor.fixed.lateralVelocityRollFactor;
    runtime.forwardAccelerationPitchFactor =
        locomotor.fixed.forwardAccelerationPitchFactor;
    runtime.lateralAccelerationRollFactor =
        locomotor.fixed.lateralAccelerationRollFactor;
    runtime.uniformAxialDamping = locomotor.fixed.uniformAxialDamping;
    runtime.turnPivotOffset = locomotor.fixed.turnPivotOffset;
    runtime.maximumWheelExtension = locomotor.fixed.maximumWheelExtension;
    runtime.maximumWheelCompression = locomotor.fixed.maximumWheelCompression;
    runtime.frontWheelTurnAngleRadians =
        locomotor.fixed.frontWheelTurnAngleRadians;
    runtime.wanderWidthFactor = locomotor.fixed.wanderWidthFactor;
    runtime.wanderLengthFactor = locomotor.fixed.wanderLengthFactor;
    runtime.wanderAboutPointRadius =
        locomotor.fixed.wanderAboutPointRadius;
    runtime.rudderCorrectionDegree = locomotor.fixed.rudderCorrectionDegree;
    runtime.rudderCorrectionRate = locomotor.fixed.rudderCorrectionRate;
    runtime.elevatorCorrectionDegree =
        locomotor.fixed.elevatorCorrectionDegree;
    runtime.elevatorCorrectionRate = locomotor.fixed.elevatorCorrectionRate;
    runtime.airborneTargetingHeight =
        locomotor.fixed.airborneTargetingHeight;
    runtime.hasFiniteAirborneTargetingHeight =
        locomotor.fixed.hasFiniteAirborneTargetingHeight;
    runtime.closeEnoughDistance3D = locomotor.closeEnoughDistance3D;
    runtime.stickToGround = locomotor.stickToGround;
    runtime.canMoveBackwards = locomotor.canMoveBackwards;
    runtime.locomotorWorksWhenDead = locomotor.locomotorWorksWhenDead;
    runtime.allowMotiveForceWhileAirborne =
        locomotor.allowMotiveForceWhileAirborne;
    runtime.apply2DFrictionWhenAirborne =
        locomotor.apply2DFrictionWhenAirborne;
    runtime.downhillOnly = locomotor.downhillOnly;
    runtime.hasSuspension = locomotor.hasSuspension;
    runtime.maximumSpeed = locomotor.fixed.maximumSpeed;
    runtime.damagedMaximumSpeed = locomotor.fixed.damagedMaximumSpeed;
    runtime.maximumTurnRate = locomotor.fixed.maximumTurnRate;
    runtime.damagedMaximumTurnRate =
        locomotor.fixed.damagedMaximumTurnRate;
    runtime.acceleration = locomotor.fixed.acceleration;
    runtime.damagedAcceleration = locomotor.fixed.damagedAcceleration;
    runtime.lift = locomotor.fixed.lift;
    runtime.damagedLift = locomotor.fixed.damagedLift;
    runtime.braking = locomotor.fixed.braking;
    runtime.minimumSpeed = locomotor.fixed.minimumSpeed;
    runtime.minimumTurnSpeed = locomotor.fixed.minimumTurnSpeed;
    runtime.preferredHeightFixed = locomotor.fixed.preferredHeight;
    runtime.preferredHeightDampingFixed =
        locomotor.fixed.preferredHeightDamping;
    runtime.speedLimitZ = locomotor.fixed.speedLimitZ;
    runtime.closeEnough = locomotor.fixed.closeEnough;
    runtime.slideIntoPlace = locomotor.fixed.slideIntoPlaceMilliseconds;
    runtime.accelerationIsInfinite =
        locomotor.fixed.accelerationIsInfinite;
    runtime.damagedAccelerationIsInfinite =
        locomotor.fixed.damagedAccelerationIsInfinite;
    runtime.brakingIsInfinite = locomotor.fixed.brakingIsInfinite;
    runtime.hasFiniteBraking = locomotor.fixed.hasFiniteBraking;
    runtime.hasFiniteSpeedLimitZ = locomotor.fixed.hasFiniteSpeedLimitZ;
    runtime.preferredHeightIsLowest =
        locomotor.fixed.preferredHeightIsLowest;
    runtime.fixedRuntimeInitialized = true;
    // RefCode swaps Locomotor instances while PhysicsBehavior retains linear
    // velocity. Keep both speed axes across surface/set changes; a fresh ECS
    // component already starts them at zero.
    runtime.movingBackward = false;
    runtime.overWater = false;
}

[[nodiscard]] game::LocomotorSurfaceMask acceptableLocomotorSurfacesAt(
    const game::terrain::TerrainLogic& terrain,
    PhysicsScalar x, PhysicsScalar y) noexcept {
    using game::LocomotorSurface;
    using game::locomotorSurfaceBit;
    const game::LocomotorSurfaceMask air =
        locomotorSurfaceBit(LocomotorSurface::Air);
    if (!terrain.isLoaded())
        return locomotorSurfaceBit(LocomotorSurface::Ground) |
               locomotorSurfaceBit(LocomotorSurface::Rubble) | air;
    if (terrain.isUnderwaterLegacyRaw(x.raw(), y.raw()))
        return locomotorSurfaceBit(LocomotorSurface::Water) | air;
    if (terrain.isCliffCellRaw(x.raw(), y.raw()))
        return locomotorSurfaceBit(LocomotorSurface::Cliff) | air;
    // TerrainLogic currently has no separate rubble query. Rubble shares the
    // walkable ground lane in the modern navigation grid, so admit both here.
    return locomotorSurfaceBit(LocomotorSurface::Ground) |
           locomotorSurfaceBit(LocomotorSurface::Rubble) | air;
}

void chooseLocomotorForPosition(ObjectLocomotionComponent& locomotion,
                                const game::terrain::TerrainLogic& terrain,
                                PhysicsScalar x, PhysicsScalar y) {
    const game::LocomotorSurfaceMask acceptable =
        acceptableLocomotorSurfacesAt(terrain, x, y);
    const auto found = std::find_if(
        locomotion.profiles.begin(), locomotion.profiles.end(),
        [acceptable](const game::FrozenLocomotorTemplate& candidate) {
            return (candidate.surfaces & acceptable) != 0;
        });
    // Legacy chooseGoodLocomotorFromCurrentSet retains the previous profile
    // when physics leaves an object slightly inside an invalid cell.
    if (found != locomotion.profiles.end() &&
        found->name != locomotion.templateName) {
        applyLocomotorTemplate(locomotion, *found);
    }
}


[[nodiscard]] bool sameActiveOrder(const ObjectLocomotionComponent& locomotion,
                                   const ObjectOrderIntent& order) noexcept {
    return locomotion.hasActiveMove && locomotion.activeOrderTick == order.issuedTick &&
           locomotion.activeOrderSequence == order.sourceSequence &&
           locomotion.activeSourceScriptId == order.sourceScriptId;
}

[[nodiscard]] bool isPathTraversable(const ObjectLocomotionComponent& locomotion,
                                     const game::terrain::TerrainLogic& terrain,
                                     const ObjectFixedTransformComponent&
                                         fixedTransform) noexcept {
    if (!terrain.isLoaded()) return true;
    if (!terrain.map().isInsidePlayableRaw(
            locomotion.goal.x.raw(), locomotion.goal.y.raw()))
        return false;
    // RefCode bypasses terrain-cell rejection for an AIR-capable current
    // profile. Navigation still owns map bounds and obstacle/path policy.
    game::LocomotorSurfaceMask availableSurfaces = 0;
    for (const game::FrozenLocomotorTemplate& profile : locomotion.profiles)
        availableSurfaces |= profile.surfaces;
    if (availableSurfaces == 0)
        availableSurfaces = locomotion.surfaces;
    if ((availableSurfaces &
         game::locomotorSurfaceBit(game::LocomotorSurface::Air)) != 0)
        return true;

    const PhysicsScalar startX = fixedTransform.position.x;
    const PhysicsScalar startY = fixedTransform.position.y;
    const PhysicsScalar dx = locomotion.goal.x - startX;
    const PhysicsScalar dy = locomotion.goal.y - startY;
    const PhysicsScalar distance = length2D(dx, dy);
    // The fallback straight path is intentionally conservative: sample every
    // terrain cell and refuse an unsupported cliff/water crossing rather than
    // pretending a direct line is a valid original pathfinder route.
    const PhysicsScalar sampleSpacing{
        game::terrain::kMapCellWorldSize};
    const uint32_t samples = std::max(
        1u, ceilPositiveMovementRatio(distance, sampleSpacing));
    for (uint32_t index = 0; index <= samples; ++index) {
        const PhysicsScalar fraction = PhysicsScalar::from_fraction(
            static_cast<int64_t>(index),
            static_cast<int64_t>(samples));
        const PhysicsScalar x = startX + dx * fraction;
        const PhysicsScalar y = startY + dy * fraction;
        game::LocomotorSurfaceMask required =
            game::locomotorSurfaceBit(game::LocomotorSurface::Ground) |
            game::locomotorSurfaceBit(game::LocomotorSurface::Rubble);
        if (terrain.isUnderwaterLegacyRaw(x.raw(), y.raw()))
            required = game::locomotorSurfaceBit(
                game::LocomotorSurface::Water);
        else if (terrain.isCliffCellRaw(x.raw(), y.raw()))
            required = game::locomotorSurfaceBit(
                game::LocomotorSurface::Cliff);
        if ((availableSurfaces & required) == 0)
            return false;
    }
    return true;
}

[[nodiscard]] PhysicsScalar terrainHeightForObjectLayer(
    const game::terrain::TerrainLogic& terrain, uint32_t pathfindLayer,
    PhysicsScalar x, PhysicsScalar y) noexcept {
    return PhysicsScalar::from_raw(
        terrain.pathfindLayerHeightRawAt(pathfindLayer, x.raw(), y.raw())
            .value_or(terrain.groundHeightRaw(x.raw(), y.raw())));
}

[[nodiscard]] PhysicsScalar locomotorSurfaceHeight(
    const game::terrain::TerrainLogic& terrain, uint32_t pathfindLayer,
    PhysicsScalar x, PhysicsScalar y) noexcept {
    if (terrain.isUnderwaterLegacyRaw(x.raw(), y.raw())) {
        if (const std::optional<int64_t> water =
                terrain.waterSurfaceHeightLegacyRawAt(x.raw(), y.raw())) {
            return PhysicsScalar::from_raw(*water);
        }
    }
    return terrainHeightForObjectLayer(terrain, pathfindLayer, x, y);
}

[[nodiscard]] PhysicsScalar locomotorTargetHeight(
    const ObjectLocomotionComponent& locomotion,
    const game::terrain::TerrainLogic& terrain, uint32_t pathfindLayer,
    PhysicsScalar x, PhysicsScalar y,
    PhysicsScalar explicitGoalZ) noexcept {
    if (locomotion.usePreciseZPosition)
        return explicitGoalZ;
    const PhysicsScalar layerHeight = terrainHeightForObjectLayer(
        terrain, pathfindLayer, x, y);
    const PhysicsScalar surfaceHeight = locomotorSurfaceHeight(
        terrain, pathfindLayer, x, y);
    const PhysicsScalar preferred = locomotion.preferredHeightIsLowest
        ? kPhysicsZero : locomotion.preferredHeightFixed;
    const auto highestLayerHeight = [&]() noexcept {
        const std::optional<int64_t> height = terrain.pathfindLayerHeightRawAt(
            terrain.highestPathfindLayerAtXYRaw(x.raw(), y.raw()),
            x.raw(), y.raw());
        return height ? PhysicsScalar::from_raw(*height) : layerHeight;
    };
    switch (locomotion.zAxisBehavior) {
    case game::LocomotorZAxisBehavior::NoZMotiveForce:
        return layerHeight + locomotion.groundOffsetFixed;
    case game::LocomotorZAxisBehavior::SeaLevel:
        return terrain.isUnderwaterLegacyRaw(x.raw(), y.raw())
            ? surfaceHeight : layerHeight;
    case game::LocomotorZAxisBehavior::SurfaceRelativeHeight:
    case game::LocomotorZAxisBehavior::FixedSurfaceRelativeHeight:
        return surfaceHeight + preferred;
    case game::LocomotorZAxisBehavior::AbsoluteHeight:
    case game::LocomotorZAxisBehavior::FixedAbsoluteHeight:
        return preferred;
    case game::LocomotorZAxisBehavior::FixedRelativeToGroundAndBuildings:
        return highestLayerHeight() + preferred;
    case game::LocomotorZAxisBehavior::SmoothRelativeToHighestLayer:
        return highestLayerHeight() + preferred;
    }
    return layerHeight + locomotion.groundOffsetFixed;
}

void updateLocomotorVertical(
    ObjectLocomotionComponent& locomotion,
    ObjectFixedTransformComponent& fixedTransform,
    const ObjectHealthComponent* health,
    const game::terrain::TerrainLogic& terrain, uint32_t pathfindLayer,
    const ObjectSimulationRules& rules,
    PhysicsScalar explicitGoalZ) noexcept {
    if (!terrain.isLoaded() || rules.logicDeltaSeconds <= kPhysicsZero)
        return;
    const PhysicsScalar deltaFixed = rules.logicDeltaSeconds;
    const PhysicsScalar target = locomotorTargetHeight(
        locomotion, terrain, pathfindLayer,
        fixedTransform.position.x,
        fixedTransform.position.y, explicitGoalZ);
    const bool fixed =
        locomotion.zAxisBehavior ==
            game::LocomotorZAxisBehavior::SeaLevel ||
        locomotion.zAxisBehavior ==
            game::LocomotorZAxisBehavior::FixedSurfaceRelativeHeight ||
        locomotion.zAxisBehavior ==
            game::LocomotorZAxisBehavior::FixedAbsoluteHeight ||
        locomotion.zAxisBehavior == game::LocomotorZAxisBehavior::
            FixedRelativeToGroundAndBuildings ||
        locomotion.zAxisBehavior ==
            game::LocomotorZAxisBehavior::NoZMotiveForce;
    if (fixed) {
        fixedTransform.position.z = target;
        locomotion.verticalSpeed = kPhysicsZero;
        return;
    }

    const bool damaged = health && isMovementPenaltyState(
        health->damageState, rules);
    const PhysicsScalar lift = damaged
        ? locomotion.damagedLift : locomotion.lift;
    const PhysicsScalar gravity = PhysicsScalar::abs(
        rules.gravityUnitsPerSecondSq);
    const PhysicsScalar upwardAcceleration = PhysicsScalar::max(
        kPhysicsZero, lift - gravity);
    const PhysicsScalar damping = PhysicsScalar::max(
        kPhysicsZero, locomotion.preferredHeightDampingFixed);
    const PhysicsScalar delta = target - fixedTransform.position.z;
    PhysicsScalar desiredVerticalSpeed = deltaFixed > kPhysicsZero
        ? delta * damping / deltaFixed : kPhysicsZero;
    if (locomotion.hasFiniteSpeedLimitZ) {
        const PhysicsScalar limit = PhysicsScalar::abs(locomotion.speedLimitZ);
        desiredVerticalSpeed = PhysicsScalar::clamp(
            desiredVerticalSpeed, -limit, limit);
    }
    // Increasing Z velocity is limited by net lift; reducing it is limited
    // by gravity. This is the seconds-unit counterpart of RefCode's
    // calcLiftToUseAtPt split between descending recovery and ascent braking.
    PhysicsScalar acceleration = desiredVerticalSpeed >=
            locomotion.verticalSpeed
        ? upwardAcceleration : gravity;
    if (locomotion.ultraAccurate) acceleration *= kPhysicsTwo;
    if (acceleration > kPhysicsZero) {
        locomotion.verticalSpeed = moveTowardsFixed(
            locomotion.verticalSpeed, desiredVerticalSpeed,
            acceleration * deltaFixed);
    }
    const PhysicsScalar step = locomotion.verticalSpeed * deltaFixed;
    if ((delta > kPhysicsZero && step >= delta) ||
        (delta < kPhysicsZero && step <= delta)) {
        fixedTransform.position.z = target;
        locomotion.verticalSpeed = kPhysicsZero;
    } else {
        fixedTransform.position.z += step;
    }
}

// RefCode Drawable::calcPhysicsXform dispatches a chassis suspension model
// only for LOCO_TREADS, LOCO_WHEELS_FOUR and LOCO_MOTORCYCLE
// (engine/client/drawable/Drawable.cpp, calcPhysicsXform).  Those three are
// the only appearances whose spring target is the terrain slope:
// LOCO_HOVER/LOCO_WINGS/LOCO_THRUST lean and wobble but never conform, and
// LOCO_LEGS_TWO/LOCO_CLIMBER/LOCO_OTHER receive no physics transform at all,
// which is why retail infantry stays upright on a hillside.
[[nodiscard]] bool locomotorConformsToTerrainSlope(
    const ObjectLocomotionComponent& locomotion) noexcept {
    switch (locomotion.appearance) {
    case game::LocomotorAppearance::Treads:
    case game::LocomotorAppearance::FourWheels:
    case game::LocomotorAppearance::Motorcycle:
        return true;
    default:
        return false;
    }
}

// Bounds every value the chassis spring can publish.  The terrain target is
// already bounded by construction (|normal| == 1 scaled by PI/2), but the
// spring accumulator, the recoil term and quantization error are not, and
// rebuildPhysicsOrientation degenerates at exactly +/- PI/2.
const PhysicsScalar kChassisAngleLimit =
    kMovementHalfPi - PhysicsScalar::from_fraction(1, 50);
// Below this the spring is treated as settled so a resting vehicle stops
// republishing an orientation and cannot dither forever on flat ground.
constexpr PhysicsScalar kChassisRestEpsilon =
    PhysicsScalar::from_fraction(1, 50'000);
constexpr PhysicsScalar kChassisUpwardRateDamping =
    PhysicsScalar::from_fraction(1, 2);

// Springs physics.pitch/roll towards the terrain slope under a grounded
// ground locomotor and republishes the authoritative Q32.32 orientation
// basis.  This is the fixed-point port of RefCode's
// Drawable::calcPhysicsXformTreads/Wheels/Motorcycle chassis block: the
// original ran it on the client because the Drawable owned the pitch/roll
// pair, but this codebase keeps orientation in simulation state, so the same
// arithmetic has to be deterministic. Only pitch/roll are written; XYZ and
// yaw remain owned by locomotion.
bool updateLocomotorAttitude(
    const ObjectLocomotionComponent& locomotion,
    ObjectPhysicsComponent& physics,
    const ObjectFixedTransformComponent& fixedTransform,
    const game::terrain::TerrainLogic& terrain,
    const ObjectSimulationRules& rules, bool airborne,
    bool immobile) noexcept {
    const PhysicsScalar publishedPitch = physics.pitch;
    const PhysicsScalar publishedRoll = physics.roll;
    // The latch is still last tick's value here, so it doubles as "the spring
    // already owns this object". A freshly admitted vehicle must not treat its
    // whole current speed as one frame of acceleration.
    const bool alreadySprung = physics.conformsToTerrain;
    const bool eligible = !immobile && terrain.isLoaded() &&
        locomotorConformsToTerrainSlope(locomotion);
    if (!eligible) {
        // Never leave a stale conform behind when a LocomotorSetUpgrade or a
        // containment transfer changes the appearance mid-match.
        if (!physics.conformsToTerrain) return false;
        physics.chassisPitch = kPhysicsZero;
        physics.chassisPitchRate = kPhysicsZero;
        physics.chassisRoll = kPhysicsZero;
        physics.chassisRollRate = kPhysicsZero;
        physics.chassisAccelerationPitch = kPhysicsZero;
        physics.chassisAccelerationPitchRate = kPhysicsZero;
        physics.chassisPreviousForwardSpeed = kPhysicsZero;
        physics.conformsToTerrain = false;
        physics.pitch = kPhysicsZero;
        physics.roll = kPhysicsZero;
        object_simulation_detail::rebuildPhysicsOrientation(physics);
        return publishedPitch.raw() != physics.pitch.raw() ||
               publishedRoll.raw() != physics.roll.raw();
    }

    PhysicsScalar groundPitch = kPhysicsZero;
    PhysicsScalar groundRoll = kPhysicsZero;
    if (!airborne) {
        // Reuse the established fixed-point slope sampler. It returns the
        // unit normal of the heightfield triangle under the point, which is
        // exactly what RefCode hands to the chassis spring.
        const container::Array<int64_t, 3> normal =
            terrain.map().groundNormalRaw(
                fixedTransform.position.x.raw(),
                fixedTransform.position.y.raw());
        const PhysicsScalar normalX = PhysicsScalar::from_raw(normal[0]);
        const PhysicsScalar normalY = PhysicsScalar::from_raw(normal[1]);
        const PhysicsScalar normalZ = PhysicsScalar::from_raw(normal[2]);
        // A degenerate or inverted sample (unloaded/clipped heightfield, or a
        // normal that has lost its upward component) must fall back to level
        // ground rather than tip a unit onto its side.
        if (normalZ > kPhysicsZero) {
            const math::q32_32_sincos heading =
                math::fixed_sincos(fixedTransform.yawRadians);
            // RefCode multiplies the horizontal projection of the normal by
            // PI/2 instead of taking an arc-tangent. That literal factor is
            // the retail tilt magnitude, so it is reproduced rather than
            // "corrected": deriving the true slope angle here would visibly
            // under-tilt every vehicle relative to the original game.
            //
            // The sign convention already matches this port's Euler
            // projection. RefCode applies the pair as Rotate_Y(pitch) then
            // Rotate_X(-roll), whose X column z-term is -sin(pitch) and whose
            // Y column z-term is -sin(roll); projectPhysicsOrientation reads
            // exactly those terms back, so positive pitch is nose-down and
            // positive roll is left-side-down in both.
            const PhysicsScalar forwardDot =
                normalX * heading.cosine + normalY * heading.sine;
            const PhysicsScalar lateralDot =
                normalX * -heading.sine + normalY * heading.cosine;
            groundPitch = PhysicsScalar::clamp(
                forwardDot * kMovementHalfPi,
                -kChassisAngleLimit, kChassisAngleLimit);
            groundRoll = PhysicsScalar::clamp(
                lateralDot * kMovementHalfPi,
                -kChassisAngleLimit, kChassisAngleLimit);
        }
    }

    // The authored Locomotor coefficients are stored verbatim from INI
    // (LocomotorTemplate PitchStiffness/PitchDamping/UniformAxialDamping),
    // and RefCode's recurrence is per logic frame, not per second. Scaling by
    // logicDeltaSeconds here would make the suspension roughly thirty times
    // too slow, so the step stays per confirmed tick like the original.
    const PhysicsScalar pitchStiffness = PhysicsScalar::clamp(
        locomotion.pitchStiffness, kPhysicsZero, kPhysicsOne);
    const PhysicsScalar rollStiffness = PhysicsScalar::clamp(
        locomotion.rollStiffness, kPhysicsZero, kPhysicsOne);
    const PhysicsScalar pitchDamping = PhysicsScalar::clamp(
        locomotion.pitchDamping, kPhysicsZero, kPhysicsOne);
    const PhysicsScalar rollDamping = PhysicsScalar::clamp(
        locomotion.rollDamping, kPhysicsZero, kPhysicsOne);
    const PhysicsScalar axialDamping = PhysicsScalar::clamp(
        locomotion.uniformAxialDamping, kPhysicsZero, kPhysicsOne);

    physics.chassisPitchRate = PhysicsScalar::clamp(
        physics.chassisPitchRate -
            pitchStiffness * (physics.chassisPitch - groundPitch) -
            pitchDamping * physics.chassisPitchRate,
        -kChassisAngleLimit, kChassisAngleLimit);
    // RefCode deliberately halves an upward pitch rate in all three ground
    // chassis models so a vehicle settles onto a crest instead of pitching
    // past it.
    if (physics.chassisPitchRate > kPhysicsZero)
        physics.chassisPitchRate *= kChassisUpwardRateDamping;
    physics.chassisPitch = PhysicsScalar::clamp(
        physics.chassisPitch + physics.chassisPitchRate * axialDamping,
        -kChassisAngleLimit, kChassisAngleLimit);

    physics.chassisRollRate = PhysicsScalar::clamp(
        physics.chassisRollRate -
            rollStiffness * (physics.chassisRoll - groundRoll) -
            rollDamping * physics.chassisRollRate,
        -kChassisAngleLimit, kChassisAngleLimit);
    physics.chassisRoll = PhysicsScalar::clamp(
        physics.chassisRoll + physics.chassisRollRate * axialDamping,
        -kChassisAngleLimit, kChassisAngleLimit);

    // Recoil/acceleration term, damped back towards zero independently of the
    // terrain target exactly as RefCode does. Locomotion never publishes an
    // acceleration vector into Physics, so the forward component is recovered
    // from the authoritative forward-speed delta; there is no trustworthy
    // lateral signal, so the roll half is intentionally absent rather than
    // fabricated.
    physics.chassisAccelerationPitchRate = PhysicsScalar::clamp(
        physics.chassisAccelerationPitchRate -
            pitchStiffness * physics.chassisAccelerationPitch -
            pitchDamping * physics.chassisAccelerationPitchRate,
        -kChassisAngleLimit, kChassisAngleLimit);
    physics.chassisAccelerationPitch += physics.chassisAccelerationPitchRate;
    if (alreadySprung) {
        // ForwardAccelerationPitchFactor is authored against RefCode's
        // per-logic-frame-squared acceleration, so the seconds-based speed
        // delta is converted back into that unit instead of being fed in as
        // units/s^2 (which would be ~900x too large).
        const PhysicsScalar forwardAccelerationPerFrameSq =
            (locomotion.forwardSpeed - physics.chassisPreviousForwardSpeed) *
            rules.logicDeltaSeconds;
        physics.chassisAccelerationPitchRate = PhysicsScalar::clamp(
            physics.chassisAccelerationPitchRate -
                locomotion.forwardAccelerationPitchFactor *
                    forwardAccelerationPerFrameSq,
            -kChassisAngleLimit, kChassisAngleLimit);
    }
    physics.chassisPreviousForwardSpeed = locomotion.forwardSpeed;
    const PhysicsScalar accelerationLimit = PhysicsScalar::clamp(
        PhysicsScalar::abs(locomotion.accelerationPitchLimitRadians),
        kPhysicsZero, kChassisAngleLimit);
    const PhysicsScalar decelerationLimit = PhysicsScalar::clamp(
        PhysicsScalar::abs(locomotion.decelerationPitchLimitRadians),
        kPhysicsZero, kChassisAngleLimit);
    physics.chassisAccelerationPitch = PhysicsScalar::clamp(
        physics.chassisAccelerationPitch,
        -accelerationLimit, decelerationLimit);

    const PhysicsScalar totalPitch = PhysicsScalar::clamp(
        physics.chassisPitch + physics.chassisAccelerationPitch,
        -kChassisAngleLimit, kChassisAngleLimit);
    const PhysicsScalar totalRoll = physics.chassisRoll;
    const bool settled =
        PhysicsScalar::abs(totalPitch) <= kChassisRestEpsilon &&
        PhysicsScalar::abs(totalRoll) <= kChassisRestEpsilon &&
        PhysicsScalar::abs(physics.chassisPitchRate) <= kChassisRestEpsilon &&
        PhysicsScalar::abs(physics.chassisRollRate) <= kChassisRestEpsilon;
    if (settled) {
        // Snap the accumulators to rest so a vehicle parked on level ground
        // cannot dither on the last fixed-point bit forever. The ownership
        // latch stays set: this object's attitude is still spring-owned, it
        // simply happens to be level, and clearing it would hand the pair
        // back to the Physics pass only for the spring to reclaim it next
        // tick.
        physics.chassisPitch = kPhysicsZero;
        physics.chassisPitchRate = kPhysicsZero;
        physics.chassisRoll = kPhysicsZero;
        physics.chassisRollRate = kPhysicsZero;
        physics.pitch = kPhysicsZero;
        physics.roll = kPhysicsZero;
    } else {
        physics.pitch = totalPitch;
        physics.roll = totalRoll;
    }
    physics.conformsToTerrain = true;
    // Always republish: yaw is written by locomotion every tick, so the basis
    // would otherwise freeze a moving vehicle's heading whenever the spring
    // happened to produce the same pitch/roll twice in a row. This costs the
    // same single rebuild the erased-attitude path used to perform here.
    object_simulation_detail::rebuildPhysicsOrientation(physics);
    return publishedPitch.raw() != physics.pitch.raw() ||
           publishedRoll.raw() != physics.roll.raw();
}

void snapToGoal(ObjectLocomotionComponent& locomotion,
                ObjectFixedTransformComponent& fixedTransform,
                const game::terrain::TerrainLogic& terrain,
                uint32_t pathfindLayer) noexcept {
    fixedTransform.position.x = locomotion.goal.x;
    fixedTransform.position.y = locomotion.goal.y;
    fixedTransform.position.z = terrain.isLoaded()
        ? locomotorTargetHeight(locomotion, terrain, pathfindLayer,
                                locomotion.goal.x,
                                locomotion.goal.y,
                                locomotion.goal.z)
        : locomotion.goal.z;
    locomotion.forwardSpeed = kPhysicsZero;
    locomotion.verticalSpeed = kPhysicsZero;
    locomotion.movingBackward = false;
}

void completeMove(ObjectLocomotionComponent& locomotion, ObjectOrderQueueComponent& queue,
                  ObjectFixedTransformComponent& fixedTransform,
                  const game::terrain::TerrainLogic& terrain,
                  uint32_t pathfindLayer,
                  ObjectId id, uint64_t tick, container::Vector<ObjectMovementEvent>& events) {
    snapToGoal(locomotion, fixedTransform, terrain, pathfindLayer);
    const uint8_t orderSource = queue.orders.empty()
        ? 0xff
        : static_cast<uint8_t>(queue.orders.front().source);
    const uint8_t systemPurpose = queue.orders.empty()
        ? 0xff
        : static_cast<uint8_t>(queue.orders.front().systemPurpose);
    // Script Enter/Garrison has a second authoritative completion phase in
    // GameSession: after locomotion, geometry/capacity/relationship are
    // revalidated and only ObjectContainmentSystem may attach the object.
    // Retain that one completed head until the resolver consumes it; popping
    // here would make an already-close actor lose its Enter intent before the
    // same-frame containment transaction can observe arrival.
    const bool retainForSessionTransaction = !queue.orders.empty() &&
        queue.orders.front().source == ObjectOrderSource::System &&
        (queue.orders.front().systemPurpose ==
             ObjectOrderSystemPurpose::ContainmentEnter ||
         queue.orders.front().systemPurpose ==
             ObjectOrderSystemPurpose::ScenarioReinforcementDeliver ||
         queue.orders.front().systemPurpose ==
             ObjectOrderSystemPurpose::ScenarioReinforcementExit ||
         queue.orders.front().systemPurpose ==
             ObjectOrderSystemPurpose::IntentionalContact);
    if (!retainForSessionTransaction) {
        if (!queue.orders.empty()) queue.orders.erase(queue.orders.begin());
        ++queue.revision;
    }
    events.push_back({
        .kind = ObjectMovementEventKind::Completed,
        .object = id,
        .targetX = locomotion.goal.x,
        .targetY = locomotion.goal.y,
        .targetZ = locomotion.goal.z,
        .confirmedTick = tick,
        .orderSource = orderSource,
        .systemPurpose = systemPurpose,
    });
    locomotion.hasActiveMove = false;
    locomotion.state = ObjectLocomotionState::Idle;
}

enum class GroundMoveStepResult : uint8_t {
    Moving,
    Blocked,
    Completed,
};

GroundMoveStepResult updateGroundMove(
                      ObjectLocomotionComponent& locomotion,
                      ObjectFixedTransformComponent& fixedTransform,
                      TransformComponent& transform,
                      const ObjectHealthComponent* health,
                      ObjectOrderQueueComponent* queue,
                      const ObjectOrderIntent* order, ObjectId id,
                      bool validatedNavigationSegment,
                      bool stopAtGoal,
                      const game::terrain::TerrainLogic& terrain,
                      uint32_t pathfindLayer,
                      const ObjectSimulationRules& rules,
                      PhysicsScalar speedLimit,
                      PhysicsScalar pathExtraDistance,
                      uint64_t tick,
                      container::Vector<ObjectMovementEvent>* events) {
    ensureFixedMovementState(fixedTransform, locomotion, transform);
    const PhysicsScalar deltaFixed = rules.logicDeltaSeconds;
    if (order && !sameActiveOrder(locomotion, *order)) {
        locomotion.goal = {
            order->targetX, order->targetY, order->targetZ};
        locomotion.activeOrderTick = order->issuedTick;
        locomotion.activeOrderSequence = order->sourceSequence;
        locomotion.activeSourceScriptId = order->sourceScriptId;
        locomotion.hasActiveMove = true;
        locomotion.movingBackward = false;
        locomotion.state = ObjectLocomotionState::Moving;
        if (events) events->push_back({
            .kind = ObjectMovementEventKind::Started,
            .object = id,
            .targetX = locomotion.goal.x,
            .targetY = locomotion.goal.y,
            .targetZ = locomotion.goal.z,
            .confirmedTick = tick,
            .orderSource = static_cast<uint8_t>(order->source),
            .systemPurpose = static_cast<uint8_t>(order->systemPurpose),
            .damagedAtStart = health &&
                health->damageState != ObjectBodyDamageState::Pristine,
        });
    }

    PhysicsScalar positionX = fixedTransform.position.x;
    PhysicsScalar positionY = fixedTransform.position.y;
    PhysicsScalar positionZ = fixedTransform.position.z;
    const PhysicsScalar goalX = locomotion.goal.x;
    const PhysicsScalar goalY = locomotion.goal.y;
    const PhysicsScalar goalZ = locomotion.goal.z;
    const PhysicsScalar initialDx = goalX - positionX;
    const PhysicsScalar initialDy = goalY - positionY;
    const PhysicsScalar initialHorizontalDistance =
        length2D(initialDx, initialDy);
    const PhysicsScalar initialVerticalDistance = goalZ - positionZ;
    const bool threeDimensionalArrival = locomotion.usePreciseZPosition ||
        locomotion.closeEnoughDistance3D ||
        locomotion.appearance == game::LocomotorAppearance::Thrust;
    const PhysicsScalar initialDistance = threeDimensionalArrival
        ? length2D(initialHorizontalDistance, initialVerticalDistance)
        : initialHorizontalDistance;
    const PhysicsScalar closeEnough = PhysicsScalar::max(
        kPhysicsZero, locomotion.closeEnough);
    if (initialDistance <= closeEnough + kMovementArrivalEpsilonFixed) {
        if (!stopAtGoal) {
            fixedTransform.position.x = locomotion.goal.x;
            fixedTransform.position.y = locomotion.goal.y;
            fixedTransform.position.z = terrain.isLoaded()
                ? locomotorTargetHeight(
                      locomotion, terrain, pathfindLayer,
                      locomotion.goal.x, locomotion.goal.y,
                      locomotion.goal.z)
                : locomotion.goal.z;
            locomotion.hasActiveMove = true;
            locomotion.state = ObjectLocomotionState::Moving;
        } else if (queue && events)
            completeMove(locomotion, *queue, fixedTransform, terrain,
                         pathfindLayer, id, tick, *events);
        else {
            snapToGoal(locomotion, fixedTransform, terrain, pathfindLayer);
            locomotion.hasActiveMove = false;
            locomotion.state = ObjectLocomotionState::Idle;
        }
        projectFixedMovementState(fixedTransform, locomotion, transform);
        return GroundMoveStepResult::Completed;
    }
    // A live Navigation PathHandle has already validated this segment for the
    // locomotor surface mask and is rejected when its topology revision goes
    // stale. The straight-line scan below is only the no-path fallback; using
    // it for a smoothed path can contradict legal cliff-border routing.
    if (!validatedNavigationSegment &&
        !isPathTraversable(locomotion, terrain, fixedTransform)) {
        const bool newlyBlocked = locomotion.state != ObjectLocomotionState::Blocked;
        locomotion.forwardSpeed = moveTowardsFixed(
            locomotion.forwardSpeed, kPhysicsZero,
            PhysicsScalar::abs(locomotion.braking) * deltaFixed);
        locomotion.state = ObjectLocomotionState::Blocked;
        if (newlyBlocked && events) {
            events->push_back({
                .kind = ObjectMovementEventKind::Blocked,
                .object = id,
                .targetX = locomotion.goal.x,
                .targetY = locomotion.goal.y,
                .targetZ = locomotion.goal.z,
                .confirmedTick = tick,
                .orderSource = order
                    ? static_cast<uint8_t>(order->source)
                    : uint8_t{0xff},
                .systemPurpose = order
                    ? static_cast<uint8_t>(order->systemPurpose)
                    : uint8_t{0xff},
            });
        }
        projectFixedMovementState(fixedTransform, locomotion, transform);
        return GroundMoveStepResult::Blocked;
    }
    if (locomotion.downhillOnly && terrain.isLoaded() &&
        locomotorSurfaceHeight(
            terrain, pathfindLayer, locomotion.goal.x,
            locomotion.goal.y) >
            fixedTransform.position.z + kMovementArrivalEpsilonFixed) {
        locomotion.forwardSpeed = moveTowardsFixed(
            locomotion.forwardSpeed, kPhysicsZero,
            PhysicsScalar::abs(locomotion.braking) * deltaFixed);
        locomotion.state = ObjectLocomotionState::Blocked;
        projectFixedMovementState(fixedTransform, locomotion, transform);
        return GroundMoveStepResult::Blocked;
    }

    const bool damaged = health && isMovementPenaltyState(health->damageState, rules);
    PhysicsScalar maxSpeed = damaged
        ? locomotion.damagedMaximumSpeed : locomotion.maximumSpeed;
    // AS_TEAM waypoint states freeze AIGroup::getSpeed() when the state
    // enters.  Zero retains FAST_AS_POSSIBLE semantics for every other move.
    // A damaged member may still run below the frozen group limit.
    if (speedLimit > kPhysicsZero) {
        maxSpeed = PhysicsScalar::min(maxSpeed, speedLimit);
    }
    const PhysicsScalar baseMaxTurnRate = damaged
        ? locomotion.damagedMaximumTurnRate : locomotion.maximumTurnRate;
    const PhysicsScalar maxTurnRate = locomotion.ultraAccurate
        ? baseMaxTurnRate * kPhysicsTwo : baseMaxTurnRate;
    const PhysicsScalar acceleration = damaged
        ? locomotion.damagedAcceleration : locomotion.acceleration;
    const PhysicsScalar toTargetAngle = math::fixed_atan2(
        initialDy, initialDx);
    const PhysicsScalar currentRotation = fixedTransform.yawRadians;

    const PhysicsScalar slideWindow = PhysicsScalar::max(
        kPhysicsZero, maxSpeed) * PhysicsScalar::max(
            kPhysicsZero,
            locomotion.slideIntoPlace) /
        PhysicsScalar{int32_t{1000}};
    const bool ultraAccurateSlide = locomotion.ultraAccurate &&
        PhysicsScalar::abs(initialDx) <= slideWindow &&
        PhysicsScalar::abs(initialDy) <= slideWindow;

    PhysicsScalar desiredHeading = ultraAccurateSlide
        ? currentRotation : toTargetAngle;
    locomotion.movingBackward = false;
    const bool wheelSteering =
        locomotion.appearance == game::LocomotorAppearance::FourWheels ||
        locomotion.appearance == game::LocomotorAppearance::Motorcycle;
    if (wheelSteering &&
        locomotion.canMoveBackwards &&
        PhysicsScalar::abs(normalizeMovementAngle(
            toTargetAngle - currentRotation)) > kMovementHalfPi) {
        desiredHeading = normalizeMovementAngle(
            toTargetAngle + kMovementPi);
        locomotion.movingBackward = true;
    }
    const PhysicsScalar headingError = normalizeMovementAngle(
        desiredHeading - currentRotation);

    PhysicsScalar yawScale = kPhysicsOne;
    PhysicsScalar desiredSpeed = maxSpeed;
    if (locomotion.appearance == game::LocomotorAppearance::Wings ||
        locomotion.appearance == game::LocomotorAppearance::Thrust) {
        desiredSpeed = PhysicsScalar::max(
            desiredSpeed,
            locomotion.minimumSpeed);
    }
    if (!wheelSteering) {
        // Treads, infantry, hover-like ground movers and generic helpers can
        // turn at rest.  Slow linearly to a stop over a 45-degree turn so the
        // shared integrator can pivot without the wheel-only speed gate.
        const PhysicsScalar turnPenalty = PhysicsScalar::min(
            PhysicsScalar::abs(headingError) / kMovementQuarterPi,
            kPhysicsOne);
        desiredSpeed *= kPhysicsOne - turnPenalty;
    } else {
        // Wheels require forward motion to turn. The source uses MinTurnSpeed
        // and an angle-dependent limit; use the same quantities in seconds
        // units and permit a small initial steering factor to avoid a zero-
        // speed deadlock in this transform-authoritative implementation.
        const PhysicsScalar turnSpeed = PhysicsScalar::max(
            locomotion.minimumTurnSpeed,
            maxSpeed * PhysicsScalar::from_fraction(1, 4));
        if (turnSpeed > kPhysicsZero) {
            yawScale = PhysicsScalar::max(
                PhysicsScalar::from_fraction(3, 20),
                PhysicsScalar::min(PhysicsScalar::abs(
                    locomotion.forwardSpeed) / turnSpeed,
                    kPhysicsOne));
            if (PhysicsScalar::abs(headingError) >
                kMovementPi / PhysicsScalar{int32_t{20}}) {
                const PhysicsScalar factor = PhysicsScalar::min(
                    PhysicsScalar::abs(headingError) / kMovementHalfPi,
                    PhysicsScalar::from_fraction(17, 20));
                desiredSpeed = PhysicsScalar::max(
                    turnSpeed * PhysicsScalar::from_fraction(1, 2),
                    maxSpeed * (kPhysicsOne - factor));
            }
        }
    }

    const PhysicsScalar braking = locomotion.braking;
    const PhysicsScalar currentSpeed = locomotion.forwardSpeed;
    const PhysicsScalar stoppingDistance =
        locomotion.hasFiniteBraking && braking > kPhysicsZero
        ? (currentSpeed * currentSpeed) / (kPhysicsTwo * braking)
        : kPhysicsZero;
    const PhysicsScalar remaining = PhysicsScalar::max(
            kPhysicsZero, initialDistance - closeEnough) +
        PhysicsScalar::max(kPhysicsZero, pathExtraDistance);
    if (stopAtGoal && locomotion.hasFiniteBraking &&
        braking > kPhysicsZero) {
        desiredSpeed = PhysicsScalar::min(desiredSpeed,
            PhysicsScalar::sqrt(kPhysicsTwo * braking * remaining));
    }
    if (locomotion.appearance == game::LocomotorAppearance::Wings ||
        locomotion.appearance == game::LocomotorAppearance::Thrust) {
        desiredSpeed = PhysicsScalar::max(
            desiredSpeed,
            locomotion.minimumSpeed);
    }
    static_cast<void>(stoppingDistance); // retained for debugger/profiling parity with source formula.

    const PhysicsScalar targetSignedSpeed = locomotion.movingBackward
        ? -desiredSpeed : desiredSpeed;
    PhysicsScalar speedTarget = targetSignedSpeed;
    if (currentSpeed * targetSignedSpeed < kPhysicsZero) {
        // A wheel/tread must brake through zero before reversing; this avoids
        // an impossible instantaneous velocity flip from an order change.
        speedTarget = kPhysicsZero;
    }
    const bool accelerating = PhysicsScalar::abs(speedTarget) >
            PhysicsScalar::abs(currentSpeed) &&
        currentSpeed * speedTarget >= kPhysicsZero;
    const bool infiniteRate = accelerating
        ? (damaged ? locomotion.damagedAccelerationIsInfinite
                   : locomotion.accelerationIsInfinite)
        : locomotion.brakingIsInfinite;
    // Confirmed simulation always advances by the session-frozen fixed step.
    const PhysicsScalar nextSpeed = infiniteRate
        ? speedTarget
        : moveTowardsFixed(currentSpeed, speedTarget,
              (accelerating ? acceleration : braking) * deltaFixed);
    locomotion.forwardSpeed = nextSpeed;

    const PhysicsScalar yawStep = PhysicsScalar::max(
        kPhysicsZero, maxTurnRate) * yawScale * deltaFixed;
    const PhysicsScalar nextRotation = normalizeMovementAngle(
        currentRotation + PhysicsScalar::clamp(
            headingError, -yawStep, yawStep));
    fixedTransform.yawRadians = nextRotation;

    const PhysicsScalar travel = PhysicsScalar::min(
        PhysicsScalar::abs(nextSpeed) * deltaFixed,
        initialDistance);
    const PhysicsScalar horizontalTravel = locomotion.usePreciseZPosition &&
            initialDistance > kMovementArrivalEpsilonFixed
        ? travel * initialHorizontalDistance / initialDistance
        : travel;
    const PhysicsScalar direction = nextSpeed >= kPhysicsZero
        ? kPhysicsOne : -kPhysicsOne;
    const PhysicsScalar travelHeading = ultraAccurateSlide
        ? toTargetAngle : nextRotation;
    const math::q32_32_sincos heading = math::fixed_sincos(travelHeading);
    positionX += heading.cosine * horizontalTravel * direction;
    positionY += heading.sine * horizontalTravel * direction;
    fixedTransform.position.x = positionX;
    fixedTransform.position.y = positionY;
    if (locomotion.appearance == game::LocomotorAppearance::Thrust &&
        threeDimensionalArrival) {
        const PhysicsScalar fraction =
            initialDistance > kMovementArrivalEpsilonFixed
            ? PhysicsScalar::clamp(
                  travel / initialDistance, kPhysicsZero, kPhysicsOne)
            : kPhysicsOne;
        positionZ += initialVerticalDistance * fraction;
        fixedTransform.position.z = positionZ;
        locomotion.verticalSpeed =
            deltaFixed > kPhysicsZero
            ? initialVerticalDistance * fraction / deltaFixed
            : kPhysicsZero;
    } else {
        updateLocomotorVertical(
            locomotion, fixedTransform, health, terrain, pathfindLayer, rules,
            locomotion.goal.z);
    }
    projectFixedMovementState(fixedTransform, locomotion, transform);
    locomotion.state = ObjectLocomotionState::Moving;

    const PhysicsScalar newDx = goalX - fixedTransform.position.x;
    const PhysicsScalar newDy = goalY - fixedTransform.position.y;
    const PhysicsScalar newHorizontalDistance = length2D(newDx, newDy);
    const PhysicsScalar newDistance = threeDimensionalArrival
        ? length2D(newHorizontalDistance,
              goalZ - fixedTransform.position.z)
        : newHorizontalDistance;
    if (newDistance <= closeEnough + kMovementArrivalEpsilonFixed) {
        if (!stopAtGoal) {
            fixedTransform.position.x = locomotion.goal.x;
            fixedTransform.position.y = locomotion.goal.y;
            fixedTransform.position.z = terrain.isLoaded()
                ? locomotorTargetHeight(
                      locomotion, terrain, pathfindLayer,
                      locomotion.goal.x, locomotion.goal.y,
                      locomotion.goal.z)
                : locomotion.goal.z;
            locomotion.hasActiveMove = true;
            locomotion.state = ObjectLocomotionState::Moving;
        } else if (queue && events)
            completeMove(locomotion, *queue, fixedTransform, terrain,
                          pathfindLayer, id, tick, *events);
        else {
            snapToGoal(locomotion, fixedTransform, terrain, pathfindLayer);
            locomotion.hasActiveMove = false;
            locomotion.state = ObjectLocomotionState::Idle;
        }
        projectFixedMovementState(fixedTransform, locomotion, transform);
        return GroundMoveStepResult::Completed;
    }
    projectFixedMovementState(fixedTransform, locomotion, transform);
    return GroundMoveStepResult::Moving;
}

[[nodiscard]] ai::AIAsyncOrderIdentity movementOrderIdentity(
    ObjectId subject, const ObjectOrderQueueComponent& queue,
    const ObjectOrderIntent& order,
    const ObjectSystemPathSequenceComponent* systemPath) noexcept {
    uint64_t queueRevision = queue.revision;
    uint64_t externalRevision = queue.externalRevision;
    // Player privateFollowPathAppend extends the active goal vector without
    // replacing the installed locomotion path. Its queue revisions advance
    // for deterministic command/history observation, while the active
    // movement correlation deliberately retains the FollowPath admission
    // identity. Comparing that correlation to the mutable tail revision made
    // the third Shift point release the path and brake the unit immediately.
    if (systemPath && systemPath->activeQueueRevision != 0 &&
        systemPath->routeSubtype == ObjectMoveRouteSubtype::FollowPath &&
        systemPath->source == ObjectOrderSource::Player &&
        systemPath->systemPurpose == ObjectOrderSystemPurpose::Generic &&
        order.kind == ObjectOrderKind::Move &&
        order.source == systemPath->source &&
        order.systemPurpose == systemPath->systemPurpose &&
        order.moveRouteSubtype == systemPath->routeSubtype &&
        order.issuedTick == systemPath->issuedTick &&
        order.sourceSequence == systemPath->firstSourceSequence) {
        queueRevision = systemPath->activeQueueRevision;
        externalRevision = systemPath->activeExternalRevision;
    }
    return {
        .subject = subject,
        .queueRevision = queueRevision,
        .externalRevision = externalRevision,
        .issuedTick = order.issuedTick,
        .sourceSequence = order.sourceSequence,
        .sourceScriptId = order.sourceScriptId,
        .systemPurposeInstance = order.systemPurposeInstance,
        .source = static_cast<uint8_t>(order.source),
        .systemPurpose = static_cast<uint8_t>(order.systemPurpose),
    };
}

[[nodiscard]] bool matchesAIMovementOrder(
    const ai::PathCorrelation& correlation, ObjectId subject,
    const ObjectOrderQueueComponent* queue,
    const ObjectSystemPathSequenceComponent* systemPath) noexcept {
    if (!queue || queue->orders.empty())
        return false;
    const ObjectOrderIntent& order = queue->orders.front();
    const bool movementOrder = order.kind == ObjectOrderKind::Move;
    if (!movementOrder && !isCombatDirectAttackOrder(&order) &&
        !isCombatTacticalAttackOrder(&order) &&
        !isCombatDropMovementOrder(&order))
        return false;
    return correlation.orderIdentity == movementOrderIdentity(
        subject, *queue, order, systemPath);
}

void releaseAIMovementPath(navigation::NavigationSystem* navigation,
                           const ObjectAIPathMovementComponent& movement) noexcept {
    if (navigation && movement.path && movement.pathRevision != 0) {
        static_cast<void>(navigation->releasePath(
            movement.path, navigation::NavigationRevision{movement.pathRevision}));
    }
}

void releaseAIMovementCommandPath(
    navigation::NavigationSystem* navigation,
    const ObjectAIMovementCommand& value) noexcept {
    if (navigation && value.command.kind == ai::MovementCommandKind::InstallPath &&
        value.command.path && value.pathRevision != 0) {
        static_cast<void>(navigation->releasePath(
            value.command.path,
            navigation::NavigationRevision{value.pathRevision}));
    }
}

void appendAIMovementFeedback(
    container::Vector<ai::MovementFeedback>& feedback,
    const ai::PathCorrelation& correlation,
    ai::MovementFeedbackStatus status, uint64_t tick,
    uint32_t blockedTicks, int64_t alongPathDistanceRaw,
    int64_t finalNodeXYDistanceRaw) {
    feedback.push_back({
        .correlation = correlation,
        .status = status,
        .confirmedTick = tick,
        .blockedTicks = blockedTicks,
        .alongPathDistanceRaw = alongPathDistanceRaw,
        .finalNodeXYDistanceRaw = finalNodeXYDistanceRaw,
    });
}

struct PathFollowTarget final {
    navigation::PathRepositoryPoint point;
    uint32_t pointIndex = 0;
    bool finalPoint = true;
    bool validatedNavigationSegment = false;
    bool advancePointOnCompletion = true;
    bool valid = false;
};

// RefCode Path::computePointOnPath() follows the segment nearest the actor,
// rather than restarting at node zero whenever a replacement path arrives.
// The replacement can be a few confirmed ticks old because the actor keeps
// moving on its immutable previous route while Patch is solved. Starting the
// new route at its authored start in that case makes the actor turn backwards
// and then forwards again. Project onto a forward segment and target its end;
// subsequent ticks never search behind the retained segment cursor.
[[nodiscard]] PathFollowTarget selectPathFollowTarget(
    const navigation::PathRepository& repository,
    const navigation::NavigationLayerSet& layers,
    ai::PathHandle path, navigation::NavigationRevision revision,
    uint32_t retainedPointIndex,
    const ObjectFixedTransformComponent& transform,
    navigation::NavigationLayerId currentLayer,
    navigation::NavigationMovementMask movementMask,
    navigation::NavigationClearanceClass clearance) noexcept {
    const navigation::PathRepositoryPointSpans points =
        repository.pointSpans(path, revision);
    if (points.status != navigation::PathRepositoryStatus::Success ||
        points.xRaw.empty() || points.xRaw.size() != points.yRaw.size() ||
        points.xRaw.size() != points.zRaw.size() ||
        points.xRaw.size() != points.layer.size()) {
        return {};
    }
    if (points.xRaw.size() == 1u) {
        const navigation::NavigationWorldPosition current{
            transform.position.x.raw(), transform.position.y.raw(),
            transform.position.z.raw()};
        const navigation::NavigationWorldPosition target{
            points.xRaw[0], points.yRaw[0], points.zRaw[0]};
        const bool validated = points.layer[0] != currentLayer ||
            navigation::navigationLinePassable(
                layers, current, target, movementMask, currentLayer,
                clearance);
        return {
            .point = {{points.xRaw[0], points.yRaw[0], points.zRaw[0]},
                      points.layer[0]},
            .pointIndex = 0,
            .finalPoint = true,
            .validatedNavigationSegment = validated,
            .valid = true,
        };
    }

    const uint32_t lastIndex = static_cast<uint32_t>(points.xRaw.size() - 1u);
    const uint32_t firstSegment = retainedPointIndex > 0u
        ? std::min(retainedPointIndex - 1u, lastIndex - 1u) : 0u;
    uint32_t bestSegment = firstSegment;
    int64_t bestDistanceSquaredRaw = std::numeric_limits<int64_t>::max();
    PhysicsScalar bestFactor = kPhysicsZero;
    bool foundCurrentLayer = false;
    const auto inspect = [&](bool requireCurrentLayer) {
        for (uint32_t segment = firstSegment; segment < lastIndex; ++segment) {
            if (requireCurrentLayer &&
                (points.layer[segment] != currentLayer ||
                 points.layer[segment + 1u] != currentLayer)) {
                continue;
            }
            const PhysicsScalar startX =
                PhysicsScalar::from_raw(points.xRaw[segment]);
            const PhysicsScalar startY =
                PhysicsScalar::from_raw(points.yRaw[segment]);
            const PhysicsScalar deltaX =
                PhysicsScalar::from_raw(points.xRaw[segment + 1u]) - startX;
            const PhysicsScalar deltaY =
                PhysicsScalar::from_raw(points.yRaw[segment + 1u]) - startY;
            const PhysicsScalar fromStartX = transform.position.x - startX;
            const PhysicsScalar fromStartY = transform.position.y - startY;
            const PhysicsScalar lengthSquared =
                deltaX * deltaX + deltaY * deltaY;
            PhysicsScalar factor = kPhysicsZero;
            if (lengthSquared > kMovementArrivalEpsilonFixed) {
                factor = PhysicsScalar::clamp(
                    (fromStartX * deltaX + fromStartY * deltaY) /
                        lengthSquared,
                    kPhysicsZero, kPhysicsOne);
            }
            const PhysicsScalar offsetX =
                transform.position.x - (startX + deltaX * factor);
            const PhysicsScalar offsetY =
                transform.position.y - (startY + deltaY * factor);
            const int64_t distanceSquaredRaw =
                (offsetX * offsetX + offsetY * offsetY).raw();
            // A tie at a shared vertex must choose the later segment, or the
            // actor can alternate between both sides of that vertex.
            if (distanceSquaredRaw <= bestDistanceSquaredRaw) {
                bestDistanceSquaredRaw = distanceSquaredRaw;
                bestSegment = segment;
                bestFactor = factor;
                foundCurrentLayer = true;
            }
        }
    };
    inspect(true);
    if (!foundCurrentLayer) {
        bestDistanceSquaredRaw = std::numeric_limits<int64_t>::max();
        inspect(false);
    }
    const uint32_t targetIndex = std::min(bestSegment + 1u, lastIndex);
    const navigation::PathRepositoryPoint endpoint{
        {points.xRaw[targetIndex], points.yRaw[targetIndex],
         points.zRaw[targetIndex]},
        points.layer[targetIndex]};
    if (points.layer[bestSegment] != currentLayer ||
        endpoint.layer != currentLayer) {
        return {
            .point = endpoint,
            .pointIndex = targetIndex,
            .finalPoint = targetIndex == lastIndex,
            .validatedNavigationSegment = true,
            .valid = true,
        };
    }

    const navigation::NavigationWorldPosition current{
        transform.position.x.raw(), transform.position.y.raw(),
        transform.position.z.raw()};
    if (navigation::navigationLinePassable(
            layers, current, endpoint.position, movementMask, currentLayer,
            clearance)) {
        return {
            .point = endpoint,
            .pointIndex = targetIndex,
            .finalPoint = targetIndex == lastIndex,
            .validatedNavigationSegment = true,
            .valid = true,
        };
    }

    const PhysicsScalar startX =
        PhysicsScalar::from_raw(points.xRaw[bestSegment]);
    const PhysicsScalar startY =
        PhysicsScalar::from_raw(points.yRaw[bestSegment]);
    const PhysicsScalar startZ =
        PhysicsScalar::from_raw(points.zRaw[bestSegment]);
    const PhysicsScalar endX = PhysicsScalar::from_raw(endpoint.position.xRaw);
    const PhysicsScalar endY = PhysicsScalar::from_raw(endpoint.position.yRaw);
    const PhysicsScalar endZ = PhysicsScalar::from_raw(endpoint.position.zRaw);
    const PhysicsScalar projectedX = startX + (endX - startX) * bestFactor;
    const PhysicsScalar projectedY = startY + (endY - startY) * bestFactor;
    const PhysicsScalar projectedZ = startZ + (endZ - startZ) * bestFactor;
    const navigation::NavigationGrid* grid = layers.find(currentLayer);
    if (grid == nullptr)
        return {};
    const PhysicsScalar maxPathError =
        PhysicsScalar::from_raw(grid->transform().cellSizeRaw) *
        PhysicsScalar{int32_t{3}};
    const PhysicsScalar offsetDistance = PhysicsScalar::sqrt(
        PhysicsScalar::from_raw(bestDistanceSquaredRaw));
    const PhysicsScalar blend = maxPathError > kPhysicsZero
        ? kPhysicsOne - PhysicsScalar::clamp(
              offsetDistance / maxPathError, kPhysicsZero, kPhysicsOne)
        : kPhysicsZero;
    const PhysicsScalar reconnectX =
        projectedX + (endX - projectedX) * blend;
    const PhysicsScalar reconnectY =
        projectedY + (endY - projectedY) * blend;
    const PhysicsScalar reconnectZ =
        projectedZ + (endZ - projectedZ) * blend;
    const navigation::NavigationWorldPosition reconnect{
        reconnectX.raw(), reconnectY.raw(), reconnectZ.raw()};
    const PhysicsScalar reconnectDistance = length2D(
        reconnectX - transform.position.x,
        reconnectY - transform.position.y);
    if (reconnectDistance <= kMovementArrivalEpsilonFixed) {
        // A Team path may begin under a large actor whose authored map pose
        // does not satisfy the shared grid's clearance phase. RefCode does
        // not reject the Path in this case: computePointOnPath falls back to
        // the next point and lets Locomotor/collision authority move the
        // object out of its occupied start. Preserve that fallback without
        // falsely labelling the segment navigation-validated.
        return {
            .point = endpoint,
            .pointIndex = targetIndex,
            .finalPoint = targetIndex == lastIndex,
            .validatedNavigationSegment = false,
            .valid = true,
        };
    }
    const bool reconnectValidated = navigation::navigationLinePassable(
        layers, current, reconnect, movementMask, currentLayer, clearance);
    return {
        .point = {reconnect, currentLayer},
        .pointIndex = targetIndex,
        .finalPoint = false,
        .validatedNavigationSegment = reconnectValidated,
        .advancePointOnCompletion = false,
        .valid = true,
    };
}

void updateAIMovement(
    ecs::registry& registry, ecs::entity entity, ObjectId subject,
    ObjectLocomotionComponent& locomotion,
    ObjectFixedTransformComponent& fixedTransform,
    TransformComponent& transform,
    const ObjectHealthComponent* health,
    const ObjectOrderQueueComponent* queue,
    const ObjectAIMovementCommand* input,
    navigation::NavigationSystem* navigation,
    const game::terrain::TerrainLogic& terrain,
    const ObjectSimulationRules& rules, uint64_t tick,
    container::Vector<ai::MovementFeedback>& feedback) {
    ObjectAIPathMovementComponent* active =
        ecs::try_get<ObjectAIPathMovementComponent>(registry, entity);
    const ObjectSystemPathSequenceComponent* systemPath =
        ecs::try_get<ObjectSystemPathSequenceComponent>(registry, entity);
    const bool inputMatchesOrder = input &&
        matchesAIMovementOrder(
            input->command.correlation, subject, queue, systemPath);
    const bool rebindExistingPath = input &&
        input->command.kind == ai::MovementCommandKind::RebindExistingPath;

    if (active && !matchesAIMovementOrder(
                      active->correlation, subject, queue, systemPath)) {
        // MoveOutOfTheWay temporarily takes ownership of the path which the
        // interrupted Move was already following. The source engine keeps the
        // same Path object here; releasing it before the temporary state can
        // bind it is what made allies stop/repath instead of yielding.
        if (inputMatchesOrder && rebindExistingPath) {
            active->correlation = input->command.correlation;
            active->ignoredObstacle = input->command.ignoredObstacle;
            active->speedLimitRaw = input->command.speedLimitRaw;
            active->allowPathThroughUnits =
                input->command.allowPathThroughUnits;
        } else {
            const ai::PathCorrelation cancelled = active->correlation;
            releaseAIMovementPath(navigation, *active);
            ecs::remove<ObjectAIPathMovementComponent>(registry, entity);
            active = nullptr;
            if (!inputMatchesOrder) {
                brakeFixedMovement(locomotion, rules.logicDeltaSeconds);
                locomotion.hasActiveMove = false;
                locomotion.movingBackward = false;
                locomotion.state = ObjectLocomotionState::Idle;
                appendAIMovementFeedback(
                    feedback, cancelled, ai::MovementFeedbackStatus::Cancelled,
                    tick, 0, 0, 0);
                if (input)
                    releaseAIMovementCommandPath(navigation, *input);
                return;
            }
        }
    }

    bool installed = false;
    bool rebound = false;
    if (input) {
        if (!inputMatchesOrder) {
            releaseAIMovementCommandPath(navigation, *input);
            appendAIMovementFeedback(
                feedback, input->command.correlation,
                ai::MovementFeedbackStatus::Unsupported,
                tick, 0, 0, 0);
            return;
        }
        if (input->command.kind == ai::MovementCommandKind::EndMovement) {
            if (active) {
                releaseAIMovementPath(navigation, *active);
                ecs::remove<ObjectAIPathMovementComponent>(registry, entity);
                active = nullptr;
            }
            brakeFixedMovement(locomotion, rules.logicDeltaSeconds);
            locomotion.hasActiveMove = false;
            locomotion.movingBackward = false;
            locomotion.state = ObjectLocomotionState::Idle;
            appendAIMovementFeedback(
                feedback, input->command.correlation,
                ai::MovementFeedbackStatus::Cancelled,
                tick, 0, 0, 0);
            return;
        }
        if (rebindExistingPath) {
            if (!active || !active->path || active->pathRevision == 0) {
                appendAIMovementFeedback(
                    feedback, input->command.correlation,
                    ai::MovementFeedbackStatus::Unsupported,
                    tick, 0, 0, 0);
                return;
            }
            active->correlation = input->command.correlation;
            active->ignoredObstacle = input->command.ignoredObstacle;
            active->speedLimitRaw = input->command.speedLimitRaw;
            active->mode = input->mode;
            active->panicking = input->panicking;
            active->allowPathThroughUnits =
                input->command.allowPathThroughUnits;
            rebound = true;
        } else {
            if (!navigation || !input->command.path || input->pathRevision == 0 ||
                navigation->isPathStale(
                    input->command.path,
                    navigation::NavigationRevision{input->pathRevision})) {
                releaseAIMovementCommandPath(navigation, *input);
                appendAIMovementFeedback(
                    feedback, input->command.correlation,
                    navigation && input->command.path && input->pathRevision != 0
                        ? ai::MovementFeedbackStatus::Stuck
                        : ai::MovementFeedbackStatus::Unsupported,
                    tick, 0, 0, 0);
                return;
            }
            const navigation::PathRepositoryPointQuery first =
                navigation->pathRepository().queryPoint(
                    input->command.path,
                    navigation::NavigationRevision{input->pathRevision}, 0);
            if (first.status != navigation::PathRepositoryStatus::Success) {
                releaseAIMovementCommandPath(navigation, *input);
                appendAIMovementFeedback(
                    feedback, input->command.correlation,
                    ai::MovementFeedbackStatus::Unsupported,
                    tick, 0, 0, 0);
                return;
            }
            if (active && (active->correlation != input->command.correlation ||
                           active->path != input->command.path ||
                           active->pathRevision != input->pathRevision)) {
                releaseAIMovementPath(navigation, *active);
                ecs::remove<ObjectAIPathMovementComponent>(registry, entity);
                active = nullptr;
            }
            if (!active) {
                active = &ecs::emplace<ObjectAIPathMovementComponent>(
                    registry, entity, ObjectAIPathMovementComponent{
                        .correlation = input->command.correlation,
                        .path = input->command.path,
                        .ignoredObstacle = input->command.ignoredObstacle,
                        .pathRevision = input->pathRevision,
                        .speedLimitRaw = input->command.speedLimitRaw,
                        .extraDistanceRaw =
                            input->command.extraDistanceRaw,
                        .mode = input->mode,
                        .panicking = input->panicking,
                        .allowPathThroughUnits =
                            input->command.allowPathThroughUnits,
                    });
                installed = true;
            } else {
                active->mode = input->mode;
                active->panicking = input->panicking;
                active->ignoredObstacle = input->command.ignoredObstacle;
                active->speedLimitRaw = input->command.speedLimitRaw;
                active->extraDistanceRaw =
                    input->command.extraDistanceRaw;
                active->allowPathThroughUnits =
                    input->command.allowPathThroughUnits;
            }
        }
    }

    if ((installed || rebound) && queue && !queue->orders.empty() &&
        queue->orders.front().systemPurpose ==
            ObjectOrderSystemPurpose::ConstructionEvacuation &&
        input && input->command.ignoredObstacle) {
        // Construction placement may begin with the mover overlapping the new
        // footprint, but that overlap is not proof that a route exists. Enable
        // the temporary collision exception only after Movement has accepted
        // the exact immutable path. A NoPath/invalid/stale result therefore
        // leaves normal collision authority intact and remains visible to the
        // retry/placement policy instead of silently phasing the unit through.
        const uint64_t duration = static_cast<uint64_t>(std::max(
            1u, rules.logicFramesPerSecond)) * 2u;
        const uint64_t ignoreUntil = duration >
                std::numeric_limits<uint64_t>::max() - tick
            ? std::numeric_limits<uint64_t>::max()
            : tick + duration;
        ObjectTemporaryCollisionIgnoreComponent* collisionIgnore =
            ecs::try_get<ObjectTemporaryCollisionIgnoreComponent>(
                registry, entity);
        if (!collisionIgnore) {
            ecs::emplace<ObjectTemporaryCollisionIgnoreComponent>(
                registry, entity,
                ObjectTemporaryCollisionIgnoreComponent{
                    .untilTick = ignoreUntil,
                    .other = input->command.ignoredObstacle,
                });
        } else {
            collisionIgnore->untilTick = ignoreUntil;
            collisionIgnore->other = input->command.ignoredObstacle;
        }
    }

    if (!active) {
        brakeFixedMovement(locomotion, rules.logicDeltaSeconds);
        locomotion.hasActiveMove = false;
        locomotion.movingBackward = false;
        locomotion.state = ObjectLocomotionState::Idle;
        return;
    }

    if (!navigation) {
        const ai::PathCorrelation correlation = active->correlation;
        const int64_t alongPathDistanceRaw = active->alongPathDistanceRaw;
        releaseAIMovementPath(navigation, *active);
        ecs::remove<ObjectAIPathMovementComponent>(registry, entity);
        brakeFixedMovement(locomotion, rules.logicDeltaSeconds);
        locomotion.hasActiveMove = false;
        locomotion.movingBackward = false;
        locomotion.state = ObjectLocomotionState::Idle;
        appendAIMovementFeedback(
            feedback, correlation,
            ai::MovementFeedbackStatus::Unsupported,
            tick, 0, alongPathDistanceRaw, 0);
        return;
    }
    // Published topology changes request a replacement route, but the old
    // repository path remains immutable and readable at its own revision.
    // Keep following it until the Patch is ready; collision authority still
    // stops a segment that became physically blocked. Releasing and braking
    // here made every incremental topology publication visibly stop/restart
    // all affected units.
    const bool pathNeedsReplacement = navigation->isPathStale(
        active->path,
        navigation::NavigationRevision{active->pathRevision});

    ObjectTerrainLayerComponent* objectLayer =
        ecs::try_get<ObjectTerrainLayerComponent>(registry, entity);
    const game::terrain::TerrainPathfindLayerId currentTerrainLayer =
        objectLayer ? objectLayer->pathfindLayer
                    : game::terrain::kGroundPathfindLayer;
    navigation::NavigationLayerId currentNavigationLayer;
    if (!navigation::tryNavigationLayerFromTerrainPathfindLayer(
            currentTerrainLayer, currentNavigationLayer)) {
        const ai::PathCorrelation correlation = active->correlation;
        const int64_t alongPathDistanceRaw = active->alongPathDistanceRaw;
        releaseAIMovementPath(navigation, *active);
        ecs::remove<ObjectAIPathMovementComponent>(registry, entity);
        appendAIMovementFeedback(
            feedback, correlation, ai::MovementFeedbackStatus::Unsupported,
            tick, 0, alongPathDistanceRaw, 0);
        return;
    }
    const navigation::NavigationGrid* currentNavigationGrid =
        navigation->layers().find(currentNavigationLayer);
    if (currentNavigationGrid == nullptr) {
        const ai::PathCorrelation correlation = active->correlation;
        const int64_t alongPathDistanceRaw = active->alongPathDistanceRaw;
        releaseAIMovementPath(navigation, *active);
        ecs::remove<ObjectAIPathMovementComponent>(registry, entity);
        appendAIMovementFeedback(
            feedback, correlation, ai::MovementFeedbackStatus::Unsupported,
            tick, 0, alongPathDistanceRaw, 0);
        return;
    }
    const ObjectGeometryComponent* geometry =
        ecs::try_get<ObjectGeometryComponent>(registry, entity);
    const navigation::NavigationClearanceClass clearance = geometry
        ? navigation::clearanceClassForRadiusRaw(
              PhysicsScalar::max(
                  kPhysicsZero, geometry->boundingCircleRadiusFixed).raw(),
              currentNavigationGrid->transform().cellSizeRaw)
        : navigation::NavigationClearanceClass::Centered1x1;
    PathFollowTarget followTarget;
    if (installed || active->nextPointIndex == 0u) {
        followTarget = selectPathFollowTarget(
            navigation->pathRepository(), navigation->layers(), active->path,
            navigation::NavigationRevision{active->pathRevision},
            active->nextPointIndex, fixedTransform, currentNavigationLayer,
            locomotion.surfaces, clearance);
    } else {
        const navigation::PathRepositoryPointQuery retained =
            navigation->pathRepository().queryPoint(
                active->path,
                navigation::NavigationRevision{active->pathRevision},
                active->nextPointIndex);
        const navigation::PathRepositoryPointQuery afterRetained =
            navigation->pathRepository().queryPoint(
                active->path,
                navigation::NavigationRevision{active->pathRevision},
                active->nextPointIndex + 1u);
        if (retained.status == navigation::PathRepositoryStatus::Success) {
            followTarget = {
                .point = retained.point,
                .pointIndex = active->nextPointIndex,
                .finalPoint = afterRetained.status ==
                    navigation::PathRepositoryStatus::PointOutOfRange,
                .validatedNavigationSegment = true,
                .valid = true,
            };
        }
    }
    if (!followTarget.valid) {
        const ai::PathCorrelation correlation = active->correlation;
        const int64_t alongPathDistanceRaw = active->alongPathDistanceRaw;
        releaseAIMovementPath(navigation, *active);
        ecs::remove<ObjectAIPathMovementComponent>(registry, entity);
        appendAIMovementFeedback(
            feedback, correlation,
            installed ? ai::MovementFeedbackStatus::Stuck
                      : ai::MovementFeedbackStatus::Unsupported,
            tick, 0, alongPathDistanceRaw, 0);
        return;
    }
    active->nextPointIndex = followTarget.pointIndex;
    const navigation::PathRepositoryPointQuery point{
        navigation::PathRepositoryStatus::Success,
        followTarget.point,
    };

    game::terrain::TerrainPathfindLayerId pointTerrainLayer =
        game::terrain::kGroundPathfindLayer;
    if (!navigation::tryTerrainPathfindLayerFromNavigationLayer(
            point.point.layer, pointTerrainLayer)) {
        const ai::PathCorrelation correlation = active->correlation;
        const int64_t alongPathDistanceRaw = active->alongPathDistanceRaw;
        releaseAIMovementPath(navigation, *active);
        ecs::remove<ObjectAIPathMovementComponent>(registry, entity);
        appendAIMovementFeedback(
            feedback, correlation, ai::MovementFeedbackStatus::Unsupported,
            tick, 0, alongPathDistanceRaw, 0);
        return;
    }
    if (pointTerrainLayer != currentTerrainLayer) {
        const PhysicsScalar portalX = PhysicsScalar::from_raw(
            point.point.position.xRaw);
        const PhysicsScalar portalY = PhysicsScalar::from_raw(
            point.point.position.yRaw);
        const PhysicsScalar portalDistance = length2D(
            portalX - fixedTransform.position.x,
            portalY - fixedTransform.position.y);
        if (portalDistance > PhysicsScalar::max(
                kPhysicsZero,
                locomotion.closeEnough) +
                kMovementArrivalEpsilonFixed) {
            const ai::PathCorrelation correlation = active->correlation;
            const int64_t alongPathDistanceRaw =
                active->alongPathDistanceRaw;
            releaseAIMovementPath(navigation, *active);
            ecs::remove<ObjectAIPathMovementComponent>(registry, entity);
            appendAIMovementFeedback(
                feedback, correlation, ai::MovementFeedbackStatus::Stuck,
                tick, 0, alongPathDistanceRaw,
                portalDistance.raw());
            return;
        }
        if (!objectLayer) {
            objectLayer = &ecs::emplace<ObjectTerrainLayerComponent>(
                registry, entity);
        }
        static_cast<void>(objectLayer->assign(pointTerrainLayer, tick));
        fixedTransform.position.x = portalX;
        fixedTransform.position.y = portalY;
        const std::optional<int64_t> layerHeight =
            terrain.pathfindLayerHeightRawAt(
                pointTerrainLayer, portalX.raw(), portalY.raw());
        fixedTransform.position.z = (layerHeight
            ? PhysicsScalar::from_raw(*layerHeight)
            : PhysicsScalar::from_raw(point.point.position.zRaw)) +
            locomotion.groundOffsetFixed;
        projectFixedMovementState(fixedTransform, locomotion, transform);
        ++active->nextPointIndex;
        const navigation::PathRepositoryPointQuery afterTransition =
            navigation->pathRepository().queryPoint(
                active->path,
                navigation::NavigationRevision{active->pathRevision},
                active->nextPointIndex);
        if (afterTransition.status ==
            navigation::PathRepositoryStatus::PointOutOfRange) {
            const ai::PathCorrelation correlation = active->correlation;
            const int64_t alongPathDistanceRaw =
                active->alongPathDistanceRaw;
            releaseAIMovementPath(navigation, *active);
            ecs::remove<ObjectAIPathMovementComponent>(registry, entity);
            appendAIMovementFeedback(
                feedback, correlation,
                ai::MovementFeedbackStatus::Completed,
                tick, 0, alongPathDistanceRaw, 0);
            return;
        }
        if (afterTransition.status !=
            navigation::PathRepositoryStatus::Success) {
            const ai::PathCorrelation correlation = active->correlation;
            const int64_t alongPathDistanceRaw =
                active->alongPathDistanceRaw;
            releaseAIMovementPath(navigation, *active);
            ecs::remove<ObjectAIPathMovementComponent>(registry, entity);
            appendAIMovementFeedback(
                feedback, correlation,
                ai::MovementFeedbackStatus::Unsupported,
                tick, 0, alongPathDistanceRaw, 0);
            return;
        }
        locomotion.hasActiveMove = true;
        locomotion.state = ObjectLocomotionState::Moving;
        appendAIMovementFeedback(
            feedback, active->correlation,
            installed || rebound ? ai::MovementFeedbackStatus::Started
                      : pathNeedsReplacement
                            ? ai::MovementFeedbackStatus::Stuck
                            : ai::MovementFeedbackStatus::Progress,
            tick, 0, active->alongPathDistanceRaw, 0);
        return;
    }

    locomotion.goal = {
        PhysicsScalar::from_raw(point.point.position.xRaw),
        PhysicsScalar::from_raw(point.point.position.yRaw),
        PhysicsScalar::from_raw(point.point.position.zRaw),
    };
    locomotion.activeOrderTick = active->correlation.orderIdentity.issuedTick;
    locomotion.activeOrderSequence =
        active->correlation.orderIdentity.sourceSequence;
    locomotion.activeSourceScriptId =
        active->correlation.orderIdentity.sourceScriptId;
    locomotion.hasActiveMove = true;

    const PhysicsScalar previousX = fixedTransform.position.x;
    const PhysicsScalar previousY = fixedTransform.position.y;
    const ObjectTerrainLayerComponent* terrainLayer =
        ecs::try_get<ObjectTerrainLayerComponent>(registry, entity);
    const GroundMoveStepResult result = updateGroundMove(
        locomotion, fixedTransform, transform, health, nullptr, nullptr,
        subject, followTarget.validatedNavigationSegment,
        followTarget.finalPoint, terrain,
        terrainLayer ? terrainLayer->pathfindLayer
                     : game::terrain::kGroundPathfindLayer,
        rules, PhysicsScalar::from_raw(active->speedLimitRaw),
        PhysicsScalar::from_raw(active->extraDistanceRaw), tick, nullptr);
    const int64_t deltaRaw = length2D(
        fixedTransform.position.x - previousX,
        fixedTransform.position.y - previousY).raw();
    if (deltaRaw > 0 && active->alongPathDistanceRaw <=
            std::numeric_limits<int64_t>::max() - deltaRaw)
        active->alongPathDistanceRaw += deltaRaw;

    const PhysicsScalar finalDistance = length2D(
        locomotion.goal.x - fixedTransform.position.x,
        locomotion.goal.y - fixedTransform.position.y);
    if (result == GroundMoveStepResult::Blocked) {
        if (active->blockedTicks != std::numeric_limits<uint32_t>::max())
            ++active->blockedTicks;
        const uint32_t stuckThreshold = std::max(
            1u, ceilPositiveMovementRatio(
                kPhysicsTwo,
                PhysicsScalar::max(
                    rules.logicDeltaSeconds,
                    PhysicsScalar::from_fraction(1, 1'000'000))));
        appendAIMovementFeedback(
            feedback, active->correlation,
            active->blockedTicks >= stuckThreshold
                ? ai::MovementFeedbackStatus::Stuck
                : ai::MovementFeedbackStatus::Blocked,
            tick, active->blockedTicks, active->alongPathDistanceRaw,
            finalDistance.raw());
        return;
    }

    active->blockedTicks = 0;
    if (result == GroundMoveStepResult::Completed) {
        if (followTarget.advancePointOnCompletion)
            ++active->nextPointIndex;
        const navigation::PathRepositoryPointQuery next =
            navigation->pathRepository().queryPoint(
                active->path,
                navigation::NavigationRevision{active->pathRevision},
                active->nextPointIndex);
        if (next.status == navigation::PathRepositoryStatus::PointOutOfRange) {
            const ai::PathCorrelation correlation = active->correlation;
            const int64_t alongPathDistanceRaw = active->alongPathDistanceRaw;
            releaseAIMovementPath(navigation, *active);
            ecs::remove<ObjectAIPathMovementComponent>(registry, entity);
            appendAIMovementFeedback(
                feedback, correlation,
                ai::MovementFeedbackStatus::Completed,
                tick, 0, alongPathDistanceRaw, 0);
            return;
        }
        if (next.status != navigation::PathRepositoryStatus::Success) {
            const ai::PathCorrelation correlation = active->correlation;
            const int64_t alongPathDistanceRaw = active->alongPathDistanceRaw;
            releaseAIMovementPath(navigation, *active);
            ecs::remove<ObjectAIPathMovementComponent>(registry, entity);
            appendAIMovementFeedback(
                feedback, correlation,
                ai::MovementFeedbackStatus::Unsupported,
                tick, 0, alongPathDistanceRaw, 0);
            return;
        }
    }

    appendAIMovementFeedback(
        feedback, active->correlation,
        installed ? ai::MovementFeedbackStatus::Started
                  : pathNeedsReplacement
                        ? ai::MovementFeedbackStatus::Stuck
                        : ai::MovementFeedbackStatus::Progress,
        tick, 0, active->alongPathDistanceRaw,
        finalDistance.raw());
}

// RefCode Locomotor::locoUpdate_maintainCurrentPosition() lets grounded
// two-leg, wheeled, tracked, climber and motorcycle locomotors sleep forever
// once they have no goal. Hover/thrust/wing/other appearances, and every
// authored Z controller except NoZMotiveForce, still require a confirmed-tick
// maintenance call. Keep that scheduling decision explicit here so dormant
// ground objects do not pay for profile selection, terrain sampling and fixed
// movement math merely because they own a Locomotor component.
[[nodiscard]] bool requiresConstantLocomotorUpdate(
    const ObjectLocomotionComponent& locomotion) noexcept {
    if (locomotion.zAxisBehavior !=
        game::LocomotorZAxisBehavior::NoZMotiveForce) {
        return true;
    }

    switch (locomotion.appearance) {
    case game::LocomotorAppearance::TwoLegs:
    case game::LocomotorAppearance::FourWheels:
    case game::LocomotorAppearance::Treads:
    case game::LocomotorAppearance::Climber:
    case game::LocomotorAppearance::Motorcycle:
        return false;
    case game::LocomotorAppearance::Hover:
    case game::LocomotorAppearance::Thrust:
    case game::LocomotorAppearance::Wings:
    case game::LocomotorAppearance::Other:
        return true;
    }
    return true;
}

// Derived scheduling state. It is intentionally registry-local and omitted
// from save/replay snapshots: a restored registry rebuilds it once from
// authoritative components, while ordinary ticks retain only locomotors
// which still have work. Stable ObjectId ordering preserves the former full
// view + sort execution order without paying that cost for sleeping objects.
struct MovementActiveSetCache final {
    container::Vector<ObjectId> ids;
    bool initialized = false;
};

[[nodiscard]] bool airfieldMovementOwnsEntity(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ObjectAirfieldComponent* airfield =
        ecs::try_get<ObjectAirfieldComponent>(registry, entity);
    if (!airfield) return false;
    const bool jetOwnsTranslation = std::any_of(
        airfield->jetAi.begin(), airfield->jetAi.end(),
        [](const ObjectJetAiRuntime& runtime) noexcept {
            return runtime.phase != ObjectJetAirfieldPhase::Airborne;
        });
    const bool chinookOwnsTranslation = std::any_of(
        airfield->chinookAi.begin(), airfield->chinookAi.end(),
        [](const ObjectChinookAiRuntime& runtime) noexcept {
            return runtime.flightPhase !=
                ObjectHelicopterFlightPhase::Airborne;
        });
    return jetOwnsTranslation || chinookOwnsTranslation;
}

[[nodiscard]] bool sharedMovementOwnsEntity(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    if (ecs::try_get<ObjectProjectileComponent>(registry, entity) ||
        ecs::try_get<ObjectWaveGuideComponent>(registry, entity)) {
        return false;
    }
    if (const ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, entity);
        physics && physics->forceFreeBodyTranslation) {
        return false;
    }
    if (const ObjectContainmentRuntimeComponent* containment =
            ecs::try_get<ObjectContainmentRuntimeComponent>(registry,
                                                             entity);
        containment && containment->plan &&
        std::any_of(containment->plan->rules.begin(),
                    containment->plan->rules.end(),
                    [](const ObjectContainmentRule& rule) {
                        return rule.kind == ObjectContainmentKind::Parachute;
                    })) {
        return false;
    }
    if (airfieldMovementOwnsEntity(registry, entity)) return false;
    return true;
}

[[nodiscard]] bool hasMovementWork(
    const ecs::registry& registry, ecs::entity entity,
    const ObjectLocomotionComponent& locomotion) noexcept {
    const ObjectOrderQueueComponent* queue =
        ecs::try_get<ObjectOrderQueueComponent>(registry, entity);
    const bool hasQueuedMove = queue && !queue->orders.empty() &&
        queue->orders.front().kind == ObjectOrderKind::Move;
    const bool hasAIPath =
        ecs::try_get<ObjectAIPathMovementComponent>(registry, entity) !=
        nullptr;
    const bool hasResidualMotion =
        locomotion.state != ObjectLocomotionState::Idle ||
        locomotion.hasActiveMove || locomotion.forwardSpeed.raw() != 0 ||
        locomotion.verticalSpeed.raw() != 0;
    return hasQueuedMove || hasAIPath || hasResidualMotion ||
        requiresConstantLocomotorUpdate(locomotion);
}

void updateAirborneTargetClassification(
    ecs::registry& registry, ecs::entity entity,
    const ObjectLocomotionComponent& locomotion,
    const ObjectFixedTransformComponent& transform,
    const game::terrain::TerrainLogic& terrain,
    uint64_t confirmedTick) {
    const game::ObjectStatusMask airborneTarget =
        game::objectStatusBit(game::ObjectStatusFlag::AirborneTarget);
    bool classifiedAirborne = false;
    if (locomotion.hasFiniteAirborneTargetingHeight && terrain.isLoaded()) {
        const PhysicsScalar ground = PhysicsScalar::from_raw(
            terrain.groundHeightRaw(transform.position.x.raw(),
                                    transform.position.y.raw()));
        classifiedAirborne =
            transform.position.z - ground > locomotion.airborneTargetingHeight;
    }
    static_cast<void>(ObjectStatusSystem::apply(
        registry, entity,
        {
            .setMask = classifiedAirborne ? airborneTarget : 0,
            .clearMask = classifiedAirborne ? 0 : airborneTarget,
            .confirmedTick = confirmedTick,
        }));
}

void updateMovement(ecs::registry& registry, ObjectLifecycle& lifecycle,
                    const game::terrain::TerrainLogic& terrain, uint64_t tick,
                    const ObjectSimulationRules& rules,
                    container::Vector<ObjectMovementEvent>& events,
                    container::Vector<ai::MovementFeedback>& aiFeedback,
                    container::Span<const ObjectId> aiMoveStopOwners,
                    container::Span<const ObjectId> aiAttackOwners,
                    container::Span<const ObjectAIMovementCommand>
                        aiMovementCommands,
                    navigation::NavigationSystem* navigation) {
    struct Candidate final {
        ObjectId id = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    MovementActiveSetCache* activeSet =
        registry.ctx().find<MovementActiveSetCache>();
    if (!activeSet) {
        activeSet = &registry.ctx().emplace<MovementActiveSetCache>();
    }

    container::Vector<ObjectId> wakeIds;
    if (!activeSet->initialized) {
        const auto bootstrap = ecs::view<
            ObjectIdentityComponent, ObjectLocomotionComponent,
            TransformComponent>(registry);
        wakeIds.reserve(bootstrap.size_hint());
        for (const ecs::entity entity : bootstrap) {
            const ObjectIdentityComponent& identity =
                bootstrap.template get<ObjectIdentityComponent>(entity);
            if (identity.id) wakeIds.push_back(identity.id);
        }
        activeSet->initialized = true;
    } else {
        // Existing order writers predate the active-set contract and mutate
        // queue vectors directly. Scan only the compact queue storage for a
        // Move head; expensive locomotor/terrain work remains active-only.
        const auto queued = ecs::view<
            ObjectIdentityComponent, ObjectOrderQueueComponent>(registry);
        wakeIds.reserve(queued.size_hint() + aiMovementCommands.size());
        for (const ecs::entity entity : queued) {
            const ObjectOrderQueueComponent& queue =
                queued.template get<ObjectOrderQueueComponent>(entity);
            if (queue.orders.empty() ||
                queue.orders.front().kind != ObjectOrderKind::Move) {
                continue;
            }
            const ObjectId id = queued
                .template get<ObjectIdentityComponent>(entity).id;
            if (id) wakeIds.push_back(id);
        }
        const auto pathMovers = ecs::view<
            ObjectIdentityComponent, ObjectAIPathMovementComponent>(
                registry);
        for (const ecs::entity entity : pathMovers) {
            const ObjectId id = pathMovers
                .template get<ObjectIdentityComponent>(entity).id;
            if (id) wakeIds.push_back(id);
        }
        // Airfield taxi/takeoff/landing owns Transform outside the generic
        // order queue. Keep those aircraft admitted so the first Airborne
        // tick can hand ownership back without waiting for a later command.
        const auto airfieldMovers = ecs::view<
            ObjectIdentityComponent, ObjectAirfieldComponent>(registry);
        for (const ecs::entity entity : airfieldMovers) {
            if (!airfieldMovementOwnsEntity(registry, entity)) continue;
            const ObjectId id = airfieldMovers
                .template get<ObjectIdentityComponent>(entity).id;
            if (id) wakeIds.push_back(id);
        }
    }
    for (const ObjectAIMovementCommand& command : aiMovementCommands) {
        if (command.command.correlation.subject) {
            wakeIds.push_back(command.command.correlation.subject);
        }
    }

    activeSet->ids.insert(activeSet->ids.end(), wakeIds.begin(),
                          wakeIds.end());
    std::sort(activeSet->ids.begin(), activeSet->ids.end());
    activeSet->ids.erase(
        std::unique(activeSet->ids.begin(), activeSet->ids.end()),
        activeSet->ids.end());

    container::Vector<Candidate> candidates;
    candidates.reserve(activeSet->ids.size());
    for (const ObjectId id : activeSet->ids) {
        const std::optional<ecs::entity> entity = lifecycle.entityFromId(id);
        if (!entity ||
            !ecs::try_get<ObjectLocomotionComponent>(registry, *entity) ||
            !ecs::try_get<TransformComponent>(registry, *entity) ||
            !sharedMovementOwnsEntity(registry, *entity)) {
            continue;
        }
        candidates.push_back({.id = id, .entity = *entity});
    }

    size_t aiOwnerCursor = 0;
    size_t aiAttackOwnerCursor = 0;
    size_t aiCommandCursor = 0;
    for (const Candidate& candidate : candidates) {
        while (aiCommandCursor < aiMovementCommands.size() &&
               aiMovementCommands[aiCommandCursor].command.correlation.subject <
                   candidate.id) {
            releaseAIMovementCommandPath(
                navigation, aiMovementCommands[aiCommandCursor]);
            appendAIMovementFeedback(
                aiFeedback,
                aiMovementCommands[aiCommandCursor].command.correlation,
                ai::MovementFeedbackStatus::Unsupported, tick, 0, 0, 0);
            ++aiCommandCursor;
        }
        const ObjectAIMovementCommand* aiCommand =
            aiCommandCursor < aiMovementCommands.size() &&
                    aiMovementCommands[aiCommandCursor]
                            .command.correlation.subject == candidate.id
                ? &aiMovementCommands[aiCommandCursor]
                : nullptr;
        while (aiOwnerCursor < aiMoveStopOwners.size() &&
               aiMoveStopOwners[aiOwnerCursor] < candidate.id)
            ++aiOwnerCursor;
        const bool aiMoveStopOwner =
            aiOwnerCursor < aiMoveStopOwners.size() &&
            aiMoveStopOwners[aiOwnerCursor] == candidate.id;
        while (aiAttackOwnerCursor < aiAttackOwners.size() &&
               aiAttackOwners[aiAttackOwnerCursor] < candidate.id)
            ++aiAttackOwnerCursor;
        const bool aiAttackOwner =
            aiAttackOwnerCursor < aiAttackOwners.size() &&
            aiAttackOwners[aiAttackOwnerCursor] == candidate.id;
        ObjectLocomotionComponent& locomotion = ecs::get<ObjectLocomotionComponent>(registry, candidate.entity);
        TransformComponent& transform = ecs::get<TransformComponent>(registry, candidate.entity);
        ObjectFixedTransformComponent* fixedTransform =
            ecs::try_get<ObjectFixedTransformComponent>(
                registry, candidate.entity);
        if (!fixedTransform || !fixedTransform->authoritative) continue;
        ensureFixedMovementState(*fixedTransform, locomotion, transform);
        const ObjectHealthComponent* health = ecs::try_get<ObjectHealthComponent>(registry, candidate.entity);
        const ObjectTerrainLayerComponent* terrainLayer =
            ecs::try_get<ObjectTerrainLayerComponent>(registry,
                                                       candidate.entity);
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(registry,
                                                  candidate.entity);
        ObjectOrderQueueComponent* queue = ecs::try_get<ObjectOrderQueueComponent>(registry, candidate.entity);
        const ObjectOrderIntent* head = queue && !queue->orders.empty()
            ? &queue->orders.front()
            : nullptr;
        const bool hasQueuedMove =
            head && head->kind == ObjectOrderKind::Move;
        const bool hasAIPath =
            ecs::try_get<ObjectAIPathMovementComponent>(
                registry, candidate.entity) != nullptr;
        const bool hasResidualMotion =
            locomotion.state != ObjectLocomotionState::Idle ||
            locomotion.hasActiveMove ||
            locomotion.forwardSpeed.raw() != 0 ||
            locomotion.verticalSpeed.raw() != 0;

        // This is the ECS equivalent of AIUpdate/doLocomotor returning
        // UPDATE_SLEEP_FOREVER. New queue heads and AI service commands are
        // explicit wake sources, while active/path/residual motion remains
        // admitted until it settles.
        if (!aiCommand && !hasAIPath && !hasQueuedMove &&
            !hasResidualMotion &&
            !requiresConstantLocomotorUpdate(locomotion)) {
            continue;
        }

        // Only admitted locomotor objects can change broad-phase position or
        // movement-owned model conditions this tick. Mark this bounded active
        // set instead of forcing downstream consumers to rescan all objects.
        markObjectDirty(
            registry, candidate.entity,
            objectDirtyBit(ObjectDirtyDomain::Spatial) |
                objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
                objectDirtyBit(ObjectDirtyDomain::RenderExtraction));

        chooseLocomotorForPosition(locomotion, terrain,
                                   fixedTransform->position.x,
                                   fixedTransform->position.y);
        locomotion.overWater =
            locomotion.appearance == game::LocomotorAppearance::Hover &&
            terrain.isLoaded() &&
            terrain.isUnderwaterLegacyRaw(
                fixedTransform->position.x.raw(),
                fixedTransform->position.y.raw());
        const ObjectMoveOrderConsumer consumer =
            resolveMoveOrderConsumer(head, aiMoveStopOwner, aiAttackOwner);
        const bool unavailable =
            ecs::try_get<ObjectContainedByComponent>(registry,
                                                      candidate.entity) != nullptr ||
            (health && health->effectivelyDead &&
             !locomotion.locomotorWorksWhenDead) ||
            isObjectDisabled(registry, candidate.entity, tick);
        if (consumer == ObjectMoveOrderConsumer::ObjectAIRuntime) {
            updateAIMovement(
                registry, candidate.entity, candidate.id, locomotion,
                *fixedTransform, transform, health,
                unavailable ? nullptr : queue,
                aiCommand, navigation, terrain, rules, tick,
                aiFeedback);
            if (locomotion.state == ObjectLocomotionState::Idle &&
                !unavailable) {
                updateLocomotorVertical(
                    locomotion, *fixedTransform, health, terrain,
                    terrainLayer ? terrainLayer->pathfindLayer
                                 : game::terrain::kGroundPathfindLayer,
                    rules, fixedTransform->position.z);
                projectFixedMovementState(
                    *fixedTransform, locomotion, transform);
            }
            if (aiCommand)
                ++aiCommandCursor;
            continue;
        }
        if (hasAIPath || aiCommand) {
            // Capability loss, Stop, containment, and specialized replacement
            // all cancel Movement's old PathHandle before any legacy consumer
            // is allowed to write the same locomotion state.
            updateAIMovement(
                registry, candidate.entity, candidate.id, locomotion,
                *fixedTransform, transform, health, nullptr, aiCommand,
                navigation, terrain,
                rules, tick, aiFeedback);
            if (aiCommand)
                ++aiCommandCursor;
        }
        if (ecs::try_get<ObjectContainedByComponent>(registry,
                                                     candidate.entity))
            continue;
        if (!queue || queue->orders.empty() || queue->orders.front().kind != ObjectOrderKind::Move ||
            (health && health->effectivelyDead && !locomotion.locomotorWorksWhenDead) ||
            isObjectDisabled(registry, candidate.entity, tick)) {
            brakeFixedMovement(locomotion, rules.logicDeltaSeconds);
            // A replacement/clear command is an explicit cancellation of
            // the old move intent.  Leaving hasActiveMove set would suppress
            // the next Started event if a same-tick/same-sequence move was
            // subsequently authored, and would advertise stale state to AI
            // or presentation consumers.
            locomotion.hasActiveMove = false;
            locomotion.movingBackward = false;
            locomotion.state = ObjectLocomotionState::Idle;
            if (!unavailable) {
                updateLocomotorVertical(
                    locomotion, *fixedTransform, health, terrain,
                    terrainLayer ? terrainLayer->pathfindLayer
                                 : game::terrain::kGroundPathfindLayer,
                    rules, fixedTransform->position.z);
            }
            projectFixedMovementState(*fixedTransform, locomotion, transform);
            continue;
        }
        if (queue->orders.front().targetObject) {
            const std::optional<ecs::entity> target =
                lifecycle.entityFromId(queue->orders.front().targetObject);
            const TransformComponent* targetTransform = target
                ? ecs::try_get<TransformComponent>(registry, *target) : nullptr;
            if (!targetTransform) {
                queue->orders.erase(queue->orders.begin());
                ++queue->revision;
                locomotion.hasActiveMove = false;
                locomotion.state = ObjectLocomotionState::Idle;
                continue;
            }
            const LogicFixedVec3 targetPosition =
                readAuthoritativeObjectPosition(
                    registry, *target, *targetTransform);
            queue->orders.front().targetX = targetPosition.x;
            queue->orders.front().targetY = targetPosition.y;
            queue->orders.front().targetZ = targetPosition.z;
            queue->orders.front().hasTargetPosition = true;
            // Preserve the order identity while following a moving target;
            // updateGroundMove refreshes the goal below without generating a
            // second Started event.
            if (locomotion.hasActiveMove) {
                locomotion.goal = targetPosition;
            }
        }
        ObjectOrderIntent& moveOrder = queue->orders.front();
        if (navigation && navigation->isInitialized() &&
            moveOrder.source == ObjectOrderSource::System &&
            adjustsSystemDestination(moveOrder.systemPurpose) &&
            moveOrder.hasTargetPosition) {
            navigation::NavigationLayerId navigationLayer;
            if (navigation::tryNavigationLayerFromTerrainPathfindLayer(
                    terrainLayer ? terrainLayer->pathfindLayer
                                 : game::terrain::kGroundPathfindLayer,
                    navigationLayer)) {
                const navigation::NavigationDestinationAdjustmentResult
                    adjusted = navigation::adjustNavigationDestination(
                        navigation->layers(),
                        {
                            .desired = {
                                moveOrder.targetX.raw(),
                                moveOrder.targetY.raw(),
                                moveOrder.targetZ.raw()},
                            .layer = navigationLayer,
                            .movementMask = locomotion.surfaces,
                            .clearance = geometry
                                ? navigation::clearanceClassForRadiusRaw(
                                      PhysicsScalar::max(
                                          kPhysicsZero,
                                          geometry->boundingCircleRadiusFixed)
                                          .raw(),
                                      navigation->grid().transform()
                                          .cellSizeRaw)
                                : navigation::NavigationClearanceClass::
                                      Centered1x1,
                            .allowAdjustment = true,
                        });
                if (adjusted.accepted()) {
                    if (moveOrder.targetX.raw() !=
                            adjusted.position.xRaw ||
                        moveOrder.targetY.raw() !=
                            adjusted.position.yRaw ||
                        moveOrder.targetZ.raw() !=
                            adjusted.position.zRaw) {
                        moveOrder.targetX = PhysicsScalar::from_raw(
                            adjusted.position.xRaw);
                        moveOrder.targetY = PhysicsScalar::from_raw(
                            adjusted.position.yRaw);
                        moveOrder.targetZ = PhysicsScalar::from_raw(
                            adjusted.position.zRaw);
                        ++queue->revision;
                        if (sameActiveOrder(locomotion, moveOrder)) {
                            locomotion.goal = {
                                PhysicsScalar::from_raw(
                                    adjusted.position.xRaw),
                                PhysicsScalar::from_raw(
                                    adjusted.position.yRaw),
                                PhysicsScalar::from_raw(
                                    adjusted.position.zRaw),
                            };
                        }
                    }
                }
            }
        }
        static_cast<void>(updateGroundMove(
            locomotion, *fixedTransform, transform, health, queue,
            &queue->orders.front(),
            candidate.id, false, true, terrain,
            terrainLayer ? terrainLayer->pathfindLayer
                         : game::terrain::kGroundPathfindLayer,
            rules, {}, {}, tick, &events));
    }
    while (aiCommandCursor < aiMovementCommands.size()) {
        releaseAIMovementCommandPath(
            navigation, aiMovementCommands[aiCommandCursor]);
        appendAIMovementFeedback(
            aiFeedback,
            aiMovementCommands[aiCommandCursor].command.correlation,
            ai::MovementFeedbackStatus::Unsupported, tick, 0, 0, 0);
        ++aiCommandCursor;
    }

    container::Vector<ObjectId> survivors;
    survivors.reserve(candidates.size());
    for (const Candidate& candidate : candidates) {
        if (!registry.valid(candidate.entity) ||
            !sharedMovementOwnsEntity(registry, candidate.entity)) {
            continue;
        }
        const ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(registry,
                                                     candidate.entity);
        const ObjectFixedTransformComponent* fixedTransform =
            ecs::try_get<ObjectFixedTransformComponent>(registry,
                                                        candidate.entity);
        if (locomotion && fixedTransform && fixedTransform->authoritative) {
            updateAirborneTargetClassification(
                registry, candidate.entity, *locomotion, *fixedTransform,
                terrain, tick);
        }
        if (locomotion &&
            hasMovementWork(registry, candidate.entity, *locomotion)) {
            survivors.push_back(candidate.id);
        }
    }
    for (const ObjectId id : activeSet->ids) {
        const std::optional<ecs::entity> entity = lifecycle.entityFromId(id);
        if (entity && airfieldMovementOwnsEntity(registry, *entity)) {
            survivors.push_back(id);
        }
    }
    std::sort(survivors.begin(), survivors.end());
    survivors.erase(std::unique(survivors.begin(), survivors.end()),
                    survivors.end());
    activeSet->ids = std::move(survivors);
}

void updateAIFacing(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain,
    const ObjectSimulationRules& rules, uint64_t confirmedTick,
    container::Span<const ai::AIStateCommand> sourceCommands,
    container::Vector<ai::AIFacingFeedback>& feedback) {
    container::Vector<ai::AIStateCommand> commands(
        sourceCommands.begin(), sourceCommands.end());
    std::sort(commands.begin(), commands.end(),
        [](const ai::AIStateCommand& left,
           const ai::AIStateCommand& right) {
            if (left.subject != right.subject)
                return left.subject < right.subject;
            if (left.request != right.request)
                return left.request < right.request;
            return static_cast<uint8_t>(left.kind) <
                static_cast<uint8_t>(right.kind);
        });

    constexpr PhysicsScalar kFacingThreshold =
        PhysicsScalar::from_fraction(7, 200);
    ai::AIStateRequestId previousRequest{};
    ObjectId previousSubject = INVALID_OBJECT_ID;
    for (const ai::AIStateCommand& command : commands) {
        if (command.subject == previousSubject &&
            command.request == previousRequest) {
            continue;
        }
        previousSubject = command.subject;
        previousRequest = command.request;

        ai::AIFacingFeedback result{
            .subject = command.subject,
            .request = command.request,
            .status = ai::AIFacingFeedbackStatus::Unsupported,
            .confirmedTick = confirmedTick,
            .orderIdentity = command.orderIdentity,
        };
        if (!command.subject || !command.request.isValid() ||
            command.confirmedTick > confirmedTick) {
            feedback.push_back(result);
            continue;
        }
        const std::optional<ecs::entity> subject =
            lifecycle.entityFromId(command.subject);
        if (!subject) {
            result.status = ai::AIFacingFeedbackStatus::TargetLost;
            feedback.push_back(result);
            continue;
        }
        TransformComponent* transform =
            ecs::try_get<TransformComponent>(registry, *subject);
        ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(registry, *subject);
        if (!transform || !locomotion) {
            feedback.push_back(result);
            continue;
        }
        ObjectFixedTransformComponent* fixedTransform =
            ecs::try_get<ObjectFixedTransformComponent>(registry, *subject);
        if (!fixedTransform || !fixedTransform->authoritative) {
            feedback.push_back(result);
            continue;
        }
        ensureFixedMovementState(*fixedTransform, *locomotion, *transform);
        if (command.confirmedTick == confirmedTick) {
            // AIFaceState::onEnter is preceded by aiIdle() and
            // resetDynamicPhysics() in RefCode. Centralize that edge here so
            // every specialized producer (script and SpecialAbilityUpdate)
            // starts facing from the same stopped deterministic state.
            locomotion->forwardSpeed = {};
            locomotion->hasActiveMove = false;
            locomotion->state = ObjectLocomotionState::Idle;
            if (ObjectPhysicsComponent* physics =
                    ecs::try_get<ObjectPhysicsComponent>(
                        registry, *subject)) {
                physics->velocityUnitsPerSecond = {};
                physics->pendingForce = {};
                physics->previousAcceleration = {};
                physics->yawRate = {};
                physics->pitchRate = {};
                physics->rollRate = {};
            }
        }
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, *subject);
        if (ecs::try_get<ObjectContainedByComponent>(registry, *subject) ||
            isObjectDisabled(registry, *subject, confirmedTick) ||
            (health && health->effectivelyDead &&
             !locomotion->locomotorWorksWhenDead)) {
            feedback.push_back(result);
            continue;
        }

        LogicFixedVec3 target{};
        if (command.kind == ai::AIStateCommandKind::FaceObject) {
            const std::optional<ecs::entity> targetEntity =
                lifecycle.entityFromId(command.targetObject);
            const TransformComponent* targetTransform = targetEntity
                ? ecs::try_get<TransformComponent>(registry, *targetEntity)
                : nullptr;
            if (!targetTransform) {
                result.status = ai::AIFacingFeedbackStatus::TargetLost;
                feedback.push_back(result);
                continue;
            }
            target = readAuthoritativeObjectPosition(
                registry, *targetEntity, *targetTransform);
        } else {
            target = {
                PhysicsScalar::from_raw(command.targetPosition.xRaw),
                PhysicsScalar::from_raw(command.targetPosition.yRaw),
                PhysicsScalar::from_raw(command.targetPosition.zRaw),
            };
        }

        const LogicFixedVec3 subjectPosition =
            readAuthoritativeObjectPosition(registry, *subject, *transform);
        const PhysicsScalar deltaX = target.x - subjectPosition.x;
        const PhysicsScalar deltaY = target.y - subjectPosition.y;
        if (deltaX.raw() == 0 && deltaY.raw() == 0) {
            result.status = ai::AIFacingFeedbackStatus::Completed;
            feedback.push_back(result);
            continue;
        }
        const PhysicsScalar desiredYaw = math::fixed_atan2(deltaY, deltaX);
        const PhysicsScalar currentYaw = fixedTransform->yawRadians;
        const PhysicsScalar error = collision_detail::shortestAngleDelta(
            currentYaw, desiredYaw);
        if (PhysicsScalar::abs(error) < kFacingThreshold) {
            result.status = ai::AIFacingFeedbackStatus::Completed;
            feedback.push_back(result);
            continue;
        }

        const bool damaged = health &&
            isMovementPenaltyState(health->damageState, rules);
        const PhysicsScalar maximumTurnRate = damaged
            ? locomotion->damagedMaximumTurnRate
            : locomotion->maximumTurnRate;
        if (maximumTurnRate <= kPhysicsZero) {
            feedback.push_back(result);
            continue;
        }
        const PhysicsScalar maximumStep =
            maximumTurnRate * rules.logicDeltaSeconds;
        const PhysicsScalar targetDistance = length2D(
            target.x - subjectPosition.x,
            target.y - subjectPosition.y);
        if (!command.canTurnInPlace &&
            targetDistance > PhysicsScalar::max(
                    kPhysicsZero,
                    locomotion->closeEnough) +
                kMovementArrivalEpsilonFixed) {
            // RefCode's AIFaceState does not spin a wheel locomotor in
            // place: it publishes the target as an explicit locomotor goal
            // and lets forward motion create steering authority.  Execute
            // one confirmed-FPS step without manufacturing an order queue
            // entry; the retained facing command repeats this next tick.
            locomotion->goal = target;
            locomotion->hasActiveMove = true;
            locomotion->state = ObjectLocomotionState::Moving;
            const ObjectTerrainLayerComponent* terrainLayer =
                ecs::try_get<ObjectTerrainLayerComponent>(
                    registry, *subject);
            static_cast<void>(updateGroundMove(
                *locomotion, *fixedTransform, *transform, health, nullptr,
                nullptr, command.subject, false, true, terrain,
                terrainLayer ? terrainLayer->pathfindLayer
                             : game::terrain::kGroundPathfindLayer,
                rules, {}, {}, confirmedTick, nullptr));
            const PhysicsScalar movedYaw = fixedTransform->yawRadians;
            if (ObjectPhysicsComponent* physics =
                    ecs::try_get<ObjectPhysicsComponent>(
                        registry, *subject)) {
                physics->yaw = movedYaw;
                physics->sleeping = false;
            }
            result.status = PhysicsScalar::abs(
                    collision_detail::shortestAngleDelta(
                        movedYaw, desiredYaw)) < kFacingThreshold
                ? ai::AIFacingFeedbackStatus::Completed
                : ai::AIFacingFeedbackStatus::Pending;
            feedback.push_back(result);
            continue;
        }
        const PhysicsScalar applied = PhysicsScalar::clamp(
            error, -maximumStep, maximumStep);
        const PhysicsScalar nextYaw = currentYaw + applied;
        writeAuthoritativeObjectYaw(registry, *subject, nextYaw);
        if (ObjectPhysicsComponent* physics =
                ecs::try_get<ObjectPhysicsComponent>(registry, *subject)) {
            physics->sleeping = false;
        }
        result.status = PhysicsScalar::abs(error - applied) <
                kFacingThreshold
            ? ai::AIFacingFeedbackStatus::Completed
            : ai::AIFacingFeedbackStatus::Pending;
        feedback.push_back(result);
    }
}

} // namespace object_simulation_detail

using namespace object_simulation_detail;

bool applyObjectLocomotorSet(
    ObjectLocomotionComponent& locomotion,
    const game::ThingTemplate& objectTemplate,
    const GameContentSnapshot& content,
    game::LocomotorSetSlot slot) {
    container::Vector<game::FrozenLocomotorTemplate> selected =
        collectRuntimeLocomotors(objectTemplate, content, slot);
    if (selected.empty()) return false;
    locomotion.profiles = std::move(selected);
    applyLocomotorTemplate(locomotion, locomotion.profiles.front());
    return true;
}

} // namespace engine
