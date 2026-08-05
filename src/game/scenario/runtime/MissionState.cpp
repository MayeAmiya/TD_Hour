#include "game/scenario/runtime/MissionState.h"

namespace engine::scenario {

bool MissionState::finish(MissionOutcome outcome) noexcept {
    if (m_outcome || outcome.state == MissionTerminalState::Running) return false;
    m_outcome = outcome;
    return true;
}

} // namespace engine::scenario
