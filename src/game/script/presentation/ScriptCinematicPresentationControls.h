#pragma once

#include "core/container/container_types.h"

#include "core/ecs/ObjectId.h"
#include "core/math/wwmath/base/wwmath.h"

#include <cstdint>
#include <optional>
namespace engine::script {

// The enum is defined with the immutable ScriptProgram action types.  Keep an
// opaque declaration here so the session/presentation boundary can retain the
// authored category without pulling runtime implementation into UI/renderer
// consumers.
enum class ScriptScreenShakeIntensity : uint8_t;
enum class ScriptScreenFadeBlendMode : uint8_t;
enum class ScriptMotionBlurMode : uint8_t;

// Source-order metadata for a presentation-only script transition.  It is
// deliberately separate from simulation/replay state: consumers use it to
// preserve same-tick ordering and to derive client-local visual randomness
// without ever advancing SimulationRandom.
struct ScriptPresentationControlStamp final {
    // A GameSession start gives presentation a new epoch even when the same
    // session object is reused. Consumers must never replay a previous
    // match's impulse merely because its local sequence happened to match.
    uint64_t presentationEpoch = 0;
    uint64_t sequence = 0;
    uint64_t confirmedTick = 0;
    uint32_t sourceScriptId = 0;
    uint32_t ordinal = 0;
};

// Letterbox is a durable desired state.  A UI consumer may start its own
// wall-clock fade whenever `stamp.sequence` changes; duplicate Begin/End
// operations intentionally retain the same stamp, matching
// W3DDisplay::enableLetterBox's idempotence.
struct ScriptLetterboxPresentationState final {
    bool enabled = false;
    ScriptPresentationControlStamp stamp{};
};

// SCREEN_SHAKE is not a durable camera pose.  Each accepted action is an
// ordered impulse for the presentation-side shaker.  That consumer applies
// the legacy "new direction, then add intensity" rule serially, so multiple
// shakes in one confirmed tick do not become independent camera tracks.
struct ScriptScreenShakeImpulse final {
    ScriptPresentationControlStamp stamp{};
    ScriptScreenShakeIntensity intensity = static_cast<ScriptScreenShakeIntensity>(0);
};

// CAMERA_ADD_SHAKER_AT is a spatial, finite-duration camera disturbance.
// This remains detached from both GameCameraDirector and simulation: the
// renderer decides its visual offset from a sealed camera/map snapshot.
struct ScriptLocalizedCameraShakeImpulse final {
    ScriptPresentationControlStamp stamp{};
    math::vec3 position{};
    float amplitude = 0.0f;
    float radius = 0.0f;
    uint32_t durationTicks = 0;
};

// MOVE_CAMERA_TO_SELECTION does not serialize a selection set or a target
// coordinate. Legacy behavior resolves the current *local* selected
// Drawables at presentation time, then only modifies an already-running
// scripted camera path. This confirmed request therefore carries ordering
// metadata only; the local-selection consumer supplies the transient view
// state and must never feed it back into ScriptRuntime/lockstep.
struct ScriptMoveCameraToSelectionPresentation final {
    ScriptPresentationControlStamp stamp{};
};

// Confirmed scripts publish camera operations, but the presentation thread
// owns their elapsed clock.  Positions and waypoint paths are resolved to
// detached values at the session boundary so this journal never exposes
// TerrainLogic, ECS, or renderer objects.  `movementRevision` advances only
// for operations which participate in CAMERA_MOVEMENT_FINISHED; modifiers
// retain the currently active revision.
enum class ScriptCameraPresentationOperation : uint8_t {
    SetPose,
    MoveTo,
    MoveAlongPath,
    Setup,
    Zoom,
    Pitch,
    Rotate,
    LookToward,
    ModifyLookToward,
    ModifyFinalZoom,
    ModifyFinalPitch,
    ModifyFinalLookToward,
    ModifyFinalPivot,
    FreezeAngle,
    ModifyFinalSpeedMultiplier,
    ModifyRollingAverage,
    Reset,
    SetDefault,
    CancelMovement,
};

struct ScriptCameraPresentationCommand final {
    ScriptPresentationControlStamp stamp{};
    ScriptCameraPresentationOperation operation =
        ScriptCameraPresentationOperation::SetPose;
    uint64_t movementRevision = 0;
    math::vec3 position{};
    math::vec3 target{};
    container::Vector<math::vec3> path;
    float durationSeconds = 0.0f;
    float easeInSeconds = 0.0f;
    float easeOutSeconds = 0.0f;
    float value = 0.0f;
    float secondaryValue = 0.0f;
    float tertiaryValue = 0.0f;
    int32_t integerValue = 0;
    bool orientAlongMotion = true;
    bool reverseRotation = false;
};

struct ScriptCameraPresentationCompletion final {
    uint64_t presentationEpoch = 0;
    uint64_t movementRevision = 0;
    math::vec3 position{};
    math::vec3 target{};
    math::vec3 up{0.0f, 0.0f, 1.0f};
    float verticalFovRadians = 0.0f;
    float horizontalFovRadians = 0.0f;
    float tacticalViewportHeightScale = 1.0f;
    float nearClip = 1.0f;
    float farClip = 10000.0f;
    float visibilityDistance = 10000.0f;
    bool fogEnabled = false;
    math::vec3 fogColor{};
    float fogStartDistance = 0.0f;
    float fogEndDistance = 10000.0f;
    uint64_t cameraCutRevision = 0;
    int32_t visualSpeedMultiplier = 1;
};

// CAMERA_ENABLE_SLAVE_MODE is a durable renderer-local request, not a logic
// camera lock. The script/runtime boundary resolves the legacy named Object
// once to a stable ObjectId; a later renderer preparation step samples the
// exact animated W3D bone from the sealed entity snapshot. Keeping only this
// value state here prevents ScriptRuntime, GameSession and audio from ever
// holding a Drawable, skeleton or GPU object.
struct ScriptCameraSlavePresentationState final {
    bool enabled = false;
    ObjectId object = INVALID_OBJECT_ID;
    container::String boneName;
    ScriptPresentationControlStamp stamp{};
};

// OBJECT_FORCE_SELECT resolves a stable ObjectId at the confirmed script
// bridge, but the resulting selection, optional dialog and tactical-view
// move remain local presentation work.  The request therefore never carries
// an ECS entity, a Drawable, a local selection set, or a replay/lockstep
// command.  Consumers must still re-check that `object` has a live visual
// when the request reaches the main presentation thread.
struct ScriptForceObjectSelectionPresentation final {
    ScriptPresentationControlStamp stamp{};
    ObjectId object = INVALID_OBJECT_ID;
    // Captured at the confirmed bridge while the selected Object is live.
    // It retains ScriptActions::doForceObjectSelection's action-time camera
    // target without giving the journal an ECS Transform pointer.
    std::optional<math::vec3> position;
    bool centerInView = false;
    // An empty legacy DIALOG is valid and means "select without an audio
    // request".  The local consumer owns the eventual device-independent
    // GameAudio event.
    container::String audioEventName;
};

// CAMERA_FADE_* is one durable, replacement-style presentation slot.  Its
// time base is confirmed logic ticks, not wall clock: ScriptEngine advances
// the existing slot before evaluating scripts for a new tick, and an action
// issued later in that tick overwrites it.  The state retains raw authored
// parameters plus the current curve sample so render extraction never needs
// to read ScriptRuntime, GameCameraDirector, or a live script action.
struct ScriptScreenFadePresentationState final {
    bool active = false;
    ScriptScreenFadeBlendMode blendMode = static_cast<ScriptScreenFadeBlendMode>(0);
    float minimumIntensity = 0.0f;
    float maximumIntensity = 0.0f;
    float currentIntensity = 0.0f;
    int32_t increaseFrames = 0;
    int32_t holdFrames = 0;
    int32_t decreaseFrames = 0;
    // RefCode stores this in an Int.  A widened internal counter preserves
    // every normal map value while avoiding signed-overflow UB in a modern
    // session that is left running for a very long time.
    int64_t currentFrame = 0;
    ScriptPresentationControlStamp stamp{};
};

// One CAMERA_BW_MODE_BEGIN/END command for a renderer-local view filter,
// rather than a simulation value. GameSession retains a bounded, ordered
// journal of these states: Begin -> End in one confirmed frame must not be
// collapsed to a final false value. A Begin always starts/restarts its local
// fade; an End is deliberately retained even if a session cannot know whether
// a later motion-blur filter already replaced BW. That decision belongs to
// the renderer that owns the active filter.
struct ScriptBlackAndWhitePresentationState final {
    // `stamp.sequence == 0` is the epoch's initial "no command yet" value;
    // a renderer must not interpret the default enabled=false as an End.
    bool enabled = false;
    // Raw signed ScriptAction frame count. The source filter treats <= 0 as
    // immediate completion, so normalizing it here would change old maps.
    int32_t transitionFrames = 0;
    ScriptPresentationControlStamp stamp{};
};

// One CAMERA_MOTION_BLUR command.  The session retains a bounded ordered
// journal because newest-only render-frame delivery must not erase a Begin,
// a Jump target, or a later EndFollow.  `jumpTarget` is already a copied map
// coordinate; no TerrainLogic pointer crosses the presentation boundary.
struct ScriptMotionBlurPresentationState final {
    ScriptMotionBlurMode mode = static_cast<ScriptMotionBlurMode>(0);
    bool saturate = false;
    std::optional<math::vec3> jumpTarget;
    int32_t followAmount = 0;
    ScriptPresentationControlStamp stamp{};
};

// DRAW_SKYBOX_BEGIN/END is a durable final world-draw preference.  Unlike BW,
// duplicate writes do not restart a transition; a renderer reads this state
// from every sealed world snapshot and owns all asset/material lifetime.
struct ScriptSkyboxPresentationState final {
    bool enabled = false;
    ScriptPresentationControlStamp stamp{};
};

} // namespace engine::script
