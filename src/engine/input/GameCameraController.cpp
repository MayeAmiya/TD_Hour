#include "engine/input/GameCameraController.h"

#include <SDL3/SDL.h>
#include <cmath>

namespace engine
{
namespace
{

constexpr float kMaximumPendingPixels = 16384.0f;
constexpr float kMaximumPendingWheelUnits = 32.0f;
constexpr float kMaximumPendingOrbitRadians = 3.14159265358979323846f;
constexpr float kDegreesToRadians =
    3.14159265358979323846f / 180.0f;
constexpr uint64_t kMiddleClickMaximumMilliseconds = 167;
constexpr float kMiddleClickMaximumDisplacementPixels = 5.0f;

bool finite(float value) noexcept
{
    return std::isfinite(value);
}

float clampPending(float value) noexcept
{
    if (!finite(value))
        return 0.0f;
    return value < -kMaximumPendingPixels  ? -kMaximumPendingPixels
           : value > kMaximumPendingPixels ? kMaximumPendingPixels
                                           : value;
}

uint64_t eventTimestampMilliseconds(const SDL_Event& event) noexcept
{
    // SDL3 timestamps are nanoseconds since SDL initialization.  Only a
    // duration is observed here, so no wall-clock conversion is required.
    return event.common.timestamp / 1'000'000ULL;
}

} // namespace

void GameCameraController::configure(const RenderCameraGameData& camera, const RenderInputGameData& input) noexcept
{
    m_cameraSettings = camera;
    m_inputSettings = input;
}

void GameCameraController::setPresentationContext(uint32_t width,
                                                  uint32_t height,
                                                  bool fullscreen,
                                                  bool cursorCaptured) noexcept
{
    m_presentationWidth = width;
    m_presentationHeight = height;
    m_fullscreen = fullscreen;
    m_cursorCaptured = cursorCaptured;
}

bool GameCameraController::reservesRightMouseForCamera() const noexcept
{
    return !m_inputSettings.useAlternateMouse || m_inputSettings.useRightMouseScrollWithAlternateMouse;
}

bool GameCameraController::screenEdgeScrollEnabled() const noexcept
{
    if (!m_cursorCaptured || m_presentationWidth == 0 || m_presentationHeight == 0)
    {
        return false;
    }
    return m_fullscreen ? m_inputSettings.screenEdgeScrollFullscreen : m_inputSettings.screenEdgeScrollWindowed;
}

void GameCameraController::queuePitchStepDegrees(float degrees) noexcept
{
    if (!finite(degrees))
        return;
    m_pendingPitchStepRadians = std::clamp(
        m_pendingPitchStepRadians + degrees * kDegreesToRadians,
        -kMaximumPendingOrbitRadians, kMaximumPendingOrbitRadians);
    if (std::abs(degrees) > math::EPSILON)
        m_manualIntentSinceConsume = true;
}

void GameCameraController::setKeyboardRotateLeft(bool active) noexcept
{
    m_rotateLeft = active;
    if (active) m_manualIntentSinceConsume = true;
}

void GameCameraController::setKeyboardRotateRight(bool active) noexcept
{
    m_rotateRight = active;
    if (active) m_manualIntentSinceConsume = true;
}

void GameCameraController::setKeyboardZoomIn(bool active) noexcept
{
    m_zoomIn = active;
    if (active) m_manualIntentSinceConsume = true;
}

void GameCameraController::setKeyboardZoomOut(bool active) noexcept
{
    m_zoomOut = active;
    if (active) m_manualIntentSinceConsume = true;
}

void GameCameraController::queueResetToHome() noexcept
{
    m_pendingResetToHome = true;
    m_manualIntentSinceConsume = true;
}

bool GameCameraController::handleEvent(const SDL_Event& event) noexcept
{
    switch (event.type)
    {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        switch (event.key.scancode)
        {
        case SDL_SCANCODE_UP:
            m_panForward = down;
            m_manualIntentSinceConsume = m_manualIntentSinceConsume || down;
            return true;
        case SDL_SCANCODE_DOWN:
            m_panBackward = down;
            m_manualIntentSinceConsume = m_manualIntentSinceConsume || down;
            return true;
        case SDL_SCANCODE_LEFT:
            m_panLeft = down;
            m_manualIntentSinceConsume = m_manualIntentSinceConsume || down;
            return true;
        case SDL_SCANCODE_RIGHT:
            m_panRight = down;
            m_manualIntentSinceConsume = m_manualIntentSinceConsume || down;
            return true;
        case SDL_SCANCODE_PAGEUP:
            m_pitchUp = down;
            m_manualIntentSinceConsume = m_manualIntentSinceConsume || down;
            return true;
        case SDL_SCANCODE_PAGEDOWN:
            m_pitchDown = down;
            m_manualIntentSinceConsume = m_manualIntentSinceConsume || down;
            return true;
        default:
            return false;
        }
    }
    case SDL_EVENT_MOUSE_WHEEL:
    {
        float wheel = event.wheel.y;
        if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
            wheel = -wheel;
        if (finite(wheel))
        {
            const float next = m_pendingZoom + wheel;
            m_pendingZoom = next < -kMaximumPendingWheelUnits  ? -kMaximumPendingWheelUnits
                            : next > kMaximumPendingWheelUnits ? kMaximumPendingWheelUnits
                                                               : next;
            if (std::abs(wheel) > math::EPSILON)
                m_manualIntentSinceConsume = true;
        }
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.button == SDL_BUTTON_RIGHT)
        {
            if (!reservesRightMouseForCamera())
                return false;
            m_rightDrag = true;
            m_hasPointerPosition = true;
            m_pointerX = finite(event.button.x) ? event.button.x : 0.0f;
            m_pointerY = finite(event.button.y) ? event.button.y : 0.0f;
            m_rightAnchorX = m_pointerX;
            m_rightAnchorY = m_pointerY;
            return true;
        }
        if (event.button.button == SDL_BUTTON_MIDDLE)
        {
            m_middleDrag = true;
            m_hasPointerPosition = true;
            m_pointerX = finite(event.button.x) ? event.button.x : 0.0f;
            m_pointerY = finite(event.button.y) ? event.button.y : 0.0f;
            m_middleAnchorX = m_pointerX;
            m_middleAnchorY = m_pointerY;
            m_middleDisplacementX = 0.0f;
            m_middleDisplacementY = 0.0f;
            m_middleDownTimestampMilliseconds = eventTimestampMilliseconds(event);
            return true;
        }
        return false;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button == SDL_BUTTON_RIGHT)
        {
            if (!m_rightDrag)
                return false;
            m_rightDrag = false;
            return true;
        }
        if (event.button.button == SDL_BUTTON_MIDDLE)
        {
            if (!m_middleDrag)
                return false;
            m_middleDrag = false;
            const float releaseX = finite(event.button.x) ? event.button.x : m_pointerX;
            const float releaseY = finite(event.button.y) ? event.button.y : m_pointerY;
            const float dx = std::max(std::abs(releaseX - m_middleAnchorX), std::abs(m_middleDisplacementX));
            const float dy = std::max(std::abs(releaseY - m_middleAnchorY), std::abs(m_middleDisplacementY));
            const uint64_t releaseMilliseconds = eventTimestampMilliseconds(event);
            const uint64_t elapsed = releaseMilliseconds >= m_middleDownTimestampMilliseconds
                                         ? releaseMilliseconds - m_middleDownTimestampMilliseconds
                                         : UINT64_MAX;
            if (dx <= kMiddleClickMaximumDisplacementPixels && dy <= kMiddleClickMaximumDisplacementPixels &&
                elapsed < kMiddleClickMaximumMilliseconds)
            {
                m_manualIntentSinceConsume = true;
                // Reuse a pending bool rather than mutating a camera from the
                // SDL event thread.  consumeInput publishes the reset at the
                // same confirmed-tick boundary as every other camera action.
                m_pendingResetToHome = true;
            }
            return true;
        }
        return false;
    case SDL_EVENT_MOUSE_MOTION: {
        m_hasPointerPosition = true;
        const float xrel = finite(event.motion.xrel) ? event.motion.xrel : 0.0f;
        const float yrel = finite(event.motion.yrel) ? event.motion.yrel : 0.0f;
        if (finite(event.motion.x) && finite(event.motion.y) &&
            (event.motion.x != 0.0f || event.motion.y != 0.0f || (xrel == 0.0f && yrel == 0.0f)))
        {
            m_pointerX = event.motion.x;
            m_pointerY = event.motion.y;
        }
        else
        {
            // Relative-mode and focused probes may not carry absolute
            // coordinates. Preserve anchor semantics by integrating the
            // same SDL relative values in that case.
            m_pointerX += xrel;
            m_pointerY += yrel;
        }
        // The middle button takes precedence if both are held.  The original
        // rotates yaw only; vertical motion is retained solely for its click
        // displacement threshold and never changes pitch.
        if (m_middleDrag || m_rightDrag)
        {
            if (m_middleDrag)
            {
                m_pendingOrbitPixelsX = clampPending(m_pendingOrbitPixelsX + xrel);
                m_middleDisplacementX = clampPending(m_middleDisplacementX + xrel);
                m_middleDisplacementY = clampPending(m_middleDisplacementY + yrel);
            }
            if (std::abs(xrel) > math::EPSILON || std::abs(yrel) > math::EPSILON)
            {
                m_manualIntentSinceConsume = true;
            }
            return true;
        }
        // Ordinary motion remains UI-visible.  It only updates the held
        // screen-edge axes sampled later by consumeInput().
        return false;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        reset();
        return false;
    default:
        return false;
    }
}

GameCameraInput GameCameraController::consumeInput() noexcept
{
    GameCameraInput input;
    float forwardAxis = (m_panForward ? 1.0f : 0.0f) - (m_panBackward ? 1.0f : 0.0f);
    float rightAxis = (m_panRight ? 1.0f : 0.0f) - (m_panLeft ? 1.0f : 0.0f);
    const float axisLengthSq = forwardAxis * forwardAxis + rightAxis * rightAxis;
    if (axisLengthSq > 1.0f)
    {
        const float inverseLength = 1.0f / std::sqrt(axisLengthSq);
        forwardAxis *= inverseLength;
        rightAxis *= inverseLength;
    }
    input.panForwardAxis = forwardAxis;
    input.panRightAxis = rightAxis;
    input.orbitPitchAxis =
        (m_pitchUp ? 1.0f : 0.0f) - (m_pitchDown ? 1.0f : 0.0f);
    input.orbitYawAxis =
        (m_rotateRight ? 1.0f : 0.0f) - (m_rotateLeft ? 1.0f : 0.0f);
    input.zoomAxis =
        (m_zoomIn ? 1.0f : 0.0f) - (m_zoomOut ? 1.0f : 0.0f);
    if (screenEdgeScrollEnabled() && m_hasPointerPosition && !m_rightDrag && !m_middleDrag)
    {
        const float edge = static_cast<float>(m_inputSettings.screenEdgeWidthPixels);
        input.screenEdgeRightAxis = (m_pointerX >= static_cast<float>(m_presentationWidth) - edge ? 1.0f : 0.0f) -
                                    (m_pointerX < edge ? 1.0f : 0.0f);
        input.screenEdgeForwardAxis = (m_pointerY < edge ? 1.0f : 0.0f) -
                                      (m_pointerY >= static_cast<float>(m_presentationHeight) - edge ? 1.0f : 0.0f);
    }
    if (m_rightDrag)
    {
        input.anchorScrollPixelsX = m_pointerX - m_rightAnchorX;
        input.anchorScrollPixelsY = m_pointerY - m_rightAnchorY;
        if (m_inputSettings.moveScrollAnchor)
        {
            const float maximumX = static_cast<float>(m_presentationWidth) * 0.5f;
            const float maximumY = static_cast<float>(m_presentationHeight) * 0.5f;
            input.anchorScrollPixelsX = std::clamp(input.anchorScrollPixelsX, -maximumX, maximumX);
            input.anchorScrollPixelsY = std::clamp(input.anchorScrollPixelsY, -maximumY, maximumY);
            m_rightAnchorX = m_pointerX - input.anchorScrollPixelsX;
            m_rightAnchorY = m_pointerY - input.anchorScrollPixelsY;
        }
    }
    input.zoomWheelUnits = m_pendingZoom;
    input.panPixelsX = m_pendingPanPixelsX;
    input.panPixelsY = m_pendingPanPixelsY;
    input.orbitPixelsX = m_pendingOrbitPixelsX;
    input.orbitPixelsY = m_pendingOrbitPixelsY;
    input.orbitPitchStepRadians = m_pendingPitchStepRadians;
    input.horizontalScrollSpeedFactor = m_cameraSettings.horizontalScrollSpeedFactor;
    input.verticalScrollSpeedFactor = m_cameraSettings.verticalScrollSpeedFactor;
    input.keyboardScrollSpeedFactor = m_cameraSettings.keyboardScrollSpeedFactor;
    input.keyboardRotateSpeed = m_cameraSettings.keyboardRotateSpeed;
    const float tacticalHeight =
        static_cast<float>(m_presentationHeight) * std::clamp(m_cameraSettings.viewportHeightScale, 0.1f, 1.0f);
    input.tacticalViewportAspectRatio =
        tacticalHeight > math::EPSILON ? static_cast<float>(m_presentationWidth) / tacticalHeight : 5.0f / 3.0f;
    input.resetToHome = m_pendingResetToHome;
    input.manualIntent = m_manualIntentSinceConsume;
    m_pendingZoom = 0.0f;
    m_pendingPanPixelsX = 0.0f;
    m_pendingPanPixelsY = 0.0f;
    m_pendingOrbitPixelsX = 0.0f;
    m_pendingOrbitPixelsY = 0.0f;
    m_pendingPitchStepRadians = 0.0f;
    m_pendingResetToHome = false;
    m_manualIntentSinceConsume = false;
    return input;
}

void GameCameraController::update(GameCameraState& camera, float deltaSeconds) noexcept
{
    // A stalled debugger/window must not turn a one-frame keyboard input into
    // a map-sized teleport; normal frame rates remain completely continuous.
    const float safeDelta =
        finite(deltaSeconds) && deltaSeconds > 0.0f ? (deltaSeconds < 0.100f ? deltaSeconds : 0.100f) : 0.0f;
    GameCameraManipulator::apply(camera, consumeInput(), safeDelta);
}

void GameCameraController::reset() noexcept
{
    m_panForward = false;
    m_panBackward = false;
    m_panLeft = false;
    m_panRight = false;
    m_pitchUp = false;
    m_pitchDown = false;
    m_rotateLeft = false;
    m_rotateRight = false;
    m_zoomIn = false;
    m_zoomOut = false;
    m_rightDrag = false;
    m_middleDrag = false;
    m_hasPointerPosition = false;
    m_pointerX = 0.0f;
    m_pointerY = 0.0f;
    m_rightAnchorX = 0.0f;
    m_rightAnchorY = 0.0f;
    m_middleAnchorX = 0.0f;
    m_middleAnchorY = 0.0f;
    m_middleDisplacementX = 0.0f;
    m_middleDisplacementY = 0.0f;
    m_middleDownTimestampMilliseconds = 0;
    m_pendingZoom = 0.0f;
    m_pendingPanPixelsX = 0.0f;
    m_pendingPanPixelsY = 0.0f;
    m_pendingOrbitPixelsX = 0.0f;
    m_pendingOrbitPixelsY = 0.0f;
    m_pendingPitchStepRadians = 0.0f;
    m_pendingResetToHome = false;
    m_manualIntentSinceConsume = false;
}

} // namespace engine
