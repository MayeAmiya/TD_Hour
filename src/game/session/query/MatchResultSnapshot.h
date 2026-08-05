#pragma once

#include "core/container/container_types.h"

#include "game/base/GameSettings.h"
#include "game/player/PlayerTypes.h"
#include "game/scenario/runtime/MissionState.h"

#include <cstdint>

namespace engine {

class GameSession;

// Presentation-owned copy of one terminal player row.  The result UI must
// never retain PlayerState, ECS entities, or a GameSession pointer across a
// same-process Next/Retry transition.
struct MatchResultPlayerRow final {
    PlayerId player = INVALID_PLAYER_ID;
    container::String displayName;
    container::String side;
    container::String baseSide;
    container::String scoreScreenImage;
    container::String scoreScreenMusic;
    bool localPlayer = false;
    bool victorious = false;

    uint64_t moneyEarned = 0;
    uint64_t moneySpent = 0;
    uint64_t unitsBuilt = 0;
    uint64_t buildingsBuilt = 0;
    uint64_t unitsDestroyed = 0;
    uint64_t buildingsDestroyed = 0;
    uint64_t unitsLost = 0;
    uint64_t buildingsLost = 0;
    uint64_t factionBuildingsCaptured = 0;
    uint64_t techBuildingsCaptured = 0;
};

// Immutable-by-convention terminal value.  It is captured exactly once after
// the confirmed frame that seals MissionOutcome and remains valid after the
// live session is destroyed for Next/Retry.
struct MatchResultSnapshot final {
    GameStartInfo startInfo;
    scenario::MissionOutcome outcome;
    uint64_t confirmedTick = 0;
    container::Vector<MatchResultPlayerRow> players;
    container::String localScoreScreenImage;
    container::String localScoreScreenMusic;

    [[nodiscard]] bool localVictory() const noexcept;
    [[nodiscard]] const MatchResultPlayerRow* localPlayer() const noexcept;
    [[nodiscard]] static MatchResultSnapshot capture(
        const GameSession& session, const scenario::MissionOutcome& outcome);
};

} // namespace engine
