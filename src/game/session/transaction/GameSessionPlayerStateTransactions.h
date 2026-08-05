#pragma once

#include "game/player/PlayerTypes.h"

#include <cstdint>

namespace engine {

class PlayerRegistry;
struct ScienceDefinition;

// Deterministic player-state mutation boundary shared by script effects and
// frame transactions. Alias resolution and effect diagnostics remain with
// the caller; this owner performs the actual authoritative write.
class GameSessionPlayerStateTransactions final {
public:
    explicit GameSessionPlayerStateTransactions(
        PlayerRegistry& players) noexcept;

    [[nodiscard]] bool setCash(PlayerId player, int64_t value);
    [[nodiscard]] bool adjustCash(PlayerId player, int64_t delta);
    [[nodiscard]] bool setLifeState(
        PlayerId player, PlayerLifeState state);
    [[nodiscard]] bool purchaseScience(
        PlayerId player, const ScienceDefinition& science);

private:
    PlayerRegistry& m_players;
};

} // namespace engine
