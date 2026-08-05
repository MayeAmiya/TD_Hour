#include "game/session/integration/GameSessionScriptAuthorityPort.h"

#include "game/session/state/GameSessionDomainState.h"

#include <algorithm>

namespace engine::script {

bool GameSessionScriptAuthorityPort::applyMapAuthority(
    const ScriptMapPresentationEffect& effect, uint64_t confirmedTick,
    uint32_t sourceScriptId, uint32_t ordinal, PlayerId currentPlayer,
    container::StringView currentPlayerAlias) {
    if (!m_content.m_active) return false;

    const auto nextStamp = [&]() noexcept {
        ++m_presentation.m_scriptPresentationSequence;
        if (m_presentation.m_scriptPresentationSequence == 0) {
            ++m_presentation.m_scriptPresentationSequence;
        }
        return ScriptPresentationControlStamp{
            .presentationEpoch = m_presentation.m_scriptPresentationEpoch,
            .sequence = m_presentation.m_scriptPresentationSequence,
            .confirmedTick = confirmedTick,
            .sourceScriptId = sourceScriptId,
            .ordinal = ordinal,
        };
    };

    const auto targetPlayers = [&](bool emptyMeansAllHumans) {
        container::Vector<PlayerId> result;
        const auto append = [&result](PlayerId player) {
            if (!player ||
                std::find(result.begin(), result.end(), player) != result.end()) {
                return;
            }
            result.push_back(player);
        };
        const auto appendAllHumans = [&]() {
            for (const PlayerId player : m_content.m_players.activePlayerIds()) {
                const PlayerState* state = m_content.m_players.get(player);
                if (state && state->controller == PlayerControllerKind::Human) {
                    append(player);
                }
            }
        };
        if (effect.playerName.empty()) {
            if (emptyMeansAllHumans) appendAllHumans();
            return result;
        }
        if (const std::optional<PlayerId> selected = resolvePlayer(
                effect.playerName, currentPlayer, currentPlayerAlias);
            selected && m_content.m_players.get(*selected)) {
            append(*selected);
        }
        if (result.empty() && emptyMeansAllHumans) appendAllHumans();
        return result;
    };

    switch (effect.command) {
    case ScriptMapPresentationCommand::RefreshRadar:
        if (!m_world.m_mapVisibility.refresh()) return false;
        m_presentation.m_scriptMapPresentation.noteMapMutation(nextStamp());
        return true;
    case ScriptMapPresentationCommand::SetBoundary: {
        if (effect.boundaryIndex < 0 || !m_content.m_terrain.isLoaded()) {
            return false;
        }
        if (!m_content.m_terrain.setActiveBoundary(
                static_cast<size_t>(effect.boundaryIndex))) {
            return false;
        }
        static_cast<void>(m_world.m_mapVisibility.refresh());
        if (m_presentation.m_scriptMapPresentation.boundaryIndex() !=
            effect.boundaryIndex) {
            static_cast<void>(m_presentation.m_scriptMapPresentation.
                setBoundary(effect.boundaryIndex, nextStamp()));
        } else {
            m_presentation.m_scriptMapPresentation.noteMapMutation(nextStamp());
        }
        return true;
    }
    case ScriptMapPresentationCommand::RevealAtWaypoint:
    case ScriptMapPresentationCommand::ShroudAtWaypoint: {
        const game::terrain::WaypointRecord* waypoint =
            m_content.m_terrain.waypointByName(effect.waypointName);
        if (!waypoint) return true;
        const math::q32_32 centerX =
            math::q32_32::from_raw(waypoint->positionRaw[0]);
        const math::q32_32 centerY =
            math::q32_32::from_raw(waypoint->positionRaw[1]);
        bool changed = false;
        for (const PlayerId player : targetPlayers(true)) {
            if (effect.command ==
                ScriptMapPresentationCommand::RevealAtWaypoint) {
                changed = m_world.m_mapVisibility.revealCircle(
                    player, centerX, centerY, effect.radius) || changed;
            } else {
                changed = m_world.m_mapVisibility.shroudCircle(
                    player, centerX, centerY, effect.radius) || changed;
            }
        }
        if (changed) {
            m_presentation.m_scriptMapPresentation.noteMapMutation(nextStamp());
        }
        return true;
    }
    case ScriptMapPresentationCommand::RevealAll:
    case ScriptMapPresentationCommand::RevealAllPermanently:
    case ScriptMapPresentationCommand::UndoRevealAllPermanently:
    case ScriptMapPresentationCommand::ShroudAll: {
        bool changed = false;
        for (const PlayerId player : targetPlayers(true)) {
            switch (effect.command) {
            case ScriptMapPresentationCommand::RevealAll:
                changed = m_world.m_mapVisibility.revealAll(player) || changed;
                break;
            case ScriptMapPresentationCommand::RevealAllPermanently:
                changed = m_world.m_mapVisibility.revealAllPermanently(player) ||
                    changed;
                break;
            case ScriptMapPresentationCommand::UndoRevealAllPermanently:
                changed = m_world.m_mapVisibility.
                    undoRevealAllPermanently(player) || changed;
                break;
            case ScriptMapPresentationCommand::ShroudAll:
                changed = m_world.m_mapVisibility.shroudAll(player) || changed;
                break;
            default:
                break;
            }
        }
        if (changed) {
            m_presentation.m_scriptMapPresentation.noteMapMutation(nextStamp());
        }
        return true;
    }
    case ScriptMapPresentationCommand::RevealPermanentlyAtWaypoint: {
        const game::terrain::WaypointRecord* waypoint =
            m_content.m_terrain.waypointByName(effect.waypointName);
        const container::Vector<PlayerId> players = targetPlayers(false);
        if (!waypoint || players.empty()) return true;
        if (m_world.m_mapVisibility.createNamedPermanentReveal(
                effect.revealName, players.front(),
                math::q32_32::from_raw(waypoint->positionRaw[0]),
                math::q32_32::from_raw(waypoint->positionRaw[1]),
                effect.radius)) {
            m_presentation.m_scriptMapPresentation.noteMapMutation(nextStamp());
        }
        return true;
    }
    case ScriptMapPresentationCommand::UndoRevealPermanentlyAtWaypoint:
        if (m_world.m_mapVisibility.undoNamedPermanentReveal(
                effect.revealName)) {
            m_presentation.m_scriptMapPresentation.noteMapMutation(nextStamp());
        }
        return true;
    default:
        return false;
    }
}

} // namespace engine::script
