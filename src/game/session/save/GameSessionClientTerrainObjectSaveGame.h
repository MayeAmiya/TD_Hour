#pragma once

#include "core/container/container_types.h"

#include <cstdint>

namespace engine {

class GameSession;

struct GameSessionClientTerrainObjectSaveGameRestoreResult final {
    bool ok = false;
    uint64_t confirmedTick = 0;
    container::String error;
};

// Formal GameSession owner for the client-only TerrainVisual object chunk.
// The full SaveGame coordinator restores authoritative simulation and rebuilds
// the map/client baseline before invoking restore(). Ordinary replay remains a
// tick-zero command stream and deliberately does not use this checkpoint.
class GameSessionClientTerrainObjectSaveGame final {
public:
    [[nodiscard]] static container::Vector<uint8_t> capture(
        const GameSession& session, container::String* error = nullptr);
    [[nodiscard]] static GameSessionClientTerrainObjectSaveGameRestoreResult
    restore(GameSession& session,
            const container::Vector<uint8_t>& transaction,
            uint64_t restoredSimulationTick);
};

} // namespace engine
