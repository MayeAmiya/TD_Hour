#include "game/object/simulation/combat/ObjectProjectileSystemDetail.h"
#include "game/object/definition/ObjectArchetype.h"

#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/combat/ObjectWeaponDamage.h"
#include "game/object/simulation/combat/ObjectHistoricWeaponLedger.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>

namespace engine::object_projectile_detail {

[[nodiscard]] bool containsKind(const ObjectKindOfComponent* kinds,
                                game::ObjectKindOf token) noexcept {
    return kinds && game::objectHasKind(kinds->mask, token);
}

[[nodiscard]] PlayerRelationship relationshipFor(const ecs::registry& registry,
                                                  ecs::entity source, ecs::entity target,
                                                  const PlayerRegistry* players) noexcept {
    const OwnerComponent* sourceOwner = ecs::try_get<OwnerComponent>(registry, source);
    const OwnerComponent* targetOwner = ecs::try_get<OwnerComponent>(registry, target);
    if (!sourceOwner || !targetOwner || !sourceOwner->player || !targetOwner->player) {
        return PlayerRelationship::Neutral;
    }
    // This is deliberately source -> target. PlayerRelationshipMatrix is
    // directed, and Weapon::shouldProjectileCollideWith asks the physical
    // projectile for its relationship to the candidate rather than inverse.
    return players
        ? relationshipBetweenObjects(registry, *players, source, target)
        : sourceOwner->player == targetOwner->player
            ? PlayerRelationship::Allies
            : PlayerRelationship::Enemies;
}

[[nodiscard]] bool containsObjectId(container::Span<const ObjectId> values,
                                    ObjectId sought) noexcept {
    return sought && std::find(values.begin(), values.end(), sought) != values.end();
}

[[nodiscard]] bool airfieldReservesTarget(
    const ObjectAirfieldComponent* airfield, ObjectId target) noexcept {
    if (!airfield || !target) return false;
    for (const ObjectAirfieldParkingRuntime& parking : airfield->parkingPlaces) {
        if (containsObjectId(parking.spaces, target)) return true;
    }
    for (const ObjectAirfieldFlightDeckRuntime& deck : airfield->flightDecks) {
        if (containsObjectId(deck.spaces, target)) return true;
    }
    return false;
}

[[nodiscard]] bool hasSneakyTargetingOffset(
    const ObjectAirfieldComponent* aircraft, uint64_t confirmedTick) noexcept {
    if (!aircraft) return false;
    return std::any_of(
        aircraft->jetAi.begin(), aircraft->jetAi.end(),
        [confirmedTick](const ObjectJetAiRuntime& jet) {
            return jet.attackersMissExpiresTick != 0 &&
                   confirmedTick < jet.attackersMissExpiresTick;
        });
}

[[nodiscard]] bool isProjectileCollisionAllowed(const ecs::registry& registry,
                                                 const ObjectLifecycle& lifecycle,
                                                 ecs::entity projectileEntity,
                                                 const ObjectProjectileComponent& projectile,
                                                 const game::WeaponTemplate& weapon,
                                                 ObjectId candidateId,
                                                 ecs::entity candidate,
                                                 const PlayerRegistry* players,
                                                 uint64_t confirmedTick) noexcept {
    const ObjectKindOfComponent* kinds = ecs::try_get<ObjectKindOfComponent>(registry, candidate);

    // These exclusions precede ProjectileCollidesWith in RefCode. The
    // intended victim bypass is handled by the caller before entering here.
    if (const std::optional<ecs::entity> launcher =
            lifecycle.entityFromIdIncludingPending(projectile.launcher)) {
        const ObjectContainedByComponent* contained =
            ecs::try_get<ObjectContainedByComponent>(registry, *launcher);
        if (contained && contained->container == candidateId) return false;
    }
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, candidate);
    if (status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::NoCollisions))) {
        return false;
    }
    if ((weapon.damageType == game::DamageType::FLAME ||
         weapon.damageType == game::DamageType::PARTICLE_BEAM) &&
        status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::Burned))) {
        return false;
    }
    const ObjectAirfieldComponent* airfield =
        ecs::try_get<ObjectAirfieldComponent>(registry, candidate);
    if (containsKind(kinds, game::ObjectKindOf::FsAirfield) &&
        airfieldReservesTarget(airfield, projectile.intendedTarget)) {
        return false;
    }
    if (hasSneakyTargetingOffset(airfield, confirmedTick)) return false;

    game::WeaponCollideMask required = 0;
    const PlayerRelationship relationship = relationshipFor(registry, projectileEntity, candidate, players);
    if (relationship == PlayerRelationship::Allies) {
        required |= game::weaponCollideBit(game::WeaponCollideTarget::Allies);
    } else if (relationship == PlayerRelationship::Enemies) {
        required |= game::weaponCollideBit(game::WeaponCollideTarget::Enemies);
    }
    if (containsKind(kinds, game::ObjectKindOf::Structure)) {
        const OwnerComponent* projectileOwner = ecs::try_get<OwnerComponent>(registry, projectileEntity);
        const OwnerComponent* candidateOwner = ecs::try_get<OwnerComponent>(registry, candidate);
        required |= projectileOwner && candidateOwner && projectileOwner->player == candidateOwner->player
            ? game::weaponCollideBit(game::WeaponCollideTarget::ControlledStructures)
            : game::weaponCollideBit(game::WeaponCollideTarget::Structures);
    }
    if (containsKind(kinds, game::ObjectKindOf::Shrubbery)) {
        required |= game::weaponCollideBit(game::WeaponCollideTarget::Shrubbery);
    }
    if (containsKind(kinds, game::ObjectKindOf::Projectile)) {
        required |= game::weaponCollideBit(game::WeaponCollideTarget::Projectiles);
    }
    if (containsKind(kinds, game::ObjectKindOf::SmallMissile)) {
        required |= game::weaponCollideBit(game::WeaponCollideTarget::SmallMissiles);
    }
    if (containsKind(kinds, game::ObjectKindOf::BallisticMissile)) {
        required |= game::weaponCollideBit(game::WeaponCollideTarget::BallisticMissiles);
    }
    const ThingTemplateComponent* candidateTemplate =
        ecs::try_get<ThingTemplateComponent>(registry, candidate);
    if (candidateTemplate && candidateTemplate->archetype &&
        candidateTemplate->archetype->templateData.fenceWidthFixed >
            math::q32_32{}) {
        required |= game::weaponCollideBit(game::WeaponCollideTarget::Walls);
    }
    return (weapon.projectileCollidesWith & required) != 0;
}

[[nodiscard]] std::optional<Fixed> sweptSphereTimeOfImpact(const FixedVec3& start,
                                                            const FixedVec3& end,
                                                            const FixedVec3& center,
                                                            Fixed radius) noexcept {
    const FixedVec3 direction = subtract(end, start);
    const FixedVec3 offset = subtract(start, center);
    const Fixed a = dot(direction, direction);
    const Fixed c = dot(offset, offset) - radius * radius;
    if (c <= kFixedZero) return kFixedZero;
    if (a <= kFixedSegmentEpsilon) return std::nullopt;
    const Fixed b = kFixedTwo * dot(direction, offset);
    const Fixed discriminant = b * b - kFixedFour * a * c;
    if (discriminant < kFixedZero) return std::nullopt;
    const Fixed time = (-b - Fixed::sqrt(discriminant)) / (kFixedTwo * a);
    return time >= kFixedZero && time <= kFixedOne ? std::optional<Fixed>{time} : std::nullopt;
}

[[nodiscard]] std::optional<ProjectileCollision> findProjectileCollision(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSpatialIndex* spatialIndex, const PlayerRegistry* players,
    ecs::entity projectileEntity, ObjectId projectileId,
    const ObjectProjectileComponent& projectile,
    const FixedVec3& start, const FixedVec3& destination,
    Fixed projectileRadius, const game::WeaponTemplate* weapon,
    bool useWeaponFilter,
    container::Span<const ObjectId> currentProjectileIds,
    container::Vector<ObjectId>& candidates,
    uint64_t confirmedTick) {
    candidates.clear();
    if (spatialIndex) {
        const FixedVec3 midpoint = scale(add(start, destination), kFixedHalf);
        const Fixed segmentRadius = planarLength(subtract(destination, start)) * kFixedHalf +
            projectileRadius;
        spatialIndex->querySphereRadiusFixed(
            midpoint, segmentRadius, candidates);
    }
    const game::WeaponCollideMask projectileMask =
        game::weaponCollideBit(game::WeaponCollideTarget::Projectiles) |
        game::weaponCollideBit(game::WeaponCollideTarget::SmallMissiles) |
        game::weaponCollideBit(game::WeaponCollideTarget::BallisticMissiles);
    if (!useWeaponFilter || !weapon ||
        (weapon->projectileCollidesWith & projectileMask) != 0) {
        candidates.insert(candidates.end(), currentProjectileIds.begin(),
                          currentProjectileIds.end());
    }
    if (projectile.intendedTarget) candidates.push_back(projectile.intendedTarget);
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    const ObjectGeometryComponent* projectileGeometry =
        ecs::try_get<ObjectGeometryComponent>(registry, projectileEntity);
    const TransformComponent* projectileTransform =
        ecs::try_get<TransformComponent>(registry, projectileEntity);
    const ObjectPhysicsComponent* projectilePhysics =
        ecs::try_get<ObjectPhysicsComponent>(registry, projectileEntity);
    const Fixed projectileYaw = projectilePhysics &&
            projectilePhysics->ownsAttitude
        ? projectilePhysics->yaw
        : projectileTransform
            ? readAuthoritativeObjectYaw(
                  registry, projectileEntity, *projectileTransform)
            : Fixed{};
    std::optional<ProjectileCollision> best;
    for (const ObjectId otherId : candidates) {
        if (!otherId || otherId == projectileId || otherId == projectile.launcher) continue;
        const std::optional<ecs::entity> other = lifecycle.entityFromId(otherId);
        if (!other || lifecycle.isPendingDestroy(otherId)) continue;
        const ObjectProjectileComponent* otherProjectile =
            ecs::try_get<ObjectProjectileComponent>(registry, *other);
        if (otherProjectile && otherProjectile->detonated) continue;
        const TransformComponent* otherTransform =
            ecs::try_get<TransformComponent>(registry, *other);
        const ObjectGeometryComponent* otherGeometry =
            ecs::try_get<ObjectGeometryComponent>(registry, *other);
        if (!otherTransform) continue;
        if (useWeaponFilter && (!weapon || (otherId != projectile.intendedTarget &&
            !isProjectileCollisionAllowed(
                registry, lifecycle, projectileEntity, projectile, *weapon,
                otherId, *other, players, confirmedTick)))) {
            continue;
        }
        std::optional<Fixed> time;
        if (projectileGeometry && otherGeometry) {
            Fixed exactTime{};
            ObjectCollisionContact contact;
            const FixedVec3 otherPosition =
                readAuthoritativeObjectPosition(
                    registry, *other, *otherTransform);
            FixedVec3 otherStart = otherPosition;
            const ObjectPhysicsComponent* otherPhysics =
                ecs::try_get<ObjectPhysicsComponent>(registry, *other);
            if (otherPhysics &&
                otherPhysics->collisionStartTick == confirmedTick) {
                otherStart = otherPhysics->collisionStartPosition;
            }
            const Fixed otherYaw = otherPhysics && otherPhysics->ownsAttitude
                ? otherPhysics->yaw
                : readAuthoritativeObjectYaw(
                      registry, *other, *otherTransform);
            if (computeObjectSweptCollisionContact(
                    start, destination, projectileYaw, *projectileGeometry,
                    otherStart, otherPosition,
                    otherYaw,
                    *otherGeometry, exactTime, contact)) {
                time = exactTime;
            }
        } else {
            FixedVec3 center = readAuthoritativeObjectPosition(
                registry, *other, *otherTransform);
            if (otherGeometry &&
                otherGeometry->shape != ObjectGeometryShape::Sphere) {
                center.z += Fixed::max(
                    kFixedZero, otherGeometry->heightFixed) /
                    Fixed{int32_t{2}};
            }
            const Fixed otherRadius = otherGeometry
                ? Fixed::max(kFixedZero,
                      otherGeometry->boundingSphereRadiusFixed)
                : kFixedZero;
            time = sweptSphereTimeOfImpact(
                start, destination, center, projectileRadius + otherRadius);
        }
        if (!time || (best && (*time > best->time ||
            (*time == best->time && otherId > best->target)))) {
            continue;
        }
        best = ProjectileCollision{.time = *time, .target = otherId};
    }
    return best;
}

[[nodiscard]] bool kindOfMaskMatches(
    const ObjectKindOfComponent* kinds,
    const game::ObjectKindOfMask& required,
    const game::ObjectKindOfMask& forbidden) noexcept {
    return kinds && game::objectKindsMatch(kinds->mask, required, forbidden);
}

[[nodiscard]] bool isClearableGarrison(
    const ecs::registry& registry, ecs::entity target) noexcept {
    const ObjectContainmentRuntimeComponent* containment =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, target);
    if (!containment || !containment->plan) return false;
    const auto garrison = std::find_if(
        containment->plan->rules.begin(), containment->plan->rules.end(),
        [](const ObjectContainmentRule& rule) {
            return rule.kind == ObjectContainmentKind::Garrison;
        });
    return garrison != containment->plan->rules.end() &&
        !garrison->immuneToClearBuildingAttacks;
}

[[nodiscard]] bool clearGarrisonOnImpact(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    ObjectId projectileId, ecs::entity projectileEntity,
    ObjectProjectileComponent& projectile, ObjectId targetId,
    const FixedVec3& impact, uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectProjectileEvent>& events) {
    if (projectile.garrisonHitKillCount == 0 || !targetId) return false;
    const std::optional<ecs::entity> target = lifecycle.entityFromId(targetId);
    if (!target || !isClearableGarrison(registry, *target)) return false;
    const ObjectContainmentComponent* contents =
        ecs::try_get<ObjectContainmentComponent>(registry, *target);
    if (!contents || contents->objects.empty()) return false;

    uint32_t killed = 0;
    for (const ObjectContainedObjectRecord& record : contents->objects) {
        if (killed >= projectile.garrisonHitKillCount) break;
        const std::optional<ecs::entity> occupant =
            lifecycle.entityFromId(record.object);
        if (!occupant || lifecycle.isPendingDestroy(record.object)) continue;
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, *occupant);
        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(registry, *occupant);
        if (!health || health->effectivelyDead ||
            !kindOfMaskMatches(kinds,
                               projectile.garrisonHitRequiredKindMask,
                               projectile.garrisonHitForbiddenKindMask)) {
            continue;
        }
        outDamage.push_back({
            .target = record.object,
            .source = projectile.launcher,
            .sourceSequence = projectile.sourceShotSequence,
            .causalGroup = projectileId,
            .amount = health->maximumFixed,
            .damageType = game::DamageType::UNRESISTABLE,
            .deathType = game::DeathType::NORMAL,
            .forceKill = true,
            .confirmedTick = confirmedTick,
        });
        ++killed;
    }
    if (killed == 0) return false;

    projectile.detonated = true;
    projectile.position = impact;
    if (RenderModelComponent* visual =
            ecs::try_get<RenderModelComponent>(registry, projectileEntity)) {
        visual->hidden = true;
    }
    static_cast<void>(lifecycle.requestDestroy(
        projectileId, ObjectDestroyReason::Combat, confirmedTick));
    const TransformComponent* targetTransform =
        ecs::try_get<TransformComponent>(registry, *target);
    const OwnerComponent* projectileOwner =
        ecs::try_get<OwnerComponent>(registry, projectileEntity);
    const LogicFixedVec3 eventPosition = targetTransform
        ? readAuthoritativeObjectPosition(registry, *target, *targetTransform)
        : impact;
    events.push_back({
        .kind = ObjectProjectileEventKind::GarrisonCleared,
        .projectile = projectileId,
        .launcher = projectile.launcher,
        .owner = projectileOwner
            ? projectileOwner->player : INVALID_PLAYER_ID,
        .target = targetId,
        .sourceShotSequence = projectile.sourceShotSequence,
        .detonationWeapon = projectile.detonationWeapon,
        .position = eventPosition,
        .fxListName = projectile.garrisonHitFx,
        .confirmedTick = confirmedTick,
    });
    return true;
}

[[nodiscard]] std::optional<FixedVec3> bridgeLayerImpact(
    ObjectProjectileComponent& projectile,
    const game::terrain::TerrainLogic& terrain,
    const FixedVec3& destination) noexcept {
    const game::terrain::TerrainPathfindLayerId oldLayer = projectile.pathfindLayer;
    const game::terrain::TerrainPathfindLayerId newLayer =
        terrain.highestPathfindLayerAtRaw(
            destination.x.raw(), destination.y.raw(), destination.z.raw());
    projectile.pathfindLayer = newLayer;
    if (oldLayer == game::terrain::kGroundPathfindLayer ||
        newLayer != game::terrain::kGroundPathfindLayer ||
        terrain.highestPathfindLayerAtXYRaw(
            destination.x.raw(), destination.y.raw()) != oldLayer) {
        return std::nullopt;
    }
    const std::optional<int64_t> bridgeHeight = terrain.pathfindLayerHeightRawAt(
        oldLayer, destination.x.raw(), destination.y.raw());
    if (!bridgeHeight) return std::nullopt;
    return FixedVec3{
        destination.x, destination.y,
        Fixed::from_raw(*bridgeHeight) + Fixed{int32_t{2}}};
}

void synchronizeProjectileTerrainLayer(
    ecs::registry& registry, ecs::entity entity,
    const ObjectProjectileComponent& projectile,
    uint64_t confirmedTick) {
    ObjectTerrainLayerComponent* layer =
        ecs::try_get<ObjectTerrainLayerComponent>(registry, entity);
    if (layer) {
        static_cast<void>(layer->assign(projectile.pathfindLayer,
                                        confirmedTick));
    } else {
        ecs::emplace<ObjectTerrainLayerComponent>(
            registry, entity,
            ObjectTerrainLayerComponent{
                .pathfindLayer = projectile.pathfindLayer,
                .lastChangedTick = confirmedTick,
            });
    }
}

void detonate(ecs::registry& registry, ObjectLifecycle& lifecycle,
              const GameContentSnapshot& content, const ObjectSpatialIndex* spatialIndex,
              const PlayerRegistry* players, ObjectId projectileId, ecs::entity projectileEntity,
              ObjectProjectileComponent& projectile, TransformComponent& transform,
              const FixedVec3& position, ObjectProjectileEventKind kind, ObjectId collidedTarget,
              uint32_t logicFramesPerSecond, uint64_t confirmedTick,
              container::Vector<ObjectId>& damageVictimScratch,
              container::Vector<ObjectDamageRequest>& outDamage,
              container::Vector<ObjectHistoricBonusWeaponFire>& outHistoricBonusWeapons,
              container::Vector<ObjectProjectileEvent>& events,
              bool applyDetonationWeapon, bool deferDestruction) {
    if (projectile.detonated) return;
    projectile.detonated = true;
    projectile.position = position;
    writeAuthoritativeObjectPosition(
        registry, projectileEntity, position);
    const OwnerComponent* detonationOwner =
        ecs::try_get<OwnerComponent>(registry, projectileEntity);
    const PrimaryTeamComponent* detonationTeam =
        ecs::try_get<PrimaryTeamComponent>(registry, projectileEntity);
    const ObjectPhysicsComponent* detonationPhysics =
        ecs::try_get<ObjectPhysicsComponent>(registry, projectileEntity);
    const ObjectVeterancyComponent* detonationVeterancy =
        ecs::try_get<ObjectVeterancyComponent>(registry, projectileEntity);
    const ObjectAirborneComponent* detonationAirborne =
        ecs::try_get<ObjectAirborneComponent>(registry, projectileEntity);
    const LogicFixedVec3 detonationVelocity = detonationPhysics
        ? detonationPhysics->velocityUnitsPerSecond : LogicFixedVec3{};
    const ObjectPhysicsComponent::Scalar detonationYaw =
        detonationPhysics && detonationPhysics->ownsAttitude
        ? detonationPhysics->yaw
        : readAuthoritativeObjectYaw(
              registry, projectileEntity, transform);
    const ObjectPhysicsComponent::Scalar detonationPitch = detonationPhysics
        ? detonationPhysics->pitch : ObjectPhysicsComponent::Scalar{};
    const ObjectPhysicsComponent::Scalar detonationRoll = detonationPhysics
        ? detonationPhysics->roll : ObjectPhysicsComponent::Scalar{};
    std::optional<LogicFixedVec3> detonationForward;
    if (projectile.tumbleRandomly && detonationPhysics &&
        detonationPhysics->ownsAttitude &&
        detonationPhysics->orientationBasisValid) {
        detonationForward = normalizedOr(
            detonationPhysics->orientationX,
            projectile.hasFlightPathForward
                ? projectile.flightPathForward
                : FixedVec3{kFixedOne, kFixedZero, kFixedZero});
    } else if (projectile.hasFlightPathForward) {
        detonationForward = projectile.flightPathForward;
    }
    // RefCode hides the drawable immediately even when DetonateCallsKill
    // delegates final lifetime to a Die module. Keep that presentation fact
    // independent of the later structural destruction transaction.
    if (RenderModelComponent* visual = ecs::try_get<RenderModelComponent>(registry, projectileEntity)) {
        visual->hidden = true;
    }
    const game::WeaponTemplate* weapon = content.findWeapon(projectile.detonationWeapon);
    if (weapon && applyDetonationWeapon) {
        game::WeaponBonusConditionMask bonusConditions =
            projectile.launcherWeaponBonusConditions;
        if (const ObjectWeaponBonusComponent* projectileBonus =
                ecs::try_get<ObjectWeaponBonusComponent>(registry, projectileEntity)) {
            bonusConditions = static_cast<game::WeaponBonusConditionMask>(
                bonusConditions | projectileBonus->conditions);
        }
        const game::WeaponBonus bonus =
            content.resolveWeaponBonus(*weapon, bonusConditions);
        processHistoricWeaponImpact(
            registry, *weapon, projectile.detonationWeapon,
            projectile.launcher, position,
            projectile.sourceShotSequence, logicFramesPerSecond,
            confirmedTick, outHistoricBonusWeapons);
        appendWeaponImpactDamage(registry, lifecycle, spatialIndex, players, {
            .filterSource = projectileId,
            .damageCredit = projectile.launcher,
            .producer = projectile.launcher,
            .causalGroup = projectileId,
            .filterSourceEntity = projectileEntity,
            .primaryTarget = INVALID_OBJECT_ID,
            .impactPosition = position,
            .fixedForward = detonationForward,
            .weapon = weapon,
            .bonus = bonus,
            .sourceSequence = projectile.sourceShotSequence,
            .confirmedTick = confirmedTick,
        }, outDamage, damageVictimScratch);
    }
    // DumbProjectileBehavior either destroys the projectile directly or
    // injects an UNRESISTABLE / DETONATED hit into its own Body so authored
    // Die modules can react. Preserve that branch even though the general Die
    // reaction list is a later ECS module family.
    if (deferDestruction) {
        // MissileAIUpdate enters KILL_SELF and lets the authored delay keep
        // its trail alive. The typed missile state performs the eventual
        // destroy/kill transaction; do not collapse it into Dumb behavior.
    } else if (projectile.detonateCallsKill || !weapon) {
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, projectileEntity);
        if (health) {
            outDamage.push_back({
                .target = projectileId,
                .source = INVALID_OBJECT_ID,
                .sourceSequence = projectile.sourceShotSequence,
                .causalGroup = projectileId,
                // ObjectDamageRequest is still a float ingress boundary for
                // the script bridge; Body state itself remains authoritative
                // Q32.32, so never source a gameplay amount from its float
                // interoperability snapshot.
                .amount = health->maximumFixed,
                .damageType = game::DamageType::UNRESISTABLE,
                .deathType = game::DeathType::DETONATED,
                .resolutionPhase = ObjectDamageResolutionPhase::PostDetonationSelfKill,
                .forceKill = false,
                .confirmedTick = confirmedTick,
            });
        } else {
            static_cast<void>(lifecycle.requestDestroy(projectileId, ObjectDestroyReason::Combat,
                                                        confirmedTick));
        }
    } else {
        static_cast<void>(lifecycle.requestDestroy(projectileId, ObjectDestroyReason::Combat,
                                                    confirmedTick));
    }
    events.push_back({
        .kind = kind,
        .projectile = projectileId,
        .launcher = projectile.launcher,
        .owner = detonationOwner
            ? detonationOwner->player : INVALID_PLAYER_ID,
        .primaryTeam = detonationTeam
            ? detonationTeam->team : INVALID_OBJECT_TEAM_ID,
        .sourcePathfindLayer = projectile.pathfindLayer,
        .target = collidedTarget,
        .sourceShotSequence = projectile.sourceShotSequence,
        .detonationWeapon = projectile.detonationWeapon,
        .position = position,
        .sourceVelocity = detonationVelocity,
        .orientationRadians = detonationYaw,
        .pitchRadians = detonationPitch,
        .rollRadians = detonationRoll,
        .veterancy = detonationVeterancy
            ? detonationVeterancy->level
            : game::ObjectVeterancyLevel::Regular,
        .weaponFxPolicy = weapon
            ? resolveObjectWeaponFxPolicy(
                  registry, projectileEntity, &lifecycle, players, *weapon,
                  weapon->suspendFxDelayMilliseconds != 0)
            : game::WeaponFxPolicy::Play,
        .sourceAirborne = detonationAirborne &&
            detonationAirborne->isAirborne,
        .sourceOwnsFullAttitude = detonationPhysics &&
            detonationPhysics->ownsAttitude,
        .confirmedTick = confirmedTick,
    });
}

} // namespace engine::object_projectile_detail
