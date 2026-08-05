#pragma once

#include "game/object/simulation/runtime/ObjectHealthEvents.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"

namespace engine {

class GameSessionContentStartState;
class GameSessionScriptPresentationState;

class GameSessionObjectDeathFeedbackPublisher final {
public:
    GameSessionObjectDeathFeedbackPublisher(
        GameSessionContentStartState& content,
        GameSessionScriptPresentationState& presentation,
        GameSessionGameplayPublicationPort publication) noexcept;

    void publish(const ObjectHealthEvent& event);

private:
    GameSessionContentStartState& m_content;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionGameplayPublicationPort m_publication;
};

} // namespace engine
