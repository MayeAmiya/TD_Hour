#pragma once

#include "core/container/container_types.h"
#include "game/object/simulation/runtime/ObjectHealthEvents.h"
#include "game/object/ai/states/combat/AITacticalAttackStateData.h"
#include "game/session/query/GameSessionAIOrderPolicy.h"

namespace engine {

class GameSessionAIState;
class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

// Owns the confirmed ECS order-queue ↔ ObjectAI attack synchronization,
// including human guard retaliation generated from frozen damage facts.
class GameSessionAIAttackOrderTransactions final {
public:
    GameSessionAIAttackOrderTransactions(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionAIState& ai,
        GameSessionScriptPresentationState& presentation) noexcept;

    void produceGuardRetaliationOrders(
        container::Span<const ObjectHealthEvent> damageEvents);
    void observeAttackOrders();
    void observeTacticalAttackOrders();
    void commitAttackCompletions();

private:
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionAIOrderPolicy m_policy;
};

} // namespace engine
