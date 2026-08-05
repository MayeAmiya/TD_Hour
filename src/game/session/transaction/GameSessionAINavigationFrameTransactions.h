#pragma once

#include "game/session/transaction/GameSessionGameplayPublicationPort.h"
#include "game/session/transaction/GameSessionNavigationPathAdapter.h"

namespace engine {

class GameSessionAIState;
class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

// Owns the confirmed ObjectAI transient ↔ navigation adapter handoff. Path
// values remain in their respective owners; this transaction only transfers
// them and publishes deterministic handoff faults.
class GameSessionAINavigationFrameTransactions final {
public:
    GameSessionAINavigationFrameTransactions(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionAIState& ai,
        GameSessionScriptPresentationState& presentation,
        GameSessionGameplayPublicationPort publication) noexcept
        : m_navigation(content, world, presentation), m_world(world), m_ai(ai),
          m_presentation(presentation),
          m_publication(publication) {}

    void pollFeedback();
    void submitRequests();

private:
    GameSessionNavigationPathAdapter m_navigation;
    GameSessionWorldState& m_world;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionGameplayPublicationPort m_publication;
};

} // namespace engine
