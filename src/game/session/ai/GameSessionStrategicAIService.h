#pragma once

#include <utility>

#include "game/session/transaction/GameSessionObjectProductionTransactions.h"
#include "game/session/transaction/GameSessionObjectStateTransactions.h"
#include "game/session/transaction/GameSessionScriptScenarioPlanTransactions.h"

namespace engine
{

class GameSessionAIState;
class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

// Owns player-level strategic planner orchestration. It reads exact frozen
// and world partitions, then dispatches decisions through the command,
// scenario and production authorities without retaining Session or
// ScriptInterface.
class GameSessionStrategicAIService final
{
public:
    GameSessionStrategicAIService(GameSessionContentStartState& content,
                                  GameSessionAIState& ai,
                                  GameSessionScriptPresentationState& presentation,
                                  GameSessionWorldState& world,
                                  GameSessionScriptScenarioPlanTransactions scenarioPlans,
                                  GameSessionObjectProductionTransactions production,
                                  GameSessionObjectStateTransactions objectState) noexcept
        : m_content(content)
        , m_ai(ai)
        , m_presentation(presentation)
        , m_world(world)
        , m_scenarioPlans(std::move(scenarioPlans))
        , m_production(std::move(production))
        , m_objectState(std::move(objectState))
    {
    }

    [[nodiscard]] bool initialize();
    void update();

private:
    GameSessionContentStartState& m_content;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionWorldState& m_world;
    GameSessionScriptScenarioPlanTransactions m_scenarioPlans;
    GameSessionObjectProductionTransactions m_production;
    GameSessionObjectStateTransactions m_objectState;
};

} // namespace engine
