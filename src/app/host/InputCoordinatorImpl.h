#pragma once

#include "InputCoordinator.h"
#include "WindowEventCoordinator.h"

#include "app/input/CommandModeInputState.h"
#include "app/input/PendingWorldCursorPresenter.h"
#include "app/input/PendingWorldTargetCaptureState.h"
#include "app/input/PlacementInputState.h"
#include "app/input/PointerCaptureState.h"
#include "app/input/PointerClickGestureState.h"
#include "app/input/PresentationCameraInputState.h"
#include "app/input/RadarInputState.h"
#include "app/input/SelectionDragInputState.h"
#include "app/input/WndInputArbiter.h"
#include "engine/input/GameCameraController.h"
#include "engine/renderer/runtime/RendererInputViewport.h"
#include "game/session/query/LocalSelectionPolicy.h"
#include "input/CommandMapRuntime.h"
#include "runtime/GameUiProjection.h"

#include <SDL3/SDL_events.h>

#include <cstdint>
#include <optional>

namespace engine {
class TextureManager;
struct CommandPosition;
}

class InGameGuiSubsystem;
class RendererSubsystem;

namespace app {
class PresentationCoordinator;
namespace runtime {
class GameLogicIntentMailbox;
}

class InputCoordinator::Impl final {
public:
    Impl(RendererSubsystem& renderer,
         InGameGuiSubsystem& inGameGui,
         engine::TextureManager& textureManager,
         PresentationCoordinator& presentation,
         runtime::GameLogicIntentMailbox& logicIntents);
    ~Impl();

    bool processFrame(float presentationDeltaSeconds, bool& running);
    void setGameProjection(const runtime::GameUiProjection& projection);
    void updateAfterLogicTick(int frameCount);
    void synchronizePresentationState();

private:
    using PointerCaptureOwner = input::PointerCaptureOwner;

    [[nodiscard]] bool gameplayInputEnabled() const noexcept;
    [[nodiscard]] bool dispatchGuiEvent(const SDL_Event& event);
    [[nodiscard]] bool handlePlacementEvent(const SDL_Event& event);
    [[nodiscard]] bool handleRadarEvent(const SDL_Event& event);
    [[nodiscard]] bool handleSelectionEvent(const SDL_Event& event);
    [[nodiscard]] bool handleCommandMapEvent(const SDL_Event& event);
    [[nodiscard]] bool handlePendingWorldCommandEvent(
        const SDL_Event& event);
    [[nodiscard]] bool dispatchCapturedPointerEvent(const SDL_Event& event);
    [[nodiscard]] bool pointerEventBlockedByWnd(
        const SDL_Event& event) const noexcept;
    void armContextualWorldCommand(const SDL_Event& event) noexcept;
    void updateContextualWorldCommandPointer(float x, float y) noexcept;
    void completeContextualWorldCommand(const SDL_Event& event);
    void submitContextualWorldCommand(
        float screenX, float screenY, uint64_t sessionRevision,
        bool forceAttack, bool forceMove,
        std::optional<engine::CommandPosition> targetPosition = std::nullopt,
        bool attackMove = false,
        bool guardPosition = false,
        std::optional<engine::selection::LocalSelectionGesture>
            fallbackSelection = std::nullopt);
    void cancelContextualWorldCommand() noexcept;
    void armDefaultRightClick(const SDL_Event& event) noexcept;
    void updateDefaultRightClick(float x, float y) noexcept;
    void completeDefaultRightClick(const SDL_Event& event);
    void cancelDefaultRightClick() noexcept;
    void cancelPendingWorldTargetCapture() noexcept;
    void beginPointerCapture(PointerCaptureOwner owner,
                             uint8_t button) noexcept;
    void clearPointerCapture() noexcept;
    void cancelGameplayCapture() noexcept;
    void resetGameplayHeldState() noexcept;
    void synchronizeGameplayInputOwnership();
    void routeEvent(const SDL_Event& event, bool& running);
    void queueCameraInput(float presentationDeltaSeconds);
    void queuePlacementIntent(int frameCount);

    RendererSubsystem& m_renderer;
    InGameGuiSubsystem& m_inGameGui;
    PresentationCoordinator& m_presentation;
    runtime::GameLogicIntentMailbox& m_logicIntents;
    WindowEventCoordinator m_windowEvents;
    input::PresentationCameraInputState m_cameraPresentation;
    input::SelectionDragInputState m_selectionDrag;
    input::WndInputArbiter m_wndInput;
    engine::RendererInputViewport m_inputViewport;
    runtime::GameUiProjection m_gameProjection;
    engine::GameCameraController m_cameraController;
    input::CommandMapRuntime m_commandMap;
    input::RadarInputState m_radarInput;
    input::PlacementInputState m_placementInput;
    // Presentation-local authoring latch only. It prevents a fast Shift-up
    // final click from becoming a nonqueued command before the confirmed
    // Build queue has reached the latest UI projection.
    engine::ObjectId m_queuedConstructionAuthoringBuilder =
        engine::INVALID_OBJECT_ID;
    input::PointerCaptureState m_pointerCapture;
    input::PointerClickGestureState m_contextualWorldCommand;
    input::PointerClickGestureState m_defaultRightClick;
    input::CommandModeInputState m_commandModes;
    bool m_cameraTrackingDrawable = false;
    input::PendingWorldTargetCaptureState m_pendingWorldTarget;
    input::PendingWorldCursorPresenter m_pendingWorldCursorPresenter;
    bool m_scriptInputCursorHidden = false;
    bool m_windowFocused = true;
    bool m_mouseGrabApplied = false;
    uint64_t m_scriptInputSuppressionSessionRevision = 0;
};

} // namespace app
