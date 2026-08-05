#pragma once

#include "game/script/runtime/ScriptTypes.h"
#include "game/script/contracts/ScriptPresentationValueTypes.h"

namespace engine::script
{

enum class ScriptDebugMessageKind : uint8_t
{
    Dialog,
    Log,
    CrashBox,
};

// Retain authored diagnostics without blocking simulation on an OS modal or
// deliberately crashing a modern release build.
struct ScriptDebugMessageAction final
{
    container::String text;
    ScriptDebugMessageKind kind = ScriptDebugMessageKind::Log;
};

struct ScriptDisplayTextAction final
{
    container::String text;
    bool localized = true;
};

// DISPLAY_CINEMATIC_TEXT is not a normal message-log entry.  Preserve the
// authored font descriptor and logic-tick lifetime as an explicit
// presentation request; parsing/loading the font belongs to the UI side of
// the confirmed presentation boundary.
struct ScriptDisplayCinematicTextAction final
{
    container::String text;
    container::String fontDescriptor;
    uint32_t durationTicks = 0;
    bool localized = true;
};

// SHOW_MILITARY_CAPTION is the lower-left military typewriter, not the
// centered speech subtitle or DISPLAY_CINEMATIC_TEXT overlay. Its legacy
// duration is authored in milliseconds and must cross the confirmed boundary
// unchanged so the presentation consumer can continue while script time is
// frozen.
struct ScriptMilitaryCaptionAction final
{
    container::String text;
    uint32_t durationMilliseconds = 0;
    bool localized = true;
};

// MOVIE_PLAY_* contains only a Video.ini label and one of the two legacy
// output surfaces. Opening/decoding media is client presentation work, never
// an immutable ScriptProgram or ScriptRuntime responsibility.
struct ScriptMovieAction final
{
    ScriptMovieTarget target = ScriptMovieTarget::Fullscreen;
    container::String movieName;
};

// A named emitter is resolved only through ScriptWorldQuery at execution
// time.  The effect contains detached identity/position values, never a live
// object reference.
struct ScriptPlayAudioAction final
{
    container::String eventName;
    std::optional<math::vec3> position;
    // Waypoints belong to TerrainLogic, which is intentionally unavailable
    // to the immutable compiler/runtime.  Retain the authored name and let
    // the session bridge resolve it immediately before presentation.
    container::String waypointName;
    container::String emitterName;
    float volumeScale = 1.0f;
    bool uninterruptible = false;
    // SPEECH_PLAY also requests the localized military subtitle used by
    // InGameUI::militarySubtitle. This remains
    // a value-only label; presentation resolves it (and suppresses a missing
    // or '*' translation) just as RefCode's InGameUI did.
    container::String subtitleLabel;
    uint32_t subtitleDurationTicks = 0;
};

// MusicTrack changes are not ordinary AudioEvent playback.  RefCode first
// stops the current music stream (optionally fading it), then starts the new
// MusicTrack with its own optional fade-in.  Keep that replacement operation
// distinct from ScriptPlayAudioAction so the session/audio bridge cannot
// accidentally apply voice interruption or 3D-emitter policy to music.
enum class ScriptMusicCommand : uint8_t
{
    SetTrack,
    SetVolume,
};

struct ScriptMusicAction final
{
    ScriptMusicCommand command = ScriptMusicCommand::SetTrack;
    // Only SetTrack uses this.  It remains a MusicTrack name, rather than a
    // resolved file or AudioEvent pointer, until the presentation consumer
    // reaches AudioEventCatalog on the client.
    container::String trackName;
    bool fadeOut = false;
    bool fadeIn = false;
    // Only SetVolume uses this normalized [0, 1] bus gain.  The legacy wire
    // value is a percent and is clamped once by the compiler, exactly as
    // ScriptActions::doAudioSetVolume did before calling TheAudio.
    float volume = 1.0f;
};

// Ambient pause/resume is a global presentation policy, not a request to
// stop individual script sounds.  The value crosses the confirmed boundary
// explicitly so an audio backend can retain/resume its ambient voices using
// its own resources without ScriptRuntime holding any device state.
struct ScriptAmbientAudioAction final
{
    bool paused = false;
};

// SET_VISUAL_SPEED_MULTIPLIER is W3DView's direct m_timeMultiplier write.
// It is deliberately an INT rather than a normalized float: legacy maps use
// values above one as a presentation/fast-forward request, while zero and
// negative values retain the old "not a speed-up" behavior at the consumer.
struct ScriptVisualSpeedAction final
{
    int32_t multiplier = 1;
};

enum class ScriptCameraCommand : uint8_t
{
    SetPose,
    MoveTo,
    MoveAlongWaypointPath,
    Setup,
    Zoom,
    Pitch,
    Rotate,
    LookTowardWaypoint,
    LookTowardNamedObject,
    ModifyLookToward,
    ModifyFinalZoom,
    ModifyFinalPitch,
    ModifyFinalLookToward,
    // MOVE_CAMERA_TO_SELECTION only requests a local final-pivot modifier;
    // it never embeds selected objects or a resolved coordinate in the
    // immutable script action.
    MoveToSelection,
    FreezeAngle,
    FreezeTimeDuringMotion,
    ModifyFinalSpeedMultiplier,
    ModifyRollingAverage,
    Reset,
    SetDefault,
    FollowNamedObject,
    StopFollow,
    SetLetterbox,
    TetherNamedObject,
    StopTether,
};

struct ScriptCameraAction final
{
    ScriptCameraCommand command = ScriptCameraCommand::SetPose;
    math::vec3 position{};
    math::vec3 target{};
    // Authored waypoint identity crosses the immutable compiler/runtime
    // boundary unchanged. Terrain resolution belongs only to the session
    // bridge, just as it does for script-issued move orders.
    container::String waypointName;
    // SETUP_CAMERA keeps its look-at waypoint distinct from its pivot
    // waypoint. CAMERA_LOOK_TOWARD_* only uses waypointName/objectName.
    container::String lookAtWaypointName;
    container::String objectName;
    // Zoom, FX pitch and rotations are authored scalar values. Their meaning
    // is command-specific and intentionally stays typed by ScriptCameraCommand
    // instead of being smuggled through a fake camera pose.
    float value = 0.0f;
    float secondaryValue = 0.0f;
    float tertiaryValue = 0.0f;
    // CAMERA_MOD_SET_ROLLING_AVERAGE is authored as a signed legacy INT.
    // Preserve it as an integer so large values and the <=0 clamp reach the
    // camera boundary without a lossy float round trip.
    int32_t rollingAverageFrames = 1;
    // CAMERA_MOD_SET_FINAL_SPEED_MULTIPLIER is likewise authored as a legacy
    // signed INT.  Keep it distinct from the floating pose slots so a large
    // or negative value is never rounded through a float transport.
    int32_t visualSpeedMultiplier = 1;
    uint32_t durationTicks = 0;
    uint32_t easeInTicks = 0;
    uint32_t easeOutTicks = 0;
    uint32_t holdTicks = 0;
    bool reverseRotation = false;
    bool enabled = true;
};

// C&C3's 3DSMax camera playback does not move the tactical camera rig.  The
// W3D client resolves a named Object's animated render-bone transform once
// per presentation frame and uses that transform as the complete view pose.
// Keep it out of ScriptCameraAction so neither ScriptRuntime nor the session
// camera director can accidentally turn a renderer-local transform override
// into lockstep camera state.
struct ScriptCameraSlaveAction final
{
    // `objectName` is the legacy action's misleadingly named
    // thingTemplateName parameter. W3DView passes it to getUnitNamed(), so
    // it is an authored script-object name rather than a ThingTemplate.
    container::String objectName;
    container::String boneName;
    bool enabled = false;
};

// RefCode's View::CameraShakeType.  SCREEN_SHAKE is authored as this enum,
// but it is not a simulation camera transition: it becomes a stamped,
// presentation-only impulse after the runtime boundary.
enum class ScriptScreenShakeIntensity : uint8_t
{
    Subtle = 0,
    Normal,
    Strong,
    Severe,
    CineExtreme,
    CineInsane,
    Count,
};

struct ScriptScreenShakeAction final
{
    ScriptScreenShakeIntensity intensity = ScriptScreenShakeIntensity::Subtle;
};

// C&C3's CAMERA_ADD_SHAKER_AT is spatial and duration-based, unlike the
// discrete SCREEN_SHAKE preset. The waypoint is resolved only by the bridge;
// ScriptRuntime keeps it as an authored value and never sees TerrainLogic.
struct ScriptLocalizedCameraShakeAction final
{
    container::String waypointName;
    float amplitude = 0.0f;
    uint32_t durationTicks = 0;
    float radius = 0.0f;
};

// The four CAMERA_FADE_* opcodes all replace one global screen-fade slot in
// RefCode.  This is deliberately not a GameCameraDirector command: it is a
// tick-driven presentation blend that covers the tactical view after its
// object-icon post draw, while leaving the control bar outside the effect.
enum class ScriptScreenFadeBlendMode : uint8_t
{
    Add,
    Subtract,
    Saturate,
    Multiply,
    Count,
};

struct ScriptScreenFadeAction final
{
    ScriptScreenFadeBlendMode blendMode = ScriptScreenFadeBlendMode::Add;
    // RefCode exposes these as REAL values and does not clamp them in
    // ScriptEngine::setFade().  Preserve finite authored values verbatim;
    // only the renderer's UNORM presentation edge quantizes valid content.
    float minimumIntensity = 0.0f;
    float maximumIntensity = 0.0f;
    // These are raw legacy logic-frame counts, not durations in seconds.
    // Negative values have odd but defined control flow in the old state
    // machine, so do not normalize them at the compiler/program boundary.
    int32_t increaseFrames = 0;
    int32_t holdFrames = 0;
    int32_t decreaseFrames = 0;
};

// CAMERA_BW_MODE_BEGIN/END controls a renderer-owned post-world filter.  It
// is deliberately separate from ScriptCameraAction: RefCode never changes
// the tactical camera pose for this effect, and the filter advances on the
// renderer's own frame clock rather than confirmed simulation ticks.
//
// `enabled` is the requested command (Begin=true, End=false), not a claim
// about the filter currently visible on a particular client.  In particular,
// End must still cross the presentation boundary so a renderer can preserve
// RefCode's "only fade out if BW is still the active filter" rule after some
// other view effect has replaced it.  Keep the signed raw frame count: zero
// and negative values are meaningful to the legacy filter state machine.
struct ScriptBlackAndWhiteAction final
{
    bool enabled = false;
    int32_t transitionFrames = 0;
};

// The four legacy CAMERA_MOTION_BLUR actions select one renderer-owned view
// filter.  They are deliberately not ScriptCameraAction: the radial/pan
// pixels are presentation work, and View::isCameraMovementFinished() treats
// the zoom variants as already finished rather than waiting on a logic rig.
//
// CAMERA_MOTION_BLUR_JUMP retains only the authored waypoint name here.  The
// session bridge resolves it to a detached position immediately before the
// presentation command is published; neither ScriptRuntime nor a renderer
// ever receives TerrainLogic.
enum class ScriptMotionBlurMode : uint8_t
{
    ZoomIn,
    ZoomOut,
    ZoomJump,
    Follow,
    EndFollow,
    Count,
};

struct ScriptMotionBlurAction final
{
    ScriptMotionBlurMode mode = ScriptMotionBlurMode::ZoomIn;
    bool saturate = false;
    container::String waypointName;
    // CAMERA_MOTION_BLUR_FOLLOW passes this signed Int by adding it to the
    // legacy PAN filter enum.  Keep the raw source value until the renderer
    // applies the old zero/default-factor rule at its safe presentation edge.
    int32_t followAmount = 0;
};

// DRAW_SKYBOX_BEGIN/END only changes W3DWater's final draw flag.  Asset
// lookup, five-face material replacement, camera-XY follow and world-pass
// draw ordering are renderer responsibilities; the script runtime preserves
// no renderer handle or cached asset state.
struct ScriptSkyboxAction final
{
    bool enabled = false;
};

// SET_TREE_SWAY writes the map-wide BreezeInfo value.  Per-tree phase and
// random variation remain renderer/client-side; this action carries only the
// five authored script parameters across the immutable runtime boundary.
struct ScriptTreeSwayAction final
{
    float directionRadians = math::PI / 3.0f;
    float intensityRadians = 0.07f * math::PI / 4.0f;
    float leanRadians = 0.07f * math::PI / 4.0f;
    int32_t periodFrames = 150;
    float randomness = 0.2f;
};

// SHOW_WEATHER is SnowManager::setVisible, not a map weather-type mutation.
struct ScriptWeatherAction final
{
    bool visible = true;
};

// RefCode keeps this as a script-owned override rather than changing the
// map's normal lighting configuration.  An empty value is RESET: it restores
// the ordinary time-of-day infantry light scale.  Renderer resources and the
// `KindOf INFANTRY` classification remain outside ScriptRuntime.
struct ScriptInfantryLightingAction final
{
    std::optional<float> overrideScale;
};

enum class ScriptWaterCommand : uint8_t
{
    SetHeight,
    SetEnabled,
};

struct ScriptWaterAction final
{
    ScriptWaterCommand command = ScriptWaterCommand::SetHeight;
    container::String waterName;
    math::q32_32 value{};
    uint32_t transitionTicks = 0;
    // WATER_CHANGE_HEIGHT_OVER_TIME carries this authored value.  Terrain
    // keeps it with the active flood state even though its object-damage
    // consumer is a later gameplay pass.
    math::q32_32 damagePerSecond{};
    bool enabled = true;
};

// OBJECT_FORCE_SELECT is a local presentation request, not a simulated
// order.  Keep the authored Team/template selector detached here; the
// confirmed GameSession bridge resolves a stable ObjectId only after the
// runtime has emitted this value.  In particular, ScriptRuntime must never
// consult LocalSelectionState, an ECS entity, or a Drawable.
struct ScriptForceObjectSelectionAction final
{
    container::String teamName;
    container::String objectTypeName;
    bool centerInView = false;
    // DIALOG may be empty in legacy content.  Empty means selection/camera
    // only and is intentionally distinct from a malformed Team/template.
    container::String audioEventName;
};

} // namespace engine::script
