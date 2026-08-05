#pragma once

#include "game/session/query/GameSessionAIOrderPolicy.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"

namespace engine {

class GameSessionAIState;
class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

class GameSessionAIResolutionTransactions final {
public:
    GameSessionAIResolutionTransactions(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionAIState& ai,
        GameSessionScriptPresentationState& presentation,
        GameSessionGameplayPublicationPort publication) noexcept
        : m_content(content), m_world(world), m_ai(ai),
          m_presentation(presentation), m_publication(publication),
          m_policy(content, world, presentation) {}

    void resolveOpportunityQueries();
    void resolveTacticalAttackQueries();
    void resolveGuardCommands();

private:
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionGameplayPublicationPort m_publication;
    GameSessionAIOrderPolicy m_policy;
};

} // namespace engine
