#pragma once

#include "engine/renderer/runtime/RendererInputViewport.h"
#include "game/command/GameCommand.h"
#include "game/session/query/LocalSelectionPolicy.h"

#include <cstdint>
#include <optional>

union SDL_Event;

namespace app {
class PresentationCoordinator;
namespace runtime {
class GameLogicIntentMailbox;
struct GameUiProjection;
}
namespace input {
class CommandModeInputState;
class PendingWorldTargetCaptureState;

enum class PendingWorldInputResult : uint8_t {
    Ignored,
    Consumed,
    ConsumedAndReleaseCapture,
};

struct WorldCommandInputContext final {
    const runtime::GameUiProjection& projection;
    PresentationCoordinator& presentation;
    runtime::GameLogicIntentMailbox& logicIntents;
    CommandModeInputState& commandModes;
    PendingWorldTargetCaptureState& pendingTarget;
    engine::RendererInputViewport viewport;
};

class WorldCommandInputDispatcher final {
public:
    static void submitContextual(
        WorldCommandInputContext context,
        float screenX, float screenY, uint64_t sessionRevision,
        bool forceAttack, bool forceMove,
        std::optional<engine::CommandPosition> targetPosition,
        bool attackMove,
        bool guardPosition,
        std::optional<engine::selection::LocalSelectionGesture>
            fallbackSelection = std::nullopt);

    [[nodiscard]] static PendingWorldInputResult handlePending(
        WorldCommandInputContext context, const SDL_Event& event);
};

} // namespace input
} // namespace app
