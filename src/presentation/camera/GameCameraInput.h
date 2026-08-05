#pragma once

#include "presentation/camera/GameCameraState.h"

namespace engine
{

// A frame-local, value-only expression of local RTS camera input.  It is
// intentionally separate from SDL and from the durable camera state so the
// session/director can arbitrate it against a scripted camera on a confirmed
// game tick.  The pan fields are normalized held-state axes.  GameSession
// applies them for exactly its fixed delta, so a network wait cannot turn a
// long sequence of presentation frames into a camera teleport.
struct GameCameraInput final
{
    float panForwardAxis = 0.0f;
    float panRightAxis = 0.0f;
    // Screen-edge scrolling is sampled separately from keyboard state.  The
    // original applies the same authored H/V factors to both, but keeping the
    // sources distinct prevents a cursor entering an edge from manufacturing
    // a held key in replay/debug input.
    float screenEdgeForwardAxis = 0.0f;
    float screenEdgeRightAxis = 0.0f;
    float zoomWheelUnits = 0.0f;
    // Persistent RMB anchor displacement.  Unlike panPixels, this is a held
    // value: it is re-applied on every confirmed tick until button-up, even
    // when SDL emits no further mouse-motion event.
    float anchorScrollPixelsX = 0.0f;
    float anchorScrollPixelsY = 0.0f;
    float panPixelsX = 0.0f;
    float panPixelsY = 0.0f;
    float orbitPixelsX = 0.0f;
    float orbitPixelsY = 0.0f;
    // CommandMap keypad camera controls are held actions.  Keep them as
    // axes, just like keyboard pan/pitch, so repeated presentation frames do
    // not accumulate an unbounded pixel delta before the next logic tick.
    float orbitYawAxis = 0.0f;
    float zoomAxis = 0.0f;
    // Discrete keyboard/debug pitch step, expressed directly in radians so
    // one key press is independent of render rate and mouse sensitivity.
    float orbitPitchStepRadians = 0.0f;
    // Held local pitch input. PageUp/PageDown publish an axis rather than
    // repeated pixel deltas, so render-frame rate cannot change its speed.
    float orbitPitchAxis = 0.0f;
    float horizontalScrollSpeedFactor = 1.6f;
    float verticalScrollSpeedFactor = 2.0f;
    float keyboardScrollSpeedFactor = 0.5f;
    float keyboardRotateSpeed = 0.1f;
    float tacticalViewportAspectRatio = 5.0f / 3.0f;
    // Radar userLookAt is an absolute local camera intent, not a replicated
    // unit command. Keeping it on this fixed-tick value path lets the camera
    // director arbitrate it against locked/cancel-on-input script moves.
    math::vec3 absoluteTarget{};
    bool hasAbsoluteTarget = false;
    GameCameraState absoluteState{};
    bool hasAbsoluteState = false;
    bool resetToHome = false;

    // Remains true for a key/mouse action which happened to produce no
    // spatial delta this frame.  A CancelOnManualInput transition must still
    // be cancelled in that situation, matching the original View behavior.
    bool manualIntent = false;

    [[nodiscard]] bool hasManualInput() const noexcept;
    void clear() noexcept;
    void accumulate(const GameCameraInput& input) noexcept;
};

// Stateless camera-rig operations shared by the SDL controller and the
// script camera director.  Keeping these operations outside the renderer
// makes the coordinate convention testable without a GPU device.
class GameCameraManipulator final
{
public:
    static void apply(GameCameraState& camera, const GameCameraInput& input, float fixedDeltaSeconds) noexcept;
};

} // namespace engine
