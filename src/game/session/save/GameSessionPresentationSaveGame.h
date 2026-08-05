#pragma once

#include "core/container/container_types.h"

#include <cstdint>

namespace engine {

class GameSession;

struct GameSessionPresentationSaveGameRestoreResult final {
    bool ok = false;
    uint64_t confirmedTick = 0;
    container::String error;
};

// Current outer owner for renderer-independent TerrainVisual SaveGame chunks.
// This envelope is intentionally not a replay checkpoint. A future full
// simulation SaveGame coordinator may embed the same payload and must restore
// its authoritative chunks to confirmedTick before invoking restore().
class GameSessionPresentationSaveGame final {
public:
    [[nodiscard]] static container::Vector<uint8_t> capture(
        const GameSession& session, container::String* error = nullptr);
    [[nodiscard]] static GameSessionPresentationSaveGameRestoreResult restore(
        GameSession& session,
        const container::Vector<uint8_t>& transaction,
        uint64_t restoredSimulationTick);
};

} // namespace engine
