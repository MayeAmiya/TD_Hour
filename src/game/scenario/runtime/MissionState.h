#pragma once

#include "game/player/PlayerTypes.h"

#include <cstdint>
#include <optional>

namespace engine::scenario {

// A mission terminal result belongs to the simulation session, rather than a
// shell CampaignManager or a WND overlay.  The latter consume this value after
// the confirmed frame has committed it.
enum class MissionTerminalState : uint8_t {
    Running,
    Victory,
    Defeat,
};

// The terminal result is shared by VICTORY and QUICKVICTORY, but their
// legacy end-game timers differ. The presentation/shell layer can reproduce
// that distinction without teaching ScriptRuntime about WNDs or wall clocks.
enum class MissionEndMode : uint8_t {
    Normal,
    Quick,
};

struct MissionOutcome final {
    MissionTerminalState state = MissionTerminalState::Running;
    // Source ScriptList player for diagnostics only. VICTORY/DEFEAT are
    // global actions and never resolve this as a target player.
    PlayerId sourcePlayer = INVALID_PLAYER_ID;
    uint64_t confirmedTick = 0;
    uint32_t sourceScriptId = 0;
    MissionEndMode mode = MissionEndMode::Normal;
};

// First committed terminal result wins.  This mirrors the required fixed-tick
// ordering rule: a later script may observe the terminal state next tick, but
// cannot rewrite an already sealed victory/defeat with iteration-dependent
// behavior.
class MissionState final {
public:
    [[nodiscard]] bool finish(MissionOutcome outcome) noexcept;
    void reset() noexcept { m_outcome.reset(); }

    [[nodiscard]] MissionTerminalState state() const noexcept {
        return m_outcome ? m_outcome->state : MissionTerminalState::Running;
    }
    [[nodiscard]] const std::optional<MissionOutcome>& outcome() const noexcept {
        return m_outcome;
    }

private:
    std::optional<MissionOutcome> m_outcome;
};

} // namespace engine::scenario
