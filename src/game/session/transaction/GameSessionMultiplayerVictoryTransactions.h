#pragma once

#include "game/session/transaction/GameSessionTransactionPorts.h"

namespace engine {

class GameSessionContentStartState;
class GameSessionWorldState;
class GameSessionScriptPresentationState;

// Owns the deterministic multiplayer elimination/victory transaction.
// It runs after confirmed simulation work and may only request lifecycle
// destruction through the same authoritative transaction boundary used by
// scripts, production and combat.
class GameSessionMultiplayerVictoryTransactions final {
public:
    GameSessionMultiplayerVictoryTransactions(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation,
        GameSessionLifecycleTransactionPort lifecycle) noexcept;

    void refresh();

private:
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionLifecycleTransactionPort m_lifecycle;
};

} // namespace engine
