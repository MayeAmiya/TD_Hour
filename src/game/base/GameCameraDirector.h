#pragma once

#include "core/container/container_types.h"

#include "presentation/camera/GameCameraInput.h"

#include <cstdint>
#include <optional>
namespace engine {

// The easing is explicit rather than an anonymous function so camera actions
// remain serializable/replayable when the future script system publishes them
// through confirmed commands.
enum class GameCameraEasing : uint8_t {
    Linear,
    SmoothStep,
    SmootherStep,
    // The legacy W3D scripted-camera curve is a parabolic acceleration /
    // deceleration profile with a linear middle, not SmoothStep.  Keep it
    // named so authored map ease-in/ease-out times remain visible at the
    // logic boundary instead of becoming an anonymous renderer behavior.
    LegacyParabolic,
};

// RefCode's View stops a scripted move for ordinary user actions unless it is
// explicitly locked.  Keep that policy visible in each request instead of
// hiding it in an input callback or renderer-side state machine.
enum class GameCameraInputPolicy : uint8_t {
    CancelOnManualInput,
    IgnoreManualInput,
    LockedUntilFinished,
};

enum class GameCameraTransitionStatus : uint8_t {
    Idle,
    Running,
    Finished,
    Cancelled,
};

struct GameCameraTransitionRequest final {
    GameCameraState destination;
    float durationSeconds = 0.0f;
    GameCameraEasing easing = GameCameraEasing::SmoothStep;
    GameCameraInputPolicy inputPolicy = GameCameraInputPolicy::CancelOnManualInput;
};

// Timing authored by the legacy ScriptAction camera operations.  It is kept
// separate from GameCameraTransitionRequest because a legacy camera does not
// interpolate one opaque pose: pivot, yaw, zoom and FX pitch are independent
// tracks which may overlap in the same confirmed frame.
struct GameCameraScriptTiming final {
    float durationSeconds = 0.0f;
    float easeInSeconds = 0.0f;
    float easeOutSeconds = 0.0f;
};

// Value-only geometry used by the legacy ZH scripted camera. W3D keeps the
// map's initial ground level separate from the moving camera pivot: authored
// zoom is measured against initialGround + CameraHeight, while maximum zoom
// is resolved at the movement endpoint from ground + MaxCameraHeight.
struct GameCameraScriptGeometry final {
    float cameraHeight = 232.0f;
    float maximumHeightAboveGround = 310.0f;
    float initialGroundLevel = 0.0f;
    float defaultPitchRadians = math::deg_to_rad(37.5f);
};

// Presentation-owned scripted camera state. It has no renderer/ECS pointer
// and no knowledge of SDL; the main presentation clock supplies elapsed time.
// Confirmed scripts publish value commands and receive only a stamped
// completion acknowledgement, so camera motion never becomes simulation
// state or stalls behind a delayed logic tick.
class GameCameraDirector final {
public:
    void configureScriptGeometry(GameCameraScriptGeometry geometry) noexcept;
    void reset(GameCameraState camera = {}) noexcept;

    [[nodiscard]] const GameCameraState& camera() const noexcept { return m_camera; }
    [[nodiscard]] uint64_t cameraCutRevision() const noexcept {
        return m_cameraCutRevision;
    }
    // The legacy tactical view calls this m_timeMultiplier.  Keep the raw
    // signed authored value observable here; GameLogic's pacing policy turns
    // only values greater than one into bounded extra single-player frames.
    [[nodiscard]] int32_t visualSpeedMultiplier() const noexcept {
        return m_visualSpeedMultiplier;
    }
    void setVisualSpeedMultiplier(int32_t multiplier) noexcept;
    [[nodiscard]] GameCameraTransitionStatus transitionStatus() const noexcept {
        return m_status;
    }
    [[nodiscard]] bool isTransitionActive() const noexcept {
        return m_status == GameCameraTransitionStatus::Running || hasScriptTracksActive();
    }
    // CAMERA_MOD_FREEZE_TIME is tied to the legacy Scripted_* camera tracks,
    // not to a modern caller's arbitrary full-pose transition.  Keep this
    // narrow query public so GameSession can make that distinction without
    // exposing track storage or renderer state.
    [[nodiscard]] bool isScriptCameraMovementActive() const noexcept {
        return hasScriptTracksActive();
    }
    [[nodiscard]] bool isTransitionFinished() const noexcept {
        return m_status == GameCameraTransitionStatus::Finished;
    }
    [[nodiscard]] bool acceptsManualInput() const noexcept;
    [[nodiscard]] bool isUserControlLocked() const noexcept {
        return isTransitionActive() &&
               m_active.inputPolicy == GameCameraInputPolicy::LockedUntilFinished;
    }

    // Starts from the director's current pose. A non-positive or malformed
    // duration commits the destination immediately and reports Finished.
    void startTransition(GameCameraTransitionRequest request) noexcept;
    bool cancelTransition() noexcept;
    // Replaces the pose immediately. It is intentionally explicit because
    // scripts should normally use a zero-duration transition for an authored
    // camera cut, while setup code can use setCamera during session startup.
    void setCamera(GameCameraState camera) noexcept;
    // Accepts the final pose produced by the presentation-owned script clock
    // together with that clock's cut generation. This is the authority
    // handoff counterpart of setCamera(); startup callers must not use it.
    void settlePresentationCamera(GameCameraState camera) noexcept;

    // Called by the presentation clock. It owns transition completion while
    // the script runtime observes only the corresponding revision receipt.
    void update(float fixedDeltaSeconds) noexcept;
    // Applies/cancels local input according to the active request. Returns
    // true iff the input changed durable camera state or cancelled a move.
    bool applyManualInput(const GameCameraInput& input, float fixedDeltaSeconds) noexcept;

    // Legacy ScriptAction-compatible camera tracks.  These are all logic
    // owned and deliberately operate on a compact rig representation rather
    // than a renderer camera.  Starting one track never silently cancels an
    // unrelated pivot/zoom/FX-pitch track; a new request for the *same*
    // property replaces only that property, matching W3DView's independent
    // Scripted_Move/Rotate/Zoom/Pitch states.
    void scriptMoveTo(math::vec3 pivot, GameCameraScriptTiming timing,
                      bool orientAlongMotion = true) noexcept;
    // The map terrain layer resolves the authored waypoint links before this
    // call. The director owns only a value path, so it remains renderer- and
    // TerrainLogic-free while preserving W3D's linked-waypoint movement.
    void scriptMoveAlongPath(container::Span<const math::vec3> pivots,
                             GameCameraScriptTiming timing,
                             bool orientAlongMotion = true) noexcept;
    // CAMERA_MOD_LOOK_TOWARD modifies an in-flight MoveTo/path without
    // creating a standalone rotation when no move exists (the RefCode
    // contract). Returns false for that intentional no-op case.
    [[nodiscard]] bool scriptSetCurrentMoveLookToward(math::vec3 target) noexcept;
    // CAMERA_MOD_SET_ROLLING_AVERAGE applies only to the active legacy
    // MoveTo/waypoint-path orientation.  RefCode clamps an authored value
    // below one to one; the returned false is the otherwise invisible
    // no-active-move case rather than a synthetic camera transition.
    [[nodiscard]] bool scriptSetCurrentMoveRollingAverage(int32_t framesToAverage) noexcept;
    // These four modifiers deliberately require an already-active legacy
    // rotate/move track.  They return false for the RefCode no-op case rather
    // than manufacturing a standalone camera transition.
    [[nodiscard]] bool scriptSetFinalZoom(float normalizedZoom, float easeInFraction,
                                          float easeOutFraction) noexcept;
    [[nodiscard]] bool scriptSetFinalPitch(float fxPitch, float easeInFraction,
                                           float easeOutFraction) noexcept;
    [[nodiscard]] bool scriptSetCurrentMoveFinalLookToward(math::vec3 target) noexcept;
    // MOVE_CAMERA_TO_SELECTION is a modifier, not a new camera transition.
    // It retargets only the XY endpoint(s) of an already active legacy
    // MoveTo/waypoint path. The caller has already projected local selection
    // to a value-only centroid; no selection, ECS, UI or renderer state is
    // retained by the director.
    [[nodiscard]] bool scriptSetCurrentMoveFinalPivot(math::vec3 target) noexcept;
    [[nodiscard]] bool scriptFreezeCurrentMoveAngle() noexcept;
    // RefCode updates Zoom, Pitch and Rotate terminal multipliers
    // independently, then updates an active waypoint movement if there is no
    // rotate.  With no applicable legacy track it writes the view multiplier
    // immediately. The bool reports whether a track absorbed the modifier.
    [[nodiscard]] bool scriptSetFinalVisualSpeedMultiplier(int32_t multiplier) noexcept;
    void scriptZoomTo(float normalizedZoom, GameCameraScriptTiming timing) noexcept;
    void scriptPitchTo(float fxPitch, GameCameraScriptTiming timing) noexcept;
    void scriptRotateBy(float rotations, GameCameraScriptTiming timing) noexcept;
    void scriptRotateToward(math::vec3 target, GameCameraScriptTiming timing,
                            bool reverseRotation = false) noexcept;
    // CAMERA_LOOK_TOWARD_OBJECT keeps the scripted-camera state active for
    // this authored hold interval after the yaw arrives. It deliberately has
    // no pose value of its own.
    void scriptHold(float durationSeconds) noexcept;
    // SETUP_CAMERA is an authored cut: it immediately assigns all four rig
    // properties and clears any preceding scripted tracks.
    // `confirmationTiming` is supplied by the legacy bridge. W3D clamps a
    // zero-millisecond setup move to one frame, so its pose can be immediate
    // while CAMERA_MOVEMENT_FINISHED remains false for that confirmation tick.
    void scriptSetup(math::vec3 pivot, float normalizedZoom, float fxPitch,
                     math::vec3 lookAt,
                     GameCameraScriptTiming confirmationTiming = {}) noexcept;
    // RESET_CAMERA moves to an authored pivot while restoring the defaults
    // captured from the session's map-framed camera.
    void scriptReset(math::vec3 pivot, GameCameraScriptTiming timing) noexcept;
    // CAMERA_SET_DEFAULT changes the durable default used by RESET_CAMERA.
    // RefCode's W3D implementation intentionally ignores `angle`; it is
    // retained in this typed API for complete authored-parameter fidelity.
    void scriptSetDefaults(float pitchRadians, float ignoredAngleRadians,
                           float maxHeightScale) noexcept;
    // Continuous CAMERA_FOLLOW_NAMED updates arrive from GameSession after it
    // resolves an ObjectId to a current ECS transform. Follow does not count
    // as a scripted move for CAMERA_MOVEMENT_FINISHED, matching W3DView's
    // CameraLock state.
    void scriptFollowPivot(math::vec3 pivot, bool snap, float fixedDeltaSeconds) noexcept;
    // CAMERA_TETHER_NAMED shares W3DView's CameraLock ObjectId boundary with
    // Follow, but its local catch-up curve is deliberately different: it
    // preserves orientation and uses the authored `play` factor inside the
    // legacy partition-cell tolerance.
    void scriptTetherPivot(math::vec3 pivot, bool snap, float play,
                           float partitionCellSize = 100.0f) noexcept;
    void scriptStopFollowing() noexcept;
    [[nodiscard]] bool hasScriptTracksActive() const noexcept;

private:
    struct ActiveTransition final {
        GameCameraState start;
        GameCameraState destination;
        float durationSeconds = 0.0f;
        float elapsedSeconds = 0.0f;
        GameCameraEasing easing = GameCameraEasing::SmoothStep;
        GameCameraInputPolicy inputPolicy = GameCameraInputPolicy::CancelOnManualInput;
    };

    struct ScriptScalarTrack final {
        bool active = false;
        float start = 0.0f;
        float destination = 0.0f;
        GameCameraScriptTiming timing{};
        float elapsedSeconds = 0.0f;
        // Used only by legacy Zoom/Pitch/Rotate tracks. It is intentionally
        // stored beside each independently advancing property instead of a
        // global target, matching W3DView's separate end fields.
        int32_t startVisualSpeedMultiplier = 1;
        int32_t endVisualSpeedMultiplier = 1;
        bool drivesVisualSpeedMultiplier = false;
    };

    struct ScriptPivotTrack final {
        bool active = false;
        math::vec3 start{};
        math::vec3 destination{};
        GameCameraScriptTiming timing{};
        float elapsedSeconds = 0.0f;
        // W3D's m_mcwpInfo resets this to one for every new movement. Keep
        // it on the movement track, never as a sticky camera-wide setting.
        int32_t rollingAverageFrames = 1;
        int32_t startVisualSpeedMultiplier = 1;
        int32_t endVisualSpeedMultiplier = 1;
    };

    struct ScriptPathTrack final {
        bool active = false;
        container::Vector<math::vec3> pivots;
        // Cumulative world-space distance at each pivot; element zero is
        // always zero and the last element is totalDistance.
        container::Vector<float> cumulativeDistances;
        // W3DView retains an integer multiplier at every waypoint. This is
        // not reducible to one global start/end pair when a script applies
        // multiple final-speed modifiers while a multi-segment path is live.
        container::Vector<int32_t> visualSpeedMultipliers;
        GameCameraScriptTiming timing{};
        float elapsedSeconds = 0.0f;
        float totalDistance = 0.0f;
        int32_t rollingAverageFrames = 1;
        int32_t startVisualSpeedMultiplier = 1;
        int32_t endVisualSpeedMultiplier = 1;
        bool orientAlongMotion = true;
        std::optional<math::vec3> lookToward;
        std::optional<math::vec3> finalLookToward;
    };

    // This mirrors the durable part of W3DView's scripted state while using
    // modern pose extraction: pivot is the map-space focus, yaw is the eye's
    // radial angle around it, zoom scales the captured baseline distance, and
    // fxPitch changes the vertical look-at relation without mutating the
    // user/manual orbital pitch.
    struct ScriptRigState final {
        math::vec3 pivot{};
        float yawRadians = 0.0f;
        float orbitalElevationRadians = 0.0f;
        float baselineDistance = 1.0f;
        float normalizedZoom = 1.0f;
        float fxPitch = 1.0f;
    };

    // W3D has an explicit Scripted_Rotate / Scripted_MoveOnWaypointPath
    // state bit, independent of overlapping zoom and pitch tracks. Preserve
    // that distinction so camera modifiers and CAMERA_MOVEMENT_FINISHED use
    // the original activation rules rather than guessing from every scalar
    // animation that happens to be active.
    enum class ScriptMotionKind : uint8_t {
        None,
        Move,
        Rotate,
    };

    GameCameraState m_camera;
    int32_t m_visualSpeedMultiplier = 1;
    ActiveTransition m_active;
    ScriptRigState m_scriptRig;
    ScriptRigState m_scriptDefaults;
    ScriptRigState m_scriptMapDefaults;
    ScriptPivotTrack m_scriptPivotTrack;
    ScriptPathTrack m_scriptPathTrack;
    ScriptScalarTrack m_scriptYawTrack;
    ScriptScalarTrack m_scriptZoomTrack;
    ScriptScalarTrack m_scriptPitchTrack;
    ScriptScalarTrack m_scriptBaselineTrack;
    ScriptScalarTrack m_scriptElevationTrack;
    ScriptScalarTrack m_scriptHoldTrack;
    bool m_scriptRigValid = false;
    bool m_scriptDefaultsValid = false;
    bool m_scriptMapDefaultsValid = false;
    float m_scriptFollowBlend = 0.0f;
    ScriptMotionKind m_scriptMotionKind = ScriptMotionKind::None;
    GameCameraTransitionStatus m_status = GameCameraTransitionStatus::Idle;
    uint64_t m_cameraCutRevision = 1;
    GameCameraScriptGeometry m_scriptGeometry;
    float m_scriptConfiguredMaximumHeight = 310.0f;

    [[nodiscard]] static float easedProgress(GameCameraEasing easing, float progress) noexcept;
    [[nodiscard]] static GameCameraState interpolate(const ActiveTransition& transition,
                                                     float progress) noexcept;
    [[nodiscard]] static float legacyParabolicProgress(float progress,
                                                        GameCameraScriptTiming timing) noexcept;
    [[nodiscard]] static float finiteOr(float value, float fallback) noexcept;
    [[nodiscard]] static float normalizedAngle(float radians) noexcept;
    [[nodiscard]] static float yawToward(math::vec3 pivot, math::vec3 target) noexcept;
    [[nodiscard]] float maximumScriptZoomAt(math::vec3 pivot) const noexcept;
    [[nodiscard]] math::vec3 scriptZoomEndpoint() const noexcept;
    [[nodiscard]] static int32_t interpolateVisualSpeedMultiplier(
        int32_t start, int32_t end, float factor) noexcept;
    static void startScriptTrack(ScriptScalarTrack& track, float start, float destination,
                                 GameCameraScriptTiming timing) noexcept;
    static void startScriptTrack(ScriptPivotTrack& track, math::vec3 start,
                                 math::vec3 destination, GameCameraScriptTiming timing) noexcept;
    [[nodiscard]] static bool advanceScriptTrack(ScriptScalarTrack& track,
                                                  float fixedDeltaSeconds, float& result) noexcept;
    [[nodiscard]] static bool advanceScriptTrack(ScriptPivotTrack& track,
                                                  float fixedDeltaSeconds, math::vec3& result) noexcept;
    void seedVisualSpeedTrack(ScriptScalarTrack& track) noexcept;
    void seedVisualSpeedTrack(ScriptPivotTrack& track) noexcept;
    void seedVisualSpeedTrack(ScriptPathTrack& track) noexcept;
    void sampleVisualSpeedTrack(const ScriptScalarTrack& track,
                                float fixedDeltaSeconds) noexcept;
    void sampleVisualSpeedTrack(const ScriptPivotTrack& track,
                                float fixedDeltaSeconds) noexcept;
    void sampleVisualSpeedTrack(const ScriptPathTrack& track,
                                float fixedDeltaSeconds) noexcept;
    [[nodiscard]] std::optional<GameCameraScriptTiming>
    activeScriptMotionTiming() const noexcept;
    void updateScriptPathTrack(float fixedDeltaSeconds) noexcept;
    void clearScriptTracks() noexcept;
    void ensureScriptRig() noexcept;
    void synchronizeScriptRigFromCamera(bool captureDefaults) noexcept;
    void applyScriptRigToCamera() noexcept;
    void updateScriptTracks(float fixedDeltaSeconds) noexcept;
    void beginScriptCameraOperation() noexcept;
    void advanceCameraCut() noexcept;
};

} // namespace engine
