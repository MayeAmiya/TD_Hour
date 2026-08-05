#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "game/script/runtime/ScriptRuntime.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"
#include "game/session/transaction/GameSessionTransactionPorts.h"

#include <cstdint>

namespace engine {

class GameSessionAIState;
class GameSessionContentStartState;
class GameSessionFrameCommitState;
class GameSessionObjectEventState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

// Owns the confirmed ScriptRuntime step and its pre-step fact projection.
// Script effects commit through explicit transaction capabilities; frame
// ingress and presentation journals remain separate owners.
class GameSessionScriptFrameTransactions final {
public:
    GameSessionScriptFrameTransactions(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionAIState& ai,
        GameSessionScriptPresentationState& presentation,
        GameSessionObjectEventState& objectEvents,
        GameSessionFrameCommitState& frame) noexcept
        : m_content(content),
          m_world(world),
          m_ai(ai),
          m_presentation(presentation),
          m_objectEvents(objectEvents),
          m_publication(content, world, presentation, frame),
          m_lifecycle(makeLifecycleTransactionPort(
              content, world, ai, presentation, objectEvents, frame)) {}

    [[nodiscard]] script::ScriptRuntimeStepResult advance(
        uint64_t confirmedInputTick,
        container::Span<const ObjectId> localSelection,
        uint64_t worldConfirmedTick);

private:
    void refreshAreaTransitions();
    void drainPresentationCompletions();
    [[nodiscard]] bool acceptsLocalPresentationCompletion() const noexcept;

    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionObjectEventState& m_objectEvents;
    GameSessionGameplayPublicationPort m_publication;
    GameSessionLifecycleTransactionPort m_lifecycle;
};

} // namespace engine
