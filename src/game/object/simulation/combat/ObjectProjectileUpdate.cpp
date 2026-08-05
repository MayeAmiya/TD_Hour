#include "game/object/simulation/combat/ObjectProjectileSystemDetail.h"

#include "core/container/string_utils.h"
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

void ObjectProjectileSystem::update(ecs::registry& registry, ObjectLifecycle& lifecycle,
                                    const GameContentSnapshot& content,
                                    const ObjectSpatialIndex* spatialIndex,
                                    const PlayerRegistry* players,
                                    const game::terrain::TerrainLogic& terrain,
                                    uint32_t logicFramesPerSecond,
                                    uint64_t confirmedTick) {
    struct Candidate final {
        ObjectId id = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    // Compatibility discovery runs once for restored/test registries which
    // predate the production creation callback. Normal creation registers an
    // ID in initializeObject(), so steady-state updates never rebuild a full
    // projectile view or candidate sort.
    if (!m_activeProjectileStoreInitialized) {
        const auto view = ecs::view<const ObjectIdentityComponent,
                                    const ObjectProjectileComponent,
                                    const TransformComponent>(registry);
        m_activeProjectileIds.reserve(view.size_hint());
        for (const ecs::entity entity : view) {
            const ObjectIdentityComponent& identity =
                view.template get<const ObjectIdentityComponent>(entity);
            if (identity.id) m_activeProjectileIds.push_back(identity.id);
        }
        std::sort(m_activeProjectileIds.begin(),
                  m_activeProjectileIds.end());
        m_activeProjectileIds.erase(
            std::unique(m_activeProjectileIds.begin(),
                        m_activeProjectileIds.end()),
            m_activeProjectileIds.end());
        m_activeProjectileStoreInitialized = true;
    }

    container::Vector<Candidate> projectiles;
    projectiles.reserve(m_activeProjectileIds.size());
    size_t liveCount = 0;
    for (const ObjectId object : m_activeProjectileIds) {
        const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
        if (!entity || lifecycle.isPendingDestroy(object) ||
            !ecs::try_get<ObjectProjectileComponent>(registry, *entity) ||
            !ecs::try_get<TransformComponent>(registry, *entity)) {
            continue;
        }
        m_activeProjectileIds[liveCount++] = object;
        projectiles.push_back({.id = object, .entity = *entity});
    }
    m_activeProjectileIds.resize(liveCount);
    const container::Vector<ObjectId>& currentProjectileIds =
        m_activeProjectileIds;
    // Every projectile consumes the complete ordered candidate set before
    // the next query clears it; retain one broad-phase capacity across ticks.
    auto& collisionCandidateScratch = m_collisionCandidateScratch;
    collisionCandidateScratch.clear();

    const uint32_t rate = std::max<uint32_t>(1, logicFramesPerSecond);
    const Fixed framesPerSecond{static_cast<int32_t>(std::min<uint32_t>(rate, kMaximumPathSegments))};
    for (const Candidate& candidate : projectiles) {
        container::Vector<ObjectDamageRequest> candidateDamage;
        container::Vector<ObjectHistoricBonusWeaponFire>
            candidateHistoricBonusWeapons;
        const size_t eventBegin = m_events.size();
        const auto updateCandidate = [&] {
        if (isObjectDisabled(registry, candidate.entity, confirmedTick)) {
            return;
        }
        ObjectProjectileComponent& projectile =
            ecs::get<ObjectProjectileComponent>(registry, candidate.entity);
        TransformComponent& transform = ecs::get<TransformComponent>(registry, candidate.entity);
        // RefCode publishes a newly-created Object only after its construction
        // callbacks have completed; its UpdateModule is then admitted by the
        // scheduler on a later frame.  Keep that creation-frame boundary for
        // every projectile controller.  Previously only the dumb-projectile
        // branch observed it, while MissileAI/Neutron could move, detonate and
        // hide in their spawn frame before zero-delay BoneFX had its first
        // lifecycle wake.
        if (projectile.detonated || confirmedTick <= projectile.spawnedTick) {
            return;
        }
        if (projectile.motion == ObjectProjectileMotion::MissileAI) {
            ObjectMissileProjectileComponent* missile =
                ecs::try_get<ObjectMissileProjectileComponent>(registry, candidate.entity);
            if (missile) {
                static_cast<void>(updateMissileProjectile(
                    registry, lifecycle, content, spatialIndex, players, terrain,
                    rate, confirmedTick, candidate.id, candidate.entity,
                    projectile, *missile, transform, collisionCandidateScratch,
                    candidateDamage, candidateHistoricBonusWeapons,
                    m_events));
            }
            synchronizeProjectileTerrainLayer(
                registry, candidate.entity, projectile, confirmedTick);
            return;
        }
        if (projectile.motion == ObjectProjectileMotion::NeutronMissile) {
            ObjectNeutronMissileProjectileComponent* neutron =
                ecs::try_get<ObjectNeutronMissileProjectileComponent>(
                    registry, candidate.entity);
            if (neutron) {
                static_cast<void>(updateNeutronProjectile(
                    registry, lifecycle, content, spatialIndex, players, terrain,
                    confirmedTick, candidate.id, candidate.entity, projectile,
                    *neutron, transform, collisionCandidateScratch,
                    candidateDamage, m_events));
            }
            synchronizeProjectileTerrainLayer(
                registry, candidate.entity, projectile, confirmedTick);
            return;
        }
        if (projectile.hasExpiryTick && confirmedTick >= projectile.expiryTick) {
            detonate(registry, lifecycle, content, spatialIndex, players, candidate.id, candidate.entity,
                     projectile, transform, projectile.position, ObjectProjectileEventKind::Expired,
                     INVALID_OBJECT_ID, logicFramesPerSecond, confirmedTick,
                     collisionCandidateScratch, candidateDamage,
                     candidateHistoricBonusWeapons, m_events);
            return;
        }

        if (projectile.targetAdjustDistancePerSecond > kFixedZero && projectile.intendedTarget) {
            const std::optional<ecs::entity> target = lifecycle.entityFromId(projectile.intendedTarget);
            if (target) {
                const TransformComponent* targetTransform =
                    ecs::try_get<TransformComponent>(registry, *target);
                const ObjectGeometryComponent* targetGeometry =
                    ecs::try_get<ObjectGeometryComponent>(registry, *target);
                if (targetTransform) {
                    FixedVec3 desired = readAuthoritativeObjectPosition(
                        registry, *target, *targetTransform);
                    if (targetGeometry &&
                        targetGeometry->shape != ObjectGeometryShape::Sphere) {
                        desired.z += Fixed::max(
                            kFixedZero, targetGeometry->heightFixed) /
                            Fixed{int32_t{2}};
                    }
                    const FixedVec3 delta = subtract(desired, projectile.target);
                    const Fixed distanceSquared = squaredLength(delta);
                    // RefCode avoids renormalising for sub-threshold target
                    // jitter (< 0.1 squared world units).
                    if (distanceSquared > kFixedTargetAdjustThresholdSquared) {
                        const Fixed distance = Fixed::sqrt(distanceSquared);
                        const Fixed maximumStep = projectile.targetAdjustDistancePerSecond / framesPerSecond;
                        if (distance > kFixedZero && maximumStep > kFixedZero) {
                            const Fixed step = minFixed(distance, maximumStep);
                            projectile.target = add(projectile.target, scale(delta, step / distance));
                            if (!rebuildDumbControls(projectile, terrain)) {
                                detonate(registry, lifecycle, content, spatialIndex, players, candidate.id,
                                         candidate.entity, projectile, transform, projectile.position,
                                         ObjectProjectileEventKind::PathInvalid, INVALID_OBJECT_ID,
                                         logicFramesPerSecond, confirmedTick,
                                         collisionCandidateScratch,
                                         candidateDamage,
                                         candidateHistoricBonusWeapons, m_events);
                                return;
                            }
                        }
                    }
                }
            }
        }

        if (projectile.currentStep >= projectile.pathSegments) {
            detonate(registry, lifecycle, content, spatialIndex, players, candidate.id, candidate.entity,
                     projectile, transform, projectile.position,
                     ObjectProjectileEventKind::ReachedDestination, INVALID_OBJECT_ID,
                     logicFramesPerSecond, confirmedTick,
                     collisionCandidateScratch, candidateDamage,
                     candidateHistoricBonusWeapons, m_events);
            return;
        }

        const uint32_t currentStep = projectile.currentStep;
        const FixedVec3 start = projectile.position;
        const FixedVec3 destination = cubicPointAtStep(projectile, currentStep);
        refreshFlightPathForward(projectile, currentStep);
        projectFlightPathYaw(
            registry, candidate.entity, transform, projectile);
        const ObjectGeometryComponent* projectileGeometry =
            ecs::try_get<ObjectGeometryComponent>(registry, candidate.entity);
        const Fixed projectileRadius = projectileGeometry
            ? Fixed::max(kFixedZero,
                  projectileGeometry->boundingSphereRadiusFixed)
            : kFixedZero;

        const game::WeaponTemplate* weapon = content.findWeapon(projectile.detonationWeapon);
        const std::optional<ProjectileCollision> collision = findProjectileCollision(
            registry, lifecycle, spatialIndex, players, candidate.entity,
            candidate.id, projectile, start, destination, projectileRadius,
            weapon, true, currentProjectileIds, collisionCandidateScratch,
            confirmedTick);
        if (collision) {
            const FixedVec3 impact = add(
                start, scale(subtract(destination, start), collision->time));
            if (clearGarrisonOnImpact(
                    registry, lifecycle, candidate.id, candidate.entity,
                    projectile, collision->target, impact, confirmedTick,
                    candidateDamage, m_events)) {
                return;
            }
            detonate(registry, lifecycle, content, spatialIndex, players, candidate.id, candidate.entity,
                     projectile, transform, impact, ObjectProjectileEventKind::Collided,
                     collision->target,
                     logicFramesPerSecond, confirmedTick,
                     collisionCandidateScratch, candidateDamage,
                     candidateHistoricBonusWeapons, m_events);
            return;
        }

        if (const std::optional<FixedVec3> bridgeImpact =
                bridgeLayerImpact(projectile, terrain, destination)) {
            detonate(registry, lifecycle, content, spatialIndex, players,
                     candidate.id, candidate.entity, projectile, transform,
                     *bridgeImpact, ObjectProjectileEventKind::Collided,
                     INVALID_OBJECT_ID, logicFramesPerSecond, confirmedTick,
                     collisionCandidateScratch, candidateDamage,
                     candidateHistoricBonusWeapons, m_events);
            return;
        }

        projectile.position = destination;
        writeAuthoritativeObjectPosition(
            registry, candidate.entity, destination);
        projectile.currentStep = currentStep + 1u;
        synchronizeProjectileTerrainLayer(
            registry, candidate.entity, projectile, confirmedTick);

        };
        updateCandidate();

        ObjectProjectileGameplayTransaction transaction{
            .projectile = candidate.id,
            .historicBonusWeapons =
                std::move(candidateHistoricBonusWeapons),
            .damage = std::move(candidateDamage),
        };
        for (size_t index = eventBegin; index < m_events.size(); ++index) {
            const ObjectProjectileEvent& event = m_events[index];
            switch (event.kind) {
            case ObjectProjectileEventKind::Collided:
            case ObjectProjectileEventKind::ReachedDestination:
            case ObjectProjectileEventKind::Expired:
            case ObjectProjectileEventKind::PathInvalid:
            case ObjectProjectileEventKind::GarrisonCleared:
                transaction.events.push_back({
                    .kind = event.kind,
                    .projectile = event.projectile,
                    .launcher = event.launcher,
                    .owner = event.owner,
                    .primaryTeam = event.primaryTeam,
                    .sourcePathfindLayer = event.sourcePathfindLayer,
                    .target = event.target,
                    .sourceShotSequence = event.sourceShotSequence,
                    .detonationWeapon = event.detonationWeapon,
                    .position = event.position,
                    .sourceVelocity = event.sourceVelocity,
                    .orientationRadians = event.orientationRadians,
                    .pitchRadians = event.pitchRadians,
                    .rollRadians = event.rollRadians,
                    .veterancy = event.veterancy,
                    .sourceAirborne = event.sourceAirborne,
                    .sourceOwnsFullAttitude =
                        event.sourceOwnsFullAttitude,
                    .confirmedTick = event.confirmedTick,
                });
                break;
            case ObjectProjectileEventKind::Spawned:
            case ObjectProjectileEventKind::Effect:
            case ObjectProjectileEventKind::GroundDecalBegin:
            case ObjectProjectileEventKind::GroundDecalEnd:
            case ObjectProjectileEventKind::UnsupportedTemplate:
                break;
            }
        }
        if (!transaction.historicBonusWeapons.empty() ||
            !transaction.damage.empty() || !transaction.events.empty()) {
            m_gameplayTransactions.push_back(std::move(transaction));
        }
    }
}

container::Vector<ObjectProjectileEvent> ObjectProjectileSystem::takeEvents() {
    container::Vector<ObjectProjectileEvent> result = std::move(m_events);
    m_events.clear();
    return result;
}

container::Vector<ObjectProjectileGameplayTransaction>
ObjectProjectileSystem::takeGameplayTransactions() {
    container::Vector<ObjectProjectileGameplayTransaction> result =
        std::move(m_gameplayTransactions);
    m_gameplayTransactions.clear();
    return result;
}

} // namespace engine
