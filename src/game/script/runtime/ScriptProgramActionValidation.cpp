#include "ScriptProgramValidationInternal.h"
#include "game/script/contracts/ScriptPresentationLimits.h"

#include <cmath>
#include <type_traits>

namespace engine::script::detail
{

[[nodiscard]] bool validActionShape(const ScriptAction& action,
                                    container::Vector<ScriptProgramBuildIssue>* issues,
                                    container::StringView scriptName)
{
    const auto invalid = [&](container::StringView reason)
    {
        addIssue(issues, "script '" + container::String(scriptName) + "' " + container::String(reason));
        return false;
    };
    return std::visit(
        [&](const auto& value) -> bool
        {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, ScriptNoOpAction>)
            {
                return true;
            }
            else if constexpr (std::is_same_v<Value, ScriptSequentialControlAction>)
            {
                const bool validTarget = value.targetKind == ScriptSequentialTargetKind::Object
                    ? value.object.valid()
                    : value.team.valid();
                const bool validControl =
                    value.operation == ScriptSequentialControlOperation::Start
                        ? static_cast<bool>(value.script)
                        : !value.script && value.remainingRequeues == 0;
                return (validTarget && validControl) ||
                       invalid("has an invalid sequential-control action");
            }
            else if constexpr (std::is_same_v<Value, ScriptSequentialWaitAction>)
            {
                const bool commandButtonWait =
                    value.kind == ScriptSequentialWaitKind::CommandButtonAllReady ||
                    value.kind == ScriptSequentialWaitKind::CommandButtonPartiallyReady;
                return (value.team.valid() &&
                        (!commandButtonWait || !value.commandButton.empty())) ||
                       invalid("has an invalid sequential-wait action");
            }
            else if constexpr (std::is_same_v<Value, ScriptSequentialTimedAction>)
            {
                const bool validTarget = value.targetKind == ScriptSequentialTargetKind::Object
                    ? value.object.valid()
                    : value.team.valid();
                const bool validCommand =
                    value.command == ScriptSequentialTimedCommand::Idle ||
                    (value.command == ScriptSequentialTimedCommand::DelayOnly &&
                     value.targetKind == ScriptSequentialTargetKind::Team) ||
                    (value.command ==
                         ScriptSequentialTimedCommand::GuardAtCurrentPosition &&
                     value.targetKind == ScriptSequentialTargetKind::Object);
                return (validTarget && validCommand) ||
                       invalid("has an invalid sequential timed command");
            }
            else if constexpr (std::is_same_v<Value, ScriptSetTeamCustomStateAction>)
            {
                return (value.team.valid() && !value.state.empty()) ||
                       invalid("has an invalid Team custom-state action");
            }
            else if constexpr (std::is_same_v<Value, ScriptSetFlagAction>)
            {
                return !value.flag.empty() || invalid("sets an empty flag name");
            }
            else if constexpr (std::is_same_v<Value, ScriptSetCounterAction> ||
                               std::is_same_v<Value, ScriptAdjustCounterAction>)
            {
                return !value.counter.empty() || invalid("writes an empty counter name");
            }
            else if constexpr (std::is_same_v<Value, ScriptSetTimerAction>)
            {
                return !value.timer.empty() || invalid("writes an empty timer name");
            }
            else if constexpr (std::is_same_v<Value, ScriptAdjustTimerAction>)
            {
                return !value.timer.empty() ||
                       invalid("has an invalid millisecond timer adjustment");
            }
            else if constexpr (std::is_same_v<Value, ScriptSetRandomTimerAction>)
            {
                const bool validRange = std::visit(
                    [](const auto& range) noexcept
                    {
                        using Range = std::decay_t<decltype(range)>;
                        if constexpr (std::is_same_v<Range, ScriptRandomFrameTimerRange>)
                        {
                            // GetGameLogicRandomValue returns its second
                            // argument when the first is not smaller. Both
                            // signed authored endpoints are therefore valid.
                            return true;
                        }
                        else
                        {
                            return range.ticksPerSecond != 0;
                        }
                    },
                    value.range);
                return (!value.timer.empty() && validRange) ||
                       invalid("has an invalid random timer range");
            }
            else if constexpr (std::is_same_v<Value, ScriptStopTimerAction> ||
                               std::is_same_v<Value, ScriptRestartTimerAction>)
            {
                return !value.timer.empty() || invalid("writes an empty timer name");
            }
            else if constexpr (std::is_same_v<Value, ScriptEnableAction> ||
                               std::is_same_v<Value, ScriptDisableAction> ||
                               std::is_same_v<Value, ScriptCallSubroutineAction>)
            {
                return validTargetShape(value.target) || invalid("has an invalid script target");
            }
            else if constexpr (std::is_same_v<Value, ScriptVictoryAction> || std::is_same_v<Value, ScriptDefeatAction>)
            {
                return true;
            }
            else if constexpr (std::is_same_v<Value, ScriptDebugMessageAction>)
            {
                return (!value.text.empty() &&
                        static_cast<uint8_t>(value.kind) <=
                            static_cast<uint8_t>(ScriptDebugMessageKind::CrashBox)) ||
                       invalid("has an invalid debug message");
            }
            else if constexpr (std::is_same_v<Value, ScriptDisplayTextAction>)
            {
                return !value.text.empty() || invalid("has an empty text effect");
            }
            else if constexpr (std::is_same_v<Value, ScriptDisplayCinematicTextAction>)
            {
                return (!value.text.empty() && !value.fontDescriptor.empty()) ||
                       invalid("has an invalid cinematic-text effect");
            }
            else if constexpr (std::is_same_v<Value, ScriptMilitaryCaptionAction>)
            {
                // A zero duration is valid legacy data: InGameUI removes the
                // preceding caption and then intentionally leaves no new one.
                return !value.text.empty() || invalid("has an empty military-caption effect");
            }
            else if constexpr (std::is_same_v<Value, ScriptMovieAction>)
            {
                return (static_cast<uint8_t>(value.target) <
                            static_cast<uint8_t>(ScriptMovieTarget::Count) &&
                        !value.movieName.empty() &&
                        value.movieName.size() <= kMaximumScriptPresentationNameLength &&
                        value.movieName.find('\0') == container::String::npos) ||
                       invalid("has an invalid movie presentation effect");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayAudioAction>)
            {
                return !value.eventName.empty() && std::isfinite(value.volumeScale) && value.volumeScale >= 0.0f &&
                           value.volumeScale <= 4.0f && (!value.position || finiteVec3(*value.position)) &&
                           (!value.position || value.waypointName.empty()) ||
                       invalid("has an invalid audio effect");
            }
            else if constexpr (std::is_same_v<Value, ScriptMusicAction>)
            {
                // Keep every emitted value finite even where a particular
                // command ignores it. This prevents a hand-authored Program
                // from smuggling NaN through a SetTrack effect for a future
                // consumer that inspects/logs the complete payload.
                if (!std::isfinite(value.volume) || value.volume < 0.0f || value.volume > 1.0f)
                    return invalid("has an invalid normalized music volume");
                switch (value.command)
                {
                case ScriptMusicCommand::SetTrack:
                    // A MusicTrack replacement must carry an authored track
                    // name.  Volume/fade fields are still inert data here;
                    // their command-specific interpretation happens at the
                    // presentation consumer.
                    return !value.trackName.empty() ||
                           invalid("has a music-track command with an empty track name");
                case ScriptMusicCommand::SetVolume:
                    return value.trackName.empty() ||
                           invalid("has an invalid normalized music volume");
                }
                return invalid("has an unsupported music command");
            }
            else if constexpr (std::is_same_v<Value, ScriptAmbientAudioAction>)
            {
                return true;
            }
            else if constexpr (std::is_same_v<Value, ScriptTimeControlAction>)
            {
                // The boolean is the complete legacy payload.  Whether the
                // current session can honor a freeze (network games cannot)
                // belongs to the session boundary, not immutable Program
                // validation.
                return true;
            }
            else if constexpr (std::is_same_v<Value, ScriptHulkLifetimeOverrideAction>)
            {
                // Negative values are a valid legacy off sentinel.  The
                // integer-frame conversion belongs to the active session so
                // its frozen FPS, rather than this immutable Program, is
                // authoritative.
                return true;
            }
            else if constexpr (std::is_same_v<Value, ScriptScoreAccumulationPolicyAction>)
            {
                // The boolean is the complete legacy payload.  Score event
                // accumulation belongs to GameSession, not Script UI state.
                return true;
            }
            else if constexpr (std::is_same_v<Value, ScriptVisualSpeedAction>)
            {
                // The raw signed INT is the complete legacy payload. Bounds
                // on extra single-player work are a GameLogic policy, not an
                // immutable-map validation rule.
                return true;
            }
            else if constexpr (std::is_same_v<Value, ScriptAudioControlAction>)
            {
                if (!std::isfinite(value.volume))
                    return invalid("has a non-finite audio-control volume");

                switch (value.command)
                {
                case ScriptAudioControlCommand::SetBackgroundSoundsPaused:
                case ScriptAudioControlCommand::RestoreAllEventVolumeOverrides:
                case ScriptAudioControlCommand::RemoveDisabledEvents:
                case ScriptAudioControlCommand::SetEvaEnabled:
                    return value.eventName.empty() ||
                           invalid("has an unexpected event name for a global audio control");
                case ScriptAudioControlCommand::SetSoundVolume:
                case ScriptAudioControlCommand::SetSpeechVolume:
                    return (value.eventName.empty() && value.volume >= 0.0f && value.volume <= 1.0f) ||
                           invalid("has an invalid normalized audio bus volume");
                case ScriptAudioControlCommand::SetEventVolumeOverride:
                    return (!value.eventName.empty() && value.volume >= 0.0f && value.volume <= 4.0f) ||
                           invalid("has an invalid event-volume override");
                case ScriptAudioControlCommand::RestoreEventVolumeOverride:
                case ScriptAudioControlCommand::RemoveEvent:
                    return !value.eventName.empty() ||
                           invalid("has an event-scoped audio control with an empty event name");
                }
                return invalid("has an unsupported audio-control command");
            }
            else if constexpr (std::is_same_v<Value, ScriptUiAction>)
            {
                switch (value.command)
                {
                case ScriptUiCommand::SetControl:
                    switch (value.control)
                    {
                    case ScriptUiControlKind::GameplayInput:
                    case ScriptUiControlKind::SpecialPowerDisplay:
                    case ScriptUiControlKind::NamedTimerDisplay:
                        return value.name.empty() && value.text.empty() &&
                               value.flashCount == 0 && value.framesPerFlash == 1 ||
                               invalid("has unrelated data on a UI-control command");
                    }
                    return invalid("has an unsupported UI control");
                case ScriptUiCommand::ShowNamedIndicator:
                    return !value.name.empty() && !value.text.empty() ||
                           invalid("has an invalid named UI indicator");
                case ScriptUiCommand::HideNamedIndicator:
                    return !value.name.empty() || invalid("has an empty named UI indicator");
                case ScriptUiCommand::ShowPopup:
                    return !value.text.empty() || invalid("has an empty popup message");
                case ScriptUiCommand::ShowLocalDefeat:
                    return value.name.empty() && value.text.empty() && value.flashCount == 0 &&
                           value.framesPerFlash == 1 ||
                           invalid("has unrelated data on a local-defeat command");
                case ScriptUiCommand::FlashCameo:
                    // A zero count is an intentional CommandButton
                    // replacement/cancellation from CAMEO_FLASH, not an
                    // absent action. The local UI consumer removes the prior
                    // same-name request and renders no new highlight.
                    return !value.name.empty() && value.framesPerFlash > 0 ||
                           invalid("has an invalid cameo-flash command");
                }
                return invalid("has an unsupported UI command");
            }
            else if constexpr (std::is_same_v<Value, ScriptClientOptionsAction>)
            {
                switch (value.command)
                {
                case ScriptClientOptionCommand::SetFrameRateLimit:
                case ScriptClientOptionCommand::SetOcclusionMode:
                case ScriptClientOptionCommand::SetDrawIconUiMode:
                case ScriptClientOptionCommand::SetDynamicParticleLodMode:
                    return true;
                }
                return invalid("has an unsupported client-options command");
            }
            else if constexpr (std::is_same_v<Value, ScriptCommandBarOverrideAction>)
            {
                // COMMANDBAR_* resolves the named ObjectType and
                // CommandButton only at the confirmed session boundary.
                // Unknown content and every signed add-slot value are valid
                // legacy no-ops, so immutable Program validation checks only
                // the action shape rather than inventing content/range errors.
                if (value.commandButtonName.empty() || value.objectTypeName.empty()) {
                    return invalid("has an empty command-bar object/button name");
                }
                switch (value.command)
                {
                case ScriptCommandBarOverrideCommand::RemoveButtonFromObjectType:
                case ScriptCommandBarOverrideCommand::AddButtonToObjectTypeSlot:
                    return true;
                }
                return invalid("has an unsupported command-bar override command");
            }
            else if constexpr (std::is_same_v<Value, ScriptMapPresentationAction>)
            {
                switch (value.command)
                {
                case ScriptMapPresentationCommand::CreateRadarEvent:
                    return finiteVec3(value.position) || invalid("has a non-finite radar-event position");
                case ScriptMapPresentationCommand::RefreshRadar:
                    return value.waypointName.empty() && value.playerName.empty() &&
                           value.revealName.empty() ||
                           invalid("has unrelated data on a radar-refresh command");
                case ScriptMapPresentationCommand::SetBorderShroud:
                    return value.waypointName.empty() && value.playerName.empty() &&
                           value.revealName.empty() ||
                           invalid("has unrelated data on a border-shroud command");
                case ScriptMapPresentationCommand::SetRadarHidden:
                case ScriptMapPresentationCommand::SetRadarForced:
                    return true;
                case ScriptMapPresentationCommand::SetBoundary:
                    return true;
                case ScriptMapPresentationCommand::RevealAtWaypoint:
                case ScriptMapPresentationCommand::ShroudAtWaypoint:
                    return !value.waypointName.empty() ||
                           invalid("has an invalid waypoint visibility command");
                case ScriptMapPresentationCommand::RevealAll:
                case ScriptMapPresentationCommand::RevealAllPermanently:
                case ScriptMapPresentationCommand::UndoRevealAllPermanently:
                case ScriptMapPresentationCommand::ShroudAll:
                    return true;
                case ScriptMapPresentationCommand::RevealPermanentlyAtWaypoint:
                    return !value.waypointName.empty() &&
                           !value.revealName.empty() ||
                           invalid("has an invalid named permanent reveal command");
                case ScriptMapPresentationCommand::UndoRevealPermanentlyAtWaypoint:
                    return !value.revealName.empty() ||
                           invalid("has an empty named permanent reveal command");
                }
                return invalid("has an unsupported map-presentation command");
            }
            else if constexpr (std::is_same_v<Value, ScriptObjectPresentationAction>)
            {
                if (value.target.name.empty())
                    return invalid("has an empty object-presentation target");
                switch (value.target.kind)
                {
                case ScriptObjectPresentationTargetKind::NamedObject:
                case ScriptObjectPresentationTargetKind::Team:
                    break;
                default:
                    return invalid("has an unsupported object-presentation target kind");
                }
                switch (value.command)
                {
                case ScriptObjectPresentationCommand::CreateRadarEvent:
                    // ScriptActions casts this authored Int directly to
                    // RadarEventType. Preserve unfamiliar mod values rather
                    // than incorrectly making the enclosing script invalid.
                    return true;
                case ScriptObjectPresentationCommand::Flash:
                    // Negative/zero legacy durations are accepted source
                    // data and become a silent no-op at the session boundary,
                    // just as Drawable receives a zero flash count.
                    return true;
                case ScriptObjectPresentationCommand::SetCustomIndicatorColor:
                    return value.target.kind == ScriptObjectPresentationTargetKind::NamedObject ||
                           invalid("uses a Team target for named custom indicator colour");
                case ScriptObjectPresentationCommand::SetEmoticon:
                    return !value.emoticonName.empty() &&
                               std::isfinite(value.emoticonDurationSeconds) ||
                           invalid("has an invalid emoticon template or duration");
                case ScriptObjectPresentationCommand::SetAmbientSoundEnabled:
                case ScriptObjectPresentationCommand::SetSpecialPowerDisplayVisible:
                    return value.target.kind == ScriptObjectPresentationTargetKind::NamedObject ||
                           invalid("uses a Team target for named object presentation control");
                }
                return invalid("has an unsupported object-presentation command");
            }
            else if constexpr (std::is_same_v<Value, ScriptForceObjectSelectionAction>)
            {
                // Team/ObjectType are mandatory legacy selectors.  DIALOG is
                // intentionally optional, but none of these text values may
                // smuggle an embedded NUL through a hand-authored Program
                // into the local presentation journal.
                const auto validText = [](const container::String& text, bool allowEmpty) noexcept {
                    return (allowEmpty || !text.empty()) &&
                        text.size() <= kMaximumScriptPresentationNameLength &&
                        text.find('\0') == container::String::npos;
                };
                return validText(value.teamName, false) &&
                           validText(value.objectTypeName, false) &&
                           validText(value.audioEventName, true) ||
                       invalid("has an invalid force-object-selection action");
            }
            else if constexpr (std::is_same_v<Value, ScriptViewCompatibilityAction>)
            {
                switch (value.command)
                {
                case ScriptViewCompatibilityCommand::SetTerrainOversizeTiles:
                    // This raw signed INT is intentionally retained without
                    // a range clamp. The current DX12 terrain path never
                    // allocates from it, so validation must not invent a
                    // legacy W3D terrain-window limit.
                    return std::isfinite(value.guardBandX) &&
                               std::isfinite(value.guardBandY) ||
                           invalid("has non-finite unrelated terrain-oversize data");
                case ScriptViewCompatibilityCommand::SetGuardBandBias:
                    return std::isfinite(value.guardBandX) &&
                               std::isfinite(value.guardBandY) ||
                           invalid("has a non-finite view guardband");
                }
                return invalid("has an unsupported view-compatibility command");
            }
            else if constexpr (std::is_same_v<Value, ScriptCameraAction>)
            {
                if (!finiteVec3(value.position) || !finiteVec3(value.target) ||
                    !std::isfinite(value.value) || !std::isfinite(value.secondaryValue) ||
                    !std::isfinite(value.tertiaryValue)) {
                    return invalid("has a non-finite camera effect");
                }
                bool validShape = true;
                switch (value.command) {
                case ScriptCameraCommand::SetPose:
                    break;
                case ScriptCameraCommand::MoveTo:
                case ScriptCameraCommand::MoveAlongWaypointPath:
                case ScriptCameraCommand::LookTowardWaypoint:
                case ScriptCameraCommand::ModifyLookToward:
                case ScriptCameraCommand::ModifyFinalLookToward:
                case ScriptCameraCommand::Reset:
                    validShape = !value.waypointName.empty();
                    break;
                case ScriptCameraCommand::Setup:
                    validShape = !value.waypointName.empty() && !value.lookAtWaypointName.empty();
                    break;
                case ScriptCameraCommand::Zoom:
                case ScriptCameraCommand::Pitch:
                case ScriptCameraCommand::Rotate:
                case ScriptCameraCommand::ModifyFinalZoom:
                case ScriptCameraCommand::ModifyFinalPitch:
                case ScriptCameraCommand::MoveToSelection:
                case ScriptCameraCommand::FreezeAngle:
                case ScriptCameraCommand::FreezeTimeDuringMotion:
                case ScriptCameraCommand::ModifyFinalSpeedMultiplier:
                case ScriptCameraCommand::ModifyRollingAverage:
                case ScriptCameraCommand::SetDefault:
                    break;
                case ScriptCameraCommand::LookTowardNamedObject:
                case ScriptCameraCommand::FollowNamedObject:
                case ScriptCameraCommand::TetherNamedObject:
                    validShape = !value.objectName.empty();
                    break;
                case ScriptCameraCommand::StopFollow:
                case ScriptCameraCommand::SetLetterbox:
                case ScriptCameraCommand::StopTether:
                    break;
                }
                return validShape || invalid("has an invalid camera command shape");
            }
            else if constexpr (std::is_same_v<Value, ScriptCameraSlaveAction>)
            {
                // Disable has no authored target. Enable must retain both
                // strings so the renderer can select the exact animated W3D
                // bone after the ObjectId boundary; a root transform is not
                // an acceptable substitute for a missing bone.
                return !value.enabled ||
                           (!value.objectName.empty() && !value.boneName.empty()) ||
                       invalid("has an invalid camera-slave command shape");
            }
            else if constexpr (std::is_same_v<Value, ScriptScreenShakeAction>)
            {
                return static_cast<uint8_t>(value.intensity) <
                           static_cast<uint8_t>(ScriptScreenShakeIntensity::Count) ||
                       invalid("has an invalid screen-shake intensity");
            }
            else if constexpr (std::is_same_v<Value, ScriptLocalizedCameraShakeAction>)
            {
                return !value.waypointName.empty() && std::isfinite(value.amplitude) &&
                           std::isfinite(value.radius) ||
                       invalid("has an invalid localized camera-shake action");
            }
            else if constexpr (std::is_same_v<Value, ScriptScreenFadeAction>)
            {
                // ScriptEngine::setFade accepts signed frame counts and does
                // not constrain the authored real range.  Keep that legacy
                // state-machine shape, while rejecting only values that
                // cannot safely cross the immutable Program boundary.
                return static_cast<uint8_t>(value.blendMode) <
                           static_cast<uint8_t>(ScriptScreenFadeBlendMode::Count) &&
                           std::isfinite(value.minimumIntensity) &&
                           std::isfinite(value.maximumIntensity) ||
                       invalid("has an invalid screen-fade effect");
            }
            else if constexpr (std::is_same_v<Value, ScriptBlackAndWhiteAction>)
            {
                // The raw signed frame count intentionally has no range
                // restriction: ScreenBWFilter itself gives <= 0 its legacy
                // immediate-completion behavior.
                return true;
            }
            else if constexpr (std::is_same_v<Value, ScriptMotionBlurAction>)
            {
                const bool validMode = static_cast<uint8_t>(value.mode) <
                    static_cast<uint8_t>(ScriptMotionBlurMode::Count);
                const bool validWaypoint =
                    value.mode != ScriptMotionBlurMode::ZoomJump ||
                    !value.waypointName.empty();
                return validMode && validWaypoint ||
                       invalid("has an invalid motion-blur action");
            }
            else if constexpr (std::is_same_v<Value, ScriptSkyboxAction>)
            {
                return true;
            }
            else if constexpr (std::is_same_v<Value, ScriptTreeSwayAction>)
            {
                // ScriptEngine::setSway only clamps period at execution time.
                // Preserve all finite direction/intensity/lean/randomness
                // values, including useful mod values outside editor ranges.
                return std::isfinite(value.directionRadians) &&
                           std::isfinite(value.intensityRadians) &&
                           std::isfinite(value.leanRadians) &&
                           std::isfinite(value.randomness) ||
                       invalid("has an invalid tree-sway action");
            }
            else if constexpr (std::is_same_v<Value, ScriptWeatherAction>)
            {
                return true;
            }
            else if constexpr (std::is_same_v<Value, ScriptInfantryLightingAction>)
            {
                // RESET intentionally carries no scalar.  A Set action must
                // retain RefCode's strictly-positive lighting multiplier;
                // accepting zero would blacken infantry instead of exposing
                // malformed map data at the immutable Program boundary.
                return !value.overrideScale ||
                           (std::isfinite(*value.overrideScale) && *value.overrideScale > 0.0f) ||
                       invalid("has an invalid infantry-lighting override");
            }
            else if constexpr (std::is_same_v<Value, ScriptWaterAction>)
            {
                return !value.waterName.empty() &&
                           value.damagePerSecond >= math::q32_32{} ||
                       invalid("has an invalid water effect");
            }
            else if constexpr (std::is_same_v<Value, ScriptIssueOrderAction>)
            {
                bool validActors = false;
                switch (value.actorSelector)
                {
                case ScriptOrderActorSelector::NamedObjects:
                    validActors = !value.actorNames.empty() && value.teamName.empty() &&
                        value.playerName.empty() &&
                        std::none_of(value.actorNames.begin(), value.actorNames.end(),
                                     [](const container::String& name) { return name.empty(); });
                    break;
                case ScriptOrderActorSelector::ScenarioTeam:
                    validActors = value.actorNames.empty() && !value.teamName.empty() &&
                        value.playerName.empty();
                    break;
                case ScriptOrderActorSelector::PlayerAssets:
                    validActors = value.actorNames.empty() && value.teamName.empty() &&
                        !value.playerName.empty();
                    break;
                }
                const bool hasExactlyOneMoveTarget = value.targetPosition.has_value() !=
                    !value.targetWaypointName.empty();
                bool validShape = validActors;
                const bool waypointIndividuals =
                    value.moveRouteSubtype == ScriptMoveRouteSubtype::WaypointPathIndividuals ||
                    value.moveRouteSubtype == ScriptMoveRouteSubtype::WaypointPathIndividualsExact;
                const bool waypointTeam =
                    value.moveRouteSubtype == ScriptMoveRouteSubtype::WaypointPathTeam ||
                    value.moveRouteSubtype == ScriptMoveRouteSubtype::WaypointPathTeamExact;
                const bool waypointWanderPanic =
                    value.moveRouteSubtype == ScriptMoveRouteSubtype::WanderWaypointPath ||
                    value.moveRouteSubtype == ScriptMoveRouteSubtype::PanicWaypointPath;
                if (waypointIndividuals || waypointTeam || waypointWanderPanic)
                {
                    const bool validWaypointActors =
                        (value.actorSelector == ScriptOrderActorSelector::NamedObjects &&
                         waypointIndividuals && value.actorNames.size() == 1u) ||
                        (value.actorSelector == ScriptOrderActorSelector::ScenarioTeam &&
                         (waypointTeam || waypointIndividuals || waypointWanderPanic));
                    validShape = validShape && value.kind == ScriptOrderKind::Move &&
                        validWaypointActors && !value.queued &&
                        !value.targetWaypointName.empty() && value.targetObjectName.empty() &&
                        value.targetTeamName.empty() && value.targetAreaName.empty() &&
                        !value.targetPosition && value.contentName.empty() &&
                        value.tacticalAttackSubtype == ScriptTacticalAttackSubtype::None &&
                        !value.forceAttack && !value.allArmyHunt &&
                        !value.useTeamCommonTarget;
                }
                else if (value.moveRouteSubtype == ScriptMoveRouteSubtype::Direct)
                {
                    switch (value.kind)
                    {
                    case ScriptOrderKind::Move:
                        validShape = validShape && hasExactlyOneMoveTarget;
                        break;
                    case ScriptOrderKind::Stop:
                        validShape = validShape && value.targetObjectName.empty() &&
                            !value.targetPosition && value.targetWaypointName.empty() &&
                            value.contentName.empty() && !value.queued &&
                            (!value.disbandAfterStop ||
                             value.actorSelector == ScriptOrderActorSelector::ScenarioTeam);
                        break;
                    case ScriptOrderKind::Attack:
                        validShape = validShape && value.targetWaypointName.empty() &&
                            (!value.targetObjectName.empty() || value.targetPosition.has_value());
                        break;
                    case ScriptOrderKind::Build:
                        validShape = validShape && value.targetWaypointName.empty() &&
                            !value.contentName.empty() && value.targetPosition.has_value();
                        break;
                    case ScriptOrderKind::CommandButton:
                        validShape = validShape && value.targetWaypointName.empty() &&
                            !value.contentName.empty();
                        break;
                    case ScriptOrderKind::SpecialPower: {
                        const uint32_t targetCount =
                            (!value.targetObjectName.empty() ? 1u : 0u) +
                            (value.targetPosition ? 1u : 0u) +
                            (!value.targetWaypointName.empty() ? 1u : 0u);
                        validShape = validShape && !value.contentName.empty() &&
                            targetCount == 1u;
                        break;
                    }
                    case ScriptOrderKind::TacticalAttack: {
                        const bool hunt = value.tacticalAttackSubtype ==
                            ScriptTacticalAttackSubtype::Hunt;
                        const bool guard = value.tacticalAttackSubtype ==
                            ScriptTacticalAttackSubtype::Guard;
                        const bool tunnelGuard = value.tacticalAttackSubtype ==
                            ScriptTacticalAttackSubtype::GuardTunnelNetwork;
                        const bool attackSquad = value.tacticalAttackSubtype ==
                            ScriptTacticalAttackSubtype::AttackSquad;
                        const bool attackArea = value.tacticalAttackSubtype ==
                            ScriptTacticalAttackSubtype::AttackArea;
                        const uint32_t guardAnchorCount =
                            (!value.targetObjectName.empty() ? 1u : 0u) +
                            (!value.targetWaypointName.empty() ? 1u : 0u) +
                            (!value.targetAreaName.empty() ? 1u : 0u);
                        const bool validGuardActors = !(guard || tunnelGuard) ||
                            value.actorSelector == ScriptOrderActorSelector::ScenarioTeam ||
                            value.actorNames.size() == 1u;
                        const bool validTacticalTarget =
                            (hunt && value.targetTeamName.empty() &&
                             value.targetAreaName.empty()) ||
                            (guard && value.targetTeamName.empty()) ||
                            (tunnelGuard && value.targetTeamName.empty() &&
                             value.targetAreaName.empty()) ||
                            (attackSquad && !value.targetTeamName.empty() &&
                             value.targetAreaName.empty()) ||
                            (attackArea && value.targetTeamName.empty() &&
                             !value.targetAreaName.empty());
                        // The first Guard slice is normal-mode guarding at
                        // each actor's current position. Position/Object/Area,
                        // Tunnel, framecount, and player-authored variants
                        // must acquire distinct typed shapes before admission.
                        validShape = validShape &&
                            (hunt || guard || tunnelGuard || attackSquad || attackArea) &&
                            validGuardActors && validTacticalTarget &&
                            !value.targetPosition &&
                            (((guard && guardAnchorCount <= 1u) ||
                              (tunnelGuard && guardAnchorCount == 0u)) ||
                             (!(guard || tunnelGuard) && value.targetObjectName.empty() &&
                              value.targetWaypointName.empty())) &&
                            value.contentName.empty() &&
                            (!value.disbandAfterStop) &&
                            (value.actorSelector != ScriptOrderActorSelector::PlayerAssets ||
                             (hunt && value.allArmyHunt)) &&
                            (!(guard || tunnelGuard) || (!value.allArmyHunt &&
                                        !value.useTeamCommonTarget)) &&
                            (!(attackSquad || attackArea) ||
                             (!value.allArmyHunt && !value.useTeamCommonTarget));
                        break;
                    }
                    }
                }
                else
                    validShape = false;
                validShape = validShape && (!value.forceAttack || value.kind == ScriptOrderKind::Attack);
                validShape = validShape &&
                    (!value.disbandAfterStop || value.kind == ScriptOrderKind::Stop);
                if (value.actorSelector == ScriptOrderActorSelector::PlayerAssets) {
                    validShape = validShape &&
                        value.kind == ScriptOrderKind::TacticalAttack &&
                        value.tacticalAttackSubtype == ScriptTacticalAttackSubtype::Hunt &&
                        value.allArmyHunt && !value.useTeamCommonTarget;
                }
                if (value.kind != ScriptOrderKind::TacticalAttack) {
                    validShape = validShape &&
                        value.tacticalAttackSubtype == ScriptTacticalAttackSubtype::None &&
                        value.targetTeamName.empty() && value.targetAreaName.empty() &&
                        !value.allArmyHunt && !value.useTeamCommonTarget;
                }
                return validShape || invalid("has an invalid script-issued order");
            }
            else if constexpr (std::is_same_v<
                                   Value,
                                   ScriptFireWeaponFollowingWaypointPathAction>)
            {
                return !value.objectName.empty() &&
                           !value.waypointPathName.empty() ||
                       invalid("has an empty waypoint-projectile selector");
            }
            else if constexpr (std::is_same_v<
                                   Value,
                                   ScriptCreateReinforcementTeamAction>)
            {
                return !value.teamName.empty() &&
                           !value.destinationWaypointName.empty() ||
                       invalid("has an empty reinforcement Team selector");
            }
            else if constexpr (std::is_same_v<Value, ScriptBuildTeamAction>)
            {
                return !value.teamName.empty() ||
                       invalid("has an empty Team production selector");
            }
            else if constexpr (std::is_same_v<
                                   Value, ScriptGuardSupplyCenterAction>)
            {
                return !value.teamName.empty() ||
                       invalid("has an empty supply-guard Team selector");
            }
            else if constexpr (std::is_same_v<Value, ScriptRecruitTeamAction>)
            {
                return !value.teamName.empty() ||
                       invalid("has an invalid Team recruitment selector");
            }
            else if constexpr (std::is_same_v<Value, ScriptUseCommandButtonAction>)
            {
                const bool validActors =
                    (value.actorSelector == ScriptOrderActorSelector::NamedObjects &&
                     value.actorNames.size() == 1u &&
                     !value.actorNames.front().empty() && value.teamName.empty()) ||
                    (value.actorSelector == ScriptOrderActorSelector::ScenarioTeam &&
                     value.actorNames.empty() && !value.teamName.empty());
                bool validTarget = false;
                switch (value.targetKind)
                {
                case ScriptCommandButtonTargetKind::None:
                    validTarget = value.targetObjectName.empty() &&
                        value.targetWaypointName.empty() &&
                        value.targetFilter.empty();
                    break;
                case ScriptCommandButtonTargetKind::NamedObject:
                    validTarget = !value.targetObjectName.empty() &&
                        value.targetWaypointName.empty() &&
                        value.targetFilter.empty();
                    break;
                case ScriptCommandButtonTargetKind::Waypoint:
                    validTarget = value.targetObjectName.empty() &&
                        !value.targetWaypointName.empty() &&
                        value.targetFilter.empty();
                    break;
                case ScriptCommandButtonTargetKind::WaypointPath:
                    validTarget = value.actorSelector ==
                            ScriptOrderActorSelector::NamedObjects &&
                        value.actorNames.size() == 1u &&
                        value.targetObjectName.empty() &&
                        !value.targetWaypointName.empty() &&
                        value.targetFilter.empty();
                    break;
                case ScriptCommandButtonTargetKind::NearestEnemyUnit:
                case ScriptCommandButtonTargetKind::NearestGarrisonedBuilding:
                case ScriptCommandButtonTargetKind::NearestEnemyBuilding:
                case ScriptCommandButtonTargetKind::MostValuableEnemy:
                    validTarget = value.targetObjectName.empty() &&
                        value.targetWaypointName.empty() &&
                        value.targetFilter.empty();
                    break;
                case ScriptCommandButtonTargetKind::NearestKindOf:
                case ScriptCommandButtonTargetKind::NearestEnemyBuildingClass:
                case ScriptCommandButtonTargetKind::NearestObjectType:
                    validTarget = value.targetObjectName.empty() &&
                        value.targetWaypointName.empty() &&
                        !value.targetFilter.empty();
                    break;
                }
                const bool validPolicy =
                    (value.actorPolicy == ScriptCommandButtonActorPolicy::All &&
                     value.actorPercentage == math::q32_32{int32_t{100}}) ||
                    (value.actorPolicy ==
                         ScriptCommandButtonActorPolicy::PartialUsable &&
                     value.actorSelector ==
                         ScriptOrderActorSelector::ScenarioTeam &&
                     value.targetKind == ScriptCommandButtonTargetKind::None &&
                     value.preselectSourceAndTarget);
                const bool nearestTarget =
                    value.targetKind >=
                        ScriptCommandButtonTargetKind::NearestEnemyUnit;
                return validActors && !value.buttonName.empty() && validTarget &&
                           validPolicy &&
                           (!nearestTarget || value.preselectSourceAndTarget) ||
                       invalid("has an invalid command-button action");
            }
            else if constexpr (std::is_same_v<Value, ScriptFacingAction>)
            {
                const bool validActors =
                    (value.actorSelector ==
                         ScriptOrderActorSelector::NamedObjects &&
                     value.actorNames.size() == 1u &&
                     !value.actorNames.front().empty() &&
                     value.teamName.empty()) ||
                    (value.actorSelector ==
                         ScriptOrderActorSelector::ScenarioTeam &&
                     value.actorNames.empty() &&
                     !value.teamName.empty());
                return validActors && !value.targetName.empty() ||
                       invalid("has an invalid script-facing target");
            }
            else if constexpr (std::is_same_v<Value, ScriptAIBehaviorMutationAction>)
            {
                if (value.targetName.empty())
                    return invalid("has an empty AI-behavior target");
                if (value.mutation ==
                    ScriptAIBehaviorMutationKind::ApplyAttackPrioritySet) {
                    return !value.attackPrioritySet.empty() &&
                               value.commandButton.empty() &&
                               value.attitude == 0 ||
                           invalid("has an empty attack-priority set name");
                }
                if (value.mutation ==
                    ScriptAIBehaviorMutationKind::SetAttitude) {
                    return value.attackPrioritySet.empty() &&
                               value.commandButton.empty() &&
                               value.attitude >= -2 && value.attitude <= 2 ||
                           invalid("has an invalid AI attitude");
                }
                if (value.mutation ==
                        ScriptAIBehaviorMutationKind::IncreaseTeamProductionPriority ||
                    value.mutation ==
                        ScriptAIBehaviorMutationKind::DecreaseTeamProductionPriority) {
                    return value.targetKind == ScriptAIBehaviorTargetKind::ScenarioTeam &&
                           value.attackPrioritySet.empty() &&
                           value.commandButton.empty() && value.attitude == 0 ||
                       invalid("priority mutation requires a ScenarioTeam target");
                }
                if (value.mutation == ScriptAIBehaviorMutationKind::WanderInPlace) {
                    return value.targetKind == ScriptAIBehaviorTargetKind::ScenarioTeam &&
                           value.attackPrioritySet.empty() &&
                           value.commandButton.empty() && value.attitude == 0 ||
                       invalid("wander-in-place requires a ScenarioTeam target");
                }
                return value.targetKind ==
                           ScriptAIBehaviorTargetKind::ScenarioTeam &&
                           value.attackPrioritySet.empty() &&
                           !value.commandButton.empty() && value.attitude == 0 ||
                       invalid("has an invalid command-button hunt request");
            }
            else if constexpr (std::is_same_v<Value, ScriptAttackPriorityMutationAction>)
            {
                if (value.setName.empty())
                    return invalid("has an empty attack-priority set name");
                const bool selectorExpected =
                    value.mutation != ScriptAttackPriorityMutationKind::Default;
                return selectorExpected == !value.selector.empty() ||
                       invalid("has an invalid attack-priority selector");
            }
            else if constexpr (std::is_same_v<Value, ScriptStoppingDistanceAction>)
            {
                return !value.targetName.empty() ||
                       invalid("has an invalid stopping-distance target or value");
            }
            else if constexpr (std::is_same_v<Value, ScriptMoveTowardsNearestAction>)
            {
                const bool validActor =
                    (value.actorSelector == ScriptOrderActorSelector::NamedObjects &&
                     !value.actorName.empty() && value.teamName.empty()) ||
                    (value.actorSelector == ScriptOrderActorSelector::ScenarioTeam &&
                     value.actorName.empty() && !value.teamName.empty());
                return validActor && !value.objectType.empty() && !value.triggerArea.empty() ||
                       invalid("has an invalid nearest-object movement request");
            }
            else if constexpr (std::is_same_v<Value, ScriptSpecialPowerCountdownAction>)
            {
                return (!value.objectName.empty() && !value.specialPower.empty()) ||
                       invalid("has an empty SpecialPower countdown target");
            }
            else if constexpr (std::is_same_v<Value, ScriptWarehouseValueAction>)
            {
                return !value.objectName.empty() ||
                       invalid("has an empty warehouse value target");
            }
            else if constexpr (std::is_same_v<Value, ScriptCaveIndexAction>)
            {
                return (!value.objectName.empty() && value.caveIndex >= 0) ||
                       invalid("has an empty cave target or negative cave index");
            }
            else if constexpr (std::is_same_v<Value, ScriptCreateObjectAction>)
            {
                const bool hasPosition = value.position.has_value();
                const bool hasWaypoint = !value.waypointName.empty();
                const bool valid = !value.templateName.empty() && !value.teamName.empty() &&
                    hasPosition != hasWaypoint;
                return valid || invalid("has an invalid scripted object-creation request");
            }
            else if constexpr (std::is_same_v<Value, ScriptDestroyNamedObjectAction>)
            {
                return !value.objectName.empty() || invalid("destroys an empty named-object reference");
            }
            else if constexpr (std::is_same_v<Value, ScriptLifecycleAction>)
            {
                return !value.targetName.empty() ||
                       invalid("has an empty lifecycle target reference");
            }
            else if constexpr (std::is_same_v<Value, ScriptContainmentAction>)
            {
                return !value.targetName.empty() ||
                       invalid("has an empty containment target reference");
            }
            else if constexpr (std::is_same_v<Value, ScriptContainmentEnterAction>)
            {
                const bool named =
                    value.kind == ScriptContainmentEnterActionKind::NamedEnterNamed ||
                    value.kind == ScriptContainmentEnterActionKind::NamedGarrisonSpecific ||
                    value.kind == ScriptContainmentEnterActionKind::NamedGarrisonNearest;
                const bool team =
                    value.kind == ScriptContainmentEnterActionKind::LoadTeamTransports ||
                    value.kind == ScriptContainmentEnterActionKind::TeamCaptureNearestUnmanned ||
                    value.kind == ScriptContainmentEnterActionKind::TeamEnterNamed ||
                    value.kind == ScriptContainmentEnterActionKind::TeamGarrisonSpecific ||
                    value.kind == ScriptContainmentEnterActionKind::TeamGarrisonNearest;
                const bool player =
                    value.kind == ScriptContainmentEnterActionKind::PlayerGarrisonAll;
                const bool specific =
                    value.kind == ScriptContainmentEnterActionKind::NamedEnterNamed ||
                    value.kind == ScriptContainmentEnterActionKind::TeamEnterNamed ||
                    value.kind == ScriptContainmentEnterActionKind::TeamGarrisonSpecific ||
                    value.kind == ScriptContainmentEnterActionKind::NamedGarrisonSpecific;
                const bool valid =
                    named == value.object.valid() &&
                    team == value.team.valid() &&
                    player == !value.player.empty() &&
                    specific == value.container.valid();
                return valid || invalid("has an invalid containment-enter selector set");
            }
            else if constexpr (std::is_same_v<Value, ScriptTransferOwnershipAction>)
            {
                bool validTarget = false;
                switch (value.selector)
                {
                case ScriptOwnershipTransferSelector::NamedObject:
                    validTarget = !value.objectName.empty() && value.teamName.empty() &&
                        value.sourcePlayer.empty() && value.targetTeamName.empty() &&
                        !value.targetPlayer.empty();
                    break;
                case ScriptOwnershipTransferSelector::ScenarioTeam:
                    validTarget = value.objectName.empty() && !value.teamName.empty() &&
                        value.sourcePlayer.empty() && value.targetTeamName.empty() &&
                        !value.targetPlayer.empty();
                    break;
                case ScriptOwnershipTransferSelector::PlayerAssets:
                    validTarget = value.objectName.empty() && value.teamName.empty() &&
                        value.targetTeamName.empty() && !value.sourcePlayer.empty() &&
                        !value.targetPlayer.empty();
                    break;
                case ScriptOwnershipTransferSelector::MergeScenarioTeam:
                    validTarget = value.objectName.empty() && !value.teamName.empty() &&
                        !value.targetTeamName.empty() && value.sourcePlayer.empty() &&
                        value.targetPlayer.empty();
                    break;
                }
                return validTarget ||
                       invalid("has an invalid scripted ownership-transfer target");
            }
            else if constexpr (std::is_same_v<Value, ScriptDamageAction>)
            {
                bool validTarget = false;
                switch (value.targetSelector)
                {
                case ScriptDamageTargetSelector::NamedObject:
                    validTarget = !value.objectName.empty() && value.teamName.empty();
                    break;
                case ScriptDamageTargetSelector::ScenarioTeam:
                    validTarget = value.objectName.empty() && !value.teamName.empty();
                    break;
                }
                return validTarget ||
                       invalid("has an invalid scripted damage target");
            }
            else if constexpr (std::is_same_v<Value, ScriptGrantObjectUpgradeAction>)
            {
                return (!value.objectName.empty() && !value.upgradeName.empty()) ||
                       invalid("has an empty object or upgrade reference");
            }
            else if constexpr (std::is_same_v<Value, ScriptObjectStateMutationAction>)
            {
                if (value.targetName.empty())
                    return invalid("has an empty object-state target");
                if (value.targetKind == ScriptObjectStateTargetKind::ScenarioTeam &&
                    (value.mutation == ScriptObjectStateMutationKind::Held ||
                     value.mutation == ScriptObjectStateMutationKind::RailroadHeld))
                    return invalid("applies a named-only object-state mutation to a Team");
                if (value.mutation ==
                        ScriptObjectStateMutationKind::TeamRecruitable &&
                    value.targetKind !=
                        ScriptObjectStateTargetKind::ScenarioTeam)
                    return invalid("applies Team recruitability to a named object");
                return true;
            }
            else if constexpr (std::is_same_v<Value, ScriptGlobalObjectAction>)
            {
                return true;
            }
            else if constexpr (std::is_same_v<Value, ScriptBoobyTrapAction>)
            {
                return (!value.targetName.empty() && !value.templateName.empty()) ||
                       invalid("has an empty booby-trap target or template");
            }
            else if constexpr (std::is_same_v<Value, ScriptToppleDirectionAction>)
            {
                return !value.objectName.empty() ||
                       invalid("has an empty object or non-finite topple direction");
            }
            else if constexpr (std::is_same_v<Value, ScriptModifyObjectTypeListAction>)
            {
                return (!value.listName.empty() && !value.objectType.empty()) ||
                       invalid("has an empty object-type list or member reference");
            }
            else if constexpr (std::is_same_v<Value, ScriptSetPlayerCashAction> ||
                               std::is_same_v<Value, ScriptAdjustPlayerCashAction>)
            {
                return !value.player.empty() || invalid("writes an empty player-cash reference");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerSellEverythingAction>)
            {
                return !value.player.empty() || invalid("has an empty sell-everything player reference");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerRepairStructureAction>)
            {
                return (!value.player.empty() && value.structure.valid()) ||
                       invalid("has an invalid player-repair-structure action");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerBuildUpgradeAction>)
            {
                return (!value.player.empty() && !value.upgrade.empty()) ||
                       invalid("has an empty AI-player upgrade request");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerBuildObjectNearTeamAction>)
            {
                return (!value.player.empty() && !value.objectType.empty() &&
                        !value.teamName.empty()) ||
                       invalid("has an empty AI-player Team construction request");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerBuildSupplyCenterAction>)
            {
                return (!value.player.empty() && !value.objectType.empty()) ||
                       invalid("has an empty AI-player supply construction request");
            }
            else if constexpr (std::is_same_v<Value, ScriptSkirmishBuildBuildingAction>)
            {
                return !value.objectType.empty() ||
                       invalid("has an empty Skirmish building request");
            }
            else if constexpr (std::is_same_v<Value, ScriptSkirmishApproachAction>)
            {
                return (!value.teamName.empty() && !value.pathPrefix.empty()) ||
                       invalid("has an empty Skirmish approach-path request");
            }
            else if constexpr (std::is_same_v<Value, ScriptSkirmishPerimeterBuildAction>)
            {
                return (value.useFactionBaseDefense ||
                        !value.objectType.empty()) ||
                       invalid("has an empty Skirmish perimeter building request");
            }
            else if constexpr (std::is_same_v<
                                   Value,
                                   ScriptSkirmishFireSpecialPowerAtMostCostAction>)
            {
                return (!value.player.empty() && !value.specialPower.empty()) ||
                       invalid("has an empty Skirmish special-power request");
            }
            else if constexpr (std::is_same_v<
                                   Value,
                                   ScriptSkirmishAttackNearestValueGroupAction>)
            {
                return !value.teamName.empty() ||
                       invalid("has an empty Skirmish value-group Team request");
            }
            else if constexpr (std::is_same_v<
                                   Value,
                                   ScriptSkirmishMostValuableCommandButtonAction>)
            {
                return (!value.teamName.empty() && !value.buttonName.empty() &&
                        value.range >= math::q32_32{}) ||
                       invalid("has an invalid Skirmish most-valuable CommandButton request");
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerConstructionAction>)
            {
                if (value.player.empty()) return invalid("has an empty construction-policy player");
                const bool needsFactory = value.operation ==
                    ScriptPlayerConstructionOperation::SetFactoryTypeEnabled;
                return needsFactory == !value.factoryType.empty() ||
                       invalid("has an invalid construction-policy factory selector");
            }
            else if constexpr (std::is_same_v<Value, ScriptObjectBuildabilityAction>)
            {
                return !value.objectType.empty() ||
                       invalid("has an empty buildability object-type reference");
            }
            else if constexpr (std::is_same_v<Value, ScriptSetPlayerScienceAvailabilityAction>)
            {
                return (!value.player.empty() && !value.science.empty()) ||
                       invalid("has an empty player or science availability reference");
            }
            else if constexpr (std::is_same_v<Value, ScriptSetPlayerRelationshipAction>)
            {
                return (!value.sourcePlayer.empty() && !value.targetPlayer.empty()) ||
                       invalid("has an empty source or target player relationship reference");
            }
            else if constexpr (std::is_same_v<Value, ScriptRelationshipOverrideAction>)
            {
                if (value.sourceName.empty())
                    return invalid("has an empty relationship-override source");
                if (value.operation ==
                    ScriptRelationshipOverrideOperation::RemoveAllFromTeam) {
                    return value.sourceKind ==
                               ScriptRelationshipEndpointKind::ScenarioTeam &&
                           value.targetName.empty() ||
                           invalid("has an invalid remove-all relationship override");
                }
                const bool supportedDirection =
                    value.sourceKind ==
                        ScriptRelationshipEndpointKind::ScenarioTeam ||
                    value.targetKind ==
                        ScriptRelationshipEndpointKind::ScenarioTeam;
                return supportedDirection && !value.targetName.empty() ||
                       invalid("has an invalid relationship-override target");
            }
            else if constexpr (std::is_same_v<Value, ScriptGlobalCombatPolicyAction>)
            {
                return true;
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerProgressionAction>)
            {
                switch (value.operation)
                {
                case ScriptPlayerProgressionOperation::SetRankLevelLimit:
                    return value.player.empty() && value.science.empty() ||
                           invalid("has unrelated player data on a rank-level-limit action");
                case ScriptPlayerProgressionOperation::GrantScience:
                case ScriptPlayerProgressionOperation::PurchaseScience:
                    return !value.player.empty() && !value.science.empty() ||
                           invalid("has an empty player or science progression reference");
                case ScriptPlayerProgressionOperation::AddSkillPoints:
                case ScriptPlayerProgressionOperation::AdjustRankLevel:
                case ScriptPlayerProgressionOperation::SetRankLevel:
                case ScriptPlayerProgressionOperation::SelectSkillset:
                case ScriptPlayerProgressionOperation::ExcludeFromScoreScreen:
                    return !value.player.empty() && value.science.empty() ||
                           invalid("has an invalid player progression reference");
                case ScriptPlayerProgressionOperation::SetExperienceMultiplier:
                    return !value.player.empty() && value.science.empty() ||
                           invalid("has a non-finite player experience multiplier");
                }
                return invalid("has an unsupported player progression operation");
            }
            return invalid("has an unsupported action value");
        },
        action);
}

} // namespace engine::script::detail
#include "game/script/contracts/ScriptPresentationLimits.h"
