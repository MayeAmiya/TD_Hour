#pragma once

#include "core/container/container_types.h"

#include "game/session/query/MatchResultSnapshot.h"

namespace gui::ingame {

struct ScoreScreenPlayerViewModel final {
    container::String name;
    container::String unitsBuilt;
    container::String unitsLost;
    container::String unitsDestroyed;
    container::String buildingsBuilt;
    container::String buildingsLost;
    container::String buildingsDestroyed;
    container::String resourcesCollected;
};

// WND-ready value projection of a frozen match result.  It deliberately owns
// every string shown by the overlay and contains no GameSession/ECS handles.
struct ScoreScreenViewModel final {
    bool victory = false;
    container::String resultTitle;
    container::String missionSummary;
    container::Vector<container::String> playerRows;
    container::Vector<ScoreScreenPlayerViewModel> players;
    container::String backgroundImage;
    container::String music;

    [[nodiscard]] static ScoreScreenViewModel fromSnapshot(
        const engine::MatchResultSnapshot& snapshot);
};

} // namespace gui::ingame
