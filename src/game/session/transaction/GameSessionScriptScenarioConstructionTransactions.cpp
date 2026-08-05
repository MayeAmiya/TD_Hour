#include "game/session/transaction/GameSessionScriptScenarioPlanTransactions.h"

#include "game/session/core/GameSession.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/script/GameSessionScriptTeamsDetail.h"
#include "game/session/object/GameSessionObjectLifecycleDetail.h"
#include "game/session/object/GameSessionObjectContracts.h"
#include "game/session/command/OrderContracts.h"
#include "game/session/command/OrderExecutor.h"

#include "core/container/string_utils.h"
#include "debug/debug.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/combat/ObjectCountermeasures.h"
#include "game/object/contracts/ObjectToppleMath.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/ai/definition/ObjectAIBehaviorPlan.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <iterator>
#include <variant>

namespace engine {
using namespace script_team_detail;
using namespace object_lifecycle_detail;
bool GameSessionScriptScenarioPlanTransactions::buildScriptSupplyCenter(
    PlayerId player, container::StringView objectType,
    int32_t minimumSupplies, uint32_t sourceSequence,
    uint64_t confirmedTick)
{
    if (!m_port) return {};
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick || objectType.empty()) {
        return false;
    }
    const PlayerState* playerState = m_content.m_players.get(player);
    if (!playerState) return false;
    if (playerState->controller != PlayerControllerKind::Ai) return true;
    const container::SharedPtr<const game::ObjectArchetype> product =
        m_content.m_contentSnapshot.findObjectArchetype(objectType);
    if (!product) return true;

    const auto objects = ecs::view<
        const ObjectIdentityComponent, const OwnerComponent,
        const ObjectLifecycleComponent, const ThingTemplateComponent,
        const TransformComponent>(m_world.m_registry);
    math::q32_32 baseX{};
    math::q32_32 baseY{};
    uint64_t baseCount = 0;
    math::q32_32 enemyX{};
    math::q32_32 enemyY{};
    uint64_t enemyCount = 0;
    for (const ecs::entity entity : objects) {
        const ObjectLifecycleComponent& lifecycle =
            objects.template get<const ObjectLifecycleComponent>(entity);
        const ThingTemplateComponent& type =
            objects.template get<const ThingTemplateComponent>(entity);
        if (lifecycle.phase != ObjectLifecyclePhase::Alive ||
            !type.archetype || !kindOfListContains(
                type.archetype, game::ObjectKindOf::Structure)) {
            continue;
        }
        const OwnerComponent& owner =
            objects.template get<const OwnerComponent>(entity);
        const TransformComponent& transform =
            objects.template get<const TransformComponent>(entity);
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            m_world.m_registry, entity, transform);
        if (owner.player == player) {
            baseX += position.x;
            baseY += position.y;
            ++baseCount;
        } else if (m_content.m_players.relationships().get(
                       player, owner.player) ==
                   PlayerRelationship::Enemies) {
            enemyX += position.x;
            enemyY += position.y;
            ++enemyCount;
        }
    }
    if (baseCount != 0) {
        const math::q32_32 count{
            static_cast<int32_t>(baseCount)};
        baseX /= count;
        baseY /= count;
    } else if (playerState->startPosition >= 0) {
        for (const game::terrain::MultiplayerStartPosition& start :
             m_content.m_terrain.multiplayerStartPositions()) {
            if (start.index != playerState->startPosition) continue;
            baseX = math::q32_32::from_raw(start.positionRaw[0]);
            baseY = math::q32_32::from_raw(start.positionRaw[1]);
            break;
        }
    }
    if (enemyCount != 0) {
        const math::q32_32 count{
            static_cast<int32_t>(enemyCount)};
        enemyX /= count;
        enemyY /= count;
    }

    struct WarehouseCandidate final {
        ObjectId object = INVALID_OBJECT_ID;
        math::q32_32 x{};
        math::q32_32 y{};
        math::q32_32 radius{};
    };
    std::optional<WarehouseCandidate> warehouse;
    uint64_t requiredValue = minimumSupplies <= 0
        ? 0u : static_cast<uint64_t>(minimumSupplies);
    do {
        math::q32_32 bestDistanceSquared{};
        bool foundWarehouse = false;
        for (const ecs::entity entity : objects) {
            const ObjectIdentityComponent& identity =
                objects.template get<const ObjectIdentityComponent>(entity);
            const OwnerComponent& owner =
                objects.template get<const OwnerComponent>(entity);
            const ObjectLifecycleComponent& lifecycle =
                objects.template get<const ObjectLifecycleComponent>(entity);
            const ThingTemplateComponent& type =
                objects.template get<const ThingTemplateComponent>(entity);
            PlayerRelationship relationship =
                m_content.m_players.relationships().get(player, owner.player);
            if (const PrimaryTeamComponent* primary =
                    ecs::try_get<PrimaryTeamComponent>(m_world.m_registry, entity)) {
                if (const std::optional<PlayerRelationship> overrideRelationship =
                        m_content.m_players.teamRelationshipOverride(
                            player, primary->team)) {
                    relationship = *overrideRelationship;
                }
            }
            if (!identity.id ||
                lifecycle.phase != ObjectLifecyclePhase::Alive ||
                !type.archetype ||
                !kindOfListContains(type.archetype,
                                    game::ObjectKindOf::Structure) ||
                !kindOfListContains(type.archetype,
                                    game::ObjectKindOf::SupplySource) ||
                relationship == PlayerRelationship::Enemies) {
                continue;
            }
            const ObjectEconomyComponent* economy =
                ecs::try_get<ObjectEconomyComponent>(m_world.m_registry, entity);
            if (!economy || economy->supplyWarehouseDocks.empty()) continue;
            uint64_t boxes = 0;
            for (const ObjectSupplyWarehouseDockRuntime& dock :
                 economy->supplyWarehouseDocks) {
                boxes = dock.boxesStored >
                        std::numeric_limits<uint64_t>::max() - boxes
                    ? std::numeric_limits<uint64_t>::max()
                    : boxes + dock.boxesStored;
            }
            const uint64_t value = boxes >
                    std::numeric_limits<uint64_t>::max() / 100u
                ? std::numeric_limits<uint64_t>::max() : boxes * 100u;
            if (value < requiredValue) continue;

            const TransformComponent& transform =
                objects.template get<const TransformComponent>(entity);
            const ObjectGeometryComponent* geometry =
                ecs::try_get<ObjectGeometryComponent>(m_world.m_registry, entity);
            const math::q32_32 radius = geometry
                ? math::q32_32::max(
                      math::q32_32{},
                      geometry->boundingCircleRadiusFixed)
                : math::q32_32{};
            const LogicFixedVec3 position = readAuthoritativeObjectPosition(
                m_world.m_registry, entity, transform);
            bool alreadyServed = false;
            for (const ecs::entity candidate : objects) {
                const OwnerComponent& candidateOwner =
                    objects.template get<const OwnerComponent>(candidate);
                const ObjectLifecycleComponent& candidateLifecycle =
                    objects.template get<const ObjectLifecycleComponent>(candidate);
                const ThingTemplateComponent& candidateType =
                    objects.template get<const ThingTemplateComponent>(candidate);
                const ObjectMapStatusComponent* candidateMap =
                    ecs::try_get<ObjectMapStatusComponent>(m_world.m_registry,
                                                           candidate);
                if (candidateOwner.player != player ||
                    candidateLifecycle.phase != ObjectLifecyclePhase::Alive ||
                    (candidateMap && candidateMap->offMap) ||
                    !candidateType.archetype ||
                    !kindOfListContains(
                        candidateType.archetype,
                        game::ObjectKindOf::CashGenerator)) {
                    continue;
                }
                const TransformComponent& candidateTransform =
                    objects.template get<const TransformComponent>(candidate);
                const ObjectGeometryComponent* candidateGeometry =
                    ecs::try_get<ObjectGeometryComponent>(m_world.m_registry,
                                                          candidate);
                const math::q32_32 candidateRadius = candidateGeometry
                    ? math::q32_32::max(
                          math::q32_32{},
                          candidateGeometry->boundingCircleRadiusFixed)
                    : math::q32_32{};
                const LogicFixedVec3 candidatePosition =
                    readAuthoritativeObjectPosition(
                        m_world.m_registry, candidate,
                        candidateTransform);
                const math::q32_32 dx = candidatePosition.x - position.x;
                const math::q32_32 dy = candidatePosition.y - position.y;
                const math::q32_32 close =
                    math::q32_32{int32_t{200}} + radius + candidateRadius;
                if (dx * dx + dy * dy <= close * close) {
                    alreadyServed = true;
                    break;
                }
            }
            if (alreadyServed) continue;
            const math::q32_32 dx = position.x - baseX;
            const math::q32_32 dy = position.y - baseY;
            const math::q32_32 distanceSquared = dx * dx + dy * dy;
            if (enemyCount != 0) {
                const math::q32_32 ex = position.x - enemyX;
                const math::q32_32 ey = position.y - enemyY;
                if (distanceSquared * math::q32_32{int32_t{2}} >
                    (ex * ex + ey * ey) *
                        math::q32_32{int32_t{3}}) {
                    continue;
                }
            }
            if (foundWarehouse && warehouse &&
                (distanceSquared > bestDistanceSquared ||
                 (distanceSquared == bestDistanceSquared &&
                  identity.id > warehouse->object))) {
                continue;
            }
            warehouse = WarehouseCandidate{
                .object = identity.id,
                .x = position.x,
                .y = position.y,
                .radius = radius,
            };
            bestDistanceSquared = distanceSquared;
            foundWarehouse = true;
        }
        if (warehouse) break;
        requiredValue /= 2u;
    } while (requiredValue > 100u);
    if (!warehouse) return true;

    math::q32_32 directionX = warehouse->x - baseX;
    math::q32_32 directionY = warehouse->y - baseY;
    math::q32_32 offsetRadius{};
    const bool cashGenerator = kindOfListContains(
        product.get(), game::ObjectKindOf::CashGenerator);
    if (cashGenerator) {
        const int64_t cellSizeRaw =
            m_content.m_navigation.grid().transform().cellSizeRaw;
        const math::q32_32 cellSize = cellSizeRaw > 0
            ? math::q32_32::from_raw(cellSizeRaw)
            : math::q32_32{int32_t{10}};
        offsetRadius = math::q32_32::max(
                           math::q32_32{}, cellSize) *
                       math::q32_32{int32_t{3}};
    } else if (enemyCount != 0) {
        directionX = warehouse->x - enemyX;
        directionY = warehouse->y - enemyY;
        offsetRadius = warehouse->radius;
    }
    math::q32_32 anchorX = warehouse->x;
    math::q32_32 anchorY = warehouse->y;
    if (offsetRadius > math::q32_32{}) {
        const math::q32_32 length = math::q32_32::sqrt(
            directionX * directionX + directionY * directionY);
        if (length > math::q32_32{}) {
            anchorX -= directionX / length * offsetRadius;
            anchorY -= directionY / length * offsetRadius;
        }
    }
    return buildScriptObjectNearAnchor(
        player, objectType, anchorX, anchorY,
        sourceSequence, confirmedTick);
}

bool GameSessionScriptScenarioPlanTransactions::buildScriptScenarioBuilding(
    PlayerId player, container::StringView objectType,
    uint32_t sourceSequence, uint64_t confirmedTick)
{
    if (!m_port) return {};
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick || objectType.empty()) {
        return false;
    }
    const PlayerState* playerState = m_content.m_players.get(player);
    if (!playerState) return false;
    if (playerState->controller != PlayerControllerKind::Ai) return true;
    if (!m_presentation.m_scenarioDefinition) return true;

    // AISkirmishPlayer::buildSpecificAIBuilding marks the first matching,
    // unbuilt BuildList entry as priority. The modern session has no mutable
    // BuildList pointer graph, so materialize that same entry through the
    // authoritative Builder transaction and retain its authored identity on
    // the construction site until completion.
    for (const scenario::ScenarioBuildIntent& intent :
         m_presentation.m_scenarioDefinition->buildIntents()) {
        if (intent.resolvedOwner != player ||
            intent.templateName != objectType) {
            continue;
        }
        if (!intent.structureName.empty() &&
            m_presentation.m_scriptObjects.liveNamedObject(intent.structureName)) {
            continue;
        }
        return buildScriptObjectNearAnchor(
            player, intent.templateName, intent.x,
            intent.y, sourceSequence, confirmedTick,
            intent.angle,
            intent.structureName, intent.sourceSideOrdinal,
            intent.sourceBuildListOrdinal);
    }
    return true;
}

bool GameSessionScriptScenarioPlanTransactions::buildScriptPerimeterStructure(
    PlayerId player, container::StringView requestedObjectType,
    bool flank, bool useFactionBaseDefense,
    uint32_t sourceSequence, uint64_t confirmedTick)
{
    if (!m_port) return {};
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick) {
        return false;
    }
    const PlayerState* playerState = m_content.m_players.get(player);
    if (!playerState) return false;
    if (playerState->controller != PlayerControllerKind::Ai) return true;

    container::String resolvedObjectType{requestedObjectType};
    if (useFactionBaseDefense) {
        // AISideInfo::BaseDefenseStructure1 is a convenience cache over the
        // faction command bar. Derive the first buildable FS_BASE_DEFENSE from
        // stable owned builders so custom factions and map overrides work
        // without a parallel mutable AISideInfo list.
        for (const ObjectId builderId : m_world.m_ownership.objects(player)) {
            const std::optional<ecs::entity> entity =
                m_world.m_objects.entityFromId(builderId);
            if (!entity ||
                !ecs::try_get<ObjectBuilderComponent>(m_world.m_registry, *entity)) {
                continue;
            }
            for (size_t slot = 0;
                 slot < game::COMMAND_SET_SLOT_COUNT; ++slot) {
                const container::StringView buttonName =
                    m_port.orderPolicy.effectiveCommandBarButton(
                        builderId, slot);
                const game::CommandButtonTemplate* button =
                    m_content.m_contentSnapshot.findCommandButton(buttonName);
                if (!button || button->descriptor.kind !=
                        game::CommandButtonKind::DozerConstruct) {
                    continue;
                }
                const container::String* candidateName =
                    commandButtonField(*button, "Object");
                const container::SharedPtr<const game::ObjectArchetype>
                    candidate = candidateName
                    ? m_content.m_contentSnapshot.findObjectArchetype(*candidateName)
                    : nullptr;
                if (!candidate || !kindOfListContains(
                        candidate.get(),
                        game::ObjectKindOf::FsBaseDefense)) {
                    continue;
                }
                resolvedObjectType = candidate->templateData.name;
                break;
            }
            if (!resolvedObjectType.empty()) break;
        }
    }
    if (resolvedObjectType.empty()) return true;
    const container::SharedPtr<const game::ObjectArchetype> product =
        m_content.m_contentSnapshot.findObjectArchetype(resolvedObjectType);
    if (!product) return true;

    const auto structures = ecs::view<
        const ObjectIdentityComponent, const OwnerComponent,
        const ObjectLifecycleComponent, const ThingTemplateComponent,
        const TransformComponent>(m_world.m_registry);
    math::q32_32 baseX{};
    math::q32_32 baseY{};
    size_t baseCount = 0;
    math::q32_32 enemyX{};
    math::q32_32 enemyY{};
    size_t enemyCount = 0;
    for (const ecs::entity entity : structures) {
        const ObjectLifecycleComponent& lifecycle =
            structures.template get<const ObjectLifecycleComponent>(entity);
        const ThingTemplateComponent& type =
            structures.template get<const ThingTemplateComponent>(entity);
        if (lifecycle.phase != ObjectLifecyclePhase::Alive ||
            !type.archetype || !kindOfListContains(
                type.archetype, game::ObjectKindOf::Structure)) {
            continue;
        }
        const OwnerComponent& owner =
            structures.template get<const OwnerComponent>(entity);
        const TransformComponent& transform =
            structures.template get<const TransformComponent>(entity);
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            m_world.m_registry, entity, transform);
        if (owner.player == player) {
            baseX += position.x;
            baseY += position.y;
            ++baseCount;
        } else if (m_content.m_players.relationships().get(
                       player, owner.player) ==
                   PlayerRelationship::Enemies) {
            enemyX += position.x;
            enemyY += position.y;
            ++enemyCount;
        }
    }
    if (baseCount == 0) return true;
    const math::q32_32 fixedBaseCount{
        static_cast<int32_t>(baseCount)};
    baseX /= fixedBaseCount;
    baseY /= fixedBaseCount;
    if (enemyCount != 0) {
        const math::q32_32 fixedEnemyCount{
            static_cast<int32_t>(enemyCount)};
        enemyX /= fixedEnemyCount;
        enemyY /= fixedEnemyCount;
    }

    math::q32_32 baseRadius{};
    size_t matchingPerimeterStructures = 0;
    for (const ecs::entity entity : structures) {
        const OwnerComponent& owner =
            structures.template get<const OwnerComponent>(entity);
        const ObjectLifecycleComponent& lifecycle =
            structures.template get<const ObjectLifecycleComponent>(entity);
        const ThingTemplateComponent& type =
            structures.template get<const ThingTemplateComponent>(entity);
        if (owner.player != player ||
            lifecycle.phase != ObjectLifecyclePhase::Alive ||
            !type.archetype || !kindOfListContains(
                type.archetype, game::ObjectKindOf::Structure)) {
            continue;
        }
        const TransformComponent& transform =
            structures.template get<const TransformComponent>(entity);
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(m_world.m_registry, entity);
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            m_world.m_registry, entity, transform);
        const math::q32_32 dx = position.x - baseX;
        const math::q32_32 dy = position.y - baseY;
        const math::q32_32 radius = geometry
            ? math::q32_32::max(
                  math::q32_32{},
                  geometry->boundingCircleRadiusFixed)
            : math::q32_32{};
        const math::q32_32 distance = math::q32_32::sqrt(
            dx * dx + dy * dy);
        baseRadius = math::q32_32::max(
            baseRadius, distance + radius);
        if (type.archetype->templateData.name == resolvedObjectType) {
            ++matchingPerimeterStructures;
        }
    }
    if (baseRadius <= math::q32_32{})
        baseRadius = math::q32_32{int32_t{200}};

    const size_t selector = flank
        ? matchingPerimeterStructures >> 1
        : matchingPerimeterStructures;
    container::String pathLabel = flank
        ? ((matchingPerimeterStructures & 1u) != 0u
            ? "Flank" : "Backdoor")
        : "Center";
    if (playerState->startPosition >= 0) {
        pathLabel += std::to_string(playerState->startPosition + 1);
    }
    const game::terrain::WaypointRecord* approach =
        m_content.m_terrain.closestWaypointOnPathRaw(
            baseX.raw(), baseY.raw(),
            pathLabel);
    const math::q32_32 goalX = approach
        ? math::q32_32::from_raw(approach->positionRaw[0]) : enemyX;
    const math::q32_32 goalY = approach
        ? math::q32_32::from_raw(approach->positionRaw[1]) : enemyY;
    if (!approach && (flank || enemyCount == 0)) return true;

    const math::q32_32 directionX = goalX - baseX;
    const math::q32_32 directionY = goalY - baseY;
    const math::q32_32 directionLength = math::q32_32::sqrt(
        directionX * directionX + directionY * directionY);
    if (directionLength <= math::q32_32{})
        return true;
    const math::q32_32 perimeterDirectionX =
        directionX / directionLength * baseRadius;
    const math::q32_32 perimeterDirectionY =
        directionY / directionLength * baseRadius;

    const math::q32_32 structureRadius = math::q32_32::max(
        math::q32_32{},
        product->templateData.geometry.boundingCircleRadiusFixed);
    const math::q32_32 angleOffset = baseRadius > math::q32_32{}
        ? math::q32_32{int32_t{4}} * structureRadius / baseRadius
        : math::q32_32{};
    const size_t angleStep = (selector & 1u) != 0u
        ? (selector + 1u) / 2u : selector / 2u;
    if (angleStep > static_cast<size_t>(
            std::numeric_limits<int32_t>::max())) return true;
    math::q32_32 angle =
        math::q32_32{static_cast<int32_t>(angleStep)} * angleOffset;
    if ((selector & 1u) != 0u) angle = -angle;
    const math::q32_32 maximumPerimeterAngle =
        game::kTopplePi / math::q32_32{int32_t{3}};
    if (math::q32_32::abs(angle) > maximumPerimeterAngle)
        return true;
    const math::q32_32_sincos direction = math::fixed_sincos(angle);
    const math::q32_32 buildX = baseX +
        perimeterDirectionX * direction.cosine -
        perimeterDirectionY * direction.sine;
    const math::q32_32 buildY = baseY +
        perimeterDirectionY * direction.cosine +
        perimeterDirectionX * direction.sine;
    return buildScriptObjectNearAnchor(
        player, resolvedObjectType, buildX, buildY,
        sourceSequence, confirmedTick);
}

bool GameSessionScriptScenarioPlanTransactions::buildScriptObjectNearestTeam(
    PlayerId player, ObjectTeamId team, container::StringView objectType,
    uint32_t sourceSequence, uint64_t confirmedTick)
{
    if (!m_port) return {};
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick || objectType.empty()) {
        return false;
    }
    const ObjectTeamRecord* teamRecord = m_world.m_objectTeams.find(team);
    if (!m_content.m_players.get(player) || !teamRecord) return false;

    // Team::getEstimateTeamPosition deliberately returns the first legacy
    // member rather than a centroid. Keep that observable selection while
    // retaining only stable ObjectIds in ObjectTeamRegistry.
    const container::Span<const ObjectId> teamMembers =
        m_world.m_objectTeams.legacyMembers(team);
    if (teamMembers.empty()) return true;
    const std::optional<ecs::entity> anchorEntity =
        m_world.m_objects.entityFromId(teamMembers.front());
    const TransformComponent* anchor = anchorEntity
        ? ecs::try_get<TransformComponent>(m_world.m_registry, *anchorEntity) : nullptr;
    if (!anchor) return true;
    const LogicFixedVec3 anchorPosition = readAuthoritativeObjectPosition(
        m_world.m_registry, *anchorEntity, *anchor);
    return buildScriptObjectNearAnchor(
        player, objectType, anchorPosition.x, anchorPosition.y,
        sourceSequence, confirmedTick);
}

bool GameSessionScriptScenarioPlanTransactions::buildScriptObjectNearAnchor(
    PlayerId player, container::StringView objectType,
    math::q32_32 anchorX, math::q32_32 anchorY,
    uint32_t sourceSequence, uint64_t confirmedTick,
    std::optional<math::q32_32> authoredYawRadians,
    container::StringView scriptName, uint32_t sourceSideOrdinal,
    uint32_t sourceBuildListOrdinal, uint64_t strategicPlanId,
    bool authoredBuildList, int32_t remainingRebuilds)
{
    if (!m_port) return {};
    if (!m_content.m_active || !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick || objectType.empty()) {
        return false;
    }
    const PlayerState* playerState = m_content.m_players.get(player);
    if (!playerState) return false;
    if (playerState->controller != PlayerControllerKind::Ai) return true;

    const container::SharedPtr<const game::ObjectArchetype> product =
        m_content.m_contentSnapshot.findObjectArchetype(objectType);
    if (!product) return true;
    const math::q32_32 placementYaw = authoredYawRadians.value_or(
        product->templateData.placementViewAngleRadiansFixed);

    constexpr size_t kMaximumPriorityBuildEntries = 4096;
    auto& entries = m_ai.m_priorityBuildEntries;
    if (entries.size() >= kMaximumPriorityBuildEntries) {
        static_cast<void>(m_port.raiseSimulationFault({
            .domain = SimulationFaultDomain::Production,
            .code = SimulationFaultCode::CapacityExceeded,
            .confirmedTick = confirmedTick,
            .sequence = sourceSequence,
        }));
        return false;
    }
    entries.push_back({
        .player = player,
        .objectType = product->templateData.name,
        .anchorX = anchorX,
        .anchorY = anchorY,
        .yawRadians = placementYaw,
        .scriptName = container::String{scriptName},
        .sourceSideOrdinal = sourceSideOrdinal,
        .sourceBuildListOrdinal = sourceBuildListOrdinal,
        .sourceSequence = sourceSequence,
        .createdTick = confirmedTick,
        .nextAttemptTick = confirmedTick,
        .remainingRebuilds = remainingRebuilds,
        .strategicPlanId = strategicPlanId,
        .authoredBuildList = authoredBuildList,
    });
    return true;
}


} // namespace engine
