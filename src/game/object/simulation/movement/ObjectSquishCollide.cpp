#include "game/object/simulation/movement/ObjectSquishCollide.h"

#include "game/base/DamageTypes.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/status/ObjectCrateCollide.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/combat/ObjectTactical.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace {

using namespace engine;

struct Candidate final {
    ObjectId id = INVALID_OBJECT_ID;
    ecs::entity entity = ecs::null;
};

[[nodiscard]] bool effectivelyUnavailable(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId id, ecs::entity entity) noexcept {
    if (!id || lifecycle.isPendingDestroy(id)) return true;
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    if (health && health->effectivelyDead) return true;
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    if (status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::Destroyed) |
            game::objectStatusBit(game::ObjectStatusFlag::Sold) |
            game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction) |
            game::objectStatusBit(game::ObjectStatusFlag::NoCollisions))) {
        return true;
    }
    if (const ObjectContainedByComponent* contained =
            ecs::try_get<ObjectContainedByComponent>(registry, entity);
        contained && contained->enclosing) {
        return true;
    }
    const ObjectMapStatusComponent* map =
        ecs::try_get<ObjectMapStatusComponent>(registry, entity);
    return map && map->offMap;
}

[[nodiscard]] uint8_t crusherLevel(const ecs::registry& registry,
                                   ecs::entity entity) noexcept {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    return type && type->archetype
        ? type->archetype->templateData.crusherLevel : 0;
}

[[nodiscard]] bool areAllies(const ecs::registry& registry,
                             const PlayerRegistry& players,
                             ecs::entity crusher,
                             ecs::entity victim) noexcept {
    return relationshipBetweenObjects(
               registry, players, crusher, victim) ==
        PlayerRelationship::Allies;
}

[[nodiscard]] bool movingTowardVictim(
    const ecs::registry& registry, ecs::entity crusher,
    const LogicFixedVec3& crusherPosition,
    math::q32_32 crusherYaw,
    const LogicFixedVec3& victimPosition) noexcept {
    // RefCode requires PhysicsBehavior to participate in a squish collision.
    // Stage-0 locomotion still owns ordinary ground translation in this
    // engine, so its current forward velocity is the authoritative projection
    // while a Physics component is present but not integrating translation.
    const ObjectPhysicsComponent* physics =
        ecs::try_get<ObjectPhysicsComponent>(registry, crusher);
    if (!physics) return false;

    math::q32_32 vx = physics->velocityUnitsPerSecond.x;
    math::q32_32 vy = physics->velocityUnitsPerSecond.y;
    if (const ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(registry, crusher);
        locomotion && locomotion->state == ObjectLocomotionState::Moving) {
        vx = math::fixed_cos(crusherYaw) * locomotion->forwardSpeed;
        vy = math::fixed_sin(crusherYaw) * locomotion->forwardSpeed;
    }
    const math::q32_32 toX = victimPosition.x - crusherPosition.x;
    const math::q32_32 toY = victimPosition.y - crusherPosition.y;
    return toX * vx + toY * vy > math::q32_32{};
}

[[nodiscard]] bool protectsIntentionalTarget(
    const ecs::registry& registry, ecs::entity victim,
    ObjectId crusher, const game::ObjectSquishCollidePlan& plan) noexcept {
    const ObjectOrderQueueComponent* orders =
        ecs::try_get<ObjectOrderQueueComponent>(registry, victim);
    if (!orders || orders->orders.empty() ||
        orders->orders.front().targetObject != crusher) {
        return false;
    }
    if (plan.hasHijackerUpdate) return true;

    const ObjectTacticalComponent* tactical =
        ecs::try_get<ObjectTacticalComponent>(registry, victim);
    if (!tactical || !tactical->plan) return false;
    const size_t count = std::min(tactical->specialAbilities.size(),
                                  tactical->plan->specialAbilities.size());
    for (size_t index = 0; index < count; ++index) {
        const ObjectSpecialAbilityRuntime& runtime =
            tactical->specialAbilities[index];
        if (runtime.active &&
            runtime.specialPowerType ==
                game::SpecialPowerType::TankHunterTntAttack) {
            return true;
        }
    }
    return false;
}

struct VerticalInterval final {
    math::q32_32 minimum{};
    math::q32_32 maximum{};
};

[[nodiscard]] VerticalInterval verticalInterval(
    const LogicFixedVec3& position,
    const ObjectGeometryComponent* geometry) noexcept {
    const math::q32_32 z = position.z;
    if (!geometry) return {z, z + math::q32_32{int32_t{2}}};
    if (geometry->shape == ObjectGeometryShape::Sphere) {
        const math::q32_32 radius = math::q32_32::max(
            math::q32_32{}, geometry->boundingSphereRadiusFixed);
        return {z - radius, z + radius};
    }
    return {z, z + math::q32_32::max(
        math::q32_32{}, geometry->heightFixed)};
}

[[nodiscard]] bool squishContact(
    const LogicFixedVec3& crusherPosition,
    math::q32_32 crusherYaw,
    const ObjectGeometryComponent* crusherGeometry,
    const LogicFixedVec3& victimPosition,
    const ObjectGeometryComponent* victimGeometry) noexcept {
    const math::q32_32 victimSquishRadius{int32_t{1}};
    const math::q32_32 dx = victimPosition.x - crusherPosition.x;
    const math::q32_32 dy = victimPosition.y - crusherPosition.y;
    const VerticalInterval crusherZ = verticalInterval(
        crusherPosition, crusherGeometry);
    const VerticalInterval victimZ = verticalInterval(
        victimPosition, victimGeometry);
    if (crusherZ.maximum < victimZ.minimum ||
        victimZ.maximum < crusherZ.minimum) return false;

    if (!crusherGeometry) {
        const math::q32_32 fallbackRadius{int32_t{2}};
        return dx * dx + dy * dy <= fallbackRadius * fallbackRadius;
    }

    // The victim's authored horizontal footprint is intentionally replaced
    // by the source engine's 1-unit squish radius. Test that circle against
    // the crusher's oriented footprint instead of accepting the much looser
    // bounding-circle broad phase for long vehicles.
    const math::q32_32 cosine = math::fixed_cos(crusherYaw);
    const math::q32_32 sine = math::fixed_sin(crusherYaw);
    const math::q32_32 localX = cosine * dx + sine * dy;
    const math::q32_32 localY = -sine * dx + cosine * dy;
    const math::q32_32 major = math::q32_32::max(
        math::q32_32{}, crusherGeometry->majorRadiusFixed) +
        victimSquishRadius;
    const math::q32_32 minor = math::q32_32::max(
        math::q32_32{}, crusherGeometry->minorRadiusFixed) +
        victimSquishRadius;
    if (crusherGeometry->shape == ObjectGeometryShape::Box) {
        return math::q32_32::abs(localX) <= major &&
            math::q32_32::abs(localY) <= minor;
    }
    if (major <= math::q32_32{} || minor <= math::q32_32{})
        return false;
    const math::q32_32 normalizedX = localX / major;
    const math::q32_32 normalizedY = localY / minor;
    return normalizedX * normalizedX + normalizedY * normalizedY <=
        math::q32_32{int32_t{1}};
}

} // namespace

namespace engine {

void ObjectSquishCollideSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity) const {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!type || !type->archetype || !type->archetype->squishCollidePlan) {
        return;
    }
    ObjectSquishCollideComponent value{
        .plan = type->archetype->squishCollidePlan,
    };
    if (ObjectSquishCollideComponent* existing =
            ecs::try_get<ObjectSquishCollideComponent>(registry, entity)) {
        *existing = std::move(value);
    } else {
        ecs::emplace<ObjectSquishCollideComponent>(
            registry, entity, std::move(value));
    }
}

void ObjectSquishCollideSystem::update(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectSpatialIndex& spatialIndex,
    const game::terrain::TerrainLogic& terrain,
    const PlayerRegistry& players, const ObjectSimulationRules& rules,
    uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& outDamage) const {
    const math::q32_32 framesPerSecond{
        static_cast<int32_t>(std::max<uint32_t>(1u,
            rules.logicFramesPerSecond))};
    const math::q32_32 significantAboveTerrainHeight =
        math::q32_32{int32_t{9}} *
        math::q32_32::abs(rules.gravityUnitsPerSecondSq) /
        (framesPerSecond * framesPerSecond);
    container::Vector<Candidate> victims;
    const auto victimView =
        ecs::view<const ObjectIdentityComponent,
                  const ObjectSquishCollideComponent,
                  const ObjectFixedTransformComponent>(registry);
    victims.reserve(victimView.size_hint());
    for (const ecs::entity entity : victimView) {
        const ObjectIdentityComponent& identity =
            victimView.template get<const ObjectIdentityComponent>(entity);
        const ObjectFixedTransformComponent& transform =
            victimView.template get<
                const ObjectFixedTransformComponent>(entity);
        if (!transform.authoritative || effectivelyUnavailable(
                registry, lifecycle, identity.id, entity)) {
            continue;
        }
        victims.push_back({.id = identity.id, .entity = entity});
    }
    std::sort(victims.begin(), victims.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.id < right.id;
              });
    if (victims.empty()) return;

    // Final-position queries cover ordinary contacts. Keep only the small
    // set of crushers that actually travelled this tick as an additional CCD
    // candidate set, matching FireWeaponCollide's swept-contact contract.
    container::Vector<ObjectId> movingCrushers;
    const auto crusherView =
        ecs::view<const ObjectIdentityComponent,
                  const ObjectFixedTransformComponent>(registry);
    for (const ecs::entity entity : crusherView) {
        const ObjectIdentityComponent& identity =
            crusherView.template get<const ObjectIdentityComponent>(entity);
        const ObjectFixedTransformComponent& transform =
            crusherView.template get<
                const ObjectFixedTransformComponent>(entity);
        if (!transform.authoritative || effectivelyUnavailable(
                registry, lifecycle, identity.id, entity)) {
            continue;
        }
        if (crusherLevel(registry, entity) == 0) continue;
        if (isObjectDisabledBy(registry, entity,
                               ObjectDisabledReason::Unmanned,
                               confirmedTick)) {
            continue;
        }
        // Legacy SquishCollide refuses crushers without PhysicsBehavior even
        // when some other controller happens to move their transform.
        const ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, entity);
        if (!physics) continue;
        if (physics->collisionStartTick == confirmedTick) {
            const LogicFixedVec3 end = transform.position;
            if (end.x != physics->collisionStartPosition.x ||
                end.y != physics->collisionStartPosition.y ||
                end.z != physics->collisionStartPosition.z) {
                movingCrushers.push_back(identity.id);
            }
        }
    }
    std::sort(movingCrushers.begin(), movingCrushers.end());

    container::Vector<ObjectId> nearby;
    for (const Candidate& victim : victims) {
        const ObjectSquishCollideComponent& squish =
            ecs::get<ObjectSquishCollideComponent>(registry, victim.entity);
        if (!squish.plan || squish.plan->rules.empty()) continue;
        const ObjectFixedTransformComponent& victimTransform =
            ecs::get<ObjectFixedTransformComponent>(registry, victim.entity);
        const LogicFixedVec3 victimPosition = victimTransform.position;
        const uint32_t authoredOrder = squish.plan->rules.front().authoredOrder;
        spatialIndex.queryRadiusFixed(
            victimPosition, math::q32_32{int32_t{1}}, nearby);
        nearby.insert(nearby.end(), movingCrushers.begin(),
                      movingCrushers.end());
        std::sort(nearby.begin(), nearby.end());
        nearby.erase(std::unique(nearby.begin(), nearby.end()), nearby.end());

        for (const ObjectId crusherId : nearby) {
            if (crusherId == victim.id) continue;
            const std::optional<ecs::entity> crusherEntity =
                lifecycle.entityFromId(crusherId);
            if (!crusherEntity || effectivelyUnavailable(
                    registry, lifecycle, crusherId, *crusherEntity) ||
                crusherLevel(registry, *crusherEntity) == 0 ||
                isObjectDisabledBy(registry, *crusherEntity,
                                   ObjectDisabledReason::Unmanned,
                                   confirmedTick)) {
                continue;
            }
            ObjectPhysicsComponent* crusherPhysicsState =
                ecs::try_get<ObjectPhysicsComponent>(registry,
                                                     *crusherEntity);
            if (!crusherPhysicsState) continue;
            if (crusherPhysicsState->overlapLedgerTick != confirmedTick) {
                crusherPhysicsState->previousOverlap =
                    crusherPhysicsState->currentOverlap;
                crusherPhysicsState->currentOverlap = INVALID_OBJECT_ID;
                crusherPhysicsState->overlapLedgerTick = confirmedTick;
            }
            const Candidate crusher{.id = crusherId,
                                    .entity = *crusherEntity};
            if (protectsIntentionalTarget(
                    registry, victim.entity, crusher.id, *squish.plan)) {
                continue;
            }
            if (areAllies(registry, players, crusher.entity,
                           victim.entity)) {
                continue;
            }
            const ObjectFixedTransformComponent* crusherTransform =
                ecs::try_get<ObjectFixedTransformComponent>(
                    registry, crusher.entity);
            if (!crusherTransform || !crusherTransform->authoritative)
                continue;
            const LogicFixedVec3 crusherPosition =
                crusherTransform->position;
            const math::q32_32 crusherYaw =
                crusherTransform->yawRadians;
            const ObjectGeometryComponent* crusherGeometry =
                ecs::try_get<ObjectGeometryComponent>(
                    registry, crusher.entity);
            const ObjectGeometryComponent* victimGeometry =
                ecs::try_get<ObjectGeometryComponent>(
                    registry, victim.entity);
            bool contact = squishContact(
                crusherPosition, crusherYaw, crusherGeometry,
                victimPosition, victimGeometry);
            if (!contact) {
                const ObjectPhysicsComponent* crusherPhysics =
                    ecs::try_get<ObjectPhysicsComponent>(
                        registry, crusher.entity);
                const ObjectPhysicsComponent* victimPhysics =
                    ecs::try_get<ObjectPhysicsComponent>(
                        registry, victim.entity);
                const LogicFixedVec3 crusherEnd = crusherPosition;
                const LogicFixedVec3 victimEnd = victimPosition;
                const LogicFixedVec3 crusherStart = crusherPhysics &&
                        crusherPhysics->collisionStartTick == confirmedTick
                    ? crusherPhysics->collisionStartPosition : crusherEnd;
                const LogicFixedVec3 victimStart = victimPhysics &&
                        victimPhysics->collisionStartTick == confirmedTick
                    ? victimPhysics->collisionStartPosition : victimEnd;
                const LogicFixedVec3 crusherTravel{
                    crusherEnd.x - crusherStart.x,
                    crusherEnd.y - crusherStart.y,
                    crusherEnd.z - crusherStart.z};
                const LogicFixedVec3 victimTravel{
                    victimEnd.x - victimStart.x,
                    victimEnd.y - victimStart.y,
                    victimEnd.z - victimStart.z};
                const LogicFixedVec3 relativeStart{
                    victimStart.x - crusherStart.x,
                    victimStart.y - crusherStart.y,
                    victimStart.z - crusherStart.z};
                const LogicFixedVec3 relativeMotion{
                    victimTravel.x - crusherTravel.x,
                    victimTravel.y - crusherTravel.y,
                    victimTravel.z - crusherTravel.z};
                const math::q32_32 motionSquared =
                    relativeMotion.x * relativeMotion.x +
                    relativeMotion.y * relativeMotion.y +
                    relativeMotion.z * relativeMotion.z;
                if (motionSquared > math::q32_32{}) {
                    const math::q32_32 one{int32_t{1}};
                    const math::q32_32 time = std::clamp(
                        -(relativeStart.x * relativeMotion.x +
                          relativeStart.y * relativeMotion.y +
                          relativeStart.z * relativeMotion.z) /
                            motionSquared,
                        math::q32_32{}, one);
                    const LogicFixedVec3 a{
                        crusherStart.x + crusherTravel.x * time,
                        crusherStart.y + crusherTravel.y * time,
                        crusherStart.z + crusherTravel.z * time};
                    const LogicFixedVec3 b{
                        victimStart.x + victimTravel.x * time,
                        victimStart.y + victimTravel.y * time,
                        victimStart.z + victimTravel.z * time};
                    contact = squishContact(
                        a, crusherYaw, crusherGeometry,
                        b, victimGeometry);
                }
            }
            if (!contact) {
                continue;
            }
            if (hasEarlierConsumingCrateCollisionPriority(
                    registry, lifecycle, terrain, players,
                    victim.id, victim.entity, crusher.id, crusher.entity,
                    authoredOrder, significantAboveTerrainHeight)) {
                continue;
            }
            if (!movingTowardVictim(
                    registry, crusher.entity, crusherPosition,
                    crusherYaw, victimPosition)) {
                continue;
            }
            ObjectPhysicsComponent& crusherPhysics =
                ecs::get<ObjectPhysicsComponent>(registry, crusher.entity);
            const bool firstOverlap =
                crusherPhysics.currentOverlap != victim.id &&
                crusherPhysics.previousOverlap != victim.id;
            crusherPhysics.currentOverlap = victim.id;
            if (firstOverlap) {
                outDamage.push_back({
                    .target = victim.id,
                    .source = crusher.id,
                    .sourceSequence = authoredOrder,
                    .causalGroup = crusher.id,
                    .amount = math::q32_32{},
                    .damageType = game::DamageType::CRUSH,
                    .deathType = game::DeathType::CRUSHED,
                    .emitZeroDamageFeedback = true,
                    .confirmedTick = confirmedTick,
                });
            }
            outDamage.push_back({
                .target = victim.id,
                .source = crusher.id,
                .sourceSequence = authoredOrder,
                .causalGroup = crusher.id,
                .damageType = game::DamageType::CRUSH,
                .deathType = game::DeathType::CRUSHED,
                .forceKill = true,
                .confirmedTick = confirmedTick,
            });
            ecs::remove<ObjectUndetectedDefectorComponent>(registry,
                                                            victim.entity);
            break;
        }
    }
}

} // namespace engine
