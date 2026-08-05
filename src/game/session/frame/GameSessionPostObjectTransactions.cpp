#include "game/session/core/GameSessionDomainComposition.h"

namespace engine::detail {

void GameSessionDomainComposition::publishPostCommandObjectTransactions(
    const GameSessionPostCombatFrameState& frame) {
    static_cast<void>(frame);
    // Structural gameplay work is admitted by GameSessionWeaponEventDrain
    // immediately after ObjectSimulation. Keep the stage as an explicit
    // compatibility barrier until the surrounding frame pipeline is folded.
}

} // namespace engine::detail
