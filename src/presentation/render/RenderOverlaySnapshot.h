#pragma once

#include "presentation/render/RenderWorldDescriptorContracts.h"

namespace engine::render {

enum class ScreenFadeBlendMode : uint8_t {
    None,
    Add,
    Subtract,
    Saturate,
    Multiply,
    Count,
};

struct ScreenFadeRenderState {
    // Every session start gets a presentation epoch.  The renderer uses this
    // together with the sealed logic frame to reject an old queued fade from
    // a previous match instead of briefly drawing it over a new world.
    uint64_t presentationEpoch = 0;
    uint64_t presentationSequence = 0;
    ScreenFadeBlendMode blendMode = ScreenFadeBlendMode::None;
    uint8_t intensity = 0;
    bool active = false;
};

// One source-ordered CAMERA_BW_MODE_BEGIN/END command. Unlike a durable
// desired-state slot, Begin -> End in one confirmed logic frame must survive
// snapshot extraction so the renderer can first install BW, then begin its
// fade-out. `transitionFrames` deliberately remains signed legacy data.
struct BlackAndWhiteRenderCommand {
    uint64_t presentationEpoch = 0;
    uint64_t presentationSequence = 0;
    uint64_t confirmedTick = 0;
    uint32_t sourceScriptId = 0;
    uint32_t ordinal = 0;
    bool enabled = false;
    int32_t transitionFrames = 0;
};

// CAMERA_BW_MODE_BEGIN/END is a renderer-frame transition, not simulation
// camera state. The latest fields are a bounded-journal resynchronization
// fallback; `commands` is the ordered replay tail consumed by the renderer's
// epoch/sequence cursor.
struct BlackAndWhiteRenderState {
    uint64_t presentationEpoch = 0;
    uint64_t presentationSequence = 0;
    uint64_t commandsTrimmedThroughSequence = 0;
    bool enabled = false;
    int32_t transitionFrames = 0;
    container::Vector<BlackAndWhiteRenderCommand> commands;
};

// The W3D motion-blur modes are command-driven, not durable simulation
// camera state. Zoom variants are finite renderer-frame effects, Follow is
// retained until EndFollow, and Jump carries a detached waypoint coordinate
// for the renderer-local view translation at its radial peak.
enum class MotionBlurRenderMode : uint8_t {
    ZoomIn,
    ZoomOut,
    ZoomJump,
    Follow,
    EndFollow,
    Count,
};

struct MotionBlurRenderCommand {
    uint64_t presentationEpoch = 0;
    uint64_t presentationSequence = 0;
    uint64_t confirmedTick = 0;
    uint32_t sourceScriptId = 0;
    uint32_t ordinal = 0;
    MotionBlurRenderMode mode = MotionBlurRenderMode::ZoomIn;
    bool saturate = false;
    bool hasJumpTarget = false;
    RenderVector jumpTarget{};
    int32_t followAmount = 0;
};

// `commands` is a non-destructive replay tail. The latest fields are the
// bounded-tail fallback, so a renderer which begins after intermediate
// snapshots were dropped can still restore a persistent Follow or observe a
// final EndFollow without reaching into GameSession.
struct MotionBlurRenderState {
    uint64_t presentationEpoch = 0;
    uint64_t presentationSequence = 0;
    uint64_t commandsTrimmedThroughSequence = 0;
    MotionBlurRenderMode mode = MotionBlurRenderMode::ZoomIn;
    bool saturate = false;
    bool hasJumpTarget = false;
    RenderVector jumpTarget{};
    int32_t followAmount = 0;
    container::Vector<MotionBlurRenderCommand> commands;
};

// Durable request for C&C3/3DSMax camera-slave playback. Object identity and
// bone spelling cross the game/render boundary as values only; the renderer
// resolves that request against the sealed animated skeleton pose of this
// frame. It never owns an ECS entity, Drawable, or live GameCameraDirector.
struct CameraSlaveRenderState {
    uint64_t presentationEpoch = 0;
    uint64_t presentationSequence = 0;
    bool enabled = false;
    RenderEntityId objectId = 0;
    container::String boneName;
};

// W3DWater owns one `new_skybox` object. Scripts only toggle its durable draw
// flag; scale and Z originate in GlobalData and are sealed here so asset/GPU
// recording never reaches back into mutable game configuration.
struct SkyboxRenderState {
    uint64_t presentationEpoch = 0;
    uint64_t presentationSequence = 0;
    bool enabled = false;
    float positionZ = 0.0f;
    float scale = 4.5f;
    // Frozen WaterTransparency faces in RefCode's fixed N/E/S/W/T order.
    // They are separate strings rather than a cube-map handle because the
    // old new_skybox W3D owns five ordinary material texture stages.
    container::Array<container::String, 5> textureNames{
        "TSMorningN.tga",
        "TSMorningE.tga",
        "TSMorningS.tga",
        "TSMorningW.tga",
        "TSMorningT.tga",
    };
};

// SET_TREE_SWAY is one durable map-wide breeze value.  Individual tree
// phases are renderer-local and derived from RenderEntityId, so presentation
// never consumes SimulationRandom or needs a mutable per-entity simulation
// component.  Every field is copied with the world snapshot.
struct TreeSwayRenderState {
    uint64_t presentationEpoch = 0;
    uint64_t presentationSequence = 0;
    uint64_t confirmedTick = 0;
    bool enabled = true;
    float directionRadians = math::PI / 3.0f;
    float intensityRadians = 0.07f * math::PI / 4.0f;
    float leanRadians = 0.07f * math::PI / 4.0f;
    uint32_t periodFrames = 150;
    float randomness = 0.2f;
};

// SHOW_WEATHER changes only `visible`; the remaining fields are a sealed
// Weather.ini snow capability/configuration.  Thus a Show action correctly
// has no visible effect when the loaded map/content did not enable snow.
struct WeatherRenderState {
    uint64_t presentationEpoch = 0;
    uint64_t presentationSequence = 0;
    bool visible = true;
    bool snowEnabled = false;
    bool usePointSprites = true;
    container::String snowTexture = "EXSnowFlake.tga";
    float frequencyScaleX = 0.0533f;
    float frequencyScaleY = 0.0275f;
    float amplitude = 5.0f;
    float pointSize = 1.0f;
    float maximumPointSize = 64.0f;
    float minimumPointSize = 0.0f;
    float quadSize = 0.5f;
    float boxDimensions = 200.0f;
    float boxDensity = 1.0f;
    float velocity = 4.0f;
};

// Detached projection of RefCode's View::CameraShakeType.  These values are
// deliberately not `GameCameraDirector` commands: a renderer consumes the
// ordered journal with its own client-local direction seed, then offsets only
// the camera value it uses for the current world pass.
enum class ScreenShakeRenderIntensity : uint8_t {
    Subtle = 0,
    Normal,
    Strong,
    Severe,
    CineExtreme,
    CineInsane,
    Count,
};

struct ScreenShakeRenderImpulse {
    uint64_t presentationEpoch = 0;
    uint64_t sequence = 0;
    uint64_t confirmedTick = 0;
    uint32_t sourceScriptId = 0;
    uint32_t ordinal = 0;
    ScreenShakeRenderIntensity intensity = ScreenShakeRenderIntensity::Subtle;
};

// Spatial C&C3 CAMERA_ADD_SHAKER_AT impulse. It is evaluated directly from
// sealed values at render time so dropped logic snapshots cannot duplicate or
// revive a decayed effect; no renderer-local simulation RNG is involved.
struct LocalizedCameraShakeRenderImpulse {
    uint64_t presentationEpoch = 0;
    uint64_t sequence = 0;
    uint64_t confirmedTick = 0;
    uint32_t sourceScriptId = 0;
    uint32_t ordinal = 0;
    RenderVector position{};
    float amplitude = 0.0f;
    float radius = 0.0f;
    uint32_t durationTicks = 0;
};

// The full bounded journal is copied with every sealed snapshot, not drained
// during extraction. A renderer keeps its own `(epoch, sequence)` cursor and
// can therefore recover a confirmed impulse even when a latest-only render
// queue deliberately discards intermediate logic frames.
struct ScreenShakeRenderState {
    uint64_t presentationEpoch = 0;
    uint64_t impulsesTrimmedThroughSequence = 0;
    uint64_t localizedImpulsesTrimmedThroughSequence = 0;
    // CAMERA_ADD_SHAKER_AT's authored duration is converted to logic ticks
    // by the legacy compiler, but its sinusoidal oscillator is specified in
    // real seconds. Seal the source tick rate with the snapshot so a mission
    // launched at a non-default game speed does not silently change its Hz.
    uint32_t logicFramesPerSecond = 30;
    container::Vector<ScreenShakeRenderImpulse> impulses;
    container::Vector<LocalizedCameraShakeRenderImpulse> localizedImpulses;
};

// Durable client rendering/UI policy authored by map scripts. This is not a
// simulation setting: it travels only in sealed presentation snapshots so a
// renderer-side marker/icon implementation never needs to query a
// live GameSession while recording commands.
struct ClientOptionsRenderState {
    uint64_t presentationEpoch = 0;
    uint64_t presentationSequence = 0;
    bool occlusionEnabled = true;
    bool drawIconUiEnabled = true;
};

// A detached world-space icon request. Script actions never carry an Anim2D
// instance or texture pointer: extraction freezes the stable ObjectId,
// anchor, template name and confirmed-clock lifetime, then the local
// presentation overlay resolves the current mapped-image frame.  The slot is
// intentionally separate from RenderEntitySnapshot so health bars, selection
// pips and future object markers can reuse it without mutating ECS visuals.
struct ObjectIconRenderSnapshot {
    RenderEntityId objectId = 0;
    RenderVector worldAnchor{};
    container::String animationName;
    uint64_t presentationEpoch = 0;
    uint64_t presentationSequence = 0;
    uint64_t startTick = 0;
    uint64_t lastVisibleTick = 0;
    uint32_t logicFramesPerSecond = 30;
    float zRisePerSecond = 0.0f;
    bool fadeOnExpire = false;
    bool permanent = false;
};

// A full value snapshot of the object-icon layer.  Its outer epoch/sequence
// makes an empty newer layer authoritative: a queued pre-clear frame cannot
// resurrect an emoticon after a newer script action or session starts.
struct ObjectIconRenderState {
    uint64_t presentationEpoch = 0;
    uint64_t presentationSequence = 0;
    SharedSnapshotVector<ObjectIconRenderSnapshot> icons;
};

struct WorldFloatingTextRenderSnapshot final {
    uint64_t identity = 0;
    RenderVector worldAnchor{};
    int64_t amount = 0;
    uint32_t color = 0xffffffffu;
    uint64_t startTick = 0;
    uint64_t timeoutTick = 0;
    uint64_t expireTick = 0;
    uint32_t logicFramesPerSecond = 30;
    float moveUpPerSecond = 30.0f;
    float vanishPerSecond = 3.0f;
};

// Transient InGameUI feedback has its own latest-value layer. It must not be
// merged by ObjectId with durable script emoticons: one object may gain a
// level and own a scripted icon in the same confirmed frame.
struct WorldFeedbackRenderState final {
    uint64_t presentationEpoch = 0;
    uint64_t presentationSequence = 0;
    SharedSnapshotVector<ObjectIconRenderSnapshot> animations;
    SharedSnapshotVector<WorldFloatingTextRenderSnapshot> floatingTexts;
};

enum class ObjectUiRelationship : uint8_t {
    Owned,
    Allied,
    Neutral,
    Enemy,
    Observer,
};

struct ObjectUiCaptionStyle final {
    container::String fontName = "Arial";
    int32_t pointSize = 10;
    uint32_t color = 0xffffffffu;
    bool bold = false;
};

// Detached input for Drawable::drawIconUI and the local selection feedback
// layer.  Every field is frozen after a confirmed game tick; the renderer
// may project and decorate it but never follows an ECS entity, Player, Body,
// Weapon or Contain interface while recording commands.
enum class ObjectUiSelectionBoundsKind : uint8_t {
    Sphere,
    Cylinder,
    Box,
};

struct ObjectUiRenderSnapshot final {
    RenderEntityId objectId = 0;
    RenderVector worldPosition{};
    RenderVector captionAnchor{};
    RenderVector healthAnchor{};
    float worldRadius = 1.0f;
    ObjectUiSelectionBoundsKind selectionBounds =
        ObjectUiSelectionBoundsKind::Sphere;
    float selectionMajorRadius = 1.0f;
    float selectionMinorRadius = 1.0f;
    float selectionHeight = 2.0f;
    float selectionYawRadians = 0.0f;
    // Object::getHealthBoxDimensions first clamps major+minor to 20..150
    // world units, then doubles it.  Preserve that detached world width so
    // the renderer can project it using the displayed camera instead of
    // deriving a differently clamped pixel width from the selection radius.
    float healthBoxWorldWidth = 40.0f;
    float healthRatio = 1.0f;
    float experienceRatio = 0.0f;
    uint32_t indicatorColor = 0xffffffffu;
    // UTF-8 value copied from confirmed object state.  An empty string is the
    // authoritative absence of Drawable caption text.
    container::String caption;
    uint16_t ammoTotal = 0;
    uint16_t ammoFull = 0;
    // RefCode's ContainInterface reports the authored slot count separately
    // from occupancy.  Zero is a valid "do not draw pips" result; extraction
    // must not manufacture capacity from the current passenger count.
    uint16_t containerTotal = 0;
    uint16_t containerFull = 0;
    uint16_t containerInfantry = 0;
    uint8_t veterancyLevel = 0;
    uint8_t damageState = 0;
    ObjectUiRelationship relationship = ObjectUiRelationship::Observer;
    LocalVisibilityRenderCellState visibility =
        LocalVisibilityRenderCellState::Visible;
    bool selected = false;
    // Monotonic one-shot Drawable::flashAsSelected identity. Zero means no
    // active external flash; renderer history prevents repeated snapshots of
    // one confirmed frame from restarting the envelope.
    uint64_t selectionFlashIdentity = 0;
    bool hovered = false;
    // Mirrors Object::isSelectable/CanSelectDrawable at the detached
    // presentation boundary. Point selection may inspect any relationship;
    // drag selection applies the additional owned/non-structure policy.
    bool selectable = false;
    bool shrubberyTarget = false;
    bool mineTarget = false;
    bool forceAttackable = false;
    bool effectivelyDead = false;
    bool underConstruction = false;
    float constructionPercent = 100.0f;
    bool disabled = false;
    bool disabledIcon = false;
    bool sold = false;
    bool recentlyHealing = false;
    uint64_t recentlyHealingUntilTick = 0;
    bool structure = false;
    bool vehicle = false;
    bool noHealIcon = false;
    bool enthusiastic = false;
    bool subliminal = false;
    bool carBomb = false;
    // StickyBombUpdate draws its remote/timed marker on the bomb Drawable,
    // whose confirmed transform follows the target.  Keep the remaining
    // timer as an absolute logic tick so a dropped render frame cannot make
    // the countdown advance at a different rate from gameplay.
    bool stickyBombAttached = false;
    bool stickyBombTimed = false;
    uint64_t stickyBombDieTick = 0;
    bool ignoredInGui = false;
    bool radarStructure = false;
    bool radarUnit = false;
    bool radarLocalOnly = false;
    bool stealthed = false;
    bool detected = false;
};

enum class OrderWaypointRenderKind : uint8_t {
    Move,
    Attack,
    AttackMove,
    Build,
    Guard,
    Ability,
};

enum class OrderWaypointRenderColor : uint8_t {
    Blue,
    Orange,
    Red,
    Green,
    Yellow,
};

// Local-player path visualization and hit-test contract. It contains only
// information already legal for the observing presentation: dynamic object
// targets are published only while their ObjectUi projection is currently
// visible/detected. The renderer/input layer never follows targetObjectId
// back into Simulation.
struct OrderWaypointRenderSnapshot final {
    RenderEntityId identity = 0;
    uint32_t actorObjectId = 0;
    uint32_t sourceSequence = 0;
    uint32_t targetObjectId = 0;
    OrderWaypointRenderKind kind = OrderWaypointRenderKind::Move;
    OrderWaypointRenderColor color = OrderWaypointRenderColor::Blue;
    RenderVector worldPosition{};
    float selectionRadius = 4.0f;
    bool dynamicTarget = false;
    bool rejected = false;
};

// A detached path edge for the local order overlay.  This deliberately owns
// no terrain material/texture state: Presentation publishes world endpoints
// and semantic order kind, while the renderer draws a solid client overlay.
// Keeping edges beside waypoint nodes also prevents route visualization from
// entering the TerrainBib resource/upload pipeline.
struct OrderWaypointSegmentRenderSnapshot final {
    RenderEntityId identity = 0;
    uint32_t actorObjectId = 0;
    uint32_t sourceSequence = 0;
    OrderWaypointRenderKind kind = OrderWaypointRenderKind::Move;
    OrderWaypointRenderColor color = OrderWaypointRenderColor::Blue;
    RenderVector startWorldPosition{};
    RenderVector endWorldPosition{};
    bool rejected = false;
};

struct ObjectUiRenderState final {
    uint64_t presentationEpoch = 0;
    uint64_t presentationSequence = 0;
    bool showObjectHealth = true;
    ObjectUiCaptionStyle captionStyle;
    uint32_t logicFramesPerSecond = 30;
    SharedSnapshotVector<ObjectUiRenderSnapshot> objects;
    SharedSnapshotVector<OrderWaypointRenderSnapshot> waypoints;
    SharedSnapshotVector<OrderWaypointSegmentRenderSnapshot> waypointSegments;
};

struct TacticalRadarEventRenderSnapshot final {
    // Stable producer identity allows latest-only snapshots to carry the same
    // event without restarting it in the renderer-local 64-slot journal.
    uint64_t eventIdentity = 0;
    RenderEntityId sourceObjectId = 0;
    RenderVector worldPosition{};
    int32_t eventType = 0;
    uint64_t createTick = 0;
    uint64_t fadeTick = 0;
    uint64_t dieTick = 0;
};

// Local radar presentation policy remains distinct from R2 visibility
// authority.  It shares the already-projected observer grid below, not the
// simulation's all-player visibility store or its diplomacy rules.
struct TacticalRadarRenderState final {
    uint64_t presentationEpoch = 0;
    uint64_t presentationSequence = 0;
    bool visible = false;
    bool forced = false;
    bool spectator = false;
    SharedSnapshotVector<TacticalRadarEventRenderSnapshot> events;
};

// Detached compatibility values for the two old W3D visible-region actions.
// The terrain value remains observable even though D3D12TerrainVisual always
// owns the complete map chunk set; guardBandX/Y are used as a conservative
// expansion of the modern radial object culler. They are not a camera pose or
// an ECS visibility bit.
struct ViewCompatibilityRenderState {
    uint64_t presentationEpoch = 0;
    uint64_t presentationSequence = 0;
    int32_t terrainOversizeTiles = 0;
    float guardBandX = 0.0f;
    float guardBandY = 0.0f;
};


} // namespace engine::render
