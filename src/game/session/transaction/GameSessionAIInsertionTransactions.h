#pragma once

namespace engine {

struct CommandBackendOutcome;

class GameSessionAIState;
class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

class GameSessionAIInsertionTransactions final {
public:
    GameSessionAIInsertionTransactions(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionAIState& ai,
        GameSessionScriptPresentationState& presentation) noexcept
        : m_content(content), m_world(world), m_ai(ai),
          m_presentation(presentation) {}

    void stageMotionFeedback();
    void observeOrders();

private:
    void emitOutcome(CommandBackendOutcome outcome);

    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
};

} // namespace engine
