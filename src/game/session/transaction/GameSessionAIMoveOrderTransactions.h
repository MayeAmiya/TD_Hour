#pragma once

#include "game/session/frame/GameSessionFramePort.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"

namespace engine {

class GameSessionAIState;
class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

// Owns ECS move-order observation, ObjectAI admission/completion and route
// marker maintenance for one confirmed frame.
class GameSessionAIMoveOrderTransactions final {
public:
    GameSessionAIMoveOrderTransactions(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionAIState& ai,
        GameSessionScriptPresentationState& presentation,
        GameSessionGameplayPublicationPort publication,
        GameSessionFramePort frame) noexcept
        : m_content(content), m_world(world), m_ai(ai),
          m_presentation(presentation), m_publication(publication),
          m_frame(frame) {}

    void observeOrders();
    void commitWaypointCompletions();
    void commitMoveCompletions();

private:
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionGameplayPublicationPort m_publication;
    GameSessionFramePort m_frame;
};

} // namespace engine
