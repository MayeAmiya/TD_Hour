#pragma once

#include "core/container/container_types.h"

#include <cstdint>

namespace engine {

class GameSession;

struct GameSessionVertexWaterSaveGameRestoreResult final {
    bool ok = false;
    uint64_t confirmedTick = 0;
    container::String error;
};

// Formal GameSession owner boundary for the TerrainVisual VertexWater chunk.
// An outer full-SaveGame coordinator must restore authoritative state to the
// same tick before calling restore(); this prevents a presentation checkpoint
// from being applied as an unrelated replay/late-session snapshot.
class GameSessionVertexWaterSaveGame final {
public:
    [[nodiscard]] static container::Vector<uint8_t> capture(
        const GameSession& session, container::String* error = nullptr);
    [[nodiscard]] static GameSessionVertexWaterSaveGameRestoreResult restore(
        GameSession& session,
        const container::Vector<uint8_t>& transaction,
        uint64_t restoredSimulationTick);
};

} // namespace engine
