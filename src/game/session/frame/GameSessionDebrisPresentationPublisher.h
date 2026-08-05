#pragma once

#include "game/session/transaction/GameSessionGameplayPublicationPort.h"

namespace engine {

class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

// Advances debris-only presentation state and publishes its detached audio/FX
// journals. It owns no gameplay authority beyond the debris presentation
// components produced by ObjectSimulation.
class GameSessionDebrisPresentationPublisher final {
public:
    GameSessionDebrisPresentationPublisher(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation,
        GameSessionGameplayPublicationPort publication) noexcept
        : m_content(content), m_world(world), m_presentation(presentation),
          m_publication(publication) {}

    void publish();

private:
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionGameplayPublicationPort m_publication;
};

} // namespace engine
