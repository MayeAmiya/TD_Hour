#include "GameSessionScriptPortDetail.h"
#include "GameSessionScriptQueryPort.h"

#include "debug/debug.h"
#include "game/base/GameBalanceConstants.h"
#include "game/base/DamageTypes.h"
#include "game/base/GameCameraDirector.h"
#include "game/base/GameSettings.h"
#include "game/audio/GameAudioEvents.h"
#include "game/command/CommandButtonStore.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/structure/ObjectTechBuilding.h"
#include "game/session/state/GameSessionDomainState.h"
#include "core/math/wwmath/base/wwmath.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace engine::script {
using detail::objectVisibleToPlayer;
using detail::scriptObjectSnapshot;
using detail::teamAreaAllowedSurfaces;
using detail::teamAreaObjectSurfaces;
using detail::templateIsInert;

bool GameSessionScriptQueryPort::namedObjectDiscovered(
    container::StringView name, PlayerId observer) const noexcept {
    const std::optional<ObjectId> object = m_presentation.m_scriptObjects.liveNamedObject(name);
    return object && objectDiscovered(*object, observer);
}

bool GameSessionScriptQueryPort::objectDiscovered(
    ObjectId object, PlayerId observer) const noexcept {
    if (!m_content.m_players.get(observer)) return false;
    const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
    return entity && objectVisibleToPlayer(
        m_world.m_registry, m_world.m_mapVisibility, *entity, observer,
        m_confirmedTick, true);
}

bool GameSessionScriptQueryPort::teamDiscovered(
    container::StringView name, PlayerId observer) const noexcept {
    const std::optional<ObjectTeamId> team = resolveScenarioTeamAlias(name);
    return team && teamDiscovered(*team, observer);
}

bool GameSessionScriptQueryPort::teamDiscovered(
    ObjectTeamId team, PlayerId observer) const noexcept {
    if (!m_world.m_objectTeams.find(team) ||
        !m_content.m_players.get(observer)) return false;
    for (const ObjectId object : m_world.m_objectTeams.members(team)) {
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
        if (entity && objectVisibleToPlayer(
                m_world.m_registry, m_world.m_mapVisibility, *entity, observer,
                m_confirmedTick, true)) return true;
    }
    return false;
}

bool GameSessionScriptQueryPort::playerDiscovered(
    PlayerId subject, PlayerId observer) const noexcept {
    if (!m_content.m_players.get(subject) || !m_content.m_players.get(observer)) return false;
    for (const ObjectId object : m_world.m_ownership.objects(subject)) {
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
        if (!entity) continue;
        // RefCode's skirmish predicate asks only Object::getShroudedStatus;
        // unlike NAMED/TEAM_DISCOVERED it deliberately does not reject Held
        // or an independently stealthed object here.
        if (objectVisibleToPlayer(
                m_world.m_registry, m_world.m_mapVisibility, *entity, observer,
                m_confirmedTick, false)) return true;
    }
    return false;
}

bool GameSessionScriptQueryPort::namedObjectSeesPlayerByRelationship(
    container::StringView sourceObject, ScriptSightRelationship relationship,
    PlayerId targetPlayer) const noexcept {
    const std::optional<ObjectId> source =
        m_presentation.m_scriptObjects.liveNamedObject(sourceObject);
    return source && objectSeesPlayerByRelationship(
        *source, relationship, targetPlayer);
}

bool GameSessionScriptQueryPort::objectSeesPlayerByRelationship(
    ObjectId sourceObject, ScriptSightRelationship relationship,
    PlayerId targetPlayer) const noexcept {
    PlayerRelationship wanted = PlayerRelationship::Enemies;
    switch (relationship) {
    case ScriptSightRelationship::Enemies: wanted = PlayerRelationship::Enemies; break;
    case ScriptSightRelationship::Neutral: wanted = PlayerRelationship::Neutral; break;
    case ScriptSightRelationship::Allies: wanted = PlayerRelationship::Allies; break;
    }
    return seesAny(sourceObject, {
        .targetPlayer = targetPlayer,
        .relationship = wanted,
        .concealment = ObjectSightConcealment::RejectHiddenStealth,
    });
}

bool GameSessionScriptQueryPort::namedObjectSeesPlayerObjectTypes(
    container::StringView sourceObject, PlayerId targetPlayer,
    container::Span<const container::String> objectTypes) const noexcept {
    const std::optional<ObjectId> source =
        m_presentation.m_scriptObjects.liveNamedObject(sourceObject);
    return source && objectSeesPlayerObjectTypes(
        *source, targetPlayer, objectTypes);
}

bool GameSessionScriptQueryPort::objectSeesPlayerObjectTypes(
    ObjectId sourceObject, PlayerId targetPlayer,
    container::Span<const container::String> objectTypes) const noexcept {
    if (objectTypes.empty()) return false;
    return seesAny(sourceObject, {
        .targetPlayer = targetPlayer,
        .exactObjectTypes = objectTypes,
        .concealment = ObjectSightConcealment::RejectHiddenStealth,
    });
}

bool GameSessionScriptQueryPort::namedInsideArea(container::StringView objectName,
                                              container::StringView areaName) const {
    const std::optional<ScriptWorldObjectSnapshot> object = findNamedObject(objectName);
    return object && objectInsideArea(object->id, areaName);
}

bool GameSessionScriptQueryPort::objectInsideArea(
    ObjectId object, container::StringView areaName) const {
    const std::optional<ScriptWorldObjectSnapshot> snapshot =
        scriptObjectSnapshot(m_world.m_registry, m_world.m_objects, object);
    if (!snapshot || !snapshot->alive) return false;

    const game::terrain::PolygonTriggerRecord* area = m_content.m_terrain.triggerByName(areaName);
    return area && m_content.m_terrain.isInsideTriggerLegacyRaw(
        *area, snapshot->positionFixed.x.raw(),
        snapshot->positionFixed.y.raw());
}

ScriptWorldTeamAreaSummary GameSessionScriptQueryPort::teamAreaSummary(
    container::StringView teamName, container::StringView areaName, uint8_t allowedSurfaces) const {
    const std::optional<ObjectTeamId> team = resolveScenarioTeamAlias(teamName);
    return team ? teamAreaSummary(*team, areaName, allowedSurfaces)
                : ScriptWorldTeamAreaSummary{};
}

ScriptWorldTeamAreaSummary GameSessionScriptQueryPort::teamAreaSummary(
    ObjectTeamId team, container::StringView areaName,
    uint8_t allowedSurfaces) const {
    ScriptWorldTeamAreaSummary summary;
    if (!m_world.m_objectTeams.find(team)) return summary;
    summary.teamExists = true;

    const game::terrain::PolygonTriggerRecord* area = m_content.m_terrain.triggerByName(areaName);
    if (!area) return summary;
    summary.areaExists = true;

    const game::LocomotorSurfaceMask allowed = teamAreaAllowedSurfaces(allowedSurfaces);
    if (allowed == 0) return summary;

    // ObjectTeamRegistry supplies ObjectId-sorted membership. Never iterate
    // its reverse hash index here: evaluation must remain deterministic even
    // though the aggregate itself contains only counts.
    for (const ObjectId object : m_world.m_objectTeams.members(team)) {
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
        if (!entity) continue;

        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity);
        if (health && health->effectivelyDead) continue;

        const ThingTemplateComponent* templateComponent =
            ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity);
        if (templateIsInert(templateComponent)) continue;

        const ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(m_world.m_registry, *entity);
        if ((teamAreaObjectSurfaces(
                 m_content.m_contentSnapshot, templateComponent, locomotion) &
             allowed) == 0) {
            continue;
        }

        const TransformComponent* transform =
            ecs::try_get<TransformComponent>(m_world.m_registry, *entity);
        if (!transform) continue;
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            m_world.m_registry, *entity, *transform);

        ++summary.considered;
        if (m_content.m_terrain.isInsideTriggerLegacyRaw(
                *area, position.x.raw(), position.y.raw())) {
            ++summary.inside;
        }
    }
    return summary;
}


} // namespace engine::script
