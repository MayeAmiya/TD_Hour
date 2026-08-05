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
#include "game/object/ai/runtime/ObjectAIOrderAdmission.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/structure/ObjectTechBuilding.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/session/state/GameSessionDomainState.h"
#include "core/math/wwmath/base/wwmath.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace engine::script {
namespace {
constexpr auto equalAsciiInsensitive = container::asciiEqualIgnoreCase;
}
using detail::kindOfContains;
using detail::scriptObjectSnapshot;

std::optional<ScriptWorldObjectSnapshot> GameSessionScriptQueryPort::findNamedObject(
    container::StringView name) const {
    const std::optional<ObjectId> object = m_presentation.m_scriptObjects.liveNamedObject(name);
    return object
        ? scriptObjectSnapshot(m_world.m_registry, m_world.m_objects, *object)
        : std::nullopt;
}

std::optional<ScriptWorldObjectSnapshot> GameSessionScriptQueryPort::resolveObjectSelector(
    const ScriptObjectSelector& selector,
    const ScriptInvocationContext& invocation) const {
    if (!selector.valid()) return std::nullopt;
    if (selector.kind == ScriptObjectSelector::Kind::Named)
        return findNamedObject(selector.name);
    return scriptObjectSnapshot(
        m_world.m_registry, m_world.m_objects, invocation.thisObject());
}

std::optional<ObjectTeamId> GameSessionScriptQueryPort::resolveTeamSelector(
    const ScriptTeamSelector& selector,
    const ScriptInvocationContext& invocation) const {
    if (!selector.valid()) return std::nullopt;
    if (selector.kind == ScriptTeamSelector::Kind::ThisTeam) {
        const ObjectTeamId team = invocation.thisTeam();
        return team && m_world.m_objectTeams.find(team)
            ? std::optional<ObjectTeamId>{team}
            : std::nullopt;
    }
    return resolveScenarioTeamAlias(
        selector.name, invocation.callingTeam, invocation.conditionTeam);
}

ScriptWorldTeamInvocationSet GameSessionScriptQueryPort::selectConditionTeamInvocation(
    container::Span<const ScriptTeamSelector> candidates) const {
    ScriptWorldTeamInvocationSet lastSingleton;
    const container::SharedPtr<const scenario::ScenarioDefinition> scenario =
        m_presentation.m_scenarioDefinition;
    if (!scenario) return {};

    for (const ScriptTeamSelector& candidate : candidates) {
        if (candidate.kind != ScriptTeamSelector::Kind::ScenarioTeam ||
            candidate.name.empty()) {
            continue;
        }
        const scenario::OwnerReference owner = scenario->resolveOwner(candidate.name);
        if (owner.kind != scenario::OwnerReferenceKind::ScriptTeam || !owner.scriptTeam)
            continue;
        const scenario::ScriptTeamDefinition* definition =
            scenario->findScriptTeam(owner.scriptTeam);
        if (!definition) continue;

        ScriptWorldTeamInvocationSet selection{
            .prototypeExists = true,
            .multiInstance = !definition->isSingleton &&
                             definition->maximumInstances >= 2,
        };
        const container::Span<const ObjectTeamId> instances =
            m_world.m_objectTeams.scenarioTeamInstances(definition->id);
        selection.instances.assign(instances.begin(), instances.end());
        if (selection.multiInstance)
            return selection;
        lastSingleton = std::move(selection);
    }
    return lastSingleton;
}

container::Vector<ObjectTeamId> GameSessionScriptQueryPort::teamHookInstances(
    container::StringView teamName) const {
    container::Vector<ObjectTeamId> result;
    const container::SharedPtr<const scenario::ScenarioDefinition> scenario =
        m_presentation.m_scenarioDefinition;
    if (!scenario || teamName.empty()) return result;

    const scenario::OwnerReference owner = scenario->resolveOwner(teamName);
    if (owner.kind != scenario::OwnerReferenceKind::ScriptTeam ||
        !owner.scriptTeam) {
        return result;
    }
    const scenario::ScriptTeamDefinition* definition =
        scenario->findScriptTeam(owner.scriptTeam);
    if (!definition) return result;

    const container::Span<const ObjectTeamId> instances =
        m_world.m_objectTeams.scenarioTeamInstances(definition->id);
    result.reserve(instances.size());
    for (auto iterator = instances.rbegin(); iterator != instances.rend();
         ++iterator) {
        // Player::updateGenericScripts visits every instance, including the
        // inactive singleton placeholder. Runtime applies the active gate
        // only to lifecycle polling, after generic scripts have run.
        if (m_world.m_objectTeams.find(*iterator)) result.push_back(*iterator);
    }
    return result;
}

ScriptWorldTeamHookState GameSessionScriptQueryPort::teamHookState(
    ObjectTeamId team) const noexcept {
    ScriptWorldTeamHookState result;
    const ObjectTeamRecord* record = m_world.m_objectTeams.find(team);
    if (!record) return result;
    result.exists = true;
    result.active = record->active;
    result.createdThisTick =
        m_world.m_objectTeams.wasCreatedAt(team, m_confirmedTick);
    result.productionActionCount =
        m_world.m_objectTeams.productionActionCountAt(team, m_confirmedTick);
    result.productionActionWithoutTeamCount =
        m_world.m_objectTeams.productionActionWithoutTeamCountAt(
            team, m_confirmedTick);
    result.owner = m_world.m_objectTeams.teamOwner(team).value_or(
        INVALID_PLAYER_ID);
    const container::Span<const ObjectId> members =
        m_world.m_objectTeams.members(team);
    result.totalMemberCount = static_cast<uint32_t>(std::min<size_t>(
        members.size(), std::numeric_limits<uint32_t>::max()));
    bool allAliveAiIdle = true;
    for (const ObjectId member : members) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(member);
        if (!entity) continue;
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity);
        if (health && health->effectivelyDead) continue;
        if (result.aliveMemberCount != std::numeric_limits<uint32_t>::max())
            ++result.aliveMemberCount;
        const std::optional<ai::ObjectAIActorStateView> aiState =
            m_ai.m_objectAI.actorState(member);
        if (!aiState) continue;
        if (result.aliveAiMemberCount !=
            std::numeric_limits<uint32_t>::max()) {
            ++result.aliveAiMemberCount;
        }
        allAliveAiIdle = allAliveAiIdle && aiState->idle;
    }
    result.allAliveAiIdle = allAliveAiIdle;
    if (!result.exists || result.aliveMemberCount == 0) return result;

    // RefCode asks each living Team member for an alive enemy in its dynamic
    // vision range and map status. GameSession owns that ECS/spatial fact;
    // this bridge only supplies the Team-hook-specific filter policy.
    const ObjectSightQuery query{
        .relationship = PlayerRelationship::Enemies,
        // Team::update() deliberately lacks the stealth rejection used by the
        // two ordinary sight conditions.
        .concealment = ObjectSightConcealment::IncludeHiddenStealth,
        .requireAliveSource = true,
    };
    for (const ObjectId member : m_world.m_objectTeams.members(team)) {
        if (seesAny(member, query)) {
            result.seesEnemy = true;
            return result;
        }
    }
    return result;
}

container::Vector<ScriptWorldTeamUnitDestroyedEvent>
GameSessionScriptQueryPort::takeTeamUnitDestroyedHookEvents() const {
    return m_eventCursor.takeTeamUnitDestroyedHookEvents();
}

container::Vector<ScriptWorldObjectHookEvent>
GameSessionScriptQueryPort::takeObjectHookEvents() const {
    return m_eventCursor.takeObjectHookEvents();
}

ScriptSequentialAuthorityState GameSessionScriptQueryPort::sequentialObjectState(
    ObjectId object) const noexcept {
    ScriptSequentialAuthorityState result;
    const std::optional<ecs::entity> entity =
        m_world.m_objects.entityFromId(object);
    if (!entity) return result;
    result.exists = true;
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity);
    result.effectivelyDead = health && health->effectivelyDead;
    const std::optional<ai::ObjectAIActorStateView> aiState =
        m_ai.m_objectAI.actorState(object);
    result.hasAI = aiState.has_value();
    if (result.hasAI) {
        result.canGuard = m_ai.m_objectAI.hasOrderCapability(
                              object, ai::ObjectAIOrderCapability::Attack) &&
            m_ai.m_objectAI.hasOrderCapability(
                object, ai::ObjectAIOrderCapability::MoveStop);
    }
    result.idle = aiState && aiState->idle;
    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(m_world.m_registry, *entity);
    const PlayerState* player = owner
        ? m_content.m_players.get(owner->player)
        : nullptr;
    if (player && player->controller == PlayerControllerKind::Ai)
        result.currentPlayer = owner->player;
    return result;
}

ScriptSequentialAuthorityState GameSessionScriptQueryPort::sequentialTeamState(
    ObjectTeamId team) const noexcept {
    ScriptSequentialAuthorityState result;
    if (!m_world.m_objectTeams.find(team) ||
        !m_world.m_objectTeams.isActive(team)) {
        return result;
    }
    result.exists = true;
    result.idle = true;
    result.effectivelyDead = true;
    const std::optional<PlayerId> owner =
        m_world.m_objectTeams.teamOwner(team);
    const PlayerState* player = owner
        ? m_content.m_players.get(*owner)
        : nullptr;
    if (owner && player && player->controller == PlayerControllerKind::Ai)
        result.currentPlayer = *owner;
    for (const ObjectId member : m_world.m_objectTeams.members(team)) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(member);
        if (!entity) continue;
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity);
        const bool dead = health && health->effectivelyDead;
        if (!dead) result.effectivelyDead = false;
        const std::optional<ai::ObjectAIActorStateView> aiState =
            m_ai.m_objectAI.actorState(member);
        if (!aiState) continue;
        result.hasAI = true;
        if (!dead && !aiState->idle) result.idle = false;
    }
    return result;
}

bool GameSessionScriptQueryPort::teamContained(
    ObjectTeamId team, bool entireTeam) const noexcept {
    if (!m_world.m_objectTeams.find(team)) return false;
    bool anyConsidered = false;
    bool anyContained = false;
    bool allContained = true;
    for (const ObjectId member : m_world.m_objectTeams.members(team)) {
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(member);
        if (!entity) continue;
        anyConsidered = true;
        const bool contained = ecs::try_get<ObjectContainedByComponent>(
            m_world.m_registry, *entity) != nullptr;
        anyContained = anyContained || contained;
        allContained = allContained && contained;
    }
    return anyConsidered && (entireTeam ? allContained : anyContained);
}

ScriptWorldNamedObjectState GameSessionScriptQueryPort::namedObjectState(
    container::StringView name) const {
    // emit() commits DELETE/KILL synchronously, so the authoritative
    // ScriptObjectIndex already reflects all earlier actions in this script
    // pass. There is deliberately no shadow name-state overlay.
    if (const std::optional<ObjectId> object = m_presentation.m_scriptObjects.liveNamedObject(name)) {
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(*object);
        if (!entity)
            return ScriptWorldNamedObjectState::Destroyed;
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity);
        return health && health->effectivelyDead ? ScriptWorldNamedObjectState::Dying
                                                 : ScriptWorldNamedObjectState::Alive;
    }
    switch (m_presentation.m_scriptObjects.namedObjectState(name)) {
    case ScriptNamedObjectState::Alive: return ScriptWorldNamedObjectState::Alive;
    case ScriptNamedObjectState::Destroyed: return ScriptWorldNamedObjectState::Destroyed;
    case ScriptNamedObjectState::Unknown: return ScriptWorldNamedObjectState::Unknown;
    }
    return ScriptWorldNamedObjectState::Unknown;
}

ScriptWorldNamedObjectState GameSessionScriptQueryPort::objectState(
    ObjectId object) const noexcept {
    const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
    if (!entity) return ScriptWorldNamedObjectState::Destroyed;
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity);
    return health && health->effectivelyDead
        ? ScriptWorldNamedObjectState::Dying
        : ScriptWorldNamedObjectState::Alive;
}

bool GameSessionScriptQueryPort::namedObjectSelected(
    container::StringView name) const noexcept {
    // RefCode explicitly returns false for multiplayer. The local selection
    // is a bounded input to this bridge pass, not lockstep or replay state.
    if (m_content.m_startInfo.network.enabled ||
        m_content.m_startInfo.mode == GameMode::Replay) {
        return false;
    }
    const std::optional<ObjectId> object =
        m_presentation.m_scriptObjects.liveNamedObject(name);
    return object && objectSelected(*object);
}

bool GameSessionScriptQueryPort::objectSelected(ObjectId object) const noexcept {
    if (m_content.m_startInfo.network.enabled ||
        m_content.m_startInfo.mode == GameMode::Replay ||
        !m_world.m_objects.entityFromId(object))
        return false;
    return m_localPresentation.contains(object);
}

bool GameSessionScriptQueryPort::multiplayerOutcome(
    ScriptMultiplayerOutcomeKind kind) const noexcept {
    const PlayerState* local = m_content.m_players.localPlayer();
    const bool observer = !local ||
        local->participation == PlayerParticipationKind::Observer;
    const auto containsLocal = [local](container::Span<const PlayerId> players) {
        return local && std::binary_search(
            players.begin(), players.end(), local->id);
    };
    const bool alliedDefeat = observer
        ? m_presentation.m_scriptMultiplayerVictory.singleAllianceRemaining
        : m_presentation.m_scriptMultiplayerVictory.singleAllianceRemaining &&
              containsLocal(
                  m_presentation.m_scriptMultiplayerVictory.defeatedPlayers);
    switch (kind) {
    case ScriptMultiplayerOutcomeKind::AlliedVictory:
        return !observer &&
            m_presentation.m_scriptMultiplayerVictory.singleAllianceRemaining &&
            containsLocal(
                m_presentation.m_scriptMultiplayerVictory.victoriousPlayers);
    case ScriptMultiplayerOutcomeKind::AlliedDefeat:
        return alliedDefeat;
    case ScriptMultiplayerOutcomeKind::PlayerDefeat:
        return !observer &&
            containsLocal(
                m_presentation.m_scriptMultiplayerVictory.defeatedPlayers) &&
            !alliedDefeat;
    }
    return false;
}

bool GameSessionScriptQueryPort::teamCommandButtonReady(
    container::StringView teamName,
    container::StringView commandButton,
    bool allReady) const {
    const std::optional<ObjectTeamId> team =
        resolveScenarioTeamAlias(teamName);
    if (!team) return false;
    return teamCommandButtonReady(*team, commandButton, allReady);
}

bool GameSessionScriptQueryPort::teamCommandButtonReady(
    ObjectTeamId team,
    container::StringView commandButton,
    bool allReady) const {
    if (!m_world.m_objectTeams.find(team)) return false;
    const game::CommandButtonTemplate* button =
        m_content.m_contentSnapshot.findCommandButton(commandButton);
    if (!button) return false;

    const SpecialPowerDefinition* power = button->specialPower.empty()
        ? nullptr
        : m_content.m_contentSnapshot.findSpecialPower(button->specialPower);
    const UpgradeCatalog* upgrades = m_content.m_contentSnapshot.upgradeCatalog();
    const UpgradeDefinition* upgrade = !power && upgrades && !button->upgrade.empty()
        ? upgrades->find(button->upgrade)
        : nullptr;
    if (!power && !upgrade) return allReady;

    for (const ObjectId member : m_world.m_objectTeams.members(team)) {
        bool ready = false;
        if (power) {
            const std::optional<ecs::entity> entity =
                m_world.m_objects.entityFromId(member);
            if (!entity) continue;
            const ObjectSpecialPowerComponent* powers =
                ecs::try_get<ObjectSpecialPowerComponent>(
                    m_world.m_registry, *entity);
            if (!powers) continue;

            bool hasPowerType = false;
            for (const ObjectSpecialPowerRuntime& runtime : powers->instances) {
                const SpecialPowerDefinition* candidate =
                    m_content.m_contentSnapshot.findSpecialPower(runtime.content);
                if (!candidate) continue;
                // Invalid Enum matches RefCode "unset type": identity is the
                // concrete SpecialPower template, not a semantic class.
                const bool sameType =
                    power->specialPowerType == game::SpecialPowerType::Invalid
                    ? candidate->id == power->id
                    : candidate->specialPowerType == power->specialPowerType;
                if (sameType) hasPowerType = true;
                if (runtime.content == power->id && runtime.pausedCount == 0 &&
                    runtime.readyTick <= m_confirmedTick) {
                    ready = true;
                }
            }
            if (!hasPowerType) continue;
        } else {
            // Reuse the UpgradeMux affectedByUpgrade/hasUpgrade authority.
            // Affordability and queue state are not part of RefCode isReady.
            ready = canReceiveUpgrade(member, upgrade->name);
        }

        if (ready && !allReady) return true;
        if (!ready && allReady) return false;
    }
    return allReady;
}

bool GameSessionScriptQueryPort::triggerAreaExists(container::StringView areaName) const {
    return !areaName.empty() && m_content.m_terrain.triggerByName(areaName) != nullptr;
}

std::optional<PlayerId> GameSessionScriptQueryPort::teamOwner(container::StringView name) const {
    const std::optional<ObjectTeamId> team = resolveScenarioTeamAlias(name);
    return team ? teamOwner(*team) : std::nullopt;
}

std::optional<PlayerId> GameSessionScriptQueryPort::teamOwner(ObjectTeamId team) const {
    return m_world.m_objectTeams.teamOwner(team);
}

ScriptWorldTeamSummary GameSessionScriptQueryPort::teamSummary(container::StringView name) const {
    const std::optional<ObjectTeamId> team = resolveScenarioTeamAlias(name);
    return team ? teamSummary(*team) : ScriptWorldTeamSummary{};
}

ScriptWorldTeamSummary GameSessionScriptQueryPort::teamSummary(ObjectTeamId team) const {
    ScriptWorldTeamSummary summary;
    if (!m_world.m_objectTeams.find(team)) return summary;

    summary.exists = true;
    summary.createdThisTick = m_world.m_objectTeams.wasCreatedAt(team, m_confirmedTick);

    // Preserve Team::hasAnyUnits/hasAnyObjects rather than using raw member
    // count. The legacy predicates ignore dead/destroyed objects, and differ
    // deliberately over structures and INERT helpers.
    for (const ObjectId object : m_world.m_objectTeams.members(team)) {
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
        if (!entity) continue;

        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity);
        if (health && health->effectivelyDead) continue;

        const ThingTemplateComponent* templateComponent =
            ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity);
        const game::ObjectArchetype* archetype =
            templateComponent ? templateComponent->archetype.get() : nullptr;
        const bool projectile = kindOfContains(
            archetype, game::ObjectKindOf::Projectile);
        const bool mine = kindOfContains(
            archetype, game::ObjectKindOf::Mine);

        if (!projectile && !mine && !kindOfContains(
                archetype, game::ObjectKindOf::Inert)) {
            summary.hasObjects = true;
        }
        if (!projectile && !mine && !kindOfContains(
                archetype, game::ObjectKindOf::Structure)) {
            summary.hasUnits = true;
        }
        if (summary.hasObjects && summary.hasUnits) break;
    }
    return summary;
}

std::optional<container::StringView> GameSessionScriptQueryPort::teamScriptState(
    ObjectTeamId team) const noexcept {
    return m_world.m_objectTeams.scriptState(team);
}

bool GameSessionScriptQueryPort::namedObjectHasAnyStatus(
    container::StringView name, uint64_t statusMask) const {
    const std::optional<ObjectId> object = m_presentation.m_scriptObjects.liveNamedObject(name);
    return object && objectHasAnyStatus(*object, statusMask);
}

bool GameSessionScriptQueryPort::objectHasAnyStatus(
    ObjectId object, uint64_t statusMask) const {
    const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
    if (!entity) return false;
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(m_world.m_registry, *entity);
    return status && status->hasAny(
        static_cast<game::ObjectStatusMask>(statusMask) & game::objectStatusKnownMask());
}

ScriptWorldTeamStatusSummary GameSessionScriptQueryPort::teamStatusSummary(
    container::StringView name, uint64_t statusMask) const {
    const std::optional<ObjectTeamId> team = resolveScenarioTeamAlias(name);
    return team ? teamStatusSummary(*team, statusMask)
                : ScriptWorldTeamStatusSummary{};
}

ScriptWorldTeamStatusSummary GameSessionScriptQueryPort::teamStatusSummary(
    ObjectTeamId team, uint64_t statusMask) const {
    ScriptWorldTeamStatusSummary summary;
    if (!m_world.m_objectTeams.find(team)) return summary;

    summary.exists = true;
    const game::ObjectStatusMask wanted =
        static_cast<game::ObjectStatusMask>(statusMask) & game::objectStatusKnownMask();
    for (const ObjectId object : m_world.m_objectTeams.members(team)) {
        ++summary.members;
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
        if (!entity) {
            // RefCode returns false immediately if its Team member list ever
            // yields a null Object, for both ALL and SOME variants.
            summary.membersValid = false;
            continue;
        }
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(m_world.m_registry, *entity);
        if (status && status->hasAny(wanted)) ++summary.matching;
    }
    return summary;
}

bool GameSessionScriptQueryPort::namedContainmentIsEmpty(container::StringView name) const {
    const std::optional<ObjectId> object = m_presentation.m_scriptObjects.liveNamedObject(name);
    return object && objectContainmentIsEmpty(*object);
}

bool GameSessionScriptQueryPort::objectContainmentIsEmpty(ObjectId object) const {
    const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
    if (!entity) return false;

    const ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(m_world.m_registry, *entity);
    if (!runtime || !runtime->plan) return false;

    const ObjectContainmentComponent* contents =
        ecs::try_get<ObjectContainmentComponent>(m_world.m_registry, *entity);
    return !contents || contents->objects.empty();
}

bool GameSessionScriptQueryPort::namedContainmentHasFreeSlots(container::StringView name) const {
    const std::optional<ObjectId> object = m_presentation.m_scriptObjects.liveNamedObject(name);
    return object && objectContainmentHasFreeSlots(*object);
}

bool GameSessionScriptQueryPort::objectContainmentHasFreeSlots(ObjectId object) const {
    const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
    if (!entity) return false;

    const ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(m_world.m_registry, *entity);
    if (!runtime || !runtime->plan) return false;

    const ObjectContainmentComponent* contents =
        ecs::try_get<ObjectContainmentComponent>(m_world.m_registry, *entity);
    const size_t currentCount = contents ? contents->objects.size() : 0;
    for (const ObjectContainmentRule& rule : runtime->plan->rules) {
        if (currentCount < static_cast<size_t>(rule.containMax)) return true;
    }
    return false;
}


} // namespace engine::script
