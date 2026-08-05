#pragma once

#include "game/session/transaction/GameSessionTransactionPorts.h"

namespace engine {

class GameSessionScriptPresentationState;
class GameSessionWorldState;

// Closes the countermeasure spawn/ack/diversion sequence in one confirmed
// transaction so projectile resolution never observes a half-created flare.
class GameSessionCountermeasureTransactions final {
public:
    GameSessionCountermeasureTransactions(
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation,
        GameSessionLifecycleTransactionPort barrier) noexcept
        : m_world(world), m_presentation(presentation), m_barrier(barrier) {}

    void updateAndResolveDiversions();

private:
    GameSessionWorldState& m_world;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionLifecycleTransactionPort m_barrier;
};

} // namespace engine
