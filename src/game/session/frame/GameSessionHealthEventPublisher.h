#pragma once

#include "game/session/frame/GameSessionObjectDeathFeedbackPublisher.h"
#include "game/session/presentation/GameSessionObjectAmbientAudioLifecycle.h"
#include "game/session/transaction/GameSessionAIAttackOrderTransactions.h"

namespace engine {

class GameSessionAIState;
class GameSessionContentStartState;
class GameSessionObjectEventState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

class GameSessionHealthEventPublisher final {
public:
    GameSessionHealthEventPublisher(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionAIState& ai,
        GameSessionScriptPresentationState& presentation,
        GameSessionObjectEventState& objectEvents,
        GameSessionGameplayPublicationPort publication) noexcept;

    void consume();

private:
    // RefCode Object::onDamage calls Radar::tryUnderAttackEvent for actual
    // damage taken by a locally controlled object that appears on the radar.
    // The radar admits at most one under-attack event per ten seconds, then
    // emits the warning beep, the RADAR: caption and, for a victory-counting
    // structure, the BaseUnderAttack / AllyUnderAttack announcement. This is
    // observer-local presentation only.
    void publishUnderAttackFeedback(const ObjectHealthEvent& event);

    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionObjectEventState& m_objectEvents;
    GameSessionGameplayPublicationPort m_publication;
    GameSessionAIAttackOrderTransactions m_attackOrders;
    GameSessionObjectAmbientAudioLifecycle m_ambientAudio;
    GameSessionObjectDeathFeedbackPublisher m_deathFeedback;
};

} // namespace engine
