#include "game/session/core/GameSession.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/object/GameSessionObjectLifecycleDetail.h"
#include "game/session/transaction/GameSessionBridgeLifecycleTransactions.h"

#include "core/container/string_utils.h"
#include "debug/debug.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/combat/ObjectCountermeasures.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/runtime/ObjectStatus.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <iterator>
#include <numeric>
#include <optional>
#include <utility>
#include <variant>

namespace engine {
using namespace object_lifecycle_detail;

namespace {

[[nodiscard]] int64_t fixedMidpointRaw(int64_t minimum,
                                       int64_t maximum) noexcept {
    return std::midpoint(minimum, maximum);
}

[[nodiscard]] math::q32_32 fixedAbsoluteRawDifference(
    int64_t value, int64_t origin) noexcept {
    const math::q32_32 difference =
        math::q32_32::from_raw(value) - math::q32_32::from_raw(origin);
    return difference < math::q32_32{} ? -difference : difference;
}

struct BridgeSurfaceQuery final {
    game::terrain::TerrainPathfindLayerId layer =
        game::terrain::kGroundPathfindLayer;
    LogicFixedVec3 center{};
    math::q32_32 radius{};
};

[[nodiscard]] std::optional<BridgeSurfaceQuery> bridgeSurfaceQuery(
    const game::terrain::TerrainElevatedPathfindSurface& surface) noexcept {
    if (surface.boundaryRaw.size() < 3) return std::nullopt;
    int64_t minimumX = surface.boundaryRaw.front()[0];
    int64_t maximumX = minimumX;
    int64_t minimumY = surface.boundaryRaw.front()[1];
    int64_t maximumY = minimumY;
    for (const container::Array<int64_t, 3>& point : surface.boundaryRaw) {
        minimumX = std::min(minimumX, point[0]);
        maximumX = std::max(maximumX, point[0]);
        minimumY = std::min(minimumY, point[1]);
        maximumY = std::max(maximumY, point[1]);
    }
    const int64_t centerX = fixedMidpointRaw(minimumX, maximumX);
    const int64_t centerY = fixedMidpointRaw(minimumY, maximumY);
    // The spatial index performs a circular broad phase.  The Manhattan
    // half-extent encloses the complete authored polygon without requiring a
    // floating-point square root; pathfindLayerHeightRawAt below remains the
    // exact fixed-point polygon/height test.
    const math::q32_32 radius =
        math::q32_32::max(fixedAbsoluteRawDifference(minimumX, centerX),
                          fixedAbsoluteRawDifference(maximumX, centerX)) +
        math::q32_32::max(fixedAbsoluteRawDifference(minimumY, centerY),
                          fixedAbsoluteRawDifference(maximumY, centerY));
    return BridgeSurfaceQuery{
        .layer = surface.layer,
        .center = {
            math::q32_32::from_raw(centerX),
            math::q32_32::from_raw(centerY),
            math::q32_32::from_raw(surface.heightRaw),
        },
        .radius = radius,
    };
}

} // namespace

GameSessionBridgeLifecycleTransactions::
GameSessionBridgeLifecycleTransactions(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionScriptPresentationState& presentation,
    GameSessionLifecycleTransactionPort lifecycle,
    GameSessionObjectDamageTransactions damage) noexcept
    : m_content(content),
      m_world(world),
      m_presentation(presentation),
      m_lifecycle(lifecycle),
      m_damage(std::move(damage)) {}

bool GameSessionBridgeLifecycleTransactions::collapseTerrainSurface(
    uint64_t sourceRecordIndex, uint64_t confirmedTick,
    bool permanentlyRemove) {
    if (sourceRecordIndex == UINT64_MAX) return false;
    container::Vector<game::terrain::TerrainPathfindLayerId> layers;
    for (const game::terrain::TerrainElevatedPathfindSurface& surface :
         m_content.m_terrain.elevatedPathfindSurfaces()) {
        if (surface.sourceRecordIndex != sourceRecordIndex ||
            std::find(layers.begin(), layers.end(), surface.layer) !=
                layers.end()) {
            continue;
        }
        layers.push_back(surface.layer);
    }
    if (layers.empty()) return false;
    if (!permanentlyRemove) {
        const bool hasActiveSurface = std::any_of(
            m_content.m_terrain.elevatedPathfindSurfaces().begin(),
            m_content.m_terrain.elevatedPathfindSurfaces().end(),
            [sourceRecordIndex](
                const game::terrain::TerrainElevatedPathfindSurface& surface) {
                return surface.sourceRecordIndex == sourceRecordIndex &&
                       surface.active;
            });
        if (!hasActiveSurface) return true;
    }
    std::sort(layers.begin(), layers.end());

    // Match RefCode's PartitionManager range query.  Refresh only dirty
    // spatial records at this confirmed barrier, then union the deterministic
    // ObjectId-sorted candidates for every terrain surface owned by this
    // bridge.  The exact polygon/layer test remains below.
    m_world.m_spatialIndex.refreshDirty(
        m_world.m_registry,
        m_world.m_objects);
    container::Vector<ObjectId> occupants;
    for (const game::terrain::TerrainElevatedPathfindSurface& surface :
         m_content.m_terrain.elevatedPathfindSurfaces()) {
        if (surface.sourceRecordIndex != sourceRecordIndex) continue;
        const std::optional<BridgeSurfaceQuery> query =
            bridgeSurfaceQuery(surface);
        if (!query) continue;
        container::Vector<ObjectId> candidates =
            m_world.m_spatialIndex.queryRadiusFixed(
                query->center, query->radius);
        occupants.insert(occupants.end(), candidates.begin(),
                         candidates.end());
    }
    std::sort(occupants.begin(), occupants.end());
    occupants.erase(std::unique(occupants.begin(), occupants.end()),
                    occupants.end());

    uint32_t killSequence = 1;
    bool queuedKill = false;
    for (const ObjectId occupant : occupants) {
        const std::optional<ecs::entity> occupantEntity =
            m_world.m_objects.entityFromId(occupant);
        if (!occupantEntity) continue;
        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(m_world.m_registry,
                                                 *occupantEntity);
        if (hasObjectKind(kinds, game::ObjectKindOf::Bridge) ||
            hasObjectKind(kinds, game::ObjectKindOf::BridgeTower)) {
            continue;
        }
        ObjectTerrainLayerComponent& layer =
            ecs::get<ObjectTerrainLayerComponent>(m_world.m_registry,
                                                   *occupantEntity);
        if (!std::binary_search(layers.begin(), layers.end(),
                                layer.pathfindLayer)) {
            continue;
        }
        const TransformComponent& transform =
            ecs::get<TransformComponent>(m_world.m_registry,
                                         *occupantEntity);
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            m_world.m_registry, *occupantEntity, transform);
        const std::optional<int64_t> oldSurface = m_content
            .m_terrain.pathfindLayerHeightRawAt(
                layer.pathfindLayer, position.x.raw(), position.y.raw());
        // BridgeBehavior ignores aircraft/debris genuinely above the bridge;
        // only objects resting on the collapsing support participate.
        if (!oldSurface || position.z >
                math::q32_32::from_raw(*oldSurface) +
                    math::q32_32::from_fraction(1, 100)) continue;

        static_cast<void>(layer.assign(
            game::terrain::kGroundPathfindLayer, confirmedTick));
        if (ObjectPhysicsComponent* physics =
                ecs::try_get<ObjectPhysicsComponent>(m_world.m_registry,
                                                       *occupantEntity)) {
            physics->allowToFall = true;
            physics->sleeping = false;
            continue;
        }
        queuedKill |= m_damage.queueObjectDamage({
            .target = occupant,
            .sourceSequence = killSequence++,
            .damageType = game::DamageType::UNRESISTABLE,
            .deathType = game::DeathType::NORMAL,
            .forceKill = true,
            .confirmedTick = confirmedTick,
        });
    }
    const bool removed = permanentlyRemove
        ? m_content.m_terrain.destroyBridgeBySourceRecordIndex(sourceRecordIndex)
        : m_content.m_terrain.setBridgeActiveBySourceRecordIndex(
              sourceRecordIndex, false);
    if (queuedKill) m_damage.resolveQueuedObjectDamage();
    return removed;
}

bool GameSessionBridgeLifecycleTransactions::createScaffolding(
    ObjectId bridge, uint64_t confirmedTick) {
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick || !bridge ||
        m_world.m_objects.isPendingDestroy(bridge)) {
        return false;
    }
    const std::optional<ecs::entity> bridgeEntity =
        m_world.m_objects.entityFromId(bridge);
    if (!bridgeEntity) return false;
    ObjectBridgeComponent* bridgeState =
        ecs::try_get<ObjectBridgeComponent>(m_world.m_registry, *bridgeEntity);
    const MapObjectProvenanceComponent* provenance =
        ecs::try_get<MapObjectProvenanceComponent>(m_world.m_registry, *bridgeEntity);
    const ThingTemplateComponent* bridgeTemplate =
        ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *bridgeEntity);
    const ObjectGeometryComponent* bridgeGeometry =
        ecs::try_get<ObjectGeometryComponent>(m_world.m_registry, *bridgeEntity);
    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(m_world.m_registry, *bridgeEntity);
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(m_world.m_registry, *bridgeEntity);
    const std::optional<ObjectTeamId> team = m_world.m_objectTeams.teamOf(bridge);
    if (!bridgeState || !bridgeState->plan ||
        bridgeState->plan->bridges.empty() ||
        !owner || !team || !transform ||
        bridgeState->scaffoldRequestSequence ==
            std::numeric_limits<uint64_t>::max()) {
        return false;
    }
    if (bridgeState->scaffoldingPresent) return false;

    // RefCode keeps one scaffold generation while it tears down. A new repair
    // reverses those exact objects (Sink->Rise / TearDownAcross->BuildAcross)
    // instead of spawning a second overlapping generation. Validate the
    // whole stable-ID set before mutating any member so partial reversal
    // cannot leave a mixed generation.
    if (!bridgeState->scaffoldObjects.empty()) {
        bool reversible = true;
        for (const ObjectId scaffold : bridgeState->scaffoldObjects) {
            const std::optional<ecs::entity> scaffoldEntity =
                m_world.m_objects.entityFromId(scaffold);
            const ObjectBridgeScaffoldComponent* motion = scaffoldEntity
                ? ecs::try_get<ObjectBridgeScaffoldComponent>(
                      m_world.m_registry, *scaffoldEntity)
                : nullptr;
            if (!motion || !motion->configured ||
                m_world.m_objects.isPendingDestroy(scaffold)) {
                reversible = false;
                break;
            }
        }
        if (reversible) {
            const uint64_t sequence = ++bridgeState->scaffoldRequestSequence;
            for (const ObjectId scaffold : bridgeState->scaffoldObjects) {
                if (!m_world.m_objectSimulation.applyBridgeScaffoldMotionRequest(
                        m_world.m_registry, m_world.m_objects,
                        {.kind = ObjectBridgeScaffoldRequestKind::Reverse,
                         .scaffold = scaffold,
                         .bridge = bridge,
                         .sequence = sequence,
                         .confirmedTick = confirmedTick})) {
                    return false;
                }
            }
            bridgeState->scaffoldingPresent = true;
            ++bridgeState->scaffoldRevision;
            return true;
        }
        // The old generation has already crossed deferred destruction and
        // cannot be resurrected. Forget only its stale IDs, then build a
        // complete replacement through the normal central spawn transaction.
        bridgeState->scaffoldObjects.clear();
        ++bridgeState->scaffoldRevision;
    }

    constexpr int32_t kBridgePoint1 = 0x00000010;
    constexpr int32_t kBridgePoint2 = 0x00000020;
    container::StringView bridgeStyleName;
    LogicFixedVec3 fromPosition{};
    LogicFixedVec3 toPosition{};
    if (provenance && (provenance->mapFlags & kBridgePoint1) != 0) {
        const auto& mapObjects = m_content.m_terrain.map().heightfield().objects;
        const uint64_t sourceIndex = provenance->sourceRecordIndex;
        if (sourceIndex >= mapObjects.size() ||
            sourceIndex + 1u >= mapObjects.size()) {
            return false;
        }
        const game::terrain::MapObjectRecord& from =
            mapObjects[static_cast<size_t>(sourceIndex)];
        const game::terrain::MapObjectRecord& to =
            mapObjects[static_cast<size_t>(sourceIndex + 1u)];
        if ((from.flags & kBridgePoint1) == 0 ||
            (to.flags & kBridgePoint2) == 0) {
            return false;
        }
        bridgeStyleName = from.name;
        fromPosition.x = math::q32_32::from_raw(from.positionRaw[0]);
        fromPosition.y = math::q32_32::from_raw(from.positionRaw[1]);
        fromPosition.z = math::q32_32::from_raw(
            m_content.m_terrain.groundHeightRaw(
                fromPosition.x.raw(), fromPosition.y.raw())) +
            math::q32_32::from_fraction(1, 4);
        toPosition.x = math::q32_32::from_raw(to.positionRaw[0]);
        toPosition.y = math::q32_32::from_raw(to.positionRaw[1]);
        toPosition.z = math::q32_32::from_raw(
            m_content.m_terrain.groundHeightRaw(
                toPosition.x.raw(), toPosition.y.raw())) +
            math::q32_32::from_fraction(1, 4);
    } else {
        // RefCode Bridge(Object*) derives landmark BridgeInfo from the live
        // BOX geometry and uses the Thing template name as its Roads.ini key.
        // This is an exact authored boundary, not a nearest-terrain-bridge
        // fallback.
        if (!bridgeTemplate || !bridgeTemplate->archetype ||
            !bridgeGeometry ||
            bridgeGeometry->shape != ObjectGeometryShape::Box ||
            bridgeGeometry->majorRadiusFixed <= math::q32_32{}) {
            return false;
        }
        bridgeStyleName = bridgeTemplate->archetype->templateData.name;
        const LogicFixedVec3 bridgePosition =
            readAuthoritativeObjectPosition(
                m_world.m_registry, *bridgeEntity, *transform);
        const math::q32_32 bridgeRadius = bridgeGeometry->majorRadiusFixed;
        const math::q32_32_sincos heading = math::fixed_sincos(
            readAuthoritativeObjectYaw(
                m_world.m_registry,
                *bridgeEntity, *transform));
        fromPosition = {
            bridgePosition.x - bridgeRadius * heading.cosine,
            bridgePosition.y - bridgeRadius * heading.sine,
            bridgePosition.z,
        };
        toPosition = {
            bridgePosition.x + bridgeRadius * heading.cosine,
            bridgePosition.y + bridgeRadius * heading.sine,
            bridgePosition.z,
        };
    }
    const script::ScriptTerrainBridgeStyle* style =
        m_presentation.m_scriptTerrainRoadPresentationSettings.findBridge(bridgeStyleName);
    if (!style || style->scaffoldObjectName.empty() ||
        style->scaffoldSupportObjectName.empty()) {
        return false;
    }
    const container::SharedPtr<const game::ObjectArchetype> scaffoldArchetype =
        m_content.m_contentSnapshot.findObjectArchetype(style->scaffoldObjectName);
    const container::SharedPtr<const game::ObjectArchetype> supportArchetype =
        m_content.m_contentSnapshot.findObjectArchetype(
            style->scaffoldSupportObjectName);
    if (!scaffoldArchetype || !supportArchetype ||
        !scaffoldArchetype->bridgeRailPlan ||
        scaffoldArchetype->bridgeRailPlan->scaffolds.empty() ||
        !supportArchetype->bridgeRailPlan ||
        supportArchetype->bridgeRailPlan->scaffolds.empty()) {
        return false;
    }

    const PlayerId scaffoldOwner = owner->player;
    const uint64_t requestSequence =
        ++bridgeState->scaffoldRequestSequence;
    const game::ObjectBridgeBehaviorRule& bridgeRule =
        bridgeState->plan->bridges.front();
    const ObjectBridgeScaffoldLayoutRequest layout{
        .bridge = bridge,
        .scaffoldTemplateName = scaffoldArchetype->templateData.name,
        .scaffoldSupportTemplateName = supportArchetype->templateData.name,
        .fromPosition = fromPosition,
        .toPosition = toPosition,
        .bridgeCenter = readAuthoritativeObjectPosition(
            m_world.m_registry, *bridgeEntity, *transform),
        .scaffoldSpacing =
            scaffoldArchetype->templateData.geometry.majorRadiusFixed *
            math::q32_32{int32_t{2}},
        .scaffoldHeight =
            scaffoldArchetype->templateData.geometry.heightFixed,
        .scaffoldSupportHeight =
            supportArchetype->templateData.geometry.heightFixed,
        .lateralSpeedPerFrame = bridgeRule.lateralScaffoldSpeed,
        .verticalSpeedPerFrame = bridgeRule.verticalScaffoldSpeed,
        .requestSequence = requestSequence,
        .confirmedTick = confirmedTick,
    };
    std::optional<ObjectBridgeScaffoldSpawnPlan> plan =
        buildObjectBridgeScaffoldSpawnPlan(layout);
    if (!plan || plan->objects.empty()) return false;

    container::Vector<ObjectId> spawnedObjects;
    spawnedObjects.reserve(plan->objects.size());
    const auto rollback = [&]() {
        for (const ObjectId object : spawnedObjects) {
            static_cast<void>(m_lifecycle.requestDestroyObject(
                object, ObjectDestroyReason::System, confirmedTick));
        }
    };
    for (ObjectBridgeScaffoldSpawnSpec& spec : plan->objects) {
        ObjectSpawnRequest spawn;
        spawn.templateName = spec.templateName;
        spawn.owner = scaffoldOwner;
        spawn.primaryTeam = *team;
        spawn.origin = ObjectCreationOrigin::System;
        spawn.confirmedTick = confirmedTick;
        spawn.transform = ObjectFixedTransformComponent{
            .position = spec.motion.createPosition,
            .yawRadians = spec.motion.orientationRadians,
            .authoritative = true,
        };
        const GameSessionObjectSpawnResult spawned =
            m_lifecycle.spawnObject(std::move(spawn));
        if (!spawned) {
            rollback();
            return false;
        }
        spec.motion.scaffold = spawned.object;
        if (!m_world.m_objectSimulation.applyBridgeScaffoldMotionRequest(
                m_world.m_registry, m_world.m_objects, spec.motion)) {
            spawnedObjects.push_back(spawned.object);
            rollback();
            return false;
        }
        spawnedObjects.push_back(spawned.object);
    }

    // Spawning may grow EnTT storage, so reacquire the bridge component before
    // committing the stable-ID generation rather than retaining a raw pointer
    // across central lifecycle calls.
    bridgeState = ecs::try_get<ObjectBridgeComponent>(
        m_world.m_registry, *bridgeEntity);
    if (!bridgeState || bridgeState->scaffoldingPresent) {
        rollback();
        return false;
    }
    bridgeState->scaffoldObjects = std::move(spawnedObjects);
    bridgeState->scaffoldingPresent = true;
    ++bridgeState->scaffoldRevision;
    return true;
}

bool GameSessionBridgeLifecycleTransactions::removeScaffolding(
    ObjectId bridge, uint64_t confirmedTick) {
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick || !bridge ||
        m_world.m_objects.isPendingDestroy(bridge)) {
        return false;
    }
    const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(bridge);
    if (!entity) return false;
    ObjectBridgeComponent* state =
        ecs::try_get<ObjectBridgeComponent>(m_world.m_registry, *entity);
    if (!state || !state->scaffoldingPresent ||
        state->scaffoldRequestSequence ==
            std::numeric_limits<uint64_t>::max()) {
        return false;
    }
    const uint64_t sequence = ++state->scaffoldRequestSequence;
    for (const ObjectId scaffold : state->scaffoldObjects) {
        static_cast<void>(m_world.m_objectSimulation.applyBridgeScaffoldMotionRequest(
            m_world.m_registry, m_world.m_objects,
            {.kind = ObjectBridgeScaffoldRequestKind::Reverse,
             .scaffold = scaffold,
             .bridge = bridge,
             .sequence = sequence,
             .confirmedTick = confirmedTick}));
    }
    // Keep the stable IDs while teardown is in flight. createScaffolding can
    // reverse this same generation if another repair starts before Sink
    // reaches deferred destruction.
    state->scaffoldingPresent = false;
    ++state->scaffoldRevision;
    return true;
}

bool GameSessionBridgeLifecycleTransactions::scaffoldingPresent(
    ObjectId bridge) const noexcept {
    const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(bridge);
    if (!entity) return false;
    const ObjectBridgeComponent* state =
        ecs::try_get<ObjectBridgeComponent>(m_world.m_registry, *entity);
    return state && state->scaffoldingPresent;
}

bool GameSessionBridgeLifecycleTransactions::scaffoldingInMotion(
    ObjectId bridge) const noexcept {
    const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(bridge);
    if (!entity) return false;
    const ObjectBridgeComponent* state =
        ecs::try_get<ObjectBridgeComponent>(m_world.m_registry, *entity);
    if (!state || !state->scaffoldingPresent) return false;
    for (const ObjectId scaffold : state->scaffoldObjects) {
        const std::optional<ecs::entity> scaffoldEntity =
            m_world.m_objects.entityFromId(scaffold);
        if (!scaffoldEntity) continue;
        const ObjectBridgeScaffoldComponent* motion =
            ecs::try_get<ObjectBridgeScaffoldComponent>(
                m_world.m_registry, *scaffoldEntity);
        if (motion && motion->configured &&
            motion->motion != ObjectBridgeScaffoldMotion::Still) {
            return true;
        }
    }
    return false;
}

} // namespace engine
