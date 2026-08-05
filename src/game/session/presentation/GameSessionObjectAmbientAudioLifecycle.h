#pragma once

#include "core/ecs/ObjectId.h"
#include "game/data/base/ObjectSimulationRules.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"

namespace engine {

class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

// Owns object-bound ambient audio presentation state and its ordered control
// journal. Gameplay health/lifecycle publishers request semantic start,
// refresh and stop operations without touching audio maps directly.
class GameSessionObjectAmbientAudioLifecycle final {
public:
    GameSessionObjectAmbientAudioLifecycle(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation,
        GameSessionGameplayPublicationPort publication) noexcept;

    void start(ObjectId object);
    void refresh(ObjectId object, ObjectBodyDamageState damageState);
    void stop(ObjectId object);

private:
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionGameplayPublicationPort m_publication;
};

} // namespace engine
