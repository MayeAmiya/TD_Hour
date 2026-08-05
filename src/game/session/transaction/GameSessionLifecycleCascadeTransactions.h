#pragma once

#include "core/container/container_types.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"
#include "game/session/transaction/GameSessionNavigationFootprintTransactions.h"

namespace engine {

class GameSessionAIState;
class GameSessionContentStartState;
class GameSessionFrameCommitState;
class GameSessionObjectEventState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

// Commits ObjectLifecycle's ordered Created/DestroyRequested/Destroyed
// journal into stable indexes, AI membership, navigation and presentation.
class GameSessionLifecycleCascadeTransactions final {
public:
    GameSessionLifecycleCascadeTransactions(
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
          m_navigation(content, world, presentation, frame) {}

    [[nodiscard]] bool consume();

private:
    void projectPresentation(
        container::Span<const ObjectLifecycleEvent> events);

    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionObjectEventState& m_objectEvents;
    GameSessionGameplayPublicationPort m_publication;
    GameSessionNavigationFootprintTransactions m_navigation;
};

} // namespace engine
