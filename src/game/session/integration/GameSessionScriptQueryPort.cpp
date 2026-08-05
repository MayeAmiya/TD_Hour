#include "game/session/integration/GameSessionScriptQueryPort.h"

#include "core/container/string_utils.h"
#include "game/session/state/GameSessionDomainState.h"

#include <algorithm>

namespace engine::script {
namespace {

constexpr auto equalAsciiInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] bool isThisPlayerReference(container::StringView value) noexcept {
    return equalAsciiInsensitive(value, "ThisPlayer") ||
           equalAsciiInsensitive(value, "<This Player>");
}

[[nodiscard]] bool isThisPlayerEnemyReference(
    container::StringView value) noexcept {
    return equalAsciiInsensitive(value, "<This Player's Enemy>");
}

[[nodiscard]] bool isLocalPlayerReference(container::StringView value) noexcept {
    return value.empty() || equalAsciiInsensitive(value, "LocalPlayer") ||
           equalAsciiInsensitive(value, "<Local Player>");
}

} // namespace

GameSessionScriptQueryPort::GameSessionScriptQueryPort(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionAIState& ai,
    GameSessionScriptPresentationState& presentation,
    GameSessionObjectEventState& objectEvents,
    uint64_t confirmedTick,
    const GameSessionScriptLocalPresentationState& localPresentation)
    : m_content(content),
      m_world(world),
      m_ai(ai),
      m_presentation(presentation),
      m_eventCursor(content, presentation, objectEvents),
      m_localPresentation(localPresentation),
      m_confirmedTick(confirmedTick) {}

std::optional<PlayerId> GameSessionScriptQueryPort::resolvePlayer(
    container::StringView name, PlayerId currentPlayer,
    container::StringView currentPlayerAlias) const noexcept {
    if (isThisPlayerReference(name)) {
        if (currentPlayer && m_content.m_players.get(currentPlayer)) {
            return currentPlayer;
        }
        if (currentPlayerAlias.empty() ||
            isThisPlayerReference(currentPlayerAlias)) {
            return std::nullopt;
        }
        return resolvePlayer(currentPlayerAlias, INVALID_PLAYER_ID, {});
    }
    if (isThisPlayerEnemyReference(name)) {
        if (currentPlayer && m_content.m_players.get(currentPlayer)) {
            return currentEnemyPlayerFor(currentPlayer);
        }
        if (currentPlayerAlias.empty() ||
            isThisPlayerReference(currentPlayerAlias) ||
            isThisPlayerEnemyReference(currentPlayerAlias)) {
            return std::nullopt;
        }
        const std::optional<PlayerId> resolvedCurrent = resolvePlayer(
            currentPlayerAlias, INVALID_PLAYER_ID, {});
        return resolvedCurrent
            ? currentEnemyPlayerFor(*resolvedCurrent)
            : std::nullopt;
    }
    if (isLocalPlayerReference(name)) {
        const PlayerId local = m_content.m_players.localPlayerId();
        return m_content.m_players.get(local)
            ? std::optional<PlayerId>{local}
            : std::nullopt;
    }
    return resolvePlayerAlias(name);
}

std::optional<PlayerId> GameSessionScriptQueryPort::resolvePlayerAlias(
    container::StringView alias) const noexcept {
    if (alias.empty()) return std::nullopt;
    if (m_presentation.m_scenarioDefinition) {
        const scenario::OwnerReference scenarioOwner =
            m_presentation.m_scenarioDefinition->resolveOwner(alias);
        if ((scenarioOwner.kind == scenario::OwnerReferenceKind::Player ||
             scenarioOwner.kind == scenario::OwnerReferenceKind::ScriptTeam) &&
            m_content.m_players.get(scenarioOwner.player)) {
            return scenarioOwner.player;
        }
    }
    if (equalAsciiInsensitive(alias, "Neutral")) {
        return m_content.m_players.neutralPlayer()
            ? std::optional<PlayerId>{NEUTRAL_PLAYER_ID}
            : std::nullopt;
    }
    if (const std::optional<PlayerId> mapPlayer =
            parseCanonicalMapPlayerAlias(alias);
        mapPlayer && m_content.m_players.get(*mapPlayer)) {
        return mapPlayer;
    }

    constexpr container::StringView legacyPlayerPrefix = "Plyr";
    container::StringView factionAlias = alias;
    const bool hasLegacyPlayerPrefix =
        alias.size() > legacyPlayerPrefix.size() &&
        equalAsciiInsensitive(
            alias.substr(0, legacyPlayerPrefix.size()), legacyPlayerPrefix);
    if (hasLegacyPlayerPrefix) {
        factionAlias = alias.substr(legacyPlayerPrefix.size());
        if (equalAsciiInsensitive(factionAlias, "Civilian")) {
            return m_content.m_players.neutralPlayer()
                ? std::optional<PlayerId>{NEUTRAL_PLAYER_ID}
                : std::nullopt;
        }
    }

    for (const PlayerId id : m_content.m_players.activePlayerIds()) {
        const PlayerState* player = m_content.m_players.get(id);
        if (!player) continue;
        if (equalAsciiInsensitive(alias, player->displayName) ||
            equalAsciiInsensitive(alias, player->side) ||
            equalAsciiInsensitive(alias, player->baseSide) ||
            (hasLegacyPlayerPrefix &&
             (equalAsciiInsensitive(factionAlias, player->displayName) ||
              equalAsciiInsensitive(factionAlias, player->side) ||
              equalAsciiInsensitive(factionAlias, player->baseSide)))) {
            return id;
        }
    }
    return std::nullopt;
}

std::optional<PlayerId> GameSessionScriptQueryPort::currentEnemyPlayerFor(
    PlayerId currentPlayer) const noexcept {
    if (!m_content.m_players.get(currentPlayer)) return std::nullopt;
    const auto eligible = [currentPlayer](
                              const PlayerState* candidate) noexcept {
        return candidate && candidate->id != currentPlayer &&
            candidate->isPlayableSide() &&
            candidate->life != PlayerLifeState::Defeated;
    };
    if (const StrategicAIPlayerBrain* brain =
            m_ai.m_strategicAI.findBrain(currentPlayer)) {
        const PlayerState* candidate =
            m_content.m_players.get(brain->currentEnemy);
        if (eligible(candidate) &&
            m_content.m_players.relationships().get(
                currentPlayer, brain->currentEnemy) ==
                PlayerRelationship::Enemies) {
            return brain->currentEnemy;
        }
    }
    for (const PlayerId candidateId : m_content.m_players.activePlayerIds()) {
        const PlayerState* candidate = m_content.m_players.get(candidateId);
        if (eligible(candidate) &&
            m_content.m_players.relationships().get(
                currentPlayer, candidateId) == PlayerRelationship::Enemies) {
            return candidateId;
        }
    }
    for (const PlayerId candidateId : m_content.m_players.activePlayerIds()) {
        const PlayerState* candidate = m_content.m_players.get(candidateId);
        if (eligible(candidate) &&
            candidate->controller == PlayerControllerKind::Human) {
            return candidateId;
        }
    }
    return std::nullopt;
}

std::optional<ObjectTeamId>
GameSessionScriptQueryPort::resolveScenarioTeamAlias(
    container::StringView alias, ObjectTeamId callingTeam,
    ObjectTeamId conditionTeam) const noexcept {
    if (!m_presentation.m_scenarioDefinition || alias.empty())
        return std::nullopt;
    const scenario::OwnerReference reference =
        m_presentation.m_scenarioDefinition->resolveOwner(alias);
    if (reference.kind != scenario::OwnerReferenceKind::ScriptTeam ||
        !reference.scriptTeam) {
        return std::nullopt;
    }
    const scenario::ScriptTeamDefinition* definition =
        m_presentation.m_scenarioDefinition->findScriptTeam(
            reference.scriptTeam);
    if (!definition) return std::nullopt;

    const auto resolvePreferred = [this, definition](ObjectTeamId preferred)
        -> std::optional<ObjectTeamId> {
        if (!preferred || !m_world.m_objectTeams.find(preferred))
            return std::nullopt;
        const container::Span<const ObjectTeamId> instances =
            m_world.m_objectTeams.scenarioTeamInstances(definition->id);
        if (std::find(instances.begin(), instances.end(), preferred) ==
            instances.end()) {
            return std::nullopt;
        }
        if (definition->isSingleton &&
            !m_world.m_objectTeams.isActive(preferred)) {
            return std::nullopt;
        }
        return preferred;
    };
    if (const auto preferred = resolvePreferred(callingTeam)) return preferred;
    if (const auto preferred = resolvePreferred(conditionTeam)) return preferred;

    const std::optional<ObjectTeamId> instance =
        m_world.m_objectTeams.scenarioTeam(definition->id);
    if (!instance ||
        (definition->isSingleton &&
         !m_world.m_objectTeams.isActive(*instance))) {
        return std::nullopt;
    }
    return instance;
}

} // namespace engine::script
