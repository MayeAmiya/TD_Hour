#pragma once

#include "core/container/container_types.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"

namespace engine {

class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;
// Projects confirmed weapon facts into render/audio journals. Gameplay
// producers only hand over the ordered value batch.
class GameSessionWeaponEventPublisher final {
public:
    GameSessionWeaponEventPublisher(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation,
        GameSessionGameplayPublicationPort publication) noexcept;

    void publish(container::Vector<ObjectWeaponEvent> events);

private:
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionGameplayPublicationPort m_publication;
};

} // namespace engine
