#pragma once

#include "core/container/hash_containers.h"
#include "core/math/fixed/q32_32.h"

#include "ScriptProgram.h"
#include "game/script/contracts/ScriptPresentationValueTypes.h"
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

namespace engine::script {

struct ScriptEffectHeader final {
    uint64_t confirmedTick = 0;
    ScriptId sourceScript = INVALID_SCRIPT_ID;
    // Stable calling/condition Object and Team IDs travel with every effect.
    // ThisPlayer is invocation.currentPlayer; the authored alias remains only
    // for standalone programs and diagnostics.
    ScriptInvocationContext invocation;
    container::String currentPlayerAlias;
    // Stable order within one confirmed tick. Consumers can preserve this
    // order without relying on callback scheduling or container iteration.
    uint32_t ordinal = 0;
};

struct ScriptVictoryEffect final {
    ScriptMissionEndMode mode = ScriptMissionEndMode::Normal;
};

struct ScriptDefeatEffect final {
};

struct ScriptTextEffect final {
    container::String text;
    bool localized = true;
};

struct ScriptCinematicTextEffect final {
    container::String text;
    container::String fontDescriptor;
    uint32_t durationTicks = 0;
    bool localized = true;
};

struct ScriptMilitaryCaptionEffect final {
    container::String text;
    uint32_t durationMilliseconds = 0;
    bool localized = true;
};

// A confirmed legacy MOVIE_PLAY_* request.  This target intentionally has no
// Bink/decoder/UI playback path: GameSession immediately records one
// source-ordered HAS_FINISHED_VIDEO compatibility fact.  No media handle,
// asynchronous callback, or presentation clock may cross into ScriptRuntime.
struct ScriptMovieEffect final {
    ScriptMovieTarget target = ScriptMovieTarget::Fullscreen;
    container::String movieName;
};

struct ScriptAudioEffect final {
    container::String eventName;
    std::optional<math::vec3> position;
    container::String waypointName;
    std::optional<ObjectId> emitter;
    container::String emitterName;
    float volumeScale = 1.0f;
    bool uninterruptible = false;
};

// Confirmed presentation command for a legacy MUSIC_* action.  This remains
// independent from GameAudioEvent because MusicTrack replacement has its own
// fade and stream-lifetime semantics; GameSessionScriptBridge is the only
// place that may hand it to the client audio service.
struct ScriptMusicEffect final {
    ScriptMusicCommand command = ScriptMusicCommand::SetTrack;
    container::String trackName;
    bool fadeOut = false;
    bool fadeIn = false;
    float volume = 1.0f;
};

// `SOUND_AMBIENT_PAUSE` / `SOUND_AMBIENT_RESUME` must not manufacture or
// destroy ordinary AudioEvent requests.  The audio consumer owns that state;
// this effect merely commits the legacy global pause intent in source order.
struct ScriptAmbientAudioEffect final {
    bool paused = false;
};

// FREEZE_TIME / UNFREEZE_TIME retain their own typed runtime consequence.
// They are authoritative clock policy, not an audio/UI presentation request:
// GameSession commits it during the script phase. GameLogic samples the
// world gate before that phase, so terrain, commands and object systems first
// observe the new value on the following confirmed tick.
struct ScriptTimeControlEffect final {
    bool frozen = false;
};

// The authored REAL is quantized when the immutable program emits a confirmed
// gameplay effect. The bridge still owns conversion from seconds to the
// active session's logic-frame count.
struct ScriptHulkLifetimeOverrideEffect final {
    math::q32_32 seconds{int32_t{-1}};
};

// This is a confirmed simulation-policy write, deliberately separate from
// ScriptUiEffect.  The bridge commits it immediately so a later same-tick
// score-producing system can observe the final authored source-order value.
struct ScriptScoreAccumulationPolicyEffect final {
    bool enabled = true;
};

// A stamped direct write to the tactical-view visual clock.  GameLogic may
// consume it for bounded single-player pacing, while a lockstep client keeps
// exactly one confirmed frame per ready network frame.
struct ScriptVisualSpeedEffect final {
    int32_t multiplier = 1;
};

// SPEECH_PLAY creates this after its audio effect.  Keeping subtitles as a
// separate confirmed presentation value prevents ScriptRuntime from querying
// localization/UI while retaining the original audio-then-subtitle order.
struct ScriptSubtitleEffect final {
    container::String label;
    uint32_t durationTicks = 0;
};

struct ScriptCameraEffect final {
    ScriptCameraCommand command = ScriptCameraCommand::SetPose;
    math::vec3 position{};
    math::vec3 target{};
    container::String waypointName;
    container::String lookAtWaypointName;
    std::optional<ObjectId> object;
    container::String objectName;
    float value = 0.0f;
    float secondaryValue = 0.0f;
    float tertiaryValue = 0.0f;
    int32_t rollingAverageFrames = 1;
    int32_t visualSpeedMultiplier = 1;
    uint32_t durationTicks = 0;
    uint32_t easeInTicks = 0;
    uint32_t easeOutTicks = 0;
    uint32_t holdTicks = 0;
    bool reverseRotation = false;
    bool enabled = true;
};

// CAMERA_ENABLE/DISABLE_SLAVE_MODE remains a renderer-local request. The
// runtime can only resolve the legacy named Object through ScriptWorldQuery;
// it never inspects a Drawable, W3D asset, skeleton, or ECS entity. A missing
// target is intentionally still emitted as an empty ObjectId request so the
// bridge can replace/clear a previously active slave just as W3DView does on
// its next presentation update.
struct ScriptCameraSlaveEffect final {
    std::optional<ObjectId> object;
    container::String objectName;
    container::String boneName;
    bool enabled = false;
};

// SCREEN_SHAKE deliberately does not share ScriptCameraEffect.  The legacy
// action applies a client-side spring impulse at the current tactical view;
// it neither changes the durable logic camera pose nor participates in
// CAMERA_MOVEMENT_FINISHED.
struct ScriptScreenShakeEffect final {
    ScriptScreenShakeIntensity intensity = ScriptScreenShakeIntensity::Subtle;
};

struct ScriptLocalizedCameraShakeEffect final {
    container::String waypointName;
    float amplitude = 0.0f;
    uint32_t durationTicks = 0;
    float radius = 0.0f;
};

// CAMERA_FADE_* stays separate from ScriptCameraEffect for the same reason
// as SCREEN_SHAKE: it neither changes the durable logic camera pose nor
// participates in CAMERA_MOVEMENT_FINISHED.  The session advances this
// single replacement slot on confirmed ticks and publishes a sealed render
// value after script execution.
struct ScriptScreenFadeEffect final {
    ScriptScreenFadeBlendMode blendMode = ScriptScreenFadeBlendMode::Add;
    float minimumIntensity = 0.0f;
    float maximumIntensity = 0.0f;
    int32_t increaseFrames = 0;
    int32_t holdFrames = 0;
    int32_t decreaseFrames = 0;
};

// Value-only presentation command for CAMERA_BW_MODE_BEGIN/END.  `enabled`
// is intentionally not a simulation-camera property and does not take part
// in CAMERA_MOVEMENT_FINISHED; a renderer owns the active filter slot and
// decides whether an End command still targets BW after replacement.
struct ScriptBlackAndWhiteEffect final {
    bool enabled = false;
    int32_t transitionFrames = 0;
};

// Detached presentation command for CAMERA_MOTION_BLUR*.  The bridge turns
// a jump waypoint into `jumpTarget`; the renderer alone owns its filter
// lifetime, render-frame timing, captured color resource, and optional local
// camera translation.  None of this is an ECS or logic-camera mutation.
struct ScriptMotionBlurEffect final {
    ScriptMotionBlurMode mode = ScriptMotionBlurMode::ZoomIn;
    bool saturate = false;
    container::String waypointName;
    std::optional<math::vec3> jumpTarget;
    int32_t followAmount = 0;
};

// Value-only desired state for DRAW_SKYBOX_BEGIN/END.  The world renderer
// resolves the legacy "new_skybox" W3D asset and its map-configured faces;
// ScriptRuntime only keeps the authored final boolean detached from assets.
struct ScriptSkyboxEffect final {
    bool enabled = false;
};

struct ScriptTreeSwayEffect final {
    float directionRadians = math::PI / 3.0f;
    float intensityRadians = 0.07f * math::PI / 4.0f;
    float leanRadians = 0.07f * math::PI / 4.0f;
    int32_t periodFrames = 150;
    float randomness = 0.2f;
};

struct ScriptWeatherEffect final {
    bool visible = true;
};

// A durable presentation override for KindOf INFANTRY directional lighting.
// `overrideScale == nullopt` is RESET.  It intentionally contains neither a
// GlobalData pointer nor a renderer resource: GameSession stamps the value
// and render extraction classifies each detached entity snapshot.
struct ScriptInfantryLightingEffect final {
    std::optional<float> overrideScale;
};

struct ScriptWaterEffect final {
    ScriptWaterCommand command = ScriptWaterCommand::SetHeight;
    container::String waterName;
    math::q32_32 value{};
    uint32_t transitionTicks = 0;
    math::q32_32 damagePerSecond{};
    bool enabled = true;
};

// Confirmed value-only transport for OBJECT_FORCE_SELECT.  It deliberately
// retains the authored selector instead of resolving Team members here:
// ScriptRuntime owns no ECS, selection, drawable, or local-client state.
struct ScriptForceObjectSelectionEffect final {
    container::String teamName;
    container::String objectTypeName;
    bool centerInView = false;
    container::String audioEventName;
};

struct ScriptOrderEffect final {
    ScriptOrderKind kind = ScriptOrderKind::Move;
    ScriptMoveRouteSubtype moveRouteSubtype = ScriptMoveRouteSubtype::Direct;
    ScriptTacticalAttackSubtype tacticalAttackSubtype =
        ScriptTacticalAttackSubtype::None;
    ScriptOrderActorSelector actorSelector = ScriptOrderActorSelector::NamedObjects;
    container::Vector<ObjectId> actors;
    // Non-empty only for ScenarioTeam. It is resolved by the session bridge
    // after earlier effects in this same confirmed tick have been applied.
    container::String teamName;
    // Non-empty only for PlayerAssets (currently PLAYER_HUNT). Ownership is
    // deliberately expanded by the bridge, after prior effects may have
    // transferred objects during this same confirmed script pass.
    container::String playerName;
    // Sequential Team execution already owns a stable live instance.  Keep
    // it distinct from the authored-name path so a second instance is never
    // redirected to the prototype's first Team at bridge admission.
    ObjectTeamId scenarioTeam = INVALID_OBJECT_TEAM_ID;
    std::optional<ObjectId> targetObject;
    std::optional<ScriptFixedVec3> targetPosition;
    container::String targetTeamName;
    container::String targetAreaName;
    // Direct carries one authored waypoint name; WaypointPathIndividuals
    // carries the unresolved legacy waypoint-path label.
    container::String targetWaypointName;
    container::String contentName;
    bool forceAttack = false;
    bool allArmyHunt = false;
    bool useTeamCommonTarget = false;
    bool disbandAfterStop = false;
    bool queued = false;
};

struct ScriptFireWeaponFollowingWaypointPathEffect final {
    ObjectId object = INVALID_OBJECT_ID;
    container::String waypointPathName;
};

struct ScriptBuildTeamEffect final {
    container::String teamName;
};

struct ScriptGuardSupplyCenterEffect final {
    container::String teamName;
    int32_t minimumSupplies = 0;
};

// Value-only request for materializing an authored Scenario Team roster.
// ScriptRuntime owns neither ScenarioDefinition nor ObjectLifecycle, so the
// prototype and waypoint remain names until the stamped session bridge.
struct ScriptCreateReinforcementTeamEffect final {
    container::String teamName;
    container::String destinationWaypointName;
};

struct ScriptRecruitTeamEffect final {
    container::String teamName;
    math::q32_32 radius{};
};

// Value-only transport for the basic and source/target-selecting legacy
// USE_COMMANDBUTTON actions.
// Named actors have already been resolved to stable ObjectIds; Team
// membership and waypoint coordinates remain live session queries so effects
// earlier in the same confirmed script pass are observed.
struct ScriptUseCommandButtonEffect final {
    ScriptOrderActorSelector actorSelector =
        ScriptOrderActorSelector::NamedObjects;
    container::Vector<ObjectId> actors;
    container::String teamName;
    container::String buttonName;
    ScriptCommandButtonActorPolicy actorPolicy =
        ScriptCommandButtonActorPolicy::All;
    math::q32_32 actorPercentage{int32_t{100}};
    bool preselectSourceAndTarget = false;
    ScriptCommandButtonTargetKind targetKind =
        ScriptCommandButtonTargetKind::None;
    std::optional<ObjectId> targetObject;
    container::String targetWaypointName;
    container::String targetFilter;
    container::Vector<container::String> targetObjectTypes;
};

struct ScriptFacingEffect final {
    ScriptOrderActorSelector actorSelector =
        ScriptOrderActorSelector::NamedObjects;
    container::Vector<ObjectId> actors;
    container::String teamName;
    std::optional<ObjectId> targetObject;
    container::String targetWaypointName;
};

struct ScriptAIBehaviorMutationEffect final {
    ScriptAIBehaviorTargetKind targetKind =
        ScriptAIBehaviorTargetKind::NamedObject;
    ObjectId object = INVALID_OBJECT_ID;
    container::String teamName;
    ScriptAIBehaviorMutationKind mutation =
        ScriptAIBehaviorMutationKind::ApplyAttackPrioritySet;
    container::String attackPrioritySet;
    container::String commandButton;
    int32_t attitude = 0;
};

struct ScriptAttackPriorityMutationEffect final {
    ScriptAttackPriorityMutationKind mutation =
        ScriptAttackPriorityMutationKind::Default;
    container::String setName;
    // ObjectType is expanded from the runtime's current object-type list;
    // KindOf carries one canonical legacy token; Default carries neither.
    container::Vector<container::String> selectors;
    int32_t priority = 1;
};

struct ScriptTeamCustomStateEffect final {
    ObjectTeamId team = INVALID_OBJECT_TEAM_ID;
    container::String state;
};

struct ScriptStoppingDistanceEffect final {
    ScriptStoppingDistanceTargetKind targetKind =
        ScriptStoppingDistanceTargetKind::NamedObject;
    ObjectId object = INVALID_OBJECT_ID;
    container::String teamName;
    math::q32_32 distance{};
};

struct ScriptMoveTowardsNearestEffect final {
    ScriptOrderActorSelector actorSelector = ScriptOrderActorSelector::NamedObjects;
    container::Vector<ObjectId> actors;
    container::String teamName;
    container::Vector<container::String> objectTypes;
    container::String triggerArea;
};

struct ScriptSpecialPowerCountdownEffect final {
    ScriptSpecialPowerCountdownOperation operation =
        ScriptSpecialPowerCountdownOperation::Pause;
    ObjectId object = INVALID_OBJECT_ID;
    container::String specialPower;
    int32_t seconds = 0;
    bool paused = false;
};

struct ScriptWarehouseValueEffect final {
    ObjectId object = INVALID_OBJECT_ID;
    int32_t cashValue = 0;
};

struct ScriptCaveIndexEffect final {
    ObjectId object = INVALID_OBJECT_ID;
    int32_t caveIndex = 0;
};

// A structural creation remains a value until the GameSession bridge reaches
// its authoritative ObjectLifecycle path. The bridge commits it synchronously
// in stamped source order, so later script actions see the real ObjectId/name
// without a synthetic identity namespace crossing the runtime boundary.
struct ScriptCreateObjectEffect final {
    container::String objectName;
    container::String templateName;
    container::String teamName;
    std::optional<ScriptFixedVec3> position;
    container::String waypointName;
    math::q32_32 rotation{};
};

// A structural mutation remains a detached value until the bridge commits it
// synchronously in stamped source order. ScriptRuntime itself never retains
// an ECS handle or calls Session code.
struct ScriptDestroyObjectEffect final {
    ObjectId object = INVALID_OBJECT_ID;
    container::String objectName;
    bool forceKill = false;
};

struct ScriptLifecycleEffect final {
    ScriptLifecycleTargetKind targetKind = ScriptLifecycleTargetKind::ScenarioTeam;
    ScriptLifecycleOperation operation = ScriptLifecycleOperation::Delete;
    container::String targetName;
};

struct ScriptContainmentEffect final {
    ScriptContainmentActionKind kind = ScriptContainmentActionKind::EjectContainerContents;
    ObjectId namedTarget = INVALID_OBJECT_ID;
    container::String targetName;
    int32_t evacuationDisposition = 0;
};

struct ScriptContainmentEnterEffect final {
    ScriptContainmentEnterActionKind kind =
        ScriptContainmentEnterActionKind::NamedEnterNamed;
    ScriptObjectSelector object;
    ScriptTeamSelector team;
    container::String player;
    ScriptObjectSelector container;
};

struct ScriptTransferOwnershipEffect final {
    ScriptOwnershipTransferSelector selector = ScriptOwnershipTransferSelector::NamedObject;
    ObjectId object = INVALID_OBJECT_ID;
    container::String teamName;
    container::String targetTeamName;
    container::String sourcePlayer;
    container::String targetPlayer;
};

// Damage stays a value-only transaction until the bridge reaches the
// session's ObjectSimulation ingress.  Named targets are resolved by the
// runtime; Scenario Team membership intentionally remains authored here and
// is expanded by the bridge after earlier stamped effects have committed.
struct ScriptDamageEffect final {
    ScriptDamageTargetSelector targetSelector = ScriptDamageTargetSelector::NamedObject;
    container::Vector<ObjectId> targets;
    container::String teamName;
    math::q32_32 amount{};
    bool forceKill = false;
};

// NAMED_RECEIVE_UPGRADE resolves its named object before crossing the effect
// boundary. The session remains the sole owner of UpgradeCatalog validation
// and the object-local UpgradeMux transaction.
struct ScriptGrantObjectUpgradeEffect final {
    ObjectId object = INVALID_OBJECT_ID;
    container::String upgradeName;
};

struct ScriptObjectStateMutationEffect final {
    ScriptObjectStateTargetKind targetKind = ScriptObjectStateTargetKind::NamedObject;
    ObjectId object = INVALID_OBJECT_ID;
    container::String teamName;
    ScriptObjectStateMutationKind mutation = ScriptObjectStateMutationKind::Held;
    bool enabled = false;
};

struct ScriptGlobalObjectEffect final {
    ScriptGlobalObjectOperation operation = ScriptGlobalObjectOperation::IdleHumanUnits;
};

struct ScriptBoobyTrapEffect final {
    ScriptObjectStateTargetKind targetKind = ScriptObjectStateTargetKind::NamedObject;
    ObjectId object = INVALID_OBJECT_ID;
    container::String teamName;
    container::String templateName;
};

struct ScriptToppleDirectionEffect final {
    container::String objectName;
    ScriptFixedVec3 direction{};
};

enum class ScriptPlayerCashOperation : uint8_t {
    Set,
    Adjust,
};

struct ScriptPlayerCashEffect final {
    container::String player;
    int64_t value = 0;
    ScriptPlayerCashOperation operation = ScriptPlayerCashOperation::Set;
};

struct ScriptPlayerSellEverythingEffect final {
    container::String player;
};

struct ScriptPlayerRepairStructureEffect final {
    container::String player;
    ScriptObjectSelector structure;
};

struct ScriptPlayerBuildUpgradeEffect final {
    container::String player;
    container::String upgrade;
};

struct ScriptPlayerBuildObjectNearTeamEffect final {
    container::String player;
    container::String objectType;
    container::String teamName;
};

struct ScriptPlayerBuildSupplyCenterEffect final {
    container::String player;
    container::String objectType;
    int32_t minimumSupplies = 0;
};

struct ScriptSkirmishBuildBuildingEffect final {
    container::String objectType;
};

struct ScriptSkirmishApproachEffect final {
    ScriptSkirmishApproachOperation operation =
        ScriptSkirmishApproachOperation::FollowPath;
    container::String teamName;
    container::String pathPrefix;
    bool asTeam = false;
};

struct ScriptSkirmishPerimeterBuildEffect final {
    container::String objectType;
    bool flank = false;
    bool useFactionBaseDefense = false;
};

struct ScriptSkirmishFireSpecialPowerAtMostCostEffect final {
    container::String player;
    container::String specialPower;
};

struct ScriptSkirmishAttackNearestValueGroupEffect final {
    container::String teamName;
    ScriptComparison comparison = ScriptComparison::GreaterEqual;
    int32_t minimumValue = 0;
};

struct ScriptSkirmishMostValuableCommandButtonEffect final {
    container::String teamName;
    container::String buttonName;
    math::q32_32 range{};
    bool allTeamMembers = false;
};

struct ScriptPlayerConstructionEffect final {
    ScriptPlayerConstructionOperation operation =
        ScriptPlayerConstructionOperation::SetBaseEnabled;
    container::String player;
    container::String factoryType;
    int32_t value = 0;
    bool enabled = true;
};

struct ScriptObjectBuildabilityEffect final {
    container::String objectType;
    ScriptObjectBuildability buildability = ScriptObjectBuildability::Yes;
};

struct ScriptPlayerScienceAvailabilityEffect final {
    container::String player;
    container::String science;
    ScriptScienceAvailability availability = ScriptScienceAvailability::Available;
};

struct ScriptPlayerRelationshipEffect final {
    container::String sourcePlayer;
    container::String targetPlayer;
    ScriptPlayerRelationship relationship = ScriptPlayerRelationship::Enemies;
};

struct ScriptRelationshipOverrideEffect final {
    ScriptRelationshipEndpointKind sourceKind =
        ScriptRelationshipEndpointKind::ScenarioTeam;
    ScriptRelationshipEndpointKind targetKind =
        ScriptRelationshipEndpointKind::ScenarioTeam;
    ScriptRelationshipOverrideOperation operation =
        ScriptRelationshipOverrideOperation::Set;
    container::String sourceName;
    container::String targetName;
    ScriptPlayerRelationship relationship =
        ScriptPlayerRelationship::Neutral;
};

struct ScriptGlobalCombatPolicyEffect final {
    ScriptGlobalCombatPolicy policy =
        ScriptGlobalCombatPolicy::ObjectDifficultyBonuses;
    bool enabled = false;
};

struct ScriptPlayerProgressionEffect final {
    ScriptPlayerProgressionOperation operation = ScriptPlayerProgressionOperation::AddSkillPoints;
    container::String player;
    container::String science;
    int32_t integerValue = 0;
    math::q32_32 realValue{};
};

struct ScriptDebugMessageEffect final {
    container::String text;
    ScriptDebugMessageKind kind = ScriptDebugMessageKind::Log;
};

using ScriptEffectPayload = std::variant<
    ScriptVictoryEffect,
    ScriptDefeatEffect,
    ScriptDebugMessageEffect,
    ScriptTextEffect,
    ScriptCinematicTextEffect,
    ScriptMilitaryCaptionEffect,
    ScriptMovieEffect,
    ScriptAudioEffect,
    ScriptMusicEffect,
    ScriptAmbientAudioEffect,
    ScriptAudioControlEffect,
    ScriptTimeControlEffect,
    ScriptHulkLifetimeOverrideEffect,
    ScriptScoreAccumulationPolicyEffect,
    ScriptVisualSpeedEffect,
    ScriptUiEffect,
    ScriptCommandBarOverrideEffect,
    ScriptClientOptionsEffect,
    ScriptMapPresentationEffect,
    ScriptObjectPresentationEffect,
    ScriptForceObjectSelectionEffect,
    ScriptViewCompatibilityEffect,
    ScriptSubtitleEffect,
    ScriptCameraEffect,
    ScriptCameraSlaveEffect,
    ScriptScreenShakeEffect,
    ScriptLocalizedCameraShakeEffect,
    ScriptScreenFadeEffect,
    ScriptBlackAndWhiteEffect,
    ScriptMotionBlurEffect,
    ScriptSkyboxEffect,
    ScriptTreeSwayEffect,
    ScriptWeatherEffect,
    ScriptInfantryLightingEffect,
    ScriptWaterEffect,
    ScriptTeamCustomStateEffect,
    ScriptOrderEffect,
    ScriptFireWeaponFollowingWaypointPathEffect,
    ScriptBuildTeamEffect,
    ScriptGuardSupplyCenterEffect,
    ScriptCreateReinforcementTeamEffect,
    ScriptRecruitTeamEffect,
    ScriptUseCommandButtonEffect,
    ScriptFacingEffect,
    ScriptAIBehaviorMutationEffect,
    ScriptAttackPriorityMutationEffect,
    ScriptStoppingDistanceEffect,
    ScriptMoveTowardsNearestEffect,
    ScriptSpecialPowerCountdownEffect,
    ScriptWarehouseValueEffect,
    ScriptCaveIndexEffect,
    ScriptCreateObjectEffect,
    ScriptDestroyObjectEffect,
    ScriptLifecycleEffect,
    ScriptContainmentEffect,
    ScriptContainmentEnterEffect,
    ScriptTransferOwnershipEffect,
    ScriptDamageEffect,
    ScriptGrantObjectUpgradeEffect,
    ScriptObjectStateMutationEffect,
    ScriptGlobalObjectEffect,
    ScriptBoobyTrapEffect,
    ScriptToppleDirectionEffect,
    ScriptPlayerCashEffect,
    ScriptPlayerSellEverythingEffect,
    ScriptPlayerRepairStructureEffect,
    ScriptPlayerBuildUpgradeEffect,
    ScriptPlayerBuildObjectNearTeamEffect,
    ScriptPlayerBuildSupplyCenterEffect,
    ScriptSkirmishBuildBuildingEffect,
    ScriptSkirmishApproachEffect,
    ScriptSkirmishPerimeterBuildEffect,
    ScriptSkirmishFireSpecialPowerAtMostCostEffect,
    ScriptSkirmishAttackNearestValueGroupEffect,
    ScriptSkirmishMostValuableCommandButtonEffect,
    ScriptPlayerConstructionEffect,
    ScriptObjectBuildabilityEffect,
    ScriptPlayerScienceAvailabilityEffect,
    ScriptPlayerRelationshipEffect,
    ScriptRelationshipOverrideEffect,
    ScriptGlobalCombatPolicyEffect,
    ScriptPlayerProgressionEffect>;

struct ScriptEffect final {
    ScriptEffectHeader header;
    ScriptEffectPayload payload;
};

class ScriptEffectSink {
public:
    virtual ~ScriptEffectSink() = default;
    // A hint only: sinks may ignore it, cap it, or retain a larger capacity.
    // It is issued once per confirmed tick and prevents ordinary maps from
    // repeatedly reallocating their per-tick effect vector.
    virtual void reserveEffects(size_t effectCountHint) { static_cast<void>(effectCountHint); }
    // Effects are consumed by value so an interpreter-created payload moves
    // into a buffering bridge instead of being copied once per boundary.
    virtual void emit(ScriptEffect effect) = 0;
};

} // namespace engine::script
