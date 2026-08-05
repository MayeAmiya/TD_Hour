#pragma once

#include "game/session/presentation/GameSessionObjectAmbientAudioLifecycle.h"
#include "game/session/transaction/GameSessionNavigationTransactions.h"

namespace engine {

class GameSessionContentStartState;
class GameSessionObjectEventState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

class GameSessionDeletePostambleTransactions final {
public:
    GameSessionDeletePostambleTransactions(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation,
        GameSessionObjectEventState& objectEvents,
        GameSessionNavigationTransactions navigation,
        GameSessionGameplayPublicationPort publication) noexcept;

    void consume();

private:
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionObjectEventState& m_objectEvents;
    GameSessionNavigationTransactions m_navigation;
    GameSessionGameplayPublicationPort m_publication;
    GameSessionObjectAmbientAudioLifecycle m_ambientAudio;
};

} // namespace engine
