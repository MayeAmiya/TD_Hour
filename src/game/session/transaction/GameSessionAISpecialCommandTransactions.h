#pragma once

#include "game/session/transaction/GameSessionGameplayPublicationPort.h"
#include "game/session/transaction/GameSessionObjectLifecycleTransactions.h"

#include <utility>

namespace engine {

class GameSessionAIState;
class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

class GameSessionAISpecialCommandTransactions final {
public:
    GameSessionAISpecialCommandTransactions(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionAIState& ai,
        GameSessionScriptPresentationState& presentation,
        GameSessionGameplayPublicationPort publication,
        GameSessionObjectLifecycleTransactions lifecycle) noexcept
        : m_content(content), m_world(world), m_ai(ai),
          m_presentation(presentation), m_publication(publication),
          m_lifecycle(std::move(lifecycle)) {}

    void resolve();

private:
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionGameplayPublicationPort m_publication;
    GameSessionObjectLifecycleTransactions m_lifecycle;
};

} // namespace engine
