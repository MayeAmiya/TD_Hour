#pragma once

#include "game/session/transaction/GameSessionGameplayPublicationPort.h"

namespace engine
{

class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

// Drains confirmed object-side presentation journals into detached FX and
// beacon output. The publisher retains only the state partitions it needs.
class GameSessionObjectEventPublisher final
{
public:
    GameSessionObjectEventPublisher(GameSessionContentStartState& content,
                                    GameSessionWorldState& world,
                                    GameSessionScriptPresentationState& presentation,
                                    GameSessionGameplayPublicationPort publication) noexcept
        : m_content(content)
        , m_world(world)
        , m_presentation(presentation)
        , m_publication(publication)
    {
    }

    void publishFx();
    void publishTechAndBeacon();

private:
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionGameplayPublicationPort m_publication;
};

} // namespace engine
