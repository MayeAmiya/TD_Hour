#include "game/session/state/GameSessionDomainState.h"
#include "game/session/transaction/GameSessionNavigationTransactions.h"
#include "game/session/transaction/GameSessionNavigationPathAdapter.h"
#include "game/navigation/integration/NavigationFootprintRasterizer.h"
#include "game/navigation/integration/NavigationTerrainLayerMapping.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/object/simulation/runtime/ObjectVisionRange.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/status/ObjectDisabled.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace engine {
namespace {

struct PathObjectCandidate final {
    ObjectId id = INVALID_OBJECT_ID;
    ecs::entity entity = ecs::null;
};

[[nodiscard]] navigation::NavigationMovementMask navigationSurfaceMask(
    const ObjectLocomotionComponent* locomotion) noexcept {
    static_assert(
        game::locomotorSurfaceBit(game::LocomotorSurface::Ground) ==
                navigation::NavigationMovement::Ground &&
            game::locomotorSurfaceBit(game::LocomotorSurface::Water) ==
                navigation::NavigationMovement::Water &&
            game::locomotorSurfaceBit(game::LocomotorSurface::Cliff) ==
                navigation::NavigationMovement::Cliff &&
            game::locomotorSurfaceBit(game::LocomotorSurface::Air) ==
                navigation::NavigationMovement::Air &&
            game::locomotorSurfaceBit(game::LocomotorSurface::Rubble) ==
                navigation::NavigationMovement::Rubble);
    if (!locomotion) return navigation::NavigationMovement::Ground;
    game::LocomotorSurfaceMask surfaces = 0;
    for (const game::FrozenLocomotorTemplate& profile :
         locomotion->profiles) {
        surfaces |= profile.surfaces;
    }
    if (surfaces == 0) surfaces = locomotion->surfaces;
    return surfaces != 0
        ? static_cast<navigation::NavigationMovementMask>(surfaces)
        : navigation::NavigationMovement::Ground;
}

[[nodiscard]] bool unavailablePathObject(
    const ecs::registry& registry, const ObjectLifecycle& objects,
    const PathObjectCandidate& candidate) noexcept {
    if (!candidate.id || objects.isPendingDestroy(candidate.id)) return true;
    const ObjectLifecycleComponent* lifecycle =
        ecs::try_get<ObjectLifecycleComponent>(registry, candidate.entity);
    if (!lifecycle || lifecycle->phase != ObjectLifecyclePhase::Alive)
        return true;
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, candidate.entity);
    if (health && health->effectivelyDead) return true;
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, candidate.entity);
    if (status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::Destroyed) |
            game::objectStatusBit(game::ObjectStatusFlag::Sold) |
            game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction) |
            game::objectStatusBit(game::ObjectStatusFlag::NoCollisions))) {
        return true;
    }
    const ObjectContainedByComponent* contained =
        ecs::try_get<ObjectContainedByComponent>(registry, candidate.entity);
    if (contained && contained->enclosing) return true;
    const ObjectMapStatusComponent* map =
        ecs::try_get<ObjectMapStatusComponent>(registry, candidate.entity);
    return map && map->offMap;
}

[[nodiscard]] bool stationaryPathObject(
    const ecs::registry& registry, ecs::entity entity,
    const ObjectLocomotionComponent& locomotion) noexcept {
    if (locomotion.state != ObjectLocomotionState::Idle ||
        locomotion.hasActiveMove || locomotion.forwardSpeed.raw() != 0 ||
        locomotion.verticalSpeed.raw() != 0) {
        return false;
    }
    const ObjectPhysicsComponent* physics =
        ecs::try_get<ObjectPhysicsComponent>(registry, entity);
    return !physics ||
        (physics->velocityUnitsPerSecond.x.raw() == 0 &&
         physics->velocityUnitsPerSecond.y.raw() == 0 &&
         physics->velocityUnitsPerSecond.z.raw() == 0);
}

[[nodiscard]] const game::ThingTemplate* objectTemplate(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    return type && type->archetype ? &type->archetype->templateData : nullptr;
}

[[nodiscard]] ObjectId objectSlaver(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ObjectSpawnedByRuntimeComponent* spawned =
        ecs::try_get<ObjectSpawnedByRuntimeComponent>(registry, entity);
    return spawned ? spawned->master : INVALID_OBJECT_ID;
}

void freezeAttackSeeThroughObstacles(
    const ecs::registry& registry, container::Vector<uint64_t>& output) {
    output.clear();
    const auto view = ecs::view<
        const ObjectIdentityComponent, const ObjectKindOfComponent>(registry);
    output.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        const ObjectKindOfComponent& kinds =
            view.template get<const ObjectKindOfComponent>(entity);
        if (identity.id && game::objectHasKind(
                kinds.mask,
                game::ObjectKindOf::CanSeeThroughStructure)) {
            output.push_back(identity.id.value);
        }
    }
    std::sort(output.begin(), output.end());
    output.erase(std::unique(output.begin(), output.end()), output.end());
}

void freezeDozerPassableObstacleIds(
    const ecs::registry& registry, const PlayerRegistry& players,
    const navigation::NavigationDynamicOverlay& dynamicOverlay,
    ecs::entity subject, ObjectId subjectId,
    container::Vector<uint64_t>& output) {
    output.clear();
    const auto view = ecs::view<
        const ObjectIdentityComponent, const ObjectGeometryComponent>(registry);
    output.reserve(view.size_hint());
    for (const ecs::entity candidate : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(candidate);
        if (!identity.id || identity.id == subjectId ||
            !dynamicOverlay.containsEntity(identity.id.value) ||
            relationshipBetweenObjects(
                registry, players, subject, candidate) ==
                PlayerRelationship::Enemies) {
            continue;
        }
        output.push_back(identity.id.value);
    }
    std::sort(output.begin(), output.end());
    output.erase(std::unique(output.begin(), output.end()), output.end());
}

[[nodiscard]] bool subjectCanCrush(
    const ecs::registry& registry, ecs::entity subject,
    ecs::entity candidate, uint64_t confirmedTick) noexcept {
    const game::ThingTemplate* crusher = objectTemplate(registry, subject);
    const ThingTemplateComponent* victimType =
        ecs::try_get<ThingTemplateComponent>(registry, candidate);
    if (!crusher || crusher->crusherLevel == 0 ||
        isObjectDisabledBy(registry, subject,
                           ObjectDisabledReason::Unmanned,
                           confirmedTick) ||
        !victimType ||
        !victimType->archetype) {
        return false;
    }
    const game::ThingTemplate& victim = victimType->archetype->templateData;
    return static_cast<bool>(victimType->archetype->squishCollidePlan) ||
        crusher->crusherLevel > victim.crushableLevel;
}

[[nodiscard]] bool objectOnNavigationLayer(
    const ecs::registry& registry, ecs::entity entity,
    navigation::NavigationLayerId wanted) noexcept {
    game::terrain::TerrainPathfindLayerId terrainLayer =
        game::terrain::kGroundPathfindLayer;
    if (const ObjectTerrainLayerComponent* layer =
            ecs::try_get<ObjectTerrainLayerComponent>(registry, entity)) {
        terrainLayer = layer->pathfindLayer;
    }
    navigation::NavigationLayerId actual;
    return navigation::tryNavigationLayerFromTerrainPathfindLayer(
               terrainLayer, actual) && actual == wanted;
}

void freezePathObjectCells(
    const ecs::registry& registry, const ObjectLifecycle& objects,
    const PlayerRegistry& players, const navigation::NavigationGrid& grid,
    navigation::NavigationLayerId layer, ecs::entity subjectEntity,
    ObjectId subjectId, ObjectId ignoredObstacle,
    int64_t subjectRadiusRaw, uint64_t confirmedTick,
    container::Vector<ai::AIPathObjectCellSnapshot>& output) {
    output.clear();
    container::Vector<PathObjectCandidate> candidates;
    const auto view = ecs::view<
        const ObjectIdentityComponent, const ObjectLocomotionComponent,
        const ObjectFixedTransformComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id || identity.id == subjectId ||
            identity.id == ignoredObstacle) {
            continue;
        }
        candidates.push_back({identity.id, entity});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const PathObjectCandidate& left,
                 const PathObjectCandidate& right) noexcept {
                  return left.id < right.id;
              });

    container::Vector<navigation::NavigationCellId> raster;
    raster.resize(16);
    for (const PathObjectCandidate& candidate : candidates) {
        if (unavailablePathObject(registry, objects, candidate) ||
            !objectOnNavigationLayer(registry, candidate.entity, layer)) {
            continue;
        }
        const ObjectLocomotionComponent& locomotion =
            ecs::get<const ObjectLocomotionComponent>(registry,
                                                       candidate.entity);
        // RefCode's Pathfinder::updatePos only classifies actors doing
        // ground movement. An airborne/air-only actor must not become a
        // stationary ground-path blocker merely because it still has a
        // terrain layer and a locomotor component.
        const ObjectAirborneComponent* airborne =
            ecs::try_get<ObjectAirborneComponent>(registry,
                                                  candidate.entity);
        const game::LocomotorSurfaceMask groundSurfaces =
            game::locomotorSurfaceBit(game::LocomotorSurface::Ground) |
            game::locomotorSurfaceBit(game::LocomotorSurface::Water) |
            game::locomotorSurfaceBit(game::LocomotorSurface::Cliff) |
            game::locomotorSurfaceBit(game::LocomotorSurface::Rubble);
        if ((locomotion.surfaces & groundSurfaces) == 0 ||
            (airborne && airborne->isAirborne)) {
            continue;
        }
        // RefCode's ordinary path expansion uses considerTransient=false:
        // moving actors are collision-time concerns, not path topology.
        if (!stationaryPathObject(registry, candidate.entity, locomotion))
            continue;

        const PlayerRelationship relation = relationshipBetweenObjects(
            registry, players, subjectEntity, candidate.entity);
        ai::AIPathObjectCellEffect effect;
        if (relation == PlayerRelationship::Allies) {
            effect = ai::AIPathObjectCellEffect::FriendlyCost;
        } else if (relation == PlayerRelationship::Enemies &&
                   !subjectCanCrush(registry, subjectEntity,
                                    candidate.entity, confirmedTick)) {
            effect = ai::AIPathObjectCellEffect::EnemyBlock;
        } else if (relation == PlayerRelationship::Neutral &&
                   !subjectCanCrush(registry, subjectEntity,
                                    candidate.entity, confirmedTick)) {
            // Neutral map actors are physical obstacles in RefCode. They are
            // not allies merely because they have no player relationship;
            // omitting them from the frozen object field makes paths run
            // straight through civilians/neutral units and leaves collision
            // to resolve an avoidable deadlock later.
            effect = ai::AIPathObjectCellEffect::NeutralBlock;
        } else {
            continue;
        }

        const ObjectFixedTransformComponent& transform =
            ecs::get<const ObjectFixedTransformComponent>(registry,
                                                           candidate.entity);
        if (!transform.authoritative) continue;
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(registry,
                                                   candidate.entity);
        const int64_t candidateRadiusRaw = geometry
            ? math::q32_32::max(
                  {}, geometry->boundingCircleRadiusFixed).raw()
            : 0;
        const int64_t maximum = std::numeric_limits<int64_t>::max();
        const int64_t combinedRadiusRaw =
            candidateRadiusRaw > maximum - subjectRadiusRaw
                ? maximum : candidateRadiusRaw + subjectRadiusRaw;
        const navigation::NavigationWorldPosition center{
            transform.position.x.raw(), transform.position.y.raw(),
            transform.position.z.raw()};

        navigation::NavigationFootprintRasterResult result;
        for (;;) {
            result = navigation::NavigationFootprintRasterizer::circle(
                grid, center, combinedRadiusRaw, raster);
            if (result.status != navigation::
                    NavigationFootprintRasterStatus::CapacityExceeded) {
                break;
            }
            if (raster.size() >= grid.cellCount()) break;
            const size_t next = std::min(
                grid.cellCount(), std::max(raster.size() + 1,
                                           raster.size() * 2));
            raster.resize(next);
        }
        if (result.status !=
            navigation::NavigationFootprintRasterStatus::Success) {
            continue;
        }
        for (uint32_t index = 0; index < result.writtenCount; ++index) {
            output.push_back({
                .object = candidate.id,
                .layer = layer.value,
                .cell = raster[index].value,
                .effect = effect,
            });
        }
    }
    std::sort(output.begin(), output.end(),
              [](const ai::AIPathObjectCellSnapshot& left,
                 const ai::AIPathObjectCellSnapshot& right) noexcept {
                  if (left.layer != right.layer)
                      return left.layer < right.layer;
                  if (left.cell != right.cell) return left.cell < right.cell;
                  return left.object < right.object;
              });
    output.erase(std::unique(output.begin(), output.end()), output.end());
}

} // namespace

navigation::NavigationAdapterSubmitResult GameSessionNavigationPathAdapter::submit(
    const ai::PathRequest& request,
    uint64_t confirmedTick) noexcept {
    if (!m_content.m_active || !m_content.m_navigation.isInitialized() ||
        !m_presentation.m_hasConfirmedFrame || confirmedTick != m_presentation.m_confirmedTick) {
        return navigation::NavigationAdapterSubmitResult::InvalidRequest;
    }
    if (request.kind == ai::PathRequestKind::Cancel)
        return m_content.m_navigation.submitPathRequest(request, confirmedTick);

    ai::PathRequest admittedRequest = request;
    admittedRequest.objectSnapshotTick = confirmedTick;
    admittedRequest.objectCells.clear();
    admittedRequest.dozerPassableObstacles.clear();
    // Never trust a producer-authored clearance value.  Admission freezes the
    // complete ZH radius/phase profile from the confirmed subject geometry on
    // every non-cancel request, so callers cannot shrink a large unit.
    navigation::NavigationClearanceClass clearance =
        navigation::NavigationClearanceClass::Centered1x1;
    int64_t subjectRadiusRaw = 0;
    std::optional<ecs::entity> admittedSubject;
    if (const std::optional<ecs::entity> subject =
            m_world.m_objects.entityFromId(
                admittedRequest.correlation.subject)) {
        admittedSubject = subject;
        // Surface admission is authoritative state just like clearance.  A
        // few child state kernels intentionally leave the transport field at
        // zero and rely on this boundary; accepting that zero reaches A* as
        // an invalid movement mask, which made out-of-range attacks aim but
        // never approach their target.
        admittedRequest.surfaceMask = navigationSurfaceMask(
            ecs::try_get<ObjectLocomotionComponent>(
                m_world.m_registry, *subject));
        if (const ObjectGeometryComponent* geometry =
                ecs::try_get<ObjectGeometryComponent>(
                    m_world.m_registry, *subject);
            geometry) {
            subjectRadiusRaw = math::q32_32::max(
                {}, geometry->boundingCircleRadiusFixed).raw();
            clearance = navigation::clearanceClassForRadiusRaw(
                subjectRadiusRaw,
                m_content.m_navigation.grid()
                    .transform().cellSizeRaw);
        }
    } else {
        admittedRequest.surfaceMask =
            navigation::NavigationMovement::Ground;
    }
    admittedRequest.clearanceProfile = {
        .radiusCells = static_cast<uint8_t>(
            navigation::clearanceRadiusCells(clearance)),
        .centerInCell = navigation::clearanceCenterInCell(clearance),
        .frozen = true,
    };
    admittedRequest.attackLineOfSightEnabled = false;
    admittedRequest.attackSubjectContainer = INVALID_OBJECT_ID;
    admittedRequest.attackTargetContainer = INVALID_OBJECT_ID;
    admittedRequest.attackSubjectSlaver = INVALID_OBJECT_ID;
    admittedRequest.attackTargetSlaver = INVALID_OBJECT_ID;
    admittedRequest.attackSeeThroughObstacles.clear();
    if (admittedRequest.kind == ai::PathRequestKind::Approach) {
        if (!admittedSubject || admittedRequest.arrivalRadiusRaw < 0 ||
            admittedRequest.minimumArrivalRadiusRaw < 0) {
            return navigation::NavigationAdapterSubmitResult::InvalidRequest;
        }
        math::q32_32 targetRadius;
        std::optional<ecs::entity> attackTarget;
        if (admittedRequest.attackTarget) {
            attackTarget = m_world.m_objects.entityFromId(
                admittedRequest.attackTarget);
            if (!attackTarget) {
                return navigation::NavigationAdapterSubmitResult::InvalidRequest;
            }
            if (const ObjectGeometryComponent* geometry =
                    ecs::try_get<ObjectGeometryComponent>(
                        m_world.m_registry, *attackTarget)) {
                targetRadius = math::q32_32::max(
                    {}, geometry->boundingCircleRadiusFixed);
            }
        }
        const math::q32_32 footprintRadius =
            math::q32_32::from_raw(subjectRadiusRaw) + targetRadius;
        admittedRequest.arrivalRadiusRaw =
            (math::q32_32::from_raw(
                 admittedRequest.arrivalRadiusRaw) +
             footprintRadius).raw();
        if (admittedRequest.minimumArrivalRadiusRaw > 0) {
            admittedRequest.minimumArrivalRadiusRaw =
                (math::q32_32::from_raw(
                     admittedRequest.minimumArrivalRadiusRaw) +
                 footprintRadius).raw();
        }

        const ObjectKindOfComponent* subjectKinds =
            ecs::try_get<ObjectKindOfComponent>(
                m_world.m_registry, *admittedSubject);
        const ObjectLocomotionComponent* subjectLocomotion =
            ecs::try_get<ObjectLocomotionComponent>(
                m_world.m_registry, *admittedSubject);
        const ObjectAirborneComponent* subjectAirborne =
            ecs::try_get<ObjectAirborneComponent>(
                m_world.m_registry, *admittedSubject);
        bool subjectOnGround =
            (subjectLocomotion &&
             (!subjectAirborne || !subjectAirborne->isAirborne)) ||
            (subjectKinds &&
             (game::objectHasKind(
                  subjectKinds->mask, game::ObjectKindOf::Immobile) ||
              game::objectHasKind(
                  subjectKinds->mask,
                  game::ObjectKindOf::SpawnsAreTheWeapons)));
        if (const ObjectContainedByComponent* contained =
                ecs::try_get<ObjectContainedByComponent>(
                    m_world.m_registry, *admittedSubject);
            contained && contained->container) {
            admittedRequest.attackSubjectContainer =
                contained->container;
            if (!subjectOnGround) {
                const std::optional<ecs::entity> host =
                    m_world.m_objects.entityFromId(contained->container);
                const ObjectAirborneComponent* hostAirborne = host
                    ? ecs::try_get<ObjectAirborneComponent>(
                          m_world.m_registry, *host)
                    : nullptr;
                const ObjectKindOfComponent* hostKinds = host
                    ? ecs::try_get<ObjectKindOfComponent>(
                          m_world.m_registry, *host)
                    : nullptr;
                subjectOnGround = host &&
                    ((hostKinds && game::objectHasKind(
                         hostKinds->mask,
                         game::ObjectKindOf::Structure)) ||
                     !hostAirborne || !hostAirborne->isAirborne);
            }
        }
        admittedRequest.attackSubjectSlaver = objectSlaver(
            m_world.m_registry, *admittedSubject);
        bool targetAirborne = false;
        if (attackTarget) {
            if (const ObjectContainedByComponent* contained =
                    ecs::try_get<ObjectContainedByComponent>(
                        m_world.m_registry, *attackTarget)) {
                admittedRequest.attackTargetContainer =
                    contained->container;
            }
            admittedRequest.attackTargetSlaver = objectSlaver(
                m_world.m_registry, *attackTarget);
            if (const ObjectAirborneComponent* airborne =
                    ecs::try_get<ObjectAirborneComponent>(
                        m_world.m_registry, *attackTarget)) {
                targetAirborne = airborne->isAirborne;
            }
        }
        admittedRequest.attackLineOfSightEnabled =
            m_content.m_objectSimulationRules.ai.attackUsesLineOfSight &&
            !admittedRequest.attackContactWeapon && subjectOnGround &&
            !targetAirborne && subjectKinds &&
            game::objectHasKind(
                subjectKinds->mask,
                game::ObjectKindOf::AttackNeedsLineOfSight);
        if (admittedRequest.attackLineOfSightEnabled) {
            freezeAttackSeeThroughObstacles(
                m_world.m_registry,
                admittedRequest.attackSeeThroughObstacles);
        }
    } else {
        admittedRequest.minimumArrivalRadiusRaw = 0;
        admittedRequest.attackTarget = INVALID_OBJECT_ID;
        admittedRequest.attackContactWeapon = false;
    }
    if (admittedRequest.groupPathId != 0) {
        // RefCode's shared AIGroup centerline uses a six-cell column
        // diameter. The published grid supports up to Centered5x5, so the
        // group owner freezes that widest topology class once here; member
        // paths are offset clones and never rerun a per-unit A*.
        admittedRequest.clearanceProfile = {
            .radiusCells = 2,
            .centerInCell = true,
            .frozen = true,
        };
    }

    if (admittedRequest.kind == ai::PathRequestKind::Safe) {
        const std::optional<ecs::entity> repulsor =
            m_world.m_objects.entityFromId(
                admittedRequest.safePathRepulsor);
        const TransformComponent* repulsorTransform = repulsor
            ? ecs::try_get<TransformComponent>(
                  m_world.m_registry, *repulsor)
            : nullptr;
        if (!repulsorTransform) {
            return navigation::NavigationAdapterSubmitResult::InvalidRequest;
        }
        const LogicFixedVec3 repulsorPosition =
            readAuthoritativeObjectPosition(
                m_world.m_registry, *repulsor,
                *repulsorTransform);
        admittedRequest.safePathRepulsorPosition = {
            repulsorPosition.x.raw(), repulsorPosition.y.raw(),
            repulsorPosition.z.raw(),
        };
        const ObjectId requestedRepulsor2 = admittedRequest.safePathRepulsor2;
        admittedRequest.safePathRepulsor2 = INVALID_OBJECT_ID;
        bool secondRepulsorFrozen = false;
        if (requestedRepulsor2 && requestedRepulsor2 != admittedRequest.safePathRepulsor) {
            const std::optional<ecs::entity> repulsor2 =
                m_world.m_objects.entityFromId(requestedRepulsor2);
            const TransformComponent* repulsor2Transform = repulsor2
                ? ecs::try_get<TransformComponent>(
                      m_world.m_registry, *repulsor2)
                : nullptr;
            if (repulsor2Transform) {
                const LogicFixedVec3 repulsor2Position =
                    readAuthoritativeObjectPosition(
                        m_world.m_registry, *repulsor2,
                        *repulsor2Transform);
                admittedRequest.safePathRepulsor2 = requestedRepulsor2;
                admittedRequest.safePathRepulsor2Position = {
                    repulsor2Position.x.raw(), repulsor2Position.y.raw(),
                    repulsor2Position.z.raw(),
                };
                secondRepulsorFrozen = true;
            }
        }
        const std::optional<ecs::entity> subjectEntity =
            m_world.m_objects.entityFromId(
                admittedRequest.correlation.subject);
        const TransformComponent* subjectTransform = subjectEntity
            ? ecs::try_get<TransformComponent>(
                  m_world.m_registry, *subjectEntity)
            : nullptr;
        if (subjectTransform && !secondRepulsorFrozen) {
            using Fixed = math::q32_32;
            const LogicFixedVec3 subjectPosition =
                readAuthoritativeObjectPosition(
                    m_world.m_registry, *subjectEntity,
                    *subjectTransform);
            Fixed bestDistanceSquared = Fixed::from_raw(
                std::numeric_limits<int64_t>::max());
            const auto repulsors = ecs::view<const ObjectIdentityComponent,
                                              const TransformComponent,
                                              const ObjectStatusComponent>(
                m_world.m_registry);
            for (const ecs::entity candidate : repulsors) {
                const ObjectIdentityComponent& identity =
                    repulsors.template get<const ObjectIdentityComponent>(candidate);
                const ObjectStatusComponent& status =
                    repulsors.template get<const ObjectStatusComponent>(candidate);
                if (!identity.id || identity.id == admittedRequest.safePathRepulsor ||
                    !status.hasAny(game::objectStatusBit(
                        game::ObjectStatusFlag::Repulsor))) {
                    continue;
                }
                const TransformComponent& transform =
                    repulsors.template get<const TransformComponent>(candidate);
                const LogicFixedVec3 candidatePosition =
                    readAuthoritativeObjectPosition(
                        m_world.m_registry, candidate,
                        transform);
                const Fixed dx = subjectPosition.x - candidatePosition.x;
                const Fixed dy = subjectPosition.y - candidatePosition.y;
                const Fixed distanceSquared = dx * dx + dy * dy;
                if (distanceSquared < bestDistanceSquared ||
                    (distanceSquared == bestDistanceSquared &&
                     identity.id < admittedRequest.safePathRepulsor2)) {
                    bestDistanceSquared = distanceSquared;
                    admittedRequest.safePathRepulsor2 = identity.id;
                    admittedRequest.safePathRepulsor2Position = {
                        candidatePosition.x.raw(), candidatePosition.y.raw(),
                        candidatePosition.z.raw(),
                    };
                }
            }
        }
        const std::optional<ecs::entity> subject =
            m_world.m_objects.entityFromId(
                admittedRequest.correlation.subject);
        const math::q32_32 vision = subject
            ? effectiveObjectVisionRangeFixed(
                  m_world.m_registry, *subject)
            : math::q32_32{};
        admittedRequest.safePathRadiusRaw =
            math::q32_32::max(
                math::q32_32{},
                vision + m_content.m_objectSimulationRules.ai
                             .repulsedDistance)
                .raw();
    }

    const ObjectLocomotionComponent* admittedLocomotion = admittedSubject
        ? ecs::try_get<ObjectLocomotionComponent>(
              m_world.m_registry, *admittedSubject)
        : nullptr;
    admittedRequest.airWings = admittedLocomotion &&
        admittedRequest.traversalMode ==
            ai::AIPathTraversalMode::DirectLine &&
        admittedLocomotion->appearance == game::LocomotorAppearance::Wings;

    game::terrain::TerrainPathfindLayerId startTerrainLayer =
        game::terrain::kGroundPathfindLayer;
    if (const std::optional<ecs::entity> subject =
            m_world.m_objects.entityFromId(admittedRequest.correlation.subject)) {
        if (const ObjectTerrainLayerComponent* layer =
                ecs::try_get<ObjectTerrainLayerComponent>(
                    m_world.m_registry, *subject)) {
            startTerrainLayer = layer->pathfindLayer;
        } else {
            startTerrainLayer = m_content.m_terrain.
                pathfindLayerForDestinationRaw(
                    admittedRequest.start.xRaw,
                    admittedRequest.start.yRaw,
                    admittedRequest.start.zRaw);
        }
    } else {
        startTerrainLayer = m_content.m_terrain.
            pathfindLayerForDestinationRaw(
                admittedRequest.start.xRaw,
                admittedRequest.start.yRaw,
                admittedRequest.start.zRaw);
    }
    const game::terrain::TerrainPathfindLayerId goalTerrainLayer =
        m_content.m_terrain.pathfindLayerForDestinationRaw(
            admittedRequest.originalGoal.xRaw,
            admittedRequest.originalGoal.yRaw,
            admittedRequest.originalGoal.zRaw);
    navigation::NavigationLayerId startLayer;
    navigation::NavigationLayerId goalLayer;
    if (!navigation::tryNavigationLayerFromTerrainPathfindLayer(
            startTerrainLayer, startLayer) ||
        !navigation::tryNavigationLayerFromTerrainPathfindLayer(
            goalTerrainLayer, goalLayer)) {
        return navigation::NavigationAdapterSubmitResult::InvalidRequest;
    }
    const ThingTemplateComponent* admittedSubjectType = admittedSubject
        ? ecs::try_get<ThingTemplateComponent>(
              m_world.m_registry, *admittedSubject)
        : nullptr;
    admittedRequest.crusherLevel = admittedSubjectType &&
            admittedSubjectType->archetype
        ? admittedSubjectType->archetype->templateData.crusherLevel : 0;
    const ObjectKindOfComponent* admittedSubjectKinds = admittedSubject
        ? ecs::try_get<ObjectKindOfComponent>(
              m_world.m_registry, *admittedSubject)
        : nullptr;
    if (admittedSubject && admittedSubjectKinds && game::objectHasKind(
            admittedSubjectKinds->mask, game::ObjectKindOf::Dozer)) {
        // RefCode's dozerHack admits CELL_OBSTACLE only when the obstacle's
        // relationship is not ENEMIES. Freeze that relationship decision at
        // confirmed admission; workers never query mutable ECS/player state.
        freezeDozerPassableObstacleIds(
            m_world.m_registry, m_content.m_players,
            m_content.m_navigation.dynamicOverlay(), *admittedSubject,
            admittedRequest.correlation.subject,
            admittedRequest.dozerPassableObstacles);
    }
    if (!admittedRequest.pathThroughUnits && admittedSubject &&
        admittedRequest.traversalMode ==
            ai::AIPathTraversalMode::Navmesh) {
        const uint8_t crusherLevel = admittedRequest.crusherLevel;
        const bool unmanned = isObjectDisabledBy(
            m_world.m_registry, *admittedSubject,
            ObjectDisabledReason::Unmanned, confirmedTick);
        const PlayerId owner = m_world.m_ownership
            .ownerOf(admittedRequest.correlation.subject)
            .value_or(INVALID_PLAYER_ID);
        const uint64_t spatialRevision =
            m_world.m_spatialIndex.revision();
        auto& caches = m_world.m_pathObjectSnapshotCache;
        caches.erase(std::remove_if(
            caches.begin(), caches.end(),
            [&](const GameSessionPathObjectSnapshotCacheEntry& entry) {
                return entry.confirmedTick != confirmedTick ||
                    entry.spatialRevision != spatialRevision;
            }), caches.end());
        for (const navigation::NavigationLayerRecord& layerRecord :
             m_content.m_navigation.layers().layers()) {
            auto found = std::find_if(
                caches.begin(), caches.end(),
                [&](const GameSessionPathObjectSnapshotCacheEntry& entry) {
                    return entry.layer == layerRecord.id &&
                        entry.subjectOwner == owner &&
                        entry.subjectRadiusRaw == subjectRadiusRaw &&
                        entry.crusherLevel == crusherLevel &&
                        entry.unmanned == unmanned;
                });
            if (found == caches.end()) {
                constexpr size_t kMaximumProfilesPerTick = 64;
                if (caches.size() >= kMaximumProfilesPerTick)
                    caches.erase(caches.begin());
                caches.push_back({
                    .confirmedTick = confirmedTick,
                    .spatialRevision = spatialRevision,
                    .layer = layerRecord.id,
                    .subjectOwner = owner,
                    .subjectRadiusRaw = subjectRadiusRaw,
                    .crusherLevel = crusherLevel,
                    .unmanned = unmanned,
                });
                found = std::prev(caches.end());
                freezePathObjectCells(
                    m_world.m_registry, m_world.m_objects,
                    m_content.m_players, layerRecord.grid,
                    layerRecord.id, *admittedSubject,
                    INVALID_OBJECT_ID, INVALID_OBJECT_ID,
                    subjectRadiusRaw, confirmedTick, found->cells);
            }
            admittedRequest.objectCells.insert(
                admittedRequest.objectCells.end(),
                found->cells.begin(), found->cells.end());
        }
        admittedRequest.objectCells.erase(std::remove_if(
            admittedRequest.objectCells.begin(),
            admittedRequest.objectCells.end(),
            [&](const ai::AIPathObjectCellSnapshot& value) {
                return value.object ==
                        admittedRequest.correlation.subject ||
                    value.object == admittedRequest.ignoredObstacle;
            }), admittedRequest.objectCells.end());

        // A collision-confirmed blocker is stronger evidence than the
        // ordinary ally occupancy penalty.  The movement transaction bumps
        // the direct order revision after a repeated contact; freeze that one
        // remembered object as a request-local hard cell so the replacement
        // path actually routes around a busy/disabled/non-yielding unit.
        // This does not mutate the global navigation overlay and expires as
        // soon as the confirmed contact is no longer fresh.
        const ObjectAIMovementObstructionStateComponent* obstruction =
            ecs::try_get<ObjectAIMovementObstructionStateComponent>(
                m_world.m_registry, *admittedSubject);
        if (obstruction && obstruction->blocker &&
            obstruction->lastContactTick <= confirmedTick &&
            confirmedTick - obstruction->lastContactTick <= 2u) {
            for (ai::AIPathObjectCellSnapshot& value :
                 admittedRequest.objectCells) {
                if (value.object == obstruction->blocker &&
                    value.effect ==
                        ai::AIPathObjectCellEffect::FriendlyCost) {
                    value.effect = ai::AIPathObjectCellEffect::NeutralBlock;
                }
            }
        }
    }
    admittedRequest.blockingBridgeCandidate =
        m_content.m_navigation.findBrokenBridgeConnecting(
            admittedRequest, startLayer, goalLayer);
    return m_content.m_navigation.submitPathRequest(
        admittedRequest, confirmedTick, startLayer, goalLayer);
}

bool GameSessionNavigationPathAdapter::poll(
    const ai::PathCorrelation& correlation,
    uint64_t confirmedTick,
    ai::PathFeedback& output) noexcept {
    if (!m_content.m_active || !m_content.m_navigation.isInitialized() ||
        !m_presentation.m_hasConfirmedFrame || confirmedTick != m_presentation.m_confirmedTick) {
        return false;
    }
    navigation::NavigationAdapterFeedback feedback;
    if (!m_content.m_navigation.pollPathFeedback(correlation, confirmedTick, feedback))
        return false;
    output = feedback.feedback;
    return true;
}

} // namespace engine
