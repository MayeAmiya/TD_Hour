#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/combat/ObjectWeaponDamage.h"

#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cmath>
#include <limits>
namespace engine {
namespace {

const math::q32_32 kHugeWeaponDamage = math::q32_32::from_raw(
    std::numeric_limits<int64_t>::max());
const math::q32_32 kFixedConeEpsilonSquared =
    math::q32_32::from_fraction(1, 100000000);
constexpr math::q32_32 kFixedPi = math::q32_32::from_raw(13493037704ll);

using container::asciiEqualIgnoreCase;

[[nodiscard]] bool containsKind(const ObjectKindOfComponent* kinds,
                                game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

[[nodiscard]] bool isAirborneForDamage(const ObjectKindOfComponent* kinds,
                                       const ObjectAirborneComponent* airborne) noexcept {
    return (airborne && airborne->isAirborne) ||
           containsKind(kinds, game::ObjectKindOf::Parachute);
}

[[nodiscard]] math::q32_32 scaledWeaponValueFixed(
    math::q32_32 value, const game::WeaponBonus& bonus,
    game::WeaponBonusField field) noexcept {
    return bonus.scale(value, field);
}

[[nodiscard]] math::q32_32 boundarySphereDistanceSquared(
    const LogicFixedVec3& point, LogicFixedVec3 target,
    const ObjectGeometryComponent* geometry) noexcept {
    if (geometry && geometry->shape != ObjectGeometryShape::Sphere) {
        target.z += math::q32_32::max(
            math::q32_32{}, geometry->heightFixed) /
            math::q32_32{int32_t{2}};
    }
    const math::q32_32 dx = target.x - point.x;
    const math::q32_32 dy = target.y - point.y;
    const math::q32_32 dz = target.z - point.z;
    const math::q32_32 centerDistance = math::q32_32::sqrt(
        dx * dx + dy * dy + dz * dz);
    const math::q32_32 sphereRadius = geometry
        ? math::q32_32::max(math::q32_32{},
              geometry->boundingSphereRadiusFixed)
        : math::q32_32{};
    const math::q32_32 boundaryDistance = math::q32_32::max(
        math::q32_32{}, centerDistance - sphereRadius);
    return boundaryDistance * boundaryDistance;
}

[[nodiscard]] bool isWithinDamageCone(const game::WeaponTemplate& weapon,
                                       const ObjectWeaponImpact& impact,
                                       const LogicFixedVec3& source,
                                       math::q32_32 sourceYaw,
                                       const LogicFixedVec3& victim) noexcept {
    if (weapon.fixed.radiusDamageAngleRadians >= kFixedPi) return true;
    const LogicFixedVec3 damageDirection{
        victim.x - source.x, victim.y - source.y, victim.z - source.z};
    const math::q32_32 damageLengthSquared =
        damageDirection.x * damageDirection.x +
        damageDirection.y * damageDirection.y +
        damageDirection.z * damageDirection.z;
    if (damageLengthSquared <= kFixedConeEpsilonSquared) return true;
    const math::q32_32 damageLength =
        math::q32_32::sqrt(damageLengthSquared);
    if (damageLength <= math::q32_32{}) return true;

    LogicFixedVec3 forward;
    if (impact.fixedForward) {
        // A projectile carries its normalized 3D tangent. Transform.rotation
        // is only a renderer-facing 2D projection and cannot represent pitch.
        forward = *impact.fixedForward;
    } else {
        const math::q32_32_sincos sourceDirection = math::fixed_sincos(
            sourceYaw);
        forward = {sourceDirection.cosine, sourceDirection.sine,
                   math::q32_32{}};
    }
    const math::q32_32 cosine =
        (forward.x * damageDirection.x +
         forward.y * damageDirection.y +
         forward.z * damageDirection.z) / damageLength;
    const math::q32_32 threshold = math::fixed_cos(
        math::q32_32::max(
            math::q32_32{}, weapon.fixed.radiusDamageAngleRadians));
    return cosine >= threshold;
}

[[nodiscard]] bool allowsSecondaryDamage(const game::WeaponTemplate& weapon,
                                         const ecs::registry& registry,
                                         const ObjectWeaponImpact& impact,
                                         ecs::entity victimEntity, ObjectId victimId,
                                         const PlayerRegistry* players,
                                         bool& killsPhysicalSource) noexcept {
    killsPhysicalSource = false;
    if (impact.filterSource == victimId &&
        (weapon.radiusDamageAffects &
         game::weaponAffectsBit(game::WeaponAffectsTarget::KillsSelf)) != 0) {
        killsPhysicalSource = true;
        return true;
    }

    const ObjectKindOfComponent* victimKinds =
        ecs::try_get<ObjectKindOfComponent>(registry, victimEntity);
    const ObjectAirborneComponent* victimAirborne =
        ecs::try_get<ObjectAirborneComponent>(registry, victimEntity);
    if ((weapon.radiusDamageAffects &
         game::weaponAffectsBit(game::WeaponAffectsTarget::NotAirborne)) != 0 &&
        isAirborneForDamage(victimKinds, victimAirborne)) {
        return false;
    }

    // RefCode skips the physical source and its producer unless SELF is
    // explicitly enabled. It then still evaluates normal allied/enemy masks,
    // so do not short-circuit SELF to true here.
    const bool permitsSelf = (weapon.radiusDamageAffects &
        game::weaponAffectsBit(game::WeaponAffectsTarget::Self)) != 0;
    if (!permitsSelf && (impact.filterSource == victimId || impact.producer == victimId)) {
        return false;
    }

    const OwnerComponent* sourceOwner =
        ecs::try_get<OwnerComponent>(registry, impact.filterSourceEntity);
    const OwnerComponent* victimOwner = ecs::try_get<OwnerComponent>(registry, victimEntity);
    PlayerRelationship relationship = PlayerRelationship::Neutral;
    if (sourceOwner && victimOwner && sourceOwner->player && victimOwner->player) {
        if (players) {
            relationship = relationshipBetweenObjects(
                registry, *players, victimEntity,
                impact.filterSourceEntity);
        } else if (sourceOwner->player == victimOwner->player) {
            relationship = PlayerRelationship::Allies;
        } else {
            relationship = PlayerRelationship::Enemies;
        }
    }

    if ((weapon.radiusDamageAffects &
         game::weaponAffectsBit(game::WeaponAffectsTarget::NotSimilar)) != 0 &&
        relationship == PlayerRelationship::Allies) {
        const ThingTemplateComponent* sourceTemplate =
            ecs::try_get<ThingTemplateComponent>(registry, impact.filterSourceEntity);
        const ThingTemplateComponent* victimTemplate =
            ecs::try_get<ThingTemplateComponent>(registry, victimEntity);
        // This is intentionally a conservative template-name comparison
        // until the content compiler exposes RefCode's full reskin/override
        // equivalence identity.
        if (sourceTemplate && victimTemplate && sourceTemplate->name == victimTemplate->name) {
            return false;
        }
    }

    game::WeaponAffectsTarget required = game::WeaponAffectsTarget::Neutrals;
    if (relationship == PlayerRelationship::Allies) required = game::WeaponAffectsTarget::Allies;
    else if (relationship == PlayerRelationship::Enemies) required = game::WeaponAffectsTarget::Enemies;
    return (weapon.radiusDamageAffects & game::weaponAffectsBit(required)) != 0;
}

} // namespace

void appendWeaponImpactDamage(ecs::registry& registry, const ObjectLifecycle& lifecycle,
                              const ObjectSpatialIndex* spatialIndex,
                              const PlayerRegistry* players,
                              const ObjectWeaponImpact& impact,
                              container::Vector<ObjectDamageRequest>& outDamage) {
    container::Vector<ObjectId> victims;
    appendWeaponImpactDamage(
        registry, lifecycle, spatialIndex, players, impact, outDamage,
        victims);
}

void appendWeaponImpactDamage(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSpatialIndex* spatialIndex, const PlayerRegistry* players,
    const ObjectWeaponImpact& impact,
    container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectId>& victims) {
    victims.clear();
    const game::WeaponTemplate* weapon = impact.weapon;
    if (!weapon || !impact.filterSource || impact.filterSourceEntity == ecs::null) return;
    const TransformComponent* sourceTransform =
        ecs::try_get<TransformComponent>(registry, impact.filterSourceEntity);
    if (!sourceTransform) return;
    const LogicFixedVec3 sourcePosition = readAuthoritativeObjectPosition(
        registry, impact.filterSourceEntity, *sourceTransform);
    const math::q32_32 sourceYaw = readAuthoritativeObjectYaw(
        registry, impact.filterSourceEntity, *sourceTransform);

    const math::q32_32 primaryRadiusFixed = math::q32_32::max(
        math::q32_32{}, scaledWeaponValueFixed(
            weapon->fixed.primaryDamageRadius, impact.bonus,
            game::WeaponBonusField::Radius));
    const math::q32_32 secondaryRadiusFixed = math::q32_32::max(
        math::q32_32{}, scaledWeaponValueFixed(
            weapon->fixed.secondaryDamageRadius, impact.bonus,
            game::WeaponBonusField::Radius));
    const math::q32_32 queryRadius = math::q32_32::max(
        primaryRadiusFixed, secondaryRadiusFixed);
    const LogicFixedVec3& fixedImpactPosition = impact.impactPosition;
    if (queryRadius > math::q32_32{} && spatialIndex) {
        spatialIndex->querySphereRadiusFixed(
            fixedImpactPosition, queryRadius, victims);
    }
    if (impact.primaryTarget) {
        // A direct object attack overrides damage position with the primary
        // target in RefCode; retain it even if a caller's broad phase is not
        // yet rebuilt.
        victims.push_back(impact.primaryTarget);
    } else if ((weapon->radiusDamageAffects &
                (game::weaponAffectsBit(game::WeaponAffectsTarget::Self) |
                 game::weaponAffectsBit(game::WeaponAffectsTarget::KillsSelf))) != 0) {
        victims.push_back(impact.filterSource);
    }

    if (queryRadius == math::q32_32{} &&
        (weapon->radiusDamageAffects &
         game::weaponAffectsBit(game::WeaponAffectsTarget::KillsSelf)) != 0) {
        // Legacy zero-radius suicide damage returns immediately after the
        // physical source receives huge ordinary damage.
        victims.assign(1, impact.filterSource);
    } else {
        std::sort(victims.begin(), victims.end());
        victims.erase(std::unique(victims.begin(), victims.end()), victims.end());
    }

    const math::q32_32 primaryRadiusSquared =
        primaryRadiusFixed * primaryRadiusFixed;
    const math::q32_32 secondaryRadiusSquared =
        secondaryRadiusFixed * secondaryRadiusFixed;
    for (const ObjectId victimId : victims) {
        const std::optional<ecs::entity> victimEntity = lifecycle.entityFromId(victimId);
        if (!victimEntity) continue;
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, *victimEntity);
        const TransformComponent* victimTransform =
            ecs::try_get<TransformComponent>(registry, *victimEntity);
        if (!health || !victimTransform || !health->acceptsDamage || health->effectivelyDead) continue;

        const bool primaryVictim = impact.primaryTarget && victimId == impact.primaryTarget;
        const ObjectGeometryComponent* victimGeometry =
            ecs::try_get<ObjectGeometryComponent>(registry, *victimEntity);
        const LogicFixedVec3 victimPosition =
            readAuthoritativeObjectPosition(
                registry, *victimEntity, *victimTransform);
        const math::q32_32 distanceSquared = boundarySphereDistanceSquared(
            fixedImpactPosition, victimPosition, victimGeometry);
        if (queryRadius > math::q32_32{} &&
            distanceSquared > secondaryRadiusSquared &&
            distanceSquared > primaryRadiusSquared) {
            continue;
        }
        if (queryRadius == math::q32_32{} && !primaryVictim &&
            victimId != impact.filterSource) continue;

        bool killsPhysicalSource = false;
        if (!primaryVictim && !allowsSecondaryDamage(*weapon, registry, impact, *victimEntity,
                                                      victimId, players, killsPhysicalSource)) {
            continue;
        }
        // The directional cone is evaluated for every iterator victim,
        // including a primary direct target.
        if (!isWithinDamageCone(
                *weapon, impact, sourcePosition, sourceYaw,
                victimPosition)) continue;

        math::q32_32 damageAmount = distanceSquared <= primaryRadiusSquared
            ? scaledWeaponValueFixed(weapon->fixed.primaryDamage, impact.bonus,
                                     game::WeaponBonusField::Damage)
            : scaledWeaponValueFixed(weapon->fixed.secondaryDamage, impact.bonus,
                                     game::WeaponBonusField::Damage);
        if (killsPhysicalSource) {
            damageAmount = kHugeWeaponDamage;
        }
        LogicFixedVec3 shockDirection{
            victimPosition.x - sourcePosition.x,
            victimPosition.y - sourcePosition.y,
            victimPosition.z - sourcePosition.z};
        constexpr math::q32_32 kShockDirectionEpsilon =
            math::q32_32::from_fraction(1, 10000);
        if (math::q32_32::abs(shockDirection.x) < kShockDirectionEpsilon &&
            math::q32_32::abs(shockDirection.y) < kShockDirectionEpsilon &&
            math::q32_32::abs(shockDirection.z) < kShockDirectionEpsilon) {
            shockDirection = {math::q32_32{}, math::q32_32{},
                              math::q32_32{int32_t{1}}};
        }
        outDamage.push_back({
            .target = victimId,
            .source = impact.damageCredit ? impact.damageCredit : impact.filterSource,
            .sourceSequence = impact.sourceSequence,
            .causalGroup = impact.causalGroup,
            .amount = damageAmount,
            .damageType = weapon->damageType,
            .damageStatusMask = weapon->damageStatusMask,
            .deathType = weapon->deathType,
            .shockWaveAmount = weapon->fixed.shockWaveAmount,
            .shockWaveRadius = weapon->fixed.shockWaveRadius,
            .shockWaveTaperOff = weapon->fixed.shockWaveTaperOff,
            .shockWaveVectorX = shockDirection.x,
            .shockWaveVectorY = shockDirection.y,
            .shockWaveVectorZ = shockDirection.z,
            .forceKill = false,
            .confirmedTick = impact.confirmedTick,
        });
    }
}

} // namespace engine
