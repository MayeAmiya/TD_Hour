#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "core/math/fixed/q32_32.h"
#include "game/player/PlayerTypes.h"
#include "core/math/wwmath/base/wwmath.h"

#include <cstdint>
#include <optional>

namespace engine::script {

// ScriptProgram/ScriptRuntime 与本地表现状态之间共享的纯值协议。
// 这里只允许枚举、标量、字符串、定点值和稳定 ID；不得加入状态类、
// renderer/audio/UI 句柄或会话回调。
enum class ScriptAudioControlCommand : uint8_t {
    SetBackgroundSoundsPaused,
    SetSoundVolume,
    SetSpeechVolume,
    SetEventVolumeOverride,
    RestoreEventVolumeOverride,
    RestoreAllEventVolumeOverrides,
    RemoveEvent,
    RemoveDisabledEvents,
    SetEvaEnabled,
};

struct ScriptAudioControlAction final {
    ScriptAudioControlCommand command = ScriptAudioControlCommand::SetBackgroundSoundsPaused;
    // Required only by per-AudioEvent commands. It remains an authored event
    // key until AudioSubsystem resolves the catalog; never store a VFS path.
    container::String eventName;
    // Normalized linear gain for SetSoundVolume, SetSpeechVolume and
    // SetEventVolumeOverride. The compiler performs legacy percent mapping.
    float volume = 1.0f;
    // Used by pause/EVA commands. For SetBackgroundSoundsPaused, true means
    // the legacy Sound lane is enabled (that is, not paused); for
    // SetEvaEnabled it is the direct EVA enabled value. Keeping this in the
    // same value type avoids expanding ScriptEffect for zero-parameter
    // legacy toggles.
    bool enabled = true;
};

struct ScriptAudioControlEffect final {
    ScriptAudioControlCommand command = ScriptAudioControlCommand::SetBackgroundSoundsPaused;
    container::String eventName;
    float volume = 1.0f;
    bool enabled = true;
};

enum class ScriptClientOptionCommand : uint8_t {
    SetFrameRateLimit,
    SetOcclusionMode,
    SetDrawIconUiMode,
    SetDynamicParticleLodMode,
};

struct ScriptClientOptionsAction final {
    ScriptClientOptionCommand command = ScriptClientOptionCommand::SetFrameRateLimit;
    int32_t frameRateLimit = 0;
    bool enabled = true;
};

struct ScriptClientOptionsEffect final {
    ScriptClientOptionCommand command = ScriptClientOptionCommand::SetFrameRateLimit;
    int32_t frameRateLimit = 0;
    bool enabled = true;
};

enum class ScriptMapPresentationCommand : uint8_t {
    CreateRadarEvent,
    RefreshRadar,
    SetBorderShroud,
    SetRadarHidden,
    SetRadarForced,
    SetBoundary,
    RevealAtWaypoint,
    ShroudAtWaypoint,
    RevealAll,
    RevealAllPermanently,
    UndoRevealAllPermanently,
    ShroudAll,
    RevealPermanentlyAtWaypoint,
    UndoRevealPermanentlyAtWaypoint,
};

struct ScriptMapPresentationAction final {
    ScriptMapPresentationCommand command = ScriptMapPresentationCommand::SetRadarHidden;
    bool enabled = false;
    math::vec3 position{};
    int32_t radarEventType = 0;
    int32_t boundaryIndex = 0;
    // Visibility actions stay name-based until the session bridge owns both
    // TerrainLogic waypoint lookup and the authoritative Player registry.
    // An empty player is meaningful for the four non-permanent legacy map
    // commands: it selects every human player.
    container::String waypointName;
    container::String playerName;
    container::String revealName;
    math::q32_32 radius{};
};

struct ScriptMapPresentationEffect final {
    ScriptMapPresentationCommand command = ScriptMapPresentationCommand::SetRadarHidden;
    bool enabled = false;
    math::vec3 position{};
    int32_t radarEventType = 0;
    int32_t boundaryIndex = 0;
    container::String waypointName;
    container::String playerName;
    container::String revealName;
    math::q32_32 radius{};
};

enum class ScriptMovieTarget : uint8_t {
    Fullscreen,
    Radar,
    Count,
};

enum class ScriptObjectPresentationTargetKind : uint8_t {
    NamedObject,
    Team,
};

struct ScriptObjectPresentationTarget final {
    ScriptObjectPresentationTargetKind kind = ScriptObjectPresentationTargetKind::NamedObject;
    container::String name;
};

// This family is for presentation actions whose authored target is a live
// named Object or Team.  The runtime resolves only the detached facts needed
// by the command; bridge/session code remains the sole owner of the live
// ObjectId-to-ECS and Team-membership projections.
enum class ScriptObjectPresentationCommand : uint8_t {
    CreateRadarEvent,
    Flash,
    SetCustomIndicatorColor,
    // TEAM_SET_EMOTICON / NAMED_SET_EMOTICON own one transient Anim2D-like
    // object-icon slot per target.  The animation asset itself remains a
    // renderer/UI concern; this command carries only its legacy template
    // name and lifetime across the script boundary.
    SetEmoticon,
    // ENABLE_OBJECT_SOUND / DISABLE_OBJECT_SOUND affect the Drawable-owned
    // ambient event.  The command carries no device handle: GameSession
    // resolves the frozen object audio name and publishes a value-only
    // ObjectId control to the presentation audio subsystem.
    SetAmbientSoundEnabled,
    // NAMED_HIDE/SHOW_SPECIAL_POWER_DISPLAY modifies only this Object's
    // superweapon/countdown entry, independently of the global display gate.
    SetSpecialPowerDisplayVisible,
};

// The normal variants use the target's current indicator colour. The white
// variants are deliberately separate instead of encoding white as a magic
// custom colour: NAMED_CUSTOM_COLOR is durable, whereas a flash colour is a
// one-shot value captured at the script effect boundary.
enum class ScriptObjectPresentationFlashColor : uint8_t {
    Indicator,
    White,
};

struct ScriptObjectPresentationAction final {
    ScriptObjectPresentationCommand command = ScriptObjectPresentationCommand::CreateRadarEvent;
    ScriptObjectPresentationTarget target;
    // Kept as the raw legacy RadarEventType integer.  RefCode casts it at
    // dispatch and does not reject unfamiliar mod-defined values here.
    int32_t radarEventType = 0;
    // NAMED_FLASH / TEAM_FLASH author whole seconds. ScriptRuntime keeps this
    // raw signed value; GameSession converts it using the match's frozen
    // logic FPS exactly once before publishing presentation state.
    int32_t durationSeconds = 0;
    ScriptObjectPresentationFlashColor flashColor =
        ScriptObjectPresentationFlashColor::Indicator;
    // RefCode's COLOR parameter is a packed legacy Color/ARGB integer.
    // Preserve all bits through the compiler/runtime boundary; the session
    // decodes the RGB lanes for its renderer-owned indicator overlay.
    uint32_t packedColor = 0;
    // Emoticon actions author a REAL number of seconds (unlike the integer
    // flash duration).  GameSession performs the legacy `(Int)(seconds *
    // LOGICFRAMES_PER_SECOND)` conversion once against its frozen tick rate.
    float emoticonDurationSeconds = 0.0f;
    container::String emoticonName;
    // ENABLE_OBJECT_SOUND deliberately restarts the currently selected
    // ambient event even when it was already enabled; the presentation audio
    // consumer therefore must not coalesce repeated true commands.
    bool ambientSoundEnabled = false;
    bool specialPowerDisplayVisible = true;
};

// A confirmed value-only result.  `object` is populated for a named-object
// target when it is still live; Team targets intentionally retain only their
// selector and estimated position.  This lets a later command such as a
// team flash expand membership at the bridge without ScriptRuntime holding a
// second Team index or an ECS container.
struct ScriptObjectPresentationEffect final {
    ScriptObjectPresentationCommand command = ScriptObjectPresentationCommand::CreateRadarEvent;
    ScriptObjectPresentationTarget target;
    std::optional<ObjectId> object;
    ObjectTeamId team = INVALID_OBJECT_TEAM_ID;
    std::optional<math::vec3> position;
    int32_t radarEventType = 0;
    int32_t durationSeconds = 0;
    ScriptObjectPresentationFlashColor flashColor =
        ScriptObjectPresentationFlashColor::Indicator;
    uint32_t packedColor = 0;
    float emoticonDurationSeconds = 0.0f;
    container::String emoticonName;
    bool ambientSoundEnabled = false;
    bool specialPowerDisplayVisible = true;
};

enum class ScriptPresentationCompletionKind : uint8_t {
    Video,
    Speech,
    Audio,
    MusicLoop,
};

enum class ScriptUiControlKind : uint8_t {
    GameplayInput,
    SpecialPowerDisplay,
    NamedTimerDisplay,
};

// ScriptProgram carries one of these value types while ScriptRuntime is
// evaluating a confirmed tick.  The payload intentionally names a UI policy,
// not a WndRuntime widget: game scripts must remain independent from local
// layout resources and SDL input state.
enum class ScriptUiCommand : uint8_t {
    SetControl,
    ShowNamedIndicator,
    HideNamedIndicator,
    ShowPopup,
    ShowLocalDefeat,
    FlashCameo,
};

// RefCode stores both named counters and countdown timers in one ordered map;
// the difference is only their display formatting.  Retain that distinction
// explicitly so a later HUD implementation does not infer it from a mutable
// ScriptRuntime counter flag.
enum class ScriptNamedIndicatorKind : uint8_t {
    Counter,
    Countdown,
};

struct ScriptUiAction final {
    ScriptUiCommand command = ScriptUiCommand::SetControl;
    ScriptUiControlKind control = ScriptUiControlKind::GameplayInput;
    ScriptNamedIndicatorKind indicatorKind = ScriptNamedIndicatorKind::Counter;
    bool enabled = true;
    container::String name;
    container::String text;
    int32_t xPercent = 0;
    int32_t yPercent = 0;
    int32_t width = 50;
    bool pauseRequested = false;
    uint32_t flashCount = 0;
    uint32_t framesPerFlash = 1;
};

struct ScriptUiEffect final {
    ScriptUiCommand command = ScriptUiCommand::SetControl;
    ScriptUiControlKind control = ScriptUiControlKind::GameplayInput;
    ScriptNamedIndicatorKind indicatorKind = ScriptNamedIndicatorKind::Counter;
    bool enabled = true;
    container::String name;
    container::String text;
    int32_t xPercent = 0;
    int32_t yPercent = 0;
    int32_t width = 50;
    bool pauseRequested = false;
    uint32_t flashCount = 0;
    uint32_t framesPerFlash = 1;
};

enum class ScriptViewCompatibilityCommand : uint8_t {
    SetTerrainOversizeTiles,
    SetGuardBandBias,
};

struct ScriptViewCompatibilityAction final {
    ScriptViewCompatibilityCommand command =
        ScriptViewCompatibilityCommand::SetTerrainOversizeTiles;
    // OVERSIZE_TERRAIN's raw legacy INT. It is intentionally signed: the
    // original W3D terrain object received it unchanged and a modern consumer
    // must not turn malformed content into an allocation request.
    int32_t terrainOversizeTiles = 0;
    // RESIZE_VIEW_GUARDBAND's world-space x/y bias. The old View stores these
    // verbatim beside its drawable overscan rectangle.
    float guardBandX = 0.0f;
    float guardBandY = 0.0f;
};

struct ScriptViewCompatibilityEffect final {
    ScriptViewCompatibilityCommand command =
        ScriptViewCompatibilityCommand::SetTerrainOversizeTiles;
    int32_t terrainOversizeTiles = 0;
    float guardBandX = 0.0f;
    float guardBandY = 0.0f;
};

} // namespace engine::script
