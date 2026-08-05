#pragma once

#include "game/session/frame/GameSessionFramePort.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"

namespace engine {

class GameSessionAIState;
class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

class GameSessionAIShadowTransactions final {
public:
    GameSessionAIShadowTransactions(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionAIState& ai,
        GameSessionScriptPresentationState& presentation,
        GameSessionGameplayPublicationPort publication,
        GameSessionFramePort frame) noexcept
        : m_content(content), m_world(world), m_ai(ai),
          m_presentation(presentation), m_publication(publication),
          m_frame(frame) {}

    void run();

private:
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionGameplayPublicationPort m_publication;
    GameSessionFramePort m_frame;
};

} // namespace engine
