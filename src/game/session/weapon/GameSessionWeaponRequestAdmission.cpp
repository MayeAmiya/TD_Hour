#include "game/session/weapon/GameSessionWeaponEventDrainDetail.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/frame/GameSessionFxAnchorSnapshot.h"

#include "debug/debug.h"
#include "core/container/string_utils.h"
#include "game/navigation/integration/NavigationTerrainLayerMapping.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/movement/ObjectFloat.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/lifecycle/ObjectStructureDestruction.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <iterator>
#include <optional>
#include <utility>
#include <variant>

namespace engine::detail {

navigation::NavigationMovementMask
objectCreationNavigationMovementMask(
    const game::ObjectArchetype& archetype,
    const GameContentSnapshot& content) noexcept {
    static_assert(game::locomotorSurfaceBit(game::LocomotorSurface::Ground) ==
                  navigation::NavigationMovement::Ground);
    static_assert(game::locomotorSurfaceBit(game::LocomotorSurface::Water) ==
                  navigation::NavigationMovement::Water);
    static_assert(game::locomotorSurfaceBit(game::LocomotorSurface::Cliff) ==
                  navigation::NavigationMovement::Cliff);
    static_assert(game::locomotorSurfaceBit(game::LocomotorSurface::Air) ==
                  navigation::NavigationMovement::Air);
    static_assert(game::locomotorSurfaceBit(game::LocomotorSurface::Rubble) ==
                  navigation::NavigationMovement::Rubble);

    navigation::NavigationMovementMask result = 0;
    const auto append = [&](const container::Vector<container::String>& names) {
        for (const container::String& name : names) {
            const game::FrozenLocomotorTemplate* locomotor =
                content.findLocomotor(name);
            if (locomotor && locomotor->supportsRuntimeLocomotion())
                result |= locomotor->surfaces;
        }
    };
    for (const game::LocomotorSetDefinition& set :
         archetype.templateData.locomotorSets) {
        if (set.slot == game::LocomotorSetSlot::Normal) append(set.templates);
    }
    if (archetype.templateData.locomotorSets.empty()) {
        append(archetype.templateData.locomotors);
        if (result == 0 && !archetype.templateData.locomotor.empty()) {
            if (const game::FrozenLocomotorTemplate* locomotor =
                    content.findLocomotor(archetype.templateData.locomotor);
                locomotor && locomotor->supportsRuntimeLocomotion()) {
                result = locomotor->surfaces;
            }
        }
    }
    return result != 0 ? result : navigation::NavigationMovement::Ground;
}

namespace {

[[nodiscard]] bool containsObjectKind(
    const ObjectKindOfComponent* kinds,
    game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

[[nodiscard]] bool cratePlacementBlocked(
    const ecs::registry& registry, const PlayerRegistry& players,
    math::q32_32 x, math::q32_32 y, PlayerId makerOwner,
    bool ignoreNonEnemyUnits) noexcept {
    const math::q32_32 crateSearchSphereRadius{int32_t{5}};
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const ObjectFixedTransformComponent>(registry);
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id) continue;
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, entity);
        if (status && status->hasAny(
                game::objectStatusBit(game::ObjectStatusFlag::NoCollisions))) {
            continue;
        }
        if (ignoreNonEnemyUnits) {
            const ObjectKindOfComponent* kinds =
                ecs::try_get<ObjectKindOfComponent>(registry, entity);
            const bool unit = containsObjectKind(
                                  kinds, game::ObjectKindOf::Infantry) ||
                              containsObjectKind(
                                  kinds, game::ObjectKindOf::Vehicle);
            if (unit) {
                const OwnerComponent* owner =
                    ecs::try_get<OwnerComponent>(registry, entity);
                const PlayerId otherOwner = owner
                    ? owner->player : NEUTRAL_PLAYER_ID;
                if (players.relationship(makerOwner, otherOwner) !=
                    PlayerRelationship::Enemies) {
                    continue;
                }
            }
        }
        const ObjectFixedTransformComponent& transform =
            view.template get<const ObjectFixedTransformComponent>(entity);
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(registry, entity);
        const math::q32_32 otherRadius = geometry
            ? std::max(math::q32_32{}, geometry->boundingCircleRadiusFixed)
            : math::q32_32{int32_t{1}};
        const math::q32_32 combined = crateSearchSphereRadius + otherRadius;
        const math::q32_32 dx = transform.position.x - x;
        const math::q32_32 dy = transform.position.y - y;
        if (dx * dx + dy * dy < combined * combined) return true;
    }
    return false;
}

} // namespace

std::optional<ObjectFixedTransformComponent> findCratePlacement(
    const ecs::registry& registry, const PlayerRegistry& players,
    const game::terrain::TerrainLogic& terrain,
    const navigation::NavigationSystem* navigationSystem,
    const ObjectCreateCrateDieEvent& event) {
    ObjectFixedTransformComponent result{
        .position = event.sourcePosition,
        .yawRadians = event.orientationRadians,
        .authoritative = true,
    };
    // RefCode bypasses findPositionAround for every non-ground layer and
    // later assigns that exact source layer to the new crate. Airborne is an
    // independent reason to retain the exact death position.
    if (event.sourcePathfindLayer !=
            game::terrain::kGroundPathfindLayer ||
        event.sourceAirborne || !terrain.isLoaded()) {
        return result;
    }

    const LogicFixedVec3 center = event.sourcePosition;
    if (!terrain.map().isInsidePlayableRaw(
            center.x.raw(), center.y.raw())) {
        // Original findPositionAround treats an off-map center as scripted
        // setup and accepts it verbatim.
        return result;
    }

    const math::q32_32 ringSpacing{int32_t{5}};
    const math::q32_32 fullTurn =
        math::q32_32::from_raw(26'986'075'409ll);
    const math::q32_32 half = math::q32_32::from_fraction(1, 2);
    const auto search = [&](math::q32_32 maximumRadius,
                            math::q32_32 startAngle,
                            bool ignoreNonEnemyUnits)
        -> std::optional<ObjectFixedTransformComponent> {
        for (math::q32_32 distance{}; distance <= maximumRadius;
             distance += ringSpacing) {
            const math::q32_32 angleSpacing = distance == math::q32_32{}
                ? fullTurn
                : (ringSpacing / (distance + math::q32_32{int32_t{1}})) *
                      (fullTurn / math::q32_32{int32_t{6}});
            const math::q32_32 halfSampleCount =
                (fullTurn / angleSpacing) * half;
            const int64_t wholeSamples = halfSampleCount.raw() >> 32u;
            const int32_t samples = std::max<int32_t>(
                1, static_cast<int32_t>(wholeSamples +
                    ((halfSampleCount.raw() & 0xffffffffll) != 0 ? 1 : 0)));
            const auto tryAngle = [&](math::q32_32 angle)
                -> std::optional<ObjectFixedTransformComponent> {
                const math::q32_32_sincos direction =
                    math::fixed_sincos(angle);
                const math::q32_32 fixedX =
                    center.x + distance * direction.cosine;
                const math::q32_32 fixedY =
                    center.y + distance * direction.sine;
                if (!terrain.map().isInsidePlayableRaw(
                        fixedX.raw(), fixedY.raw()) ||
                    terrain.isCliffCellRaw(fixedX.raw(), fixedY.raw()) ||
                    terrain.isUnderwaterLegacyRaw(fixedX.raw(), fixedY.raw()) ||
                    cratePlacementBlocked(registry, players, fixedX, fixedY,
                                          event.makerOwner,
                                          ignoreNonEnemyUnits)) {
                    return std::nullopt;
                }
                if (navigationSystem && navigationSystem->isInitialized()) {
                    navigation::NavigationLayerId groundLayer;
                    if (!navigation::tryNavigationLayerFromTerrainPathfindLayer(
                            game::terrain::kGroundPathfindLayer,
                            groundLayer)) {
                        return std::nullopt;
                    }
                    const navigation::NavigationDestinationAdjustmentResult
                        admission = navigation::adjustNavigationDestination(
                            navigationSystem->layers(),
                            {
                                .desired = {
                                    fixedX.raw(),
                                    fixedY.raw(),
                                    terrain.groundHeightRaw(
                                        fixedX.raw(), fixedY.raw()),
                                },
                                .layer = groundLayer,
                                .movementMask =
                                    navigation::NavigationMovement::Ground,
                                .allowAdjustment = false,
                            });
                    if (!admission.accepted()) return std::nullopt;
                }
                return ObjectFixedTransformComponent{
                    .position = {
                        fixedX,
                        fixedY,
                        math::q32_32::from_raw(terrain.groundHeightRaw(
                            fixedX.raw(), fixedY.raw())),
                    },
                    .yawRadians = event.orientationRadians,
                    .authoritative = true,
                };
            };
            for (int32_t sample = 0; sample < samples; ++sample) {
                if (std::optional<ObjectFixedTransformComponent> found =
                        tryAngle(startAngle + angleSpacing *
                            math::q32_32{sample})) {
                    return found;
                }
                if (sample != 0) {
                    if (std::optional<ObjectFixedTransformComponent> found =
                            tryAngle(startAngle - angleSpacing *
                                math::q32_32{sample})) {
                        return found;
                    }
                }
            }
        }
        return std::nullopt;
    };

    if (std::optional<ObjectFixedTransformComponent> found = search(
            math::q32_32{int32_t{5}}, event.nearSearchAngleRadians, true)) {
        return found;
    }
    return search(
        math::q32_32{int32_t{125}}, event.wideSearchAngleRadians, false);
}


bool GameSessionWeaponEventDrain::processReplacement(
    const WorkItem& item) {
    const ObjectReplacementInvocation& replacement =
        item.replacement;
    if (++m_processedReplacements > kMaximumObjectReplacements) {
        TD_LOG_ERROR(
            "[GameSession] ReplaceObjectUpgrade chain exceeded {} replacements at tick {}; dropping the malformed tail",
            kMaximumObjectReplacements, m_presentation.m_confirmedTick);
        discardPendingWork();
        return false;
    }
    const std::optional<ecs::entity> source =
        m_world.m_objects.entityFromId(replacement.source);
    const OwnerComponent* owner = source
        ? ecs::try_get<OwnerComponent>(m_world.m_registry, *source)
        : nullptr;
    const PrimaryTeamComponent* team = source
        ? ecs::try_get<PrimaryTeamComponent>(m_world.m_registry, *source)
        : nullptr;
    if (!source || !owner || owner->player != replacement.owner ||
        !team || team->team != replacement.primaryTeam ||
        replacement.replacementTemplate.empty() ||
        !m_content.m_contentSnapshot.findObjectArchetype(
            replacement.replacementTemplate)) {
        return true;
    }
    if (!m_lifecycle.requestDestroyObject(replacement.source,
                              ObjectDestroyReason::System,
                              replacement.confirmedTick)) {
        return true;
    }
    const GameSessionObjectSpawnResult spawned = m_lifecycle.spawnObject({
        .templateName = replacement.replacementTemplate,
        .owner = replacement.owner,
        .primaryTeam = replacement.primaryTeam,
        .transform = {
            .position = replacement.position,
            .yawRadians = replacement.orientationRadians,
            .authoritative = true,
        },
        .origin = ObjectCreationOrigin::System,
        .confirmedTick = replacement.confirmedTick,
        .scoreAsBuilt = true,
        .academyAsProduction = true,
        .scoreConstructionCost = true,
    });
    if (spawned) {
        if (replacement.sourceOwnsFullAttitude && spawned.entity) {
            if (ObjectPhysicsComponent* physics =
                    ecs::try_get<ObjectPhysicsComponent>(
                        m_world.m_registry, *spawned.entity)) {
                physics->yaw = replacement.orientationRadians;
                physics->pitch = replacement.pitchRadians;
                physics->roll = replacement.rollRadians;
                physics->ownsAttitude = true;
            }
        }
        // ReplaceObjectUpgrade removes the predecessor from the path
        // map before inserting its successor. Rebuild this modern
        // broad phase at the same structural boundary so causally
        // nested OCL/weapon work can already see the new object.
        m_world.m_spatialIndex.refreshDirty(
            m_world.m_registry,
            m_world.m_objects);
    }
    container::Vector<WorkItem> nested;
    collectPendingWork(nested);
    pushPendingWork(std::move(nested));
    return true;
}

void GameSessionWeaponEventDrain::processCrate(const WorkItem& item) {
    const ObjectCreateCrateDieEvent& source = item.crate;
    if (source.crateObjectTemplate.empty() || !source.owner ||
        !m_content.m_players.get(source.owner) ||
        !m_content.m_contentSnapshot.findObjectArchetype(
            source.crateObjectTemplate)) {
        return;
    }
    const std::optional<ObjectFixedTransformComponent> placement =
        findCratePlacement(m_world.m_registry, m_content.m_players, m_content.m_terrain,
                           &m_content.m_navigation, source);
    if (!placement) return;
    const GameSessionObjectSpawnResult spawnedCrate = m_lifecycle.spawnObject({
        .templateName = source.crateObjectTemplate,
        .owner = source.owner,
        .transform = *placement,
        .initialPathfindLayer = source.sourcePathfindLayer,
        .origin = ObjectCreationOrigin::System,
        .confirmedTick = source.confirmedTick,
    });
    if (spawnedCrate && spawnedCrate.entity) {
        ecs::emplace<ObjectCrateTerrainDecalComponent>(
            m_world.m_registry, *spawnedCrate.entity,
            ObjectCrateTerrainDecalComponent{
                .createdAtTick = source.confirmedTick,
            });
        setObjectTerrainDecalKind(
            m_world.m_registry, *spawnedCrate.entity,
            ObjectTerrainDecalKind::Crate,
            source.confirmedTick, true);
        setObjectTerrainDecalFade(
            m_world.m_registry, *spawnedCrate.entity,
            math::q32_32{int32_t{1}},
            math::q32_32::from_fraction(3, 100), source.confirmedTick);
    }
    container::Vector<WorkItem> nested;
    collectPendingWork(nested);
    pushPendingWork(std::move(nested));
}

bool GameSessionWeaponEventDrain::processWeapon(WorkItem item) {
    if (++m_processedCommands > kMaximumSystemWeaponCommands) {
        TD_LOG_ERROR(
            "[GameSession] System weapon reaction chain exceeded {} commands at tick {}; dropping the malformed tail",
            kMaximumSystemWeaponCommands,
            m_presentation.m_confirmedTick);
        discardPendingWork();
        return false;
    }

    const game::WeaponTemplate* definition =
        m_content.m_contentSnapshot.findWeapon(
            item.weapon.content);
    if (!definition) return false;

    size_t veterancyIndex = 0;
    PlayerId sourceOwner = INVALID_PLAYER_ID;
    ObjectTeamId sourceTeam = INVALID_OBJECT_TEAM_ID;
    LogicFixedVec3 sourceVelocity;
    ObjectPhysicsComponent::Scalar sourceOrientation{};
    ObjectPhysicsComponent::Scalar sourcePitch{};
    ObjectPhysicsComponent::Scalar sourceRoll{};
    uint32_t sourcePathfindLayer =
        game::terrain::kGroundPathfindLayer;
    bool sourceOwnsFullAttitude = false;
    bool sourceAirborne = false;
    if (const std::optional<ecs::entity> sourceEntity =
            m_world.m_objects
                .entityFromIdIncludingPending(item.weapon.source)) {
        if (const ObjectVeterancyComponent* veterancy =
                ecs::try_get<ObjectVeterancyComponent>(
                    m_world.m_registry,
                    *sourceEntity)) {
            veterancyIndex = std::min<size_t>(
                static_cast<size_t>(veterancy->level),
                game::WeaponTemplate::kVeterancyLevelCount - 1);
        }
        if (const OwnerComponent* owner =
                ecs::try_get<OwnerComponent>(
                    m_world.m_registry,
                    *sourceEntity)) {
            sourceOwner = owner->player;
        }
        if (const PrimaryTeamComponent* team =
                ecs::try_get<PrimaryTeamComponent>(
                    m_world.m_registry,
                    *sourceEntity)) {
            sourceTeam = team->team;
        }
        if (const ObjectPhysicsComponent* physics =
                ecs::try_get<ObjectPhysicsComponent>(
                    m_world.m_registry,
                    *sourceEntity)) {
            sourceVelocity = physics->velocityUnitsPerSecond;
            if (physics->ownsAttitude) {
                sourceOrientation = physics->yaw;
                sourcePitch = physics->pitch;
                sourceRoll = physics->roll;
                sourceOwnsFullAttitude = true;
            }
        }
        if (const TransformComponent* transform =
                ecs::try_get<TransformComponent>(
                    m_world.m_registry,
                    *sourceEntity)) {
            if (!sourceOwnsFullAttitude) {
                sourceOrientation = readAuthoritativeObjectYaw(
                    m_world.m_registry,
                    *sourceEntity, *transform);
            }
        }
        if (const ObjectAirborneComponent* airborne =
                ecs::try_get<ObjectAirborneComponent>(
                    m_world.m_registry,
                    *sourceEntity)) {
            sourceAirborne = airborne->isAirborne;
        }
        if (const ObjectTerrainLayerComponent* terrainLayer =
                ecs::try_get<ObjectTerrainLayerComponent>(
                    m_world.m_registry,
                    *sourceEntity)) {
            sourcePathfindLayer = terrainLayer->pathfindLayer;
        }
    }

    const game::ObjectCreationListContentId fireOcl =
        definition->fireOclIds[veterancyIndex];
    if (!fireOcl || !sourceOwner || !sourceTeam) {
        return processWeaponImpact(std::move(item));
    }

    ObjectCreationListInvocation invocation{
        .content = fireOcl,
        .source = item.weapon.source,
        .owner = sourceOwner,
        .primaryTeam = sourceTeam,
        .primaryPosition = item.weapon.sourcePosition,
        .sourceVelocity = sourceVelocity,
        .orientationRadians = sourceOrientation,
        .pitchRadians = sourcePitch,
        .rollRadians = sourceRoll,
        .veterancy =
            static_cast<game::ObjectVeterancyLevel>(veterancyIndex),
        .authoredOrder = item.weapon.authoredOrder,
        .emissionSequence = item.weapon.emissionSequence,
        .confirmedTick = item.weapon.confirmedTick,
        .sourcePathfindLayer = sourcePathfindLayer,
        .sourceAirborne = sourceAirborne,
        .sourceOwnsFullAttitude = sourceOwnsFullAttitude,
    };

    // ZH Weapon::fire executes FireOCL synchronously before it creates a
    // projectile or applies direct damage. Preserve that call-stack order by
    // placing the impact continuation below the OCL on the LIFO journal.
    ++m_reservedStructuralTransactions;
    item.kind = WorkKind::WeaponImpact;
    pushWork(std::move(item));
    pushWork({
        .kind = WorkKind::ObjectCreationList,
        .ocl = std::move(invocation),
        .oclState = std::make_shared<OclWorkState>(),
    });
    return true;
}

bool GameSessionWeaponEventDrain::processWeaponImpact(WorkItem item) {
    container::Vector<ObjectDamageRequest> damage;
    container::Vector<ObjectProjectileSpawnRequest> projectiles;
    m_world.m_objectCombat
        .executeSystemWeaponFires(
            m_world.m_registry,
            m_world.m_objects,
            m_content.m_contentSnapshot,
            &m_world.m_spatialIndex,
            &m_content.m_players,
            container::Span<const ObjectSystemWeaponFireCommand>{
                &item.weapon, 1},
            static_cast<uint32_t>(std::max(
                1, m_content.m_startInfo.gameSpeedFPS)),
            damage, &projectiles);
    m_weaponEvents.publish(
        m_world.m_objectCombat.takeEvents());

    // Structural projectile creation stays centralized in GameSession.
    // Pending-destroy launchers remain readable until the normal final
    // lifecycle flush, so death weapons retain their owner/team snapshot.
    for (const ObjectProjectileSpawnRequest& request : projectiles) {
        static_cast<void>(m_projectiles.spawn(request));
    }

    // Reverse-push preserves the executor's deterministic radius-target
    // order while allowing each target's nested chain to finish first.
    for (auto iterator = damage.rbegin(); iterator != damage.rend();
         ++iterator) {
        pushWork({
            .kind = WorkKind::Damage,
            .damage = std::move(*iterator),
        });
    }
    container::Vector<ObjectHistoricBonusWeaponFire> historicBonus =
        m_world.m_objectCombat.takeHistoricBonusWeaponFires();
    for (auto iterator = historicBonus.rbegin();
         iterator != historicBonus.rend(); ++iterator) {
        pushWork({
            .kind = WorkKind::Weapon,
            .weapon = {
                .source = iterator->source,
                .content = iterator->content,
                .sourcePosition = iterator->position,
                .impactPosition = iterator->position,
                .sourceShotSequence = iterator->sourceSequence,
                .authoredOrder = iterator->authoredOrder,
                .emissionSequence = m_world.m_objectSimulation
                    .reserveGameplaySubmissionOrdinal(),
                .confirmedTick = iterator->confirmedTick,
            },
        });
    }
    container::Vector<WorkItem> nested;
    collectPendingWork(nested);
    pushPendingWork(std::move(nested));
    return true;
}

} // namespace engine::detail
