#pragma once

#include "game/session/object/GameSessionObjectContracts.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/audio/GameAudioEvents.h"
#include "game/base/FrameCommitResult.h"
#include "game/fx/runtime/GameFxEvents.h"
#include "game/script/bridge/ScriptSessionEvents.h"

#include <optional>

namespace engine {

class GameSessionContentStartState;
class GameSessionFrameCommitState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

// Confirmed gameplay publication capability shared by object transactions,
// weapon drains and map import. It exposes no Session or ECS owner.
class GameSessionGameplayPublicationPort final {
public:
    GameSessionGameplayPublicationPort(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation,
        GameSessionFrameCommitState& frame) noexcept
        : m_content(&content), m_world(&world), m_presentation(&presentation),
          m_frame(&frame) {}
    [[nodiscard]] bool raiseSimulationFault(
        SimulationFault fault) noexcept;
    [[nodiscard]] bool emitAudioEvent(game::GameAudioEvent event);
    [[nodiscard]] bool emitAudioControlEvent(
        game::GameAudioControlEvent event);
    [[nodiscard]] bool emitFxInvocationEvent(game::FxInvocationEvent event);
    void emitScriptSessionEvent(script::ScriptSessionEvent event);

private:
    [[nodiscard]] std::optional<PlayerId> ownerPlayerFor(
        ObjectId object) const noexcept;

    GameSessionContentStartState* m_content = nullptr;
    GameSessionWorldState* m_world = nullptr;
    GameSessionScriptPresentationState* m_presentation = nullptr;
    GameSessionFrameCommitState* m_frame = nullptr;
};

} // namespace engine
