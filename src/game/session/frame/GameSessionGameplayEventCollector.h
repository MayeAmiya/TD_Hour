#pragma once

#include "game/session/transaction/GameSessionGameplayPublicationPort.h"

namespace engine {

class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;
struct ObjectSpecialPowerExecutionEvent;

// Collects confirmed ObjectSimulation producer facts into player/script
// journals. It has no lifecycle, AI, renderer or Session access.
class GameSessionGameplayEventCollector final {
public:
    GameSessionGameplayEventCollector(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation,
        GameSessionGameplayPublicationPort publication) noexcept
        : m_content(content),
          m_world(world),
          m_presentation(presentation),
          m_publication(publication) {}

    void collectSpecialPowerProducerEvents();

private:
    // Observer-local counterpart of the EVA ladder inside RefCode
    // SpecialPowerModule::aboutToDoSpecialPower.
    void publishSuperweaponLaunched(
        const ObjectSpecialPowerExecutionEvent& event);

    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionGameplayPublicationPort m_publication;
};

} // namespace engine
