#include "game/session/transaction/GameSessionPlayerStateTransactions.h"

#include "game/player/PlayerRegistry.h"
#include "game/data/base/ScienceCatalog.h"

namespace engine {

GameSessionPlayerStateTransactions::GameSessionPlayerStateTransactions(
    PlayerRegistry& players) noexcept
    : m_players(players) {}

bool GameSessionPlayerStateTransactions::setCash(
    PlayerId player, int64_t value) {
    return m_players.setCash(player, value);
}

bool GameSessionPlayerStateTransactions::adjustCash(
    PlayerId player, int64_t delta) {
    return m_players.adjustCash(player, delta);
}

bool GameSessionPlayerStateTransactions::setLifeState(
    PlayerId player, PlayerLifeState state) {
    return m_players.setLifeState(player, state);
}

bool GameSessionPlayerStateTransactions::purchaseScience(
    PlayerId player, const ScienceDefinition& science) {
    return science.grantable && m_players.tryPurchaseScience(player, science);
}

} // namespace engine
