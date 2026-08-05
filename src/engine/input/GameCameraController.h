#pragma once

#include <cstdint>

#include "presentation/camera/GameCameraInput.h"
#include "presentation/render/RenderGameDataSettings.h"

union SDL_Event;

namespace engine
{

// SDL presentation adapter for the value-only game camera. It owns
// transient SDL button/key state only and produces a value-only input sample;
// GameSession/GameCameraDirector remains the durable camera authority. The
// application calls handleEvent() before UI dispatch while a game world is
// active, then queues consumeInput() before the next confirmed logic tick.
class GameCameraController final
{
public:
    // Frozen session settings are copied into this presentation-only input
    // translator.  It never retains the shared settings handle and never
    // reaches back into Options.ini/GlobalData while processing an event.
    void configure(const RenderCameraGameData& camera, const RenderInputGameData& input) noexcept;

    // Screen-edge policy depends on the current display mode and cursor
    // capture.  Supplying those values explicitly keeps SDL window ownership
    // in the application composition root.
    void setPresentationContext(uint32_t width, uint32_t height, bool fullscreen, bool cursorCaptured) noexcept;

    // Returns true exactly for a world-camera input event. In alternate-mouse
    // mode an RMB event returns false unless RMB scrolling is explicitly
    // enabled, leaving contextual command/cancel routing reachable by UI.
    [[nodiscard]] bool handleEvent(const SDL_Event& event) noexcept;

    // Consumes this presentation frame's discrete input and samples held
    // keyboard axes. It never writes a camera, renderer or ECS object.
    [[nodiscard]] GameCameraInput consumeInput() noexcept;

    // Queue one frame-rate-independent pitch step. Debug hotkeys use degrees
    // because their authored increment is user-facing; the value-only camera
    // input crossing the runtime boundary remains radians.
    void queuePitchStepDegrees(float degrees) noexcept;
    void setKeyboardRotateLeft(bool active) noexcept;
    void setKeyboardRotateRight(bool active) noexcept;
    void setKeyboardZoomIn(bool active) noexcept;
    void setKeyboardZoomOut(bool active) noexcept;
    void queueResetToHome() noexcept;

    // Compatibility helper for tools/standalone callers. Runtime code should
    // queue consumeInput() into GameSession and let its fixed tick apply it.
    void update(GameCameraState& camera, float deltaSeconds) noexcept;

    // Clears transient input when focus changes or a game session ends, so a
    // lost key/button-up event cannot move a later session's camera.
    void reset() noexcept;

    [[nodiscard]] bool isDragging() const noexcept
    {
        return m_rightDrag || m_middleDrag;
    }
    [[nodiscard]] bool reservesRightMouseForCamera() const noexcept;
    [[nodiscard]] bool rightMouseDragging() const noexcept {
        return m_rightDrag;
    }
    [[nodiscard]] float rightMouseAnchorX() const noexcept {
        return m_rightAnchorX;
    }
    [[nodiscard]] float rightMouseAnchorY() const noexcept {
        return m_rightAnchorY;
    }

private:
    bool m_panForward = false;
    bool m_panBackward = false;
    bool m_panLeft = false;
    bool m_panRight = false;
    bool m_pitchUp = false;
    bool m_pitchDown = false;
    bool m_rotateLeft = false;
    bool m_rotateRight = false;
    bool m_zoomIn = false;
    bool m_zoomOut = false;
    bool m_rightDrag = false;
    bool m_middleDrag = false;
    bool m_hasPointerPosition = false;
    float m_pointerX = 0.0f;
    float m_pointerY = 0.0f;
    float m_rightAnchorX = 0.0f;
    float m_rightAnchorY = 0.0f;
    float m_middleAnchorX = 0.0f;
    float m_middleAnchorY = 0.0f;
    float m_middleDisplacementX = 0.0f;
    float m_middleDisplacementY = 0.0f;
    uint64_t m_middleDownTimestampMilliseconds = 0;
    uint32_t m_presentationWidth = 0;
    uint32_t m_presentationHeight = 0;
    bool m_fullscreen = false;
    bool m_cursorCaptured = false;
    RenderCameraGameData m_cameraSettings;
    RenderInputGameData m_inputSettings;
    float m_pendingZoom = 0.0f;
    float m_pendingPanPixelsX = 0.0f;
    float m_pendingPanPixelsY = 0.0f;
    float m_pendingOrbitPixelsX = 0.0f;
    float m_pendingOrbitPixelsY = 0.0f;
    float m_pendingPitchStepRadians = 0.0f;
    bool m_pendingResetToHome = false;
    bool m_manualIntentSinceConsume = false;

    [[nodiscard]] bool screenEdgeScrollEnabled() const noexcept;
};

} // namespace engine
