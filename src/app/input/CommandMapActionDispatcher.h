#pragma once

#include "app/input/CommandMapRuntime.h"
#include "engine/renderer/runtime/RendererInputViewport.h"

union SDL_Event;

class InGameGuiSubsystem;
class RendererSubsystem;

namespace engine {
class GameCameraController;
}

namespace app {
class PresentationCoordinator;
namespace runtime {
class GameLogicIntentMailbox;
struct GameUiProjection;
}
namespace input {
class CommandModeInputState;
class PresentationCameraInputState;
class RadarInputState;

struct CommandMapViewDispatchContext final {
    RendererSubsystem& renderer;
    PresentationCoordinator& presentation;
    PresentationCameraInputState& cameraPresentation;
    engine::GameCameraController& cameraController;
    RadarInputState& radarInput;
    const runtime::GameUiProjection& projection;
    bool& cameraTrackingDrawable;
};

struct CommandMapUiDispatchContext final {
    InGameGuiSubsystem& inGameGui;
};

struct CommandMapSelectionDispatchContext final {
    PresentationCoordinator& presentation;
    runtime::GameLogicIntentMailbox& logicIntents;
    CommandModeInputState& commandModes;
    const runtime::GameUiProjection& projection;
    engine::RendererInputViewport viewport;
};

struct CommandMapGameplayDispatchContext final {
    runtime::GameLogicIntentMailbox& logicIntents;
    CommandModeInputState& commandModes;
    const runtime::GameUiProjection& projection;
};

struct CommandMapDispatchContext final {
    CommandMapViewDispatchContext view;
    CommandMapUiDispatchContext ui;
    CommandMapSelectionDispatchContext selection;
    CommandMapGameplayDispatchContext gameplay;
};

class CommandMapActionDispatcher final {
public:
    [[nodiscard]] static bool dispatch(
        CommandMapDispatchContext context,
        const SDL_Event& event,
        const CommandMapBinding& binding,
        bool inGame);
};

} // namespace input
} // namespace app
