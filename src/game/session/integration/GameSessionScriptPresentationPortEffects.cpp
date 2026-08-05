#include "GameSessionScriptPortDetail.h"
#include "GameSessionScriptPresentationPort.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/object/definition/ObjectArchetype.h"

#include "debug/debug.h"
#include "game/base/GameBalanceConstants.h"
#include "game/base/DamageTypes.h"
#include "game/base/GameCameraDirector.h"
#include "game/base/GameSettings.h"
#include "game/audio/GameAudioEvents.h"
#include "game/command/CommandButtonStore.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/structure/ObjectTechBuilding.h"
#include "core/math/wwmath/base/wwmath.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace engine::script {
namespace {
[[nodiscard]] float durationSeconds(uint32_t ticks, int framesPerSecond) noexcept {
    if (ticks == 0) return 0.0f;
    const int rate = std::max(1, framesPerSecond);
    return static_cast<float>(ticks) / static_cast<float>(rate);
}

[[nodiscard]] GameCameraScriptTiming cameraTiming(const ScriptCameraEffect& effect,
                                                   int framesPerSecond) noexcept {
    const int rate = std::max(1, framesPerSecond);
    const float confirmationTick = 1.0f / static_cast<float>(rate);
    // Every RefCode operation below enters W3DView through moveCameraTo,
    // moveCameraAlongWaypointPath, zoomCamera, pitchCamera or rotateCamera.
    // Each clamps milliseconds<1 to one frame and keeps its Scripted_* bit
    // long enough for later same-frame script conditions to observe it.
    const bool needsLegacyConfirmationTick = [&effect]() noexcept {
        switch (effect.command) {
        case ScriptCameraCommand::MoveTo:
        case ScriptCameraCommand::MoveAlongWaypointPath:
        case ScriptCameraCommand::Setup:
        case ScriptCameraCommand::Zoom:
        case ScriptCameraCommand::Pitch:
        case ScriptCameraCommand::Rotate:
        case ScriptCameraCommand::LookTowardWaypoint:
        case ScriptCameraCommand::LookTowardNamedObject:
        case ScriptCameraCommand::Reset:
            return true;
        case ScriptCameraCommand::SetPose:
        case ScriptCameraCommand::ModifyLookToward:
        case ScriptCameraCommand::ModifyFinalZoom:
        case ScriptCameraCommand::ModifyFinalPitch:
        case ScriptCameraCommand::ModifyFinalLookToward:
        case ScriptCameraCommand::MoveToSelection:
        case ScriptCameraCommand::FreezeAngle:
        case ScriptCameraCommand::FreezeTimeDuringMotion:
        case ScriptCameraCommand::ModifyFinalSpeedMultiplier:
        case ScriptCameraCommand::ModifyRollingAverage:
        case ScriptCameraCommand::SetDefault:
        case ScriptCameraCommand::FollowNamedObject:
        case ScriptCameraCommand::StopFollow:
        case ScriptCameraCommand::SetLetterbox:
        case ScriptCameraCommand::TetherNamedObject:
        case ScriptCameraCommand::StopTether:
            return false;
        }
        return false;
    }();
    return {
        .durationSeconds = needsLegacyConfirmationTick
            ? std::max(durationSeconds(effect.durationTicks, rate), confirmationTick)
            : durationSeconds(effect.durationTicks, rate),
        .easeInSeconds = durationSeconds(effect.easeInTicks, rate),
        .easeOutSeconds = durationSeconds(effect.easeOutTicks, rate),
    };
}

// RefCode finds every matching Team member and retains the Object with the
// lower legacy ObjectID, because that allocator counted downward and a lower
// ID meant a newer instance. Modern ObjectId values are monotonically
// increasing, while ObjectTeamRegistry exposes them in ascending order, so
// retaining the final match gives the same "newest matching object" result.
//
// Do not test RenderModelComponent here. The original chooses its best Guess
// first and only then checks `getDrawable()`; if that newest candidate has no
// Drawable, it is a silent no-op rather than a fallback to an older visible
// member. The local presentation consumer owns that final visual/liveness
// check after this confirmed bridge request is stamped.
} // namespace

std::optional<GameSessionScriptPresentationPort::ForceObjectSelectionTarget>
GameSessionScriptPresentationPort::resolveForceObjectSelectionTarget(
    ObjectTeamId team, container::StringView objectTypeName,
    bool capturePosition) const noexcept {
    if (!team || !m_world.m_objectTeams.find(team)) return std::nullopt;

    ObjectId selected = INVALID_OBJECT_ID;
    for (const ObjectId member : m_world.m_objectTeams.members(team)) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(member);
        if (!entity) continue;
        const ThingTemplateComponent* templateComponent =
            ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity);
        if (!templateComponent) continue;
        const container::StringView templateName = templateComponent->archetype
            ? container::StringView{templateComponent->archetype->templateData.name}
            : container::StringView{templateComponent->name};
        if (templateName == objectTypeName) selected = member;
    }
    if (!selected) return std::nullopt;

    ForceObjectSelectionTarget result{.object = selected};
    if (!capturePosition) return result;
    const std::optional<ecs::entity> entity =
        m_world.m_objects.entityFromId(selected);
    const TransformComponent* transform = entity
        ? ecs::try_get<TransformComponent>(m_world.m_registry, *entity) : nullptr;
    if (transform && std::isfinite(transform->x) && std::isfinite(transform->y) &&
        std::isfinite(transform->z)) {
        result.position = math::vec3{transform->x, transform->y, transform->z};
    }
    return result;
}

using detail::kindOfContains;

namespace detail {

bool applyPresentationEffect(
    GameSessionScriptPresentationPort& bridge, const ScriptEffect& effect) {
    bool handled = false;
    std::visit([&](const auto& payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, ScriptDebugMessageEffect> ||
                      std::is_same_v<Payload, ScriptTextEffect> ||
                      std::is_same_v<Payload, ScriptCinematicTextEffect> ||
                      std::is_same_v<Payload, ScriptMilitaryCaptionEffect> ||
                      std::is_same_v<Payload, ScriptMovieEffect> ||
                      std::is_same_v<Payload, ScriptSubtitleEffect> ||
                      std::is_same_v<Payload, ScriptAudioEffect> ||
                      std::is_same_v<Payload, ScriptMusicEffect> ||
                      std::is_same_v<Payload, ScriptAmbientAudioEffect> ||
                      std::is_same_v<Payload, ScriptVisualSpeedEffect> ||
                      std::is_same_v<Payload, ScriptAudioControlEffect> ||
                      std::is_same_v<Payload, ScriptUiEffect> ||
                      std::is_same_v<Payload, ScriptCommandBarOverrideEffect> ||
                      std::is_same_v<Payload, ScriptClientOptionsEffect> ||
                      std::is_same_v<Payload, ScriptViewCompatibilityEffect> ||
                      std::is_same_v<Payload, ScriptMapPresentationEffect> ||
                      std::is_same_v<Payload, ScriptObjectPresentationEffect> ||
                      std::is_same_v<Payload, ScriptForceObjectSelectionEffect> ||
                      std::is_same_v<Payload, ScriptCameraEffect> ||
                      std::is_same_v<Payload, ScriptCameraSlaveEffect> ||
                      std::is_same_v<Payload, ScriptScreenShakeEffect> ||
                      std::is_same_v<Payload, ScriptLocalizedCameraShakeEffect> ||
                      std::is_same_v<Payload, ScriptScreenFadeEffect> ||
                      std::is_same_v<Payload, ScriptBlackAndWhiteEffect> ||
                      std::is_same_v<Payload, ScriptMotionBlurEffect> ||
                      std::is_same_v<Payload, ScriptSkyboxEffect> ||
                      std::is_same_v<Payload, ScriptTreeSwayEffect> ||
                      std::is_same_v<Payload, ScriptWeatherEffect> ||
                      std::is_same_v<Payload, ScriptInfantryLightingEffect>) {
            handled = true;
        if constexpr (std::is_same_v<Payload, ScriptDebugMessageEffect>) {
            const container::StringView prefix = payload.kind == ScriptDebugMessageKind::Dialog
                ? "Legacy debug message: "
                : payload.kind == ScriptDebugMessageKind::CrashBox
                    ? "Suppressed legacy debug crash: "
                    : "Legacy debug log: ";
            bridge.emitDiagnostic(effect.header, container::String(prefix) + payload.text);
        } else if constexpr (std::is_same_v<Payload, ScriptTextEffect>) {
            bridge.emitSessionEvent({
                .kind = ScriptSessionEventKind::Text,
                .confirmedTick = effect.header.confirmedTick,
                .sourceScriptId = effect.header.sourceScript.value,
                .ordinal = effect.header.ordinal,
                .text = payload.text,
                .localized = payload.localized,
            });
        } else if constexpr (std::is_same_v<Payload, ScriptCinematicTextEffect>) {
            // This is an overlay command rather than a normal message-log
            // entry. The presentation layer owns font resolution and the
            // single-active-overlay replacement policy; the script bridge
            // merely preserves the confirmed data and source order.
            bridge.emitSessionEvent({
                .kind = ScriptSessionEventKind::CinematicText,
                .confirmedTick = effect.header.confirmedTick,
                .sourceScriptId = effect.header.sourceScript.value,
                .ordinal = effect.header.ordinal,
                .text = payload.text,
                .localized = payload.localized,
                .fontDescriptor = payload.fontDescriptor,
                .durationTicks = payload.durationTicks,
            });
        } else if constexpr (std::is_same_v<Payload, ScriptMilitaryCaptionEffect>) {
            // The label remains unresolved until UI so the active locale can
            // apply RefCode's missing/empty-label clear rule. Milliseconds
            // stay raw: this caption intentionally progresses on the
            // presentation clock while a script freezes simulation time.
            bridge.emitSessionEvent({
                .kind = ScriptSessionEventKind::MilitaryCaption,
                .confirmedTick = effect.header.confirmedTick,
                .sourceScriptId = effect.header.sourceScript.value,
                .ordinal = effect.header.ordinal,
                .text = payload.text,
                .localized = payload.localized,
                .durationMilliseconds = payload.durationMilliseconds,
            });
        } else if constexpr (std::is_same_v<Payload, ScriptMovieEffect>) {
            // Playback is currently disabled by product policy. The session
            // records a deterministic completed-video fact immediately; the
            // target is still validated so malformed script values cannot
            // masquerade as a successful request.
            if (!bridge.emitMoviePresentation(
                    payload.target, payload.movieName, effect.header.confirmedTick,
                    effect.header.sourceScript.value, effect.header.ordinal)) {
                bridge.emitDiagnostic(effect.header,
                    "Script movie presentation request was rejected by GameSession");
            }
        } else if constexpr (std::is_same_v<Payload, ScriptSubtitleEffect>) {
            bridge.emitSessionEvent({
                .kind = ScriptSessionEventKind::Subtitle,
                .confirmedTick = effect.header.confirmedTick,
                .sourceScriptId = effect.header.sourceScript.value,
                .ordinal = effect.header.ordinal,
                .text = payload.label,
                .localized = true,
                .durationTicks = payload.durationTicks,
            });
        } else if constexpr (std::is_same_v<Payload, ScriptAudioEffect>) {
            game::GameAudioEvent audio;
            audio.eventName = payload.eventName;
            audio.position = payload.position;
            audio.volumeScale = payload.volumeScale;
            audio.uninterruptible = payload.uninterruptible;
            if (!payload.waypointName.empty()) {
                const game::terrain::WaypointRecord* waypoint =
                    bridge.terrain().waypointByName(payload.waypointName);
                // PLAY_SOUND_EFFECT_AT has the same silent no-op contract as
                // RefCode when a map is missing the authored waypoint.
                if (!waypoint) return;
                audio.position = waypoint->position;
            }
            if (!payload.emitter && !payload.emitterName.empty()) {
                if (const std::optional<ScriptWorldObjectSnapshot> object =
                        bridge.m_queries.findNamedObject(payload.emitterName)) {
                    audio.emitter = object->id;
                    if (!audio.position) audio.position = object->position;
                }
            } else {
                audio.emitter = payload.emitter;
            }
            if (!bridge.emitAudioEvent(std::move(audio))) {
                bridge.emitDiagnostic(effect.header, "Script audio effect was rejected by GameSession");
            }
        } else if constexpr (std::is_same_v<Payload, ScriptMusicEffect>) {
            game::GameAudioControlEvent control;
            switch (payload.command) {
            case ScriptMusicCommand::SetTrack:
                control.kind = game::GameAudioControlKind::SetMusicTrack;
                control.trackName = payload.trackName;
                control.fadeOut = payload.fadeOut;
                control.fadeIn = payload.fadeIn;
                break;
            case ScriptMusicCommand::SetVolume:
                control.kind = game::GameAudioControlKind::SetMusicVolume;
                control.volume = payload.volume;
                break;
            }
            if (!bridge.emitAudioControlEvent(std::move(control))) {
                bridge.emitDiagnostic(effect.header, "Script music effect was rejected by GameSession");
            } else if (payload.command == ScriptMusicCommand::SetTrack &&
                       !bridge.beginMusicCompletionTracking(
                           payload.trackName, effect.header.confirmedTick,
                           effect.header.sourceScript.value, effect.header.ordinal)) {
                // The audio control has safely reached the presentation
                // queue, but without a session-owned completion stamp a
                // later MUSIC_TRACK_HAS_COMPLETED could be satisfied by a
                // stale loop report. Keep that fault visible instead of
                // silently retaining the previous track's ledger state.
                bridge.emitDiagnostic(effect.header,
                    "Script music completion tracking was rejected by GameSession");
            }
        } else if constexpr (std::is_same_v<Payload, ScriptAmbientAudioEffect>) {
            if (!bridge.emitAudioControlEvent({
                    .kind = game::GameAudioControlKind::SetAmbientPaused,
                    .paused = payload.paused,
                })) {
                bridge.emitDiagnostic(effect.header, "Script ambient-audio effect was rejected by GameSession");
            }
        } else if constexpr (std::is_same_v<Payload, ScriptVisualSpeedEffect>) {
            // This owns only W3DView-compatible visual-clock state.  It does
            // not alter a fixed delta or current confirmed tick here: the
            // GameLogic single-player loop reads the director at its next
            // outer update, while the network branch deliberately ignores
            // the acceleration request.
            bridge.cameraDirector().setVisualSpeedMultiplier(payload.multiplier);
        } else if constexpr (std::is_same_v<Payload, ScriptAudioControlEffect>) {
            // Legacy AudioManager policy calls are client presentation work,
            // but they are ordered relative to ordinary AudioEvents.  Publish
            // exactly one confirmed control value; the media presentation port and
            // AudioSubsystem retain the event-name/volume policy without a
            // device handle or AudioEvent pointer crossing this bridge.
            game::GameAudioControlEvent control;
            switch (payload.command) {
            case ScriptAudioControlCommand::SetBackgroundSoundsPaused:
                control.kind = game::GameAudioControlKind::SetBackgroundSoundsPaused;
                control.paused = !payload.enabled;
                break;
            case ScriptAudioControlCommand::SetSoundVolume:
                control.kind = game::GameAudioControlKind::SetSoundVolume;
                control.volume = payload.volume;
                break;
            case ScriptAudioControlCommand::SetSpeechVolume:
                control.kind = game::GameAudioControlKind::SetSpeechVolume;
                control.volume = payload.volume;
                break;
            case ScriptAudioControlCommand::SetEventVolumeOverride:
                control.kind = game::GameAudioControlKind::SetEventVolumeOverride;
                control.eventName = payload.eventName;
                control.volume = payload.volume;
                break;
            case ScriptAudioControlCommand::RestoreEventVolumeOverride:
                control.kind = game::GameAudioControlKind::RestoreEventVolumeOverride;
                control.eventName = payload.eventName;
                break;
            case ScriptAudioControlCommand::RestoreAllEventVolumeOverrides:
                control.kind = game::GameAudioControlKind::RestoreAllEventVolumeOverrides;
                break;
            case ScriptAudioControlCommand::RemoveEvent:
                control.kind = game::GameAudioControlKind::RemoveEvent;
                control.eventName = payload.eventName;
                break;
            case ScriptAudioControlCommand::RemoveDisabledEvents:
                control.kind = game::GameAudioControlKind::RemoveDisabledEvents;
                break;
            case ScriptAudioControlCommand::SetEvaEnabled:
                control.kind = game::GameAudioControlKind::SetEvaEnabled;
                control.enabled = payload.enabled;
                break;
            }
            if (!bridge.emitAudioControlEvent(std::move(control))) {
                bridge.emitDiagnostic(effect.header, "Script audio-control effect was rejected by GameSession");
            }
        } else if constexpr (std::is_same_v<Payload, ScriptUiEffect>) {
            if (!bridge.applyUiPresentation(payload, effect.header.confirmedTick,
                                                     effect.header.sourceScript.value,
                                                     effect.header.ordinal)) {
                bridge.emitDiagnostic(effect.header, "Script UI presentation effect was rejected by GameSession");
            }
        } else if constexpr (std::is_same_v<Payload, ScriptCommandBarOverrideEffect>) {
            if (!bridge.applyCommandBarOverride(payload, effect.header.confirmedTick,
                                                         effect.header.sourceScript.value,
                                                         effect.header.ordinal)) {
                bridge.emitDiagnostic(effect.header,
                               "Script command-bar override effect was rejected by GameSession");
            }
        } else if constexpr (std::is_same_v<Payload, ScriptClientOptionsEffect>) {
            if (!bridge.applyClientOptions(payload, effect.header.confirmedTick,
                                                     effect.header.sourceScript.value,
                                                     effect.header.ordinal)) {
                bridge.emitDiagnostic(effect.header, "Script client-options effect was rejected by GameSession");
            }
        } else if constexpr (std::is_same_v<Payload, ScriptViewCompatibilityEffect>) {
            if (!bridge.applyViewCompatibility(payload, effect.header.confirmedTick,
                                                        effect.header.sourceScript.value,
                                                        effect.header.ordinal)) {
                bridge.emitDiagnostic(effect.header,
                               "Script view-compatibility effect was rejected by GameSession");
            }
        } else if constexpr (std::is_same_v<Payload, ScriptMapPresentationEffect>) {
            if (!bridge.applyMapPresentation(
                    payload, effect.header.confirmedTick,
                    effect.header.sourceScript.value, effect.header.ordinal)) {
                bridge.emitDiagnostic(effect.header, "Script map-presentation effect was rejected by GameSession");
            }
        } else if constexpr (std::is_same_v<Payload, ScriptObjectPresentationEffect>) {
            // Object/team radar actions resolve their transient live target
            // before this effect boundary. Reuse the one authoritative radar
            // state path so their stamp, four-second lifetime, fade and
            // bounded history cannot diverge from RADAR_CREATE_EVENT.
            switch (payload.command) {
            case ScriptObjectPresentationCommand::CreateRadarEvent:
                if (!payload.position) {
                    bridge.emitDiagnostic(effect.header,
                        "Script object-presentation radar event has no resolved position");
                    return;
                }
                if (!bridge.applyMapPresentation(
                        {.command = ScriptMapPresentationCommand::CreateRadarEvent,
                         .position = *payload.position,
                         .radarEventType = payload.radarEventType},
                        effect.header.confirmedTick, effect.header.sourceScript.value,
                        effect.header.ordinal)) {
                    bridge.emitDiagnostic(effect.header,
                        "Script object-presentation radar event was rejected by GameSession");
                }
                return;
            case ScriptObjectPresentationCommand::Flash:
            case ScriptObjectPresentationCommand::SetCustomIndicatorColor:
            case ScriptObjectPresentationCommand::SetEmoticon:
            case ScriptObjectPresentationCommand::SetAmbientSoundEnabled:
            case ScriptObjectPresentationCommand::SetSpecialPowerDisplayVisible:
                if (!bridge.applyObjectPresentation(
                        payload, effect.header.confirmedTick, effect.header.sourceScript.value,
                        effect.header.ordinal, effect.header.invocation.currentPlayer,
                        effect.header.currentPlayerAlias)) {
                    bridge.emitDiagnostic(effect.header,
                        "Script object-presentation effect was rejected by GameSession");
                }
                return;
            default:
                bridge.emitDiagnostic(effect.header,
                    "Script object-presentation effect has an unsupported command");
                return;
            }
        } else if constexpr (std::is_same_v<Payload, ScriptForceObjectSelectionEffect>) {
            // Resolve the Team/template while this confirmed source-ordered
            // bridge can see authoritative ObjectId membership. The emitted
            // journal contains only a stable ID and copied position; it does
            // not let ScriptRuntime observe ECS or let the session own a
            // local selection set.
            const auto target =
                [&]() {
                    const std::optional<ObjectTeamId> team =
                        bridge.resolveEffectTeam(payload.teamName, effect.header);
                    return team ? bridge.resolveForceObjectSelectionTarget(
                        *team, payload.objectTypeName,
                        payload.centerInView) : std::nullopt;
                }();
            if (!target) return; // RefCode: absent Team/object is a no-op.
            if (!bridge.emitForceObjectSelection(
                    target->object, target->position, payload.centerInView,
                    payload.audioEventName, effect.header.confirmedTick,
                    effect.header.sourceScript.value, effect.header.ordinal)) {
                bridge.emitDiagnostic(effect.header,
                    "Script force-object-selection presentation request was rejected by GameSession");
            } else if (!bridge.startInfo().network.enabled) {
                // The presentation consumer runs after ScriptRuntime, but
                // RefCode mutates InGameUI immediately. Keep a bridge-local
                // overlay so a later NAMED_SELECTED condition in this same
                // source-ordered pass observes the accepted replacement.
                bridge.m_localPresentation.replace(target->object);
            }
        } else if constexpr (std::is_same_v<Payload, ScriptCameraEffect>) {
            if (payload.command == ScriptCameraCommand::SetLetterbox) {
                // W3DDisplay owns the wall-clock fade and UI owns control-bar
                // suppression. The session only commits the idempotent desired
                // state and its confirmed source stamp; it does not read or
                // alter the logic camera.
                static_cast<void>(bridge.setLetterbox(
                    payload.enabled, effect.header.confirmedTick,
                    effect.header.sourceScript.value, effect.header.ordinal));
                return;
            }
            const GameCameraScriptTiming timing =
                cameraTiming(payload, bridge.startInfo().gameSpeedFPS);
            const auto publish = [&](ScriptCameraPresentationCommand command,
                                     bool startsMovement = false) {
                command.durationSeconds = timing.durationSeconds;
                command.easeInSeconds = timing.easeInSeconds;
                command.easeOutSeconds = timing.easeOutSeconds;
                bridge.emitCameraCommand(
                    std::move(command), effect.header.confirmedTick,
                    effect.header.sourceScript.value, effect.header.ordinal,
                    startsMovement);
            };
            const auto waypoint = [&bridge, &payload]() noexcept {
                return bridge.terrain().waypointByName(payload.waypointName);
            };
            const auto onGround = [&bridge](math::vec3 position) noexcept {
                position[2] = bridge.terrain().groundHeight(
                    position.x(), position.y());
                return position;
            };
            switch (payload.command) {
            case ScriptCameraCommand::SetPose: {
                publish({
                    .operation = ScriptCameraPresentationOperation::SetPose,
                    .position = payload.position,
                    .target = payload.target,
                });
                return;
            }
            case ScriptCameraCommand::MoveTo: {
                const game::terrain::WaypointRecord* target = waypoint();
                // RefCode silently ignores an absent waypoint instead of
                // manufacturing an origin camera cut on incomplete mod maps.
                if (target) publish({
                    .operation = ScriptCameraPresentationOperation::MoveTo,
                    .position = onGround(target->position),
                }, true);
                return;
            }
            case ScriptCameraCommand::MoveAlongWaypointPath: {
                const game::terrain::WaypointRecord* current = waypoint();
                if (!current) return;
                // W3DView follows link zero and caps the authored path. Keep
                // the modern copy deterministic even if malformed map data
                // creates a cycle or an extremely long chain.
                constexpr size_t kMaximumPathWaypoints = 64;
                container::Vector<math::vec3> path;
                container::Vector<uint32_t> visited;
                path.reserve(kMaximumPathWaypoints);
                visited.reserve(kMaximumPathWaypoints);
                while (current && path.size() < kMaximumPathWaypoints &&
                       std::find(visited.begin(), visited.end(), current->id) == visited.end()) {
                    path.push_back(onGround(current->position));
                    visited.push_back(current->id);
                    current = current->links.empty()
                        ? nullptr : bridge.terrain().waypointById(current->links.front());
                }
                publish({
                    .operation =
                        ScriptCameraPresentationOperation::MoveAlongPath,
                    .path = std::move(path),
                }, true);
                return;
            }
            case ScriptCameraCommand::MoveToSelection:
                // Selection belongs to the local presentation client, not to
                // ScriptRuntime or this deterministic world bridge. Publish a
                // stamped request and let GameLogic resolve its current
                // LocalSelectionState after this source-ordered flush.
                bridge.emitMoveCameraToSelection(
                    effect.header.confirmedTick, effect.header.sourceScript.value,
                    effect.header.ordinal);
                return;
            case ScriptCameraCommand::Setup: {
                const game::terrain::WaypointRecord* pivot = waypoint();
                const game::terrain::WaypointRecord* lookAt =
                    bridge.terrain().waypointByName(payload.lookAtWaypointName);
                if (pivot && lookAt) {
                    publish({
                        .operation = ScriptCameraPresentationOperation::Setup,
                        .position = onGround(pivot->position),
                        .target = lookAt->position,
                        .value = payload.value,
                        .secondaryValue = payload.secondaryValue,
                    }, true);
                }
                return;
            }
            case ScriptCameraCommand::Zoom:
                publish({.operation = ScriptCameraPresentationOperation::Zoom,
                         .value = payload.value}, true);
                return;
            case ScriptCameraCommand::Pitch:
                publish({.operation = ScriptCameraPresentationOperation::Pitch,
                         .value = payload.value}, true);
                return;
            case ScriptCameraCommand::Rotate:
                publish({.operation = ScriptCameraPresentationOperation::Rotate,
                         .value = payload.value}, true);
                return;
            case ScriptCameraCommand::LookTowardWaypoint: {
                const game::terrain::WaypointRecord* target = waypoint();
                if (target) publish({
                    .operation = ScriptCameraPresentationOperation::LookToward,
                    .position = onGround(target->position),
                    .reverseRotation = payload.reverseRotation,
                }, true);
                return;
            }
            case ScriptCameraCommand::ModifyLookToward: {
                const game::terrain::WaypointRecord* target = waypoint();
                if (target) publish({
                    .operation =
                        ScriptCameraPresentationOperation::ModifyLookToward,
                    .position = target->position,
                });
                return;
            }
            case ScriptCameraCommand::ModifyFinalZoom:
                publish({
                    .operation =
                        ScriptCameraPresentationOperation::ModifyFinalZoom,
                    .value = payload.value,
                    .secondaryValue = payload.secondaryValue,
                    .tertiaryValue = payload.tertiaryValue,
                });
                return;
            case ScriptCameraCommand::ModifyFinalPitch:
                publish({
                    .operation =
                        ScriptCameraPresentationOperation::ModifyFinalPitch,
                    .value = payload.value,
                    .secondaryValue = payload.secondaryValue,
                    .tertiaryValue = payload.tertiaryValue,
                });
                return;
            case ScriptCameraCommand::ModifyFinalLookToward: {
                const game::terrain::WaypointRecord* target = waypoint();
                if (target) {
                    publish({
                        .operation = ScriptCameraPresentationOperation::
                            ModifyFinalLookToward,
                        .position = target->position,
                    });
                }
                return;
            }
            case ScriptCameraCommand::FreezeAngle:
                publish({.operation =
                    ScriptCameraPresentationOperation::FreezeAngle});
                return;
            case ScriptCameraCommand::FreezeTimeDuringMotion:
                bridge.armCameraTimeFreeze();
                return;
            case ScriptCameraCommand::ModifyFinalSpeedMultiplier:
                publish({
                    .operation = ScriptCameraPresentationOperation::
                        ModifyFinalSpeedMultiplier,
                    .integerValue = payload.visualSpeedMultiplier,
                });
                return;
            case ScriptCameraCommand::ModifyRollingAverage:
                publish({
                    .operation = ScriptCameraPresentationOperation::
                        ModifyRollingAverage,
                    .integerValue = payload.rollingAverageFrames,
                });
                return;
            case ScriptCameraCommand::LookTowardNamedObject:
                // ScriptRuntime resolves this named object before the effect
                // crosses the pure-runtime boundary. Missing/deleted targets
                // preserve RefCode's harmless no-op behavior.
                if (!payload.object) return;
                publish({
                    .operation = ScriptCameraPresentationOperation::LookToward,
                    .position = payload.position,
                    .tertiaryValue = payload.holdTicks != 0
                        ? timing.durationSeconds + durationSeconds(
                              payload.holdTicks,
                              bridge.startInfo().gameSpeedFPS)
                        : 0.0f,
                }, true);
                return;
            case ScriptCameraCommand::Reset: {
                const game::terrain::WaypointRecord* target = waypoint();
                if (target) publish({
                    .operation = ScriptCameraPresentationOperation::Reset,
                    .position = onGround(target->position),
                }, true);
                return;
            }
            case ScriptCameraCommand::SetDefault:
                publish({
                    .operation = ScriptCameraPresentationOperation::SetDefault,
                    .value = payload.value,
                    .secondaryValue = payload.secondaryValue,
                    .tertiaryValue = payload.tertiaryValue,
                });
                return;
            case ScriptCameraCommand::FollowNamedObject:
                if (payload.object) {
                    publish({.operation = ScriptCameraPresentationOperation::
                        CancelMovement});
                    bridge.setCameraFollow(*payload.object, payload.enabled);
                }
                return;
            case ScriptCameraCommand::StopFollow:
                bridge.stopCameraFollow();
                return;
            case ScriptCameraCommand::SetLetterbox:
                // Handled before the logic-camera reference is acquired.
                return;
            case ScriptCameraCommand::TetherNamedObject:
                if (payload.object) {
                    publish({.operation = ScriptCameraPresentationOperation::
                        CancelMovement});
                    bridge.setCameraTether(*payload.object, payload.enabled, payload.value);
                }
                return;
            case ScriptCameraCommand::StopTether:
                bridge.stopCameraTether();
                return;
            }
        } else if constexpr (std::is_same_v<Payload, ScriptCameraSlaveEffect>) {
            // The named Object was resolved by ScriptRuntime through the
            // value-only ScriptWorldQuery. This bridge commits only an
            // ObjectId/bone presentation request; the renderer later samples
            // a sealed animated pose and never calls back into the session.
            static_cast<void>(bridge.setCameraSlave(
                payload.object.value_or(INVALID_OBJECT_ID), payload.boneName,
                payload.enabled, effect.header.confirmedTick,
                effect.header.sourceScript.value, effect.header.ordinal));
        } else if constexpr (std::is_same_v<Payload, ScriptScreenShakeEffect>) {
            // ScriptActions shakes the current tactical view, not a world
            // object. Preserve the ordered impulse for a presentation-side
            // spring/damper; SimulationRandom and GameCameraDirector remain
            // intentionally untouched.
            bridge.emitScreenShake(payload.intensity, effect.header.confirmedTick,
                                            effect.header.sourceScript.value, effect.header.ordinal);
        } else if constexpr (std::is_same_v<Payload, ScriptLocalizedCameraShakeEffect>) {
            // C&C3 resolves the authored waypoint at execution time. A stale
            // or absent waypoint remains a harmless no-op, never an origin
            // shake; the renderer receives only detached world-space values.
            const game::terrain::WaypointRecord* waypoint =
                bridge.terrain().waypointByName(payload.waypointName);
            if (!waypoint) return;
            bridge.emitLocalizedCameraShake(
                waypoint->position, payload.amplitude, payload.radius,
                payload.durationTicks, effect.header.confirmedTick,
                effect.header.sourceScript.value, effect.header.ordinal);
        } else if constexpr (std::is_same_v<Payload, ScriptScreenFadeEffect>) {
            // CAMERA_FADE_* owns one presentation blend slot. It must not
            // become a logic-camera track, an ECS component, or a generic UI
            // alpha overlay: GameSession advances the exact authored curve at
            // confirmed-frame start and render extraction seals its sample.
            if (!bridge.setScreenFade(
                    payload.blendMode, payload.minimumIntensity, payload.maximumIntensity,
                    payload.increaseFrames, payload.holdFrames, payload.decreaseFrames,
                    effect.header.confirmedTick, effect.header.sourceScript.value,
                    effect.header.ordinal)) {
                bridge.emitDiagnostic(effect.header, "Script screen-fade effect was rejected by GameSession");
            }
        } else if constexpr (std::is_same_v<Payload, ScriptBlackAndWhiteEffect>) {
            // The renderer owns the active post-process slot.  In particular,
            // an End command cannot be discarded here merely because a prior
            // Begin is not visible in session state: RefCode only fades out
            // when BW remains the active client filter, which this bridge is
            // deliberately unable (and forbidden) to inspect.
            if (!bridge.setBlackAndWhite(
                    payload.enabled, payload.transitionFrames,
                    effect.header.confirmedTick, effect.header.sourceScript.value,
                    effect.header.ordinal)) {
                bridge.emitDiagnostic(effect.header,
                    "Script black-and-white presentation command was rejected by GameSession");
            }
        } else if constexpr (std::is_same_v<Payload, ScriptMotionBlurEffect>) {
            // Motion blur is another W3D tactical-view filter, not a camera
            // rig transition.  JUMP is the sole form with map data: RefCode
            // silently returns before enabling its filter when the waypoint
            // is absent, so resolve it here and publish only a detached point.
            std::optional<math::vec3> jumpTarget = payload.jumpTarget;
            if (payload.mode == ScriptMotionBlurMode::ZoomJump) {
                const game::terrain::WaypointRecord* waypoint =
                    bridge.terrain().waypointByName(payload.waypointName);
                if (!waypoint) return;
                jumpTarget = waypoint->position;
            }
            if (!bridge.emitMotionBlur(
                    payload.mode, payload.saturate, std::move(jumpTarget),
                    payload.followAmount, effect.header.confirmedTick,
                    effect.header.sourceScript.value, effect.header.ordinal)) {
                bridge.emitDiagnostic(effect.header,
                    "Script motion-blur presentation command was rejected by GameSession");
            }
        } else if constexpr (std::is_same_v<Payload, ScriptSkyboxEffect>) {
            // This is a map/world desired state, not a screen-color fallback.
            // The renderer will resolve the legacy new_skybox asset and face
            // materials from the sealed state; no logic camera or ECS object
            // is involved here.
            static_cast<void>(bridge.setSkybox(
                payload.enabled, effect.header.confirmedTick,
                effect.header.sourceScript.value, effect.header.ordinal));
        } else if constexpr (std::is_same_v<Payload, ScriptTreeSwayEffect>) {
            // BreezeInfo is script-owned but only SwayClientUpdate visuals
            // consume it. The session stamps a new presentation generation;
            // neither Object/ECS state nor SimulationRandom is touched here.
            static_cast<void>(bridge.setTreeSwayPresentation(
                payload.directionRadians, payload.intensityRadians,
                payload.leanRadians, payload.periodFrames, payload.randomness,
                effect.header.confirmedTick, effect.header.sourceScript.value,
                effect.header.ordinal));
        } else if constexpr (std::is_same_v<Payload, ScriptWeatherEffect>) {
            // RefCode's ScriptActions::doWeather is exactly
            // SnowManager::setVisible. A duplicate final value is harmless;
            // only the session-owned renderer presentation slot observes it.
            static_cast<void>(bridge.setWeatherPresentation(
                payload.visible, effect.header.confirmedTick,
                effect.header.sourceScript.value, effect.header.ordinal));
        } else if constexpr (std::is_same_v<Payload, ScriptInfantryLightingEffect>) {
            // The original writes a global script override which W3D consumes
            // only while building the infantry light environment.  Keep the
            // modern value session-scoped and detached; render extraction
            // copies it into a sealed snapshot without an ECS/render pointer
            // ever entering ScriptRuntime.
            static_cast<void>(bridge.setInfantryLightingPresentation(
                payload.overrideScale, effect.header.confirmedTick,
                effect.header.sourceScript.value, effect.header.ordinal));
        }
        }
    }, effect.payload);
    return handled;
}

} // namespace detail
} // namespace engine::script
