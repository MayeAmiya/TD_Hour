#include "GameSessionScriptPortDetail.h"
#include "GameSessionScriptQueryPort.h"
#include "game/object/definition/ObjectArchetype.h"

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
namespace {
constexpr auto equalAsciiInsensitive = container::asciiEqualIgnoreCase;

template <typename Predicate>
[[nodiscard]] bool anyScriptTargetObject(
    const script::ScriptObjectIndex& scriptObjects,
    const ObjectLifecycle& objects, container::StringView target,
    const Predicate& predicate) noexcept {
    const std::optional<ObjectId> object =
        scriptObjects.liveNamedObject(target);
    if (!object) return false;
    const std::optional<ecs::entity> entity = objects.entityFromId(*object);
    return entity && predicate(*entity, false);
}

template <typename Predicate>
[[nodiscard]] bool anyScriptTeamObject(
    const ObjectTeamRegistry& teams, const ObjectLifecycle& objects,
    ObjectTeamId team,
    const Predicate& predicate) noexcept {
    if (!teams.find(team)) return false;
    for (const ObjectId object : teams.members(team)) {
        const std::optional<ecs::entity> entity = objects.entityFromId(object);
        if (entity && predicate(*entity, true)) return true;
    }
    return false;
}

[[nodiscard]] bool objectWasLastAttackedByTypes(
    const ecs::registry& registry, const ObjectLifecycle& objects,
    ecs::entity entity,
    container::Span<const container::String> objectTypes) noexcept {
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    if (!health || !health->hasLastDamageInfo) return false;
    container::StringView sourceType;
    if (health->lastDamageSourceArchetype) {
        sourceType = health->lastDamageSourceArchetype->templateData.name;
    } else if (const std::optional<ecs::entity> source =
                   objects.entityFromId(health->lastDamageSource)) {
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(registry, *source);
        if (type) {
            sourceType = !type->name.empty()
                ? container::StringView{type->name}
                : (type->archetype
                    ? container::StringView{type->archetype->templateData.name}
                    : container::StringView{});
        }
    }
    return !sourceType.empty() && std::any_of(
        objectTypes.begin(), objectTypes.end(),
        [sourceType](const container::String& expected) noexcept {
            return equalAsciiInsensitive(sourceType, expected);
        });
}

[[nodiscard]] bool objectWasLastAttackedByPlayer(
    const ecs::registry& registry, const ObjectLifecycle& objects,
    ecs::entity entity,
    PlayerId player, bool teamTarget) noexcept {
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    if (!health || !health->hasLastDamageInfo) return false;
    if (!teamTarget && health->lastDamageSourcePlayer)
        return health->lastDamageSourcePlayer == player;
    const std::optional<ecs::entity> source =
        objects.entityFromId(health->lastDamageSource);
    if (!source) return false;
    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(registry, *source);
    return owner && owner->player == player;
}

} // namespace

using detail::kindOfContains;
using detail::teamAreaAllowedSurfaces;
using detail::teamAreaObjectSurfaces;
using detail::templateIsInert;

bool GameSessionScriptQueryPort::targetLastAttackedByObjectTypes(
    container::StringView target, bool team,
    container::Span<const container::String> objectTypes) const noexcept {
    if (objectTypes.empty()) return false;
    if (team) {
        const std::optional<ObjectTeamId> resolved =
            resolveScenarioTeamAlias(target);
        return resolved && teamLastAttackedByObjectTypes(*resolved, objectTypes);
    }
    return anyScriptTargetObject(
        m_presentation.m_scriptObjects, m_world.m_objects, target,
        [this, objectTypes](ecs::entity entity, bool) noexcept {
            return objectWasLastAttackedByTypes(
                m_world.m_registry, m_world.m_objects, entity, objectTypes);
        });
}

bool GameSessionScriptQueryPort::teamLastAttackedByObjectTypes(
    ObjectTeamId team,
    container::Span<const container::String> objectTypes) const noexcept {
    if (objectTypes.empty()) return false;
    return anyScriptTeamObject(
        m_world.m_objectTeams, m_world.m_objects, team,
        [this, objectTypes](ecs::entity entity, bool) noexcept {
            return objectWasLastAttackedByTypes(
                m_world.m_registry, m_world.m_objects, entity, objectTypes);
        });
}

bool GameSessionScriptQueryPort::objectLastAttackedByObjectTypes(
    ObjectId object,
    container::Span<const container::String> objectTypes) const noexcept {
    const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
    return entity && !objectTypes.empty() &&
        objectWasLastAttackedByTypes(
            m_world.m_registry, m_world.m_objects, *entity, objectTypes);
}

bool GameSessionScriptQueryPort::targetLastAttackedByPlayer(
    container::StringView target, bool team, PlayerId player) const noexcept {
    if (!m_content.m_players.get(player)) return false;
    if (team) {
        const std::optional<ObjectTeamId> resolved =
            resolveScenarioTeamAlias(target);
        return resolved && teamLastAttackedByPlayer(*resolved, player);
    }
    return anyScriptTargetObject(
        m_presentation.m_scriptObjects, m_world.m_objects, target,
        [this, player](ecs::entity entity, bool teamTarget) noexcept {
            return objectWasLastAttackedByPlayer(
                m_world.m_registry, m_world.m_objects, entity, player,
                teamTarget);
        });
}

bool GameSessionScriptQueryPort::teamLastAttackedByPlayer(
    ObjectTeamId team, PlayerId player) const noexcept {
    if (!m_content.m_players.get(player)) return false;
    return anyScriptTeamObject(
        m_world.m_objectTeams, m_world.m_objects, team,
        [this, player](ecs::entity entity, bool teamTarget) noexcept {
            return objectWasLastAttackedByPlayer(
                m_world.m_registry, m_world.m_objects, entity, player,
                teamTarget);
        });
}

bool GameSessionScriptQueryPort::objectLastAttackedByPlayer(
    ObjectId object, PlayerId player) const noexcept {
    if (!m_content.m_players.get(player)) return false;
    const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
    return entity && objectWasLastAttackedByPlayer(
        m_world.m_registry, m_world.m_objects, *entity, player, false);
}

bool GameSessionScriptQueryPort::playerWasAttackedBy(
    PlayerId victim, PlayerId attacker) const noexcept {
    return m_content.m_players.wasAttackedBy(victim, attacker);
}

bool GameSessionScriptQueryPort::bridgeTransitionObserved(
    container::StringView bridgeObject, bool broken) const noexcept {
    const std::optional<ObjectId> object =
        m_presentation.m_scriptObjects.liveNamedObject(bridgeObject);
    return object && bridgeTransitionObserved(*object, broken);
}

bool GameSessionScriptQueryPort::bridgeTransitionObserved(
    ObjectId bridgeObject, bool broken) const noexcept {
    return m_world.m_objects.entityFromId(bridgeObject) &&
        m_presentation.m_scriptGameplayEvents.bridgeTransitionObserved(
            bridgeObject, !broken, m_confirmedTick);
}

bool GameSessionScriptQueryPort::unitEmptied(
    container::StringView objectName) const {
    const std::optional<ObjectId> object =
        m_presentation.m_scriptObjects.liveNamedObject(objectName);
    return object && unitEmptied(*object);
}

bool GameSessionScriptQueryPort::unitEmptied(ObjectId object) const {
    const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
    if (!entity) return false;
    const ObjectContainmentComponent* containment =
        ecs::try_get<ObjectContainmentComponent>(m_world.m_registry, *entity);
    return m_presentation.m_scriptGameplayEvents.observeUnitEmptied(
        object, containment ? containment->objects.size() : 0, m_confirmedTick);
}

bool GameSessionScriptQueryPort::buildingEnteredByPlayer(
    container::StringView buildingObject, PlayerId player) const noexcept {
    const std::optional<ObjectId> building =
        m_presentation.m_scriptObjects.liveNamedObject(buildingObject);
    return building && buildingEnteredByPlayer(*building, player);
}

bool GameSessionScriptQueryPort::buildingEnteredByPlayer(
    ObjectId buildingObject, PlayerId player) const noexcept {
    return m_world.m_objects.entityFromId(buildingObject) &&
        m_presentation.m_scriptGameplayEvents.buildingEnteredByPlayer(
            buildingObject, player, m_confirmedTick);
}

bool GameSessionScriptQueryPort::namedAreaTransition(
    container::StringView objectName, container::StringView areaName,
    ScriptAreaTransitionKind kind) const noexcept {
    const std::optional<ObjectId> object =
        m_presentation.m_scriptObjects.liveNamedObject(objectName);
    return object && objectAreaTransition(*object, areaName, kind);
}

bool GameSessionScriptQueryPort::objectAreaTransition(
    ObjectId object, container::StringView areaName,
    ScriptAreaTransitionKind kind) const noexcept {
    if (!m_world.m_objects.entityFromId(object) ||
        !m_content.m_terrain.triggerByName(areaName)) return false;
    if (kind == ScriptAreaTransitionKind::Entered) {
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
        const ThingTemplateComponent* type = entity
            ? ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity)
            : nullptr;
        if (templateIsInert(type)) return false;
    }
    return m_presentation.m_scriptGameplayEvents.objectAreaTransition(
        object, areaName, kind, m_confirmedTick);
}

bool GameSessionScriptQueryPort::teamAreaTransition(
    container::StringView teamName, container::StringView areaName,
    uint8_t allowedSurfaces, ScriptAreaTransitionKind kind,
    bool entireTeam) const noexcept {
    const std::optional<ObjectTeamId> team =
        resolveScenarioTeamAlias(teamName);
    return team && teamAreaTransition(
        *team, areaName, allowedSurfaces, kind, entireTeam);
}

bool GameSessionScriptQueryPort::teamAreaTransition(
    ObjectTeamId team, container::StringView areaName,
    uint8_t allowedSurfaces, ScriptAreaTransitionKind kind,
    bool entireTeam) const noexcept {
    if (!m_world.m_objectTeams.find(team) ||
        !m_content.m_terrain.triggerByName(areaName)) return false;
    const game::LocomotorSurfaceMask allowed =
        teamAreaAllowedSurfaces(allowedSurfaces);
    if (allowed == 0) return false;

    bool considered = false;
    bool anyTransition = false;
    bool allOnDestinationSide = true;
    for (const ObjectId object : m_world.m_objectTeams.members(team)) {
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
        if (!entity) continue;
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity);
        if (health && health->effectivelyDead) continue;
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity);
        if (templateIsInert(type)) continue;
        const ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(m_world.m_registry, *entity);
        if ((teamAreaObjectSurfaces(
                 m_content.m_contentSnapshot, type, locomotion) & allowed) == 0)
            continue;
        considered = true;
        const bool inside = m_presentation.m_scriptGameplayEvents.objectInsideArea(
            object, areaName);
        if ((kind == ScriptAreaTransitionKind::Entered && !inside) ||
            (kind == ScriptAreaTransitionKind::Exited && inside)) {
            allOnDestinationSide = false;
        }
        anyTransition = anyTransition ||
            m_presentation.m_scriptGameplayEvents.objectAreaTransition(
                object, areaName, kind, m_confirmedTick);
    }
    if (!considered || !anyTransition) return false;
    return entireTeam ? allOnDestinationSide : true;
}

std::optional<math::vec3> GameSessionScriptQueryPort::teamRadarEventPosition(
    container::StringView name) const {
    const std::optional<ObjectTeamId> team = resolveScenarioTeamAlias(name);
    return team ? teamRadarEventPosition(*team) : std::nullopt;
}

std::optional<math::vec3> GameSessionScriptQueryPort::teamRadarEventPosition(
    ObjectTeamId team) const {
    if (!m_world.m_objectTeams.find(team)) return std::nullopt;

    // This one query is intentionally order-sensitive: RefCode's
    // getEstimateTeamPosition() returns the head of the prepended
    // TeamMemberList. Do not use the modern ObjectId-sorted aggregate view.
    const container::Span<const ObjectId> members =
        m_world.m_objectTeams.legacyMembers(team);
    bool hasAnyUnits = false;
    for (const ObjectId member : members) {
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(member);
        if (!entity) continue;

        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity);
        if (health && health->effectivelyDead) continue;

        const ThingTemplateComponent* templateComponent =
            ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity);
        // This is Team::hasAnyUnits(), not the broader hasAnyObjects() and
        // not the Team-area condition's INERT filter. Its only job is the
        // legacy gate before getEstimateTeamPosition().
        if (kindOfContains(templateComponent ? templateComponent->archetype : nullptr,
                           game::ObjectKindOf::Structure) ||
            kindOfContains(templateComponent ? templateComponent->archetype : nullptr,
                           game::ObjectKindOf::Projectile) ||
            kindOfContains(templateComponent ? templateComponent->archetype : nullptr,
                           game::ObjectKindOf::Mine)) {
            continue;
        }
        hasAnyUnits = true;
        break;
    }
    if (!hasAnyUnits || members.empty()) return std::nullopt;

    // RefCode's getEstimateTeamPosition() intentionally does *not* apply the
    // above filters: after hasAnyUnits() passes, it returns the first Team
    // member's coordinate. ObjectTeamRegistry reproduces that observable
    // newest-assignment-first order with a stable-ID sidecar.
    const std::optional<ecs::entity> first = m_world.m_objects.entityFromId(members.front());
    if (!first) return std::nullopt;
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(m_world.m_registry, *first);
    if (!transform) return std::nullopt;
    return math::vec3{transform->x, transform->y, transform->z};
}

bool GameSessionScriptQueryPort::cameraMovementFinished() const noexcept {
    // Elapsed camera time belongs to the client presentation clock. Scripts
    // see completion only after that client acknowledges the latest issued
    // movement revision for this presentation epoch.
    return m_presentation.m_scriptCameraCompletedRevision >=
        m_presentation.m_scriptCameraMovementRevision;
}

bool GameSessionScriptQueryPort::consumePresentationCompletion(
    ScriptPresentationCompletionKind kind, container::StringView mediaName) const noexcept {
    return m_eventCursor.consumePresentationCompletion(kind, mediaName);
}

bool GameSessionScriptQueryPort::musicTrackHasCompleted(
    container::StringView trackName, int32_t minimumCompletedLoops) const noexcept {
    return m_eventCursor.musicTrackHasCompleted(
        trackName, minimumCompletedLoops);
}


} // namespace engine::script
