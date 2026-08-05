#pragma once

#include "game/session/transaction/GameSessionGameplayPublicationPort.h"

namespace engine {

class GameSessionWorldState;

// Converts confirmed dynamic-geometry events into detached FX requests.
// Gameplay OCL consequences are consumed before this publisher and cannot be
// triggered from the presentation path.
class GameSessionDynamicGeometryEventPublisher final {
public:
    GameSessionDynamicGeometryEventPublisher(
        GameSessionWorldState& world,
        GameSessionGameplayPublicationPort publication) noexcept
        : m_world(world),
          m_publication(publication) {}

    void publish();

private:
    GameSessionWorldState& m_world;
    GameSessionGameplayPublicationPort m_publication;
};

} // namespace engine
