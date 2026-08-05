#include "game/session/query/MatchResultSnapshot.h"

#include "game/player/FactionTemplate.h"
#include "game/player/PlayerRegistry.h"
#include "game/session/core/GameSession.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/query/GameSessionRulesetQueryPort.h"

namespace engine {

bool MatchResultSnapshot::localVictory() const noexcept {
    return outcome.state == scenario::MissionTerminalState::Victory;
}

const MatchResultPlayerRow* MatchResultSnapshot::localPlayer() const noexcept {
    for (const MatchResultPlayerRow& row : players) {
        if (row.localPlayer) return &row;
    }
    return nullptr;
}

MatchResultSnapshot MatchResultSnapshot::capture(
    const GameSession& session, const scenario::MissionOutcome& outcome) {
    MatchResultSnapshot result;
    const GameSessionContentStartState& content =
        session.domainState().contentState();
    result.startInfo = content.m_startInfo;
    result.outcome = outcome;
    result.confirmedTick = outcome.confirmedTick != 0
        ? outcome.confirmedTick
        : session.confirmedTick();

    const PlayerRegistry& registry = content.m_players;
    const PlayerId localPlayerId = registry.localPlayerId();
    for (const PlayerId playerId : registry.activePlayerIds()) {
        const PlayerState* player = registry.get(playerId);
        if (!player || !player->progress.listedInScoreScreen) continue;

        MatchResultPlayerRow row;
        row.player = player->id;
        row.displayName = player->displayName;
        row.side = player->side;
        row.baseSide = player->baseSide;
        row.localPlayer = player->id == localPlayerId;
        // MissionOutcome is global.  Team placement is not yet represented by
        // a result ledger, so only the local row receives the global outcome;
        // other rows remain available for roster/stat presentation.
        row.victorious = row.localPlayer &&
            outcome.state == scenario::MissionTerminalState::Victory;

        row.moneyEarned = player->score.moneyEarned;
        row.moneySpent = player->score.moneySpent;
        row.unitsBuilt = player->score.unitsBuilt;
        row.buildingsBuilt = player->score.buildingsBuilt;
        row.unitsLost = player->score.unitsLost;
        row.buildingsLost = player->score.buildingsLost;
        row.factionBuildingsCaptured = player->score.factionBuildingsCaptured;
        row.techBuildingsCaptured = player->score.techBuildingsCaptured;
        for (const uint64_t count : player->score.unitsDestroyed) {
            row.unitsDestroyed += count;
        }
        for (const uint64_t count : player->score.buildingsDestroyed) {
            row.buildingsDestroyed += count;
        }

        if (const std::optional<PlayerTemplatePresentationData> presentation =
                session.rulesetQuery().factionPresentation(player->faction)) {
            row.scoreScreenImage = presentation->scoreScreenImage;
            row.scoreScreenMusic = presentation->scoreScreenMusic;
        }
        if (row.localPlayer) {
            result.localScoreScreenImage = row.scoreScreenImage;
            result.localScoreScreenMusic = row.scoreScreenMusic;
        }
        result.players.push_back(std::move(row));
    }
    return result;
}

} // namespace engine
