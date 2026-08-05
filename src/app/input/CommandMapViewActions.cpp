#include "app/input/CommandMapDomainDispatch.h"

#include "app/host/PresentationCoordinator.h"
#include "app/input/PresentationCameraInputState.h"
#include "app/input/RadarInputState.h"
#include "app/runtime/GameUiProjection.h"
#include "core/config/GlobalData.h"
#include "engine/input/GameCameraController.h"
#include "engine/system/RendererSubsystem.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <filesystem>

namespace app::input::command_map_domain {
namespace {

[[nodiscard]] container::String nextScreenshotPath() {
    namespace fs = std::filesystem;
    std::error_code error;
    fs::path root = config::TheGlobalData.getUserDataPath();
    if (root.empty()) root = fs::current_path(error);
    if (error) return {};
    fs::create_directories(root, error);
    if (error) return {};
    for (uint32_t index = 1; index <= 9999u; ++index) {
        char leaf[32]{};
        std::snprintf(leaf, sizeof(leaf), "sshot%03u.bmp", index);
        const fs::path candidate = root / leaf;
        if (!fs::exists(candidate, error))
            return error ? container::String{} : candidate.string();
        if (error) return {};
    }
    return {};
}

} // namespace

bool dispatchView(
    CommandMapViewDispatchContext context,
    const SDL_Event&,
    const CommandMapBinding& binding,
    bool inGame) {
    const CommandMapAction action = binding.action;
    const bool pressed = binding.transition == CommandMapTransition::Down;
    if (action == CommandMapAction::TakeScreenshot && pressed) {
        context.renderer.captureScreenshot(nextScreenshotPath());
        return true;
    }
    if (!inGame) return false;
    if (action == CommandMapAction::BeginCameraRotateLeft ||
        action == CommandMapAction::EndCameraRotateLeft) {
        context.cameraController.setKeyboardRotateLeft(pressed);
        return true;
    }
    if (action == CommandMapAction::BeginCameraRotateRight ||
        action == CommandMapAction::EndCameraRotateRight) {
        context.cameraController.setKeyboardRotateRight(pressed);
        return true;
    }
    if (action == CommandMapAction::BeginCameraZoomIn ||
        action == CommandMapAction::EndCameraZoomIn) {
        context.cameraController.setKeyboardZoomIn(pressed);
        return true;
    }
    if (action == CommandMapAction::BeginCameraZoomOut ||
        action == CommandMapAction::EndCameraZoomOut) {
        context.cameraController.setKeyboardZoomOut(pressed);
        return true;
    }
    if (!pressed) return false;
    if (action == CommandMapAction::SaveView && binding.ordinal >= 1u &&
        binding.ordinal <= 8u && context.projection.hasCameraState) {
        context.cameraPresentation.saveView(
            binding.ordinal, context.projection.cameraState);
        return true;
    }
    if (action == CommandMapAction::ViewSaved && binding.ordinal >= 1u &&
        binding.ordinal <= 8u) {
        static_cast<void>(
            context.cameraPresentation.requestRestore(binding.ordinal));
        return true;
    }
    if (action == CommandMapAction::CameraReset) {
        context.cameraController.queueResetToHome();
        return true;
    }
    if (action == CommandMapAction::ViewLastRadarEvent) {
        if (const auto world = context.presentation.lastRadarEventWorld())
            context.radarInput.queueLookAt(*world);
        return true;
    }
    if (action == CommandMapAction::ToggleCameraTrackingDrawable) {
        // Toggle, not set: the assignment form meant pressing the follow hotkey
        // a second time did nothing, manual scrolling stayed overridden every
        // presentation frame, and the lock persisted until the tracked object
        // died or the match ended.
        context.cameraTrackingDrawable = !context.cameraTrackingDrawable;
        return true;
    }
    return false;
}

} // namespace app::input::command_map_domain
