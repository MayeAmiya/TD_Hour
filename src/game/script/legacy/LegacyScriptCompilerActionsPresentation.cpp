#include "LegacyScriptCompilerInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace engine::script::legacy::detail
{

[[nodiscard]] bool appendEnableDisableActions(const container::String& targetName,
                                              bool enable,
                                              container::Vector<ScriptAction>& output,
                                              CompileContext& context,
                                              const LegacyScriptInstructionSource& source,
                                              container::StringView scriptName)
{
    bool found = false;
    // RefCode enables a matching group and script independently (group first
    // for enable; script first for disable). Preserve that ordering rather
    // than resolving a string to an arbitrary single target.
    const NamedGroup* group = findGroupByName(context, targetName);
    const NamedScript* script = findScriptByName(context, targetName);
    const auto append = [&](ScriptTarget target)
    {
        if (enable)
            output.emplace_back(ScriptEnableAction{.target = target});
        else
            output.emplace_back(ScriptDisableAction{.target = target});
    };
    if (enable)
    {
        if (group)
        {
            append(ScriptTarget::groupTarget(group->id));
            found = true;
        }
        if (script)
        {
            append(ScriptTarget::scriptTarget(script->id));
            found = true;
        }
    }
    else
    {
        if (script)
        {
            append(ScriptTarget::scriptTarget(script->id));
            found = true;
        }
        if (group)
        {
            append(ScriptTarget::groupTarget(group->id));
            found = true;
        }
    }
    if (!found)
    {
        context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                           "script '" + container::String(scriptName) + "' targets unknown script/group '" + targetName + "'",
                           source.serialized);
    }
    return found;
}

[[nodiscard]] bool appendSubroutineAction(const container::String& targetName,
                                          container::Vector<ScriptAction>& output,
                                          CompileContext& context,
                                          const LegacyScriptInstructionSource& source,
                                          container::StringView scriptName)
{
    // RefCode resolves a group before a script for CALL_SUBROUTINE.
    if (const NamedGroup* group = findGroupByName(context, targetName))
    {
        if (!group->isSubroutine)
        {
            context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                               "script '" + container::String(scriptName) + "' calls non-subroutine group '" + targetName +
                                   "'",
                               source.serialized);
            return false;
        }
        output.emplace_back(ScriptCallSubroutineAction{.target = ScriptTarget::groupTarget(group->id)});
        return true;
    }
    if (const NamedScript* script = findScriptByName(context, targetName))
    {
        if (!script->isSubroutine)
        {
            context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                               "script '" + container::String(scriptName) + "' calls non-subroutine script '" + targetName +
                                   "'",
                               source.serialized);
            return false;
        }
        output.emplace_back(ScriptCallSubroutineAction{.target = ScriptTarget::scriptTarget(script->id)});
        return true;
    }
    context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                       "script '" + container::String(scriptName) + "' calls unknown subroutine '" + targetName + "'",
                       source.serialized);
    return false;
}

[[nodiscard]] std::optional<bool> compileControlAndPresentationAction(
    const LegacyScriptInstructionSource& instruction,
    container::Vector<ScriptAction>& output,
    CompileContext& context,
    container::StringView scriptName,
    const container::String& name)
{
    if (name == "NO_OP") {
        output.emplace_back(ScriptNoOpAction{});
        return true;
    }
    if (name == "TEAM_COLLECT_NEARBY_FOR_TEAM")
    {
        // RefCode exposes this authored action and validates a Team parameter,
        // but the implementation is only a DEBUG_CRASH saying it was never
        // implemented. Preserve release semantics as a diagnosed no-op.
        const auto teamName = textParameter(
            instruction, 0, context, scriptName, name);
        if (!teamName) return false;
        context.diagnostic(
            LegacyScriptCompileDiagnosticSeverity::Info,
            "script '" + container::String(scriptName) + "' uses legacy action '" +
                name + "', which is an unimplemented RefCode debug-crash no-op",
            instruction.serialized);
        output.emplace_back(ScriptNoOpAction{});
        return true;
    }
    if (name == "PLAYER_CREATE_TEAM_FROM_CAPTURED_UNITS")
    {
        // The shipped RefCode handler resolves the Team and then returns
        // without reading the player or mutating membership. Preserve that
        // release behavior explicitly; this is compatibility with a genuine
        // original no-op, not a placeholder for future ECS work.
        const auto playerName = textParameter(
            instruction, 0, context, scriptName, name);
        const auto teamName = textParameter(
            instruction, 1, context, scriptName, name);
        if (!playerName || !teamName ||
            rejectUnsupportedDynamicPlayerSelector(
                context, scriptName, name, *playerName,
                instruction.serialized) ||
            rejectDynamicScriptContextSelector(
                context, scriptName, name, *teamName,
                instruction.serialized)) {
            return false;
        }
        context.diagnostic(
            LegacyScriptCompileDiagnosticSeverity::Info,
            "script '" + container::String(scriptName) +
                "' uses original no-op action '" + name + "'",
            instruction.serialized);
        output.emplace_back(ScriptNoOpAction{});
        return true;
    }
    if (name == "TEAM_SET_STATE")
    {
        const auto teamName = textParameter(
            instruction, 0, context, scriptName, name);
        const auto state = textParameter(
            instruction, 1, context, scriptName, name);
        if (!teamName || !state) return false;
        output.emplace_back(ScriptSetTeamCustomStateAction{
            .team = teamSelector(*teamName),
            .state = *state,
        });
        return true;
    }
    if (name == "DEBUG_MESSAGE_BOX" || name == "DEBUG_STRING" || name == "DEBUG_CRASH_BOX")
    {
        const auto text = textParameter(instruction, 0, context, scriptName, name);
        if (!text)
            return false;
        ScriptDebugMessageKind kind = ScriptDebugMessageKind::Log;
        if (name == "DEBUG_MESSAGE_BOX")
            kind = ScriptDebugMessageKind::Dialog;
        else if (name == "DEBUG_CRASH_BOX")
            kind = ScriptDebugMessageKind::CrashBox;
        output.emplace_back(ScriptDebugMessageAction{.text = *text, .kind = kind});
        return true;
    }
    if (name == "SET_FLAG")
    {
        const auto flag = textParameter(instruction, 0, context, scriptName, name);
        const auto value = integerParameter(instruction, 1, context, scriptName, name);
        if (!flag || !value)
            return false;
        output.emplace_back(ScriptSetFlagAction{.flag = *flag, .value = *value != 0});
        return true;
    }
    if (name == "SET_COUNTER")
    {
        const auto counter = textParameter(instruction, 0, context, scriptName, name);
        const auto value = integerParameter(instruction, 1, context, scriptName, name);
        if (!counter || !value)
            return false;
        output.emplace_back(ScriptSetCounterAction{.counter = *counter, .value = *value});
        return true;
    }
    if (name == "INCREMENT_COUNTER" || name == "DECREMENT_COUNTER")
    {
        // Legacy template: [INT delta, COUNTER name]. The former Stage-0
        // implementation accidentally read parameter 0 as the counter name
        // and always added/subtracted one, which changes script control flow
        // even on otherwise supported maps.
        const auto delta = integerParameter(instruction, 0, context, scriptName, name);
        const auto counter = textParameter(instruction, 1, context, scriptName, name);
        if (!delta || !counter)
            return false;
        output.emplace_back(
            ScriptAdjustCounterAction{
                .counter = *counter,
                .delta = name == "INCREMENT_COUNTER" ? *delta : saturatedNegate(*delta),
            });
        return true;
    }
    if (name == "SET_TIMER")
    {
        const auto timer = textParameter(instruction, 0, context, scriptName, name);
        const auto frames = integerParameter(instruction, 1, context, scriptName, name);
        if (!timer || !frames)
            return false;
        output.emplace_back(ScriptSetTimerAction{.timer = *timer, .durationTicks = *frames});
        return true;
    }
    if (name == "SET_MILLISECOND_TIMER")
    {
        const auto timer = textParameter(instruction, 0, context, scriptName, name);
        const auto seconds = realParameter(instruction, 1, context, scriptName, name);
        if (!timer || !seconds)
            return false;
        const auto ticks = signedSecondsToTicks(*seconds, context, scriptName, instruction.serialized);
        if (!ticks)
            return false;
        output.emplace_back(ScriptSetTimerAction{.timer = *timer, .durationTicks = *ticks});
        return true;
    }
    if (name == "SET_RANDOM_TIMER")
    {
        const auto timer = textParameter(instruction, 0, context, scriptName, name);
        const auto minimum = integerParameter(instruction, 1, context, scriptName, name);
        const auto maximum = integerParameter(instruction, 2, context, scriptName, name);
        if (!timer || !minimum || !maximum)
            return false;
        output.emplace_back(ScriptSetRandomTimerAction{
            .timer = *timer,
            .range = ScriptRandomFrameTimerRange{
                .minimumDurationTicks = *minimum,
                .maximumDurationTicks = *maximum,
            },
        });
        return true;
    }
    if (name == "SET_RANDOM_MSEC_TIMER")
    {
        const auto timer = textParameter(instruction, 0, context, scriptName, name);
        const auto minimumSeconds = realParameter(instruction, 1, context, scriptName, name);
        const auto maximumSeconds = realParameter(instruction, 2, context, scriptName, name);
        if (!timer || !minimumSeconds || !maximumSeconds)
            return false;
        const auto minimum = randomTimerSecondsEndpoint(
            *minimumSeconds, context, scriptName, instruction.serialized);
        const auto maximum = randomTimerSecondsEndpoint(
            *maximumSeconds, context, scriptName, instruction.serialized);
        if (!minimum || !maximum)
            return false;
        // Despite its Real parameter template, RefCode calls the integer
        // GameLogicRandomValue macro here. Its C++ argument conversion
        // truncates both endpoints before it samples the shared logic RNG.
        output.emplace_back(ScriptSetRandomTimerAction{
            .timer = *timer,
            .range = ScriptRandomSecondTimerRange{
                .minimumSeconds = *minimum,
                .maximumSeconds = *maximum,
                .ticksPerSecond = context.options.logicFramesPerSecond,
            },
        });
        return true;
    }
    if (name == "STOP_TIMER" || name == "RESTART_TIMER")
    {
        const auto timer = textParameter(instruction, 0, context, scriptName, name);
        if (!timer)
            return false;
        if (name == "STOP_TIMER")
            output.emplace_back(ScriptStopTimerAction{.timer = *timer});
        else
            output.emplace_back(ScriptRestartTimerAction{.timer = *timer});
        return true;
    }
    if (name == "ADD_TO_MSEC_TIMER" || name == "SUB_FROM_MSEC_TIMER")
    {
        const auto seconds = realParameter(instruction, 0, context, scriptName, name);
        const auto timer = textParameter(instruction, 1, context, scriptName, name);
        if (!seconds || !timer)
            return false;
        const float signedSeconds =
            name == "ADD_TO_MSEC_TIMER" ? *seconds : -*seconds;
        const auto ticks = signedSecondsToTicks(
            signedSeconds, context, scriptName, instruction.serialized);
        if (!ticks)
            return false;
        output.emplace_back(
            ScriptAdjustTimerAction{
                .timer = *timer,
                .deltaTicks = *ticks,
            });
        return true;
    }
    if (name == "ENABLE_SCRIPT" || name == "DISABLE_SCRIPT")
    {
        const auto target = textParameter(instruction, 0, context, scriptName, name);
        return target &&
               appendEnableDisableActions(*target, name == "ENABLE_SCRIPT", output, context, instruction, scriptName);
    }
    if (name == "CALL_SUBROUTINE")
    {
        const auto target = textParameter(instruction, 0, context, scriptName, name);
        return target && appendSubroutineAction(*target, output, context, instruction, scriptName);
    }
    if (name == "VICTORY" || name == "QUICKVICTORY")
    {
        output.emplace_back(ScriptVictoryAction{
            .mode = name == "QUICKVICTORY" ? ScriptMissionEndMode::Quick
                                             : ScriptMissionEndMode::Normal,
        });
        return true;
    }
    if (name == "DEFEAT")
    {
        output.emplace_back(ScriptDefeatAction{});
        return true;
    }
    if (name == "MOVIE_PLAY_FULLSCREEN" || name == "MOVIE_PLAY_RADAR")
    {
        const auto movieName = textParameter(instruction, 0, context, scriptName, name);
        if (!movieName) return false;
        output.emplace_back(ScriptMovieAction{
            .target = name == "MOVIE_PLAY_FULLSCREEN"
                ? ScriptMovieTarget::Fullscreen : ScriptMovieTarget::Radar,
            .movieName = *movieName,
        });
        return true;
    }
    if (name == "PLAY_SOUND_EFFECT" || name == "SPEECH_PLAY")
    {
        const auto eventName = textParameter(instruction, 0, context, scriptName, name);
        if (!eventName)
            return false;
        ScriptPlayAudioAction action{.eventName = *eventName};
        if (name == "SPEECH_PLAY")
        {
            const auto allowOverlap = integerParameter(instruction, 1, context, scriptName, name);
            if (!allowOverlap)
                return false;
            // The legacy dialog parameter is positive phrasing: true means
            // overlap is allowed, therefore the AudioEvent is interruptible.
            action.uninterruptible = *allowOverlap == 0;
            action.subtitleLabel = "DIALOGEVENT:" + *eventName + "Subtitle";
            constexpr uint32_t kLegacySubtitleDurationMilliseconds = 8000;
            const uint64_t ticks =
                (static_cast<uint64_t>(kLegacySubtitleDurationMilliseconds) *
                 static_cast<uint64_t>(context.options.logicFramesPerSecond) + 999u) / 1000u;
            action.subtitleDurationTicks = static_cast<uint32_t>(
                std::min<uint64_t>(ticks, std::numeric_limits<uint32_t>::max()));
        }
        output.emplace_back(std::move(action));
        return true;
    }
    if (name == "PLAY_SOUND_EFFECT_AT")
    {
        const auto eventName = textParameter(instruction, 0, context, scriptName, name);
        const auto waypoint = textParameter(instruction, 1, context, scriptName, name);
        if (!eventName || !waypoint)
            return false;
        output.emplace_back(ScriptPlayAudioAction{
            .eventName = *eventName,
            .waypointName = *waypoint,
        });
        return true;
    }
    if (name == "SOUND_PLAY_NAMED")
    {
        const auto eventName = textParameter(instruction, 0, context, scriptName, name);
        const auto objectName = textParameter(instruction, 1, context, scriptName, name);
        if (!eventName || !objectName ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *objectName,
                                               instruction.serialized))
            return false;
        output.emplace_back(ScriptPlayAudioAction{
            .eventName = *eventName,
            .emitterName = *objectName,
        });
        return true;
    }
    if (name == "MUSIC_SET_TRACK")
    {
        const auto trackName = textParameter(instruction, 0, context, scriptName, name);
        const auto fadeOut = integerParameter(instruction, 1, context, scriptName, name);
        const auto fadeIn = integerParameter(instruction, 2, context, scriptName, name);
        if (!trackName || !fadeOut || !fadeIn)
            return false;
        // ScriptActions::doMusicTrackChange first removes the existing music
        // with either StopTheMusic or StopTheMusicFade, then starts this
        // MusicTrack with its independent should-fade bit. Preserve both
        // positive-Boolean wire values instead of collapsing the operation
        // into a generic AudioEvent request.
        output.emplace_back(ScriptMusicAction{
            .command = ScriptMusicCommand::SetTrack,
            .trackName = *trackName,
            .fadeOut = *fadeOut != 0,
            .fadeIn = *fadeIn != 0,
        });
        return true;
    }
    if (name == "MUSIC_SET_VOLUME")
    {
        const auto percent = realParameter(instruction, 0, context, scriptName, name);
        if (!percent)
            return false;
        // RefCode divides the authored percentage by 100 and clamps before
        // it reaches TheAudio.  Normalize once here so later presentation
        // consumers do not each make subtly different choices for -ve or
        // >100 legacy content.
        output.emplace_back(ScriptMusicAction{
            .command = ScriptMusicCommand::SetVolume,
            .volume = std::clamp(*percent / 100.0f, 0.0f, 1.0f),
        });
        return true;
    }
    if (name == "SOUND_AMBIENT_PAUSE" || name == "SOUND_AMBIENT_RESUME")
    {
        output.emplace_back(ScriptAmbientAudioAction{
            .paused = name == "SOUND_AMBIENT_PAUSE",
        });
        return true;
    }
    if (name == "FREEZE_TIME" || name == "UNFREEZE_TIME")
    {
        // RefCode keeps ScriptEngine evaluating while GameEngine suppresses
        // ordinary logic updates.  Retain a dedicated typed action instead
        // of mapping this to a local UI pause or stopping ScriptRuntime.
        output.emplace_back(ScriptTimeControlAction{
            .frozen = name == "FREEZE_TIME",
        });
        return true;
    }
    if (name == "SCRIPTING_OVERRIDE_HULK_LIFETIME")
    {
        const auto seconds = realParameter(instruction, 0, context, scriptName, name);
        if (!seconds) return false;
        // RefCode does not convert until ScriptActions applies the action:
        // `(Int)(seconds * LOGICFRAMES_PER_SECOND)`.  Preserve the finite
        // authored REAL here so a compiled map can run at its session's
        // frozen FPS rather than the loader's incidental compile option.
        output.emplace_back(ScriptHulkLifetimeOverrideAction{
            .seconds = math::q32_32{*seconds}});
        return true;
    }
    if (name == "SUSPEND_BACKGROUND_SOUNDS" || name == "RESUME_BACKGROUND_SOUNDS")
    {
        // RefCode pauses/resumes AudioAffect_Sound.  The presentation audio
        // service owns the actual bus/voice handles, while this retained
        // enabled value preserves the exact authored final policy.
        output.emplace_back(ScriptAudioControlAction{
            .command = ScriptAudioControlCommand::SetBackgroundSoundsPaused,
            .enabled = name == "RESUME_BACKGROUND_SOUNDS",
        });
        return true;
    }
    if (name == "SOUND_SET_VOLUME" || name == "SPEECH_SET_VOLUME")
    {
        const auto percent = realParameter(instruction, 0, context, scriptName, name);
        if (!percent)
            return false;
        // ScriptActions::doAudioSetVolume divides percent by 100 and clamps
        // before it reaches AudioManager. Normalize once here so the bridge
        // and client audio service never diverge on malformed map values.
        output.emplace_back(ScriptAudioControlAction{
            .command = name == "SOUND_SET_VOLUME"
                ? ScriptAudioControlCommand::SetSoundVolume
                : ScriptAudioControlCommand::SetSpeechVolume,
            .volume = std::clamp(*percent / 100.0f, 0.0f, 1.0f),
        });
        return true;
    }
    if (name == "SOUND_DISABLE_TYPE" || name == "SOUND_ENABLE_TYPE" ||
        name == "SOUND_REMOVE_TYPE" || name == "AUDIO_RESTORE_VOLUME_TYPE")
    {
        const auto eventName = textParameter(instruction, 0, context, scriptName, name);
        if (!eventName)
            return false;

        ScriptAudioControlCommand command = ScriptAudioControlCommand::RemoveEvent;
        if (name == "SOUND_DISABLE_TYPE") {
            command = ScriptAudioControlCommand::SetEventVolumeOverride;
        } else if (name == "SOUND_ENABLE_TYPE" || name == "AUDIO_RESTORE_VOLUME_TYPE") {
            command = ScriptAudioControlCommand::RestoreEventVolumeOverride;
        }

        // AudioManager::setAudioEventVolumeOverride treats an empty name as
        // its historical "clear every override" sentinel. Preserve that
        // edge form rather than turning a valid legacy record into a modern
        // validation failure; SOUND_REMOVE_TYPE with an empty name remains a
        // harmless name-scoped remove request, matching removePlayingAudio.
        if (eventName->empty() && command != ScriptAudioControlCommand::RemoveEvent) {
            output.emplace_back(ScriptAudioControlAction{
                .command = ScriptAudioControlCommand::RestoreAllEventVolumeOverrides,
            });
            return true;
        }
        output.emplace_back(ScriptAudioControlAction{
            .command = command,
            .eventName = *eventName,
            .volume = command == ScriptAudioControlCommand::SetEventVolumeOverride ? 0.0f : 1.0f,
        });
        return true;
    }
    if (name == "SOUND_ENABLE_ALL" || name == "AUDIO_RESTORE_VOLUME_ALL_TYPE" ||
        name == "SOUND_REMOVE_ALL_DISABLED")
    {
        // All three have no authored parameter. AudioManager implements the
        // first two by clearing its adjusted-volume table; the remove action
        // only stops currently muted instances and retains that table.
        const ScriptAudioControlCommand command = name == "SOUND_REMOVE_ALL_DISABLED"
            ? ScriptAudioControlCommand::RemoveDisabledEvents
            : ScriptAudioControlCommand::RestoreAllEventVolumeOverrides;
        output.emplace_back(ScriptAudioControlAction{.command = command});
        return true;
    }
    if (name == "AUDIO_OVERRIDE_VOLUME_TYPE")
    {
        const auto eventName = textParameter(instruction, 0, context, scriptName, name);
        const auto percent = realParameter(instruction, 1, context, scriptName, name);
        if (!eventName || !percent)
            return false;
        if (eventName->empty()) {
            // See the empty-name AudioManager sentinel above: even an
            // otherwise nonzero override clears every existing adjustment.
            output.emplace_back(ScriptAudioControlAction{
                .command = ScriptAudioControlCommand::RestoreAllEventVolumeOverrides,
            });
            return true;
        }
        // RefCode converts authored percent to an absolute AudioEvent gain.
        // The modern presentation policy permits the existing 0..4 gain
        // range, preserving useful mod amplification without letting NaN or
        // an unbounded value reach a device command.
        output.emplace_back(ScriptAudioControlAction{
            .command = ScriptAudioControlCommand::SetEventVolumeOverride,
            .eventName = *eventName,
            .volume = std::clamp(*percent / 100.0f, 0.0f, 4.0f),
        });
        return true;
    }
    if (name == "EVA_SET_ENABLED_DISABLED")
    {
        const auto enabled = integerParameter(instruction, 0, context, scriptName, name);
        if (!enabled)
            return false;
        output.emplace_back(ScriptAudioControlAction{
            .command = ScriptAudioControlCommand::SetEvaEnabled,
            .enabled = *enabled != 0,
        });
        return true;
    }
    if (name == "OVERSIZE_TERRAIN")
    {
        const auto tiles = integerParameter(instruction, 0, context, scriptName, name);
        if (!tiles) return false;
        // BaseHeightMapRenderObjClass::oversizeTerrain receives the raw
        // signed Int. DX12 has no mutable visible-terrain window, so retain
        // the value as compatibility state instead of treating it as a mesh
        // allocation or silently dropping the otherwise valid ScriptList.
        output.emplace_back(ScriptViewCompatibilityAction{
            .command = ScriptViewCompatibilityCommand::SetTerrainOversizeTiles,
            .terrainOversizeTiles = *tiles,
        });
        return true;
    }
    if (name == "RESIZE_VIEW_GUARDBAND")
    {
        const auto x = realParameter(instruction, 0, context, scriptName, name);
        const auto y = realParameter(instruction, 1, context, scriptName, name);
        if (!x || !y || !std::isfinite(*x) || !std::isfinite(*y)) {
            context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                               "script '" + container::String(scriptName) + "' action '" + name +
                                   "' requires finite world-space guardband values",
                               instruction.serialized);
            return false;
        }
        // View::setGuardBandBias stores these world-space reals directly.
        // The renderer later maps only the non-negative expansion to its
        // modern radial culler; raw values remain in retained state for
        // diagnostics and future screen-frustum culling.
        output.emplace_back(ScriptViewCompatibilityAction{
            .command = ScriptViewCompatibilityCommand::SetGuardBandBias,
            .guardBandX = *x,
            .guardBandY = *y,
        });
        return true;
    }
    if (name == "SET_TREE_SWAY")
    {
        // ScriptEngine::setSway reads five values in this exact order:
        // direction angle, intensity angle, lean angle, period Int, and
        // randomness Real.  It clamps only a period below one at execution.
        const auto direction = realParameter(instruction, 0, context, scriptName, name);
        const auto intensity = realParameter(instruction, 1, context, scriptName, name);
        const auto lean = realParameter(instruction, 2, context, scriptName, name);
        const auto period = integerParameter(instruction, 3, context, scriptName, name);
        const auto randomness = realParameter(instruction, 4, context, scriptName, name);
        if (!direction || !intensity || !lean || !period || !randomness) return false;
        if (!std::isfinite(*direction) || !std::isfinite(*intensity) ||
            !std::isfinite(*lean) || !std::isfinite(*randomness)) {
            context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                               "script '" + container::String(scriptName) + "' action '" + name +
                                   "' requires finite breeze values",
                               instruction.serialized);
            return false;
        }
        output.emplace_back(ScriptTreeSwayAction{
            .directionRadians = *direction,
            .intensityRadians = *intensity,
            .leanRadians = *lean,
            .periodFrames = *period,
            .randomness = *randomness,
        });
        return true;
    }
    if (name == "SHOW_WEATHER")
    {
        const auto visible = integerParameter(instruction, 0, context, scriptName, name);
        if (!visible) return false;
        // ScriptActions forwards getInt() directly to SnowManager::setVisible.
        // Preserve standard legacy BOOLEAN truthiness (nonzero means show).
        output.emplace_back(ScriptWeatherAction{.visible = *visible != 0});
        return true;
    }
    if (name == "SET_INFANTRY_LIGHTING_OVERRIDE")
    {
        const auto scale = realParameter(instruction, 0, context, scriptName, name);
        // ScriptActions asserts this is strictly positive.  Keep RESET as its
        // own opcode instead of accepting -1 here: the typed payload then
        // cannot confuse a valid dim light with the legacy reset sentinel.
        if (!scale || !std::isfinite(*scale) || *scale <= 0.0f) {
            context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                               "script '" + container::String(scriptName) + "' action '" + name +
                                   "' requires a finite positive light scale",
                               instruction.serialized);
            return false;
        }
        output.emplace_back(ScriptInfantryLightingAction{.overrideScale = *scale});
        return true;
    }
    if (name == "RESET_INFANTRY_LIGHTING_OVERRIDE")
    {
        output.emplace_back(ScriptInfantryLightingAction{.overrideScale = std::nullopt});
        return true;
    }
    if (name == "SET_FPS_LIMIT")
    {
        const auto framesPerSecond = integerParameter(instruction, 0, context, scriptName, name);
        if (!framesPerSecond) return false;
        output.emplace_back(ScriptClientOptionsAction{
            .command = ScriptClientOptionCommand::SetFrameRateLimit,
            .frameRateLimit = *framesPerSecond,
        });
        return true;
    }
    if (name == "OPTIONS_SET_OCCLUSION_MODE" || name == "OPTIONS_SET_DRAWICON_UI_MODE" ||
        name == "OPTIONS_SET_PARTICLE_CAP_MODE")
    {
        const auto enabled = integerParameter(instruction, 0, context, scriptName, name);
        if (!enabled) return false;
        ScriptClientOptionCommand command = ScriptClientOptionCommand::SetOcclusionMode;
        if (name == "OPTIONS_SET_DRAWICON_UI_MODE") {
            command = ScriptClientOptionCommand::SetDrawIconUiMode;
        } else if (name == "OPTIONS_SET_PARTICLE_CAP_MODE") {
            command = ScriptClientOptionCommand::SetDynamicParticleLodMode;
        }
        output.emplace_back(ScriptClientOptionsAction{
            .command = command,
            .enabled = *enabled != 0,
        });
        return true;
    }
    if (name == "TEAM_SET_EMOTICON" || name == "NAMED_SET_EMOTICON")
    {
        // RefCode declares the third parameter as REAL and performs the
        // `(Int)(seconds * LOGICFRAMES_PER_SECOND)` conversion only while
        // applying the action.  Preserve the authored fractional duration
        // through the typed program instead of prematurely treating it as an
        // integer flash duration.
        const auto targetName = textParameter(instruction, 0, context, scriptName, name);
        const auto emoticonName = textParameter(instruction, 1, context, scriptName, name);
        const auto durationSeconds = realParameter(instruction, 2, context, scriptName, name);
        if (!targetName || !emoticonName || !durationSeconds ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *targetName,
                                               instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptObjectPresentationAction{
            .command = ScriptObjectPresentationCommand::SetEmoticon,
            .target = {
                .kind = name == "TEAM_SET_EMOTICON"
                    ? ScriptObjectPresentationTargetKind::Team
                    : ScriptObjectPresentationTargetKind::NamedObject,
                .name = *targetName,
            },
            .emoticonDurationSeconds = *durationSeconds,
            .emoticonName = *emoticonName,
        });
        return true;
    }
    if (name == "NAMED_FLASH" || name == "TEAM_FLASH" ||
        name == "NAMED_FLASH_WHITE" || name == "TEAM_FLASH_WHITE")
    {
        const auto targetName = textParameter(instruction, 0, context, scriptName, name);
        const auto durationSeconds = integerParameter(instruction, 1, context, scriptName, name);
        if (!targetName || !durationSeconds ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *targetName,
                                               instruction.serialized)) {
            return false;
        }
        const bool teamTarget = name == "TEAM_FLASH" || name == "TEAM_FLASH_WHITE";
        const bool white = name == "NAMED_FLASH_WHITE" || name == "TEAM_FLASH_WHITE";
        output.emplace_back(ScriptObjectPresentationAction{
            .command = ScriptObjectPresentationCommand::Flash,
            .target = {
                .kind = teamTarget ? ScriptObjectPresentationTargetKind::Team
                                   : ScriptObjectPresentationTargetKind::NamedObject,
                .name = *targetName,
            },
            .durationSeconds = *durationSeconds,
            .flashColor = white ? ScriptObjectPresentationFlashColor::White
                                : ScriptObjectPresentationFlashColor::Indicator,
        });
        return true;
    }
    if (name == "NAMED_CUSTOM_COLOR")
    {
        const auto targetName = textParameter(instruction, 0, context, scriptName, name);
        const auto packedColor = integerParameter(instruction, 1, context, scriptName, name);
        if (!targetName || !packedColor ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *targetName,
                                               instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptObjectPresentationAction{
            .command = ScriptObjectPresentationCommand::SetCustomIndicatorColor,
            .target = {
                .kind = ScriptObjectPresentationTargetKind::NamedObject,
                .name = *targetName,
            },
            .packedColor = static_cast<uint32_t>(*packedColor),
        });
        return true;
    }
    if (name == "ENABLE_OBJECT_SOUND" || name == "DISABLE_OBJECT_SOUND")
    {
        const auto targetName = textParameter(instruction, 0, context, scriptName, name);
        if (!targetName ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *targetName,
                                               instruction.serialized)) {
            return false;
        }
        // The original resolves the UNIT at execution time and asks its
        // Drawable to start or stop the currently selected ambient event.
        // Keep the selector typed until ScriptRuntime resolves a stable
        // ObjectId; no AudioEvent pointer is allowed in a script action.
        output.emplace_back(ScriptObjectPresentationAction{
            .command = ScriptObjectPresentationCommand::SetAmbientSoundEnabled,
            .target = {
                .kind = ScriptObjectPresentationTargetKind::NamedObject,
                .name = *targetName,
            },
            .ambientSoundEnabled = name == "ENABLE_OBJECT_SOUND",
        });
        return true;
    }
    if (name == "OBJECT_FORCE_SELECT")
    {
        const auto teamName = textParameter(instruction, 0, context, scriptName, name);
        const auto objectTypeName = textParameter(instruction, 1, context, scriptName, name);
        const auto centerInView = integerParameter(instruction, 2, context, scriptName, name);
        // DIALOG is optional in authored maps. RefCode still performs the
        // selection and camera move for an empty AudioEventRTS name, so do
        // not turn that valid no-audio form into a blocked script.
        const auto audioEventName =
            textParameterAllowEmpty(instruction, 3, context, scriptName, name);
        if (!teamName || !objectTypeName || !centerInView || !audioEventName ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *teamName,
                                               instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptForceObjectSelectionAction{
            .teamName = *teamName,
            .objectTypeName = *objectTypeName,
            .centerInView = *centerInView != 0,
            .audioEventName = *audioEventName,
        });
        return true;
    }
    if (name == "RADAR_CREATE_EVENT")
    {
        const auto position = coordinateParameter(instruction, 0, context, scriptName, name);
        const auto eventType = integerParameter(instruction, 1, context, scriptName, name);
        if (!position || !eventType) return false;
        output.emplace_back(ScriptMapPresentationAction{
            .command = ScriptMapPresentationCommand::CreateRadarEvent,
            .position = *position,
            .radarEventType = *eventType,
        });
        return true;
    }
    if (name == "OBJECT_CREATE_RADAR_EVENT" || name == "TEAM_CREATE_RADAR_EVENT")
    {
        const auto targetName = textParameter(instruction, 0, context, scriptName, name);
        const auto eventType = integerParameter(instruction, 1, context, scriptName, name);
        if (!targetName || !eventType ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *targetName,
                                               instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptObjectPresentationAction{
            .command = ScriptObjectPresentationCommand::CreateRadarEvent,
            .target = {
                .kind = name == "OBJECT_CREATE_RADAR_EVENT"
                    ? ScriptObjectPresentationTargetKind::NamedObject
                    : ScriptObjectPresentationTargetKind::Team,
                .name = *targetName,
            },
            // RefCode passes its authored Int through a C-style
            // RadarEventType cast. Keep that exact raw range here: a modded
            // value is not an unsupported script instruction.
            .radarEventType = *eventType,
        });
        return true;
    }
    if (name == "REFRESH_RADAR")
    {
        output.emplace_back(ScriptMapPresentationAction{
            .command = ScriptMapPresentationCommand::RefreshRadar,
        });
        return true;
    }
    if (name == "DISABLE_BORDER_SHROUD" || name == "ENABLE_BORDER_SHROUD")
    {
        output.emplace_back(ScriptMapPresentationAction{
            .command = ScriptMapPresentationCommand::SetBorderShroud,
            .enabled = name == "ENABLE_BORDER_SHROUD",
        });
        return true;
    }
    if (name == "RADAR_DISABLE" || name == "RADAR_ENABLE")
    {
        output.emplace_back(ScriptMapPresentationAction{
            .command = ScriptMapPresentationCommand::SetRadarHidden,
            .enabled = name == "RADAR_DISABLE",
        });
        return true;
    }
    if (name == "RADAR_FORCE_ENABLE" || name == "RADAR_REVERT_TO_NORMAL")
    {
        output.emplace_back(ScriptMapPresentationAction{
            .command = ScriptMapPresentationCommand::SetRadarForced,
            .enabled = name == "RADAR_FORCE_ENABLE",
        });
        return true;
    }
    if (name == "MAP_SWITCH_BORDER")
    {
        const auto boundary = integerParameter(instruction, 0, context, scriptName, name);
        if (!boundary) return false;
        output.emplace_back(ScriptMapPresentationAction{
            .command = ScriptMapPresentationCommand::SetBoundary,
            .boundaryIndex = *boundary,
        });
        return true;
    }
    if (name == "MAP_REVEAL_AT_WAYPOINT" || name == "MAP_SHROUD_AT_WAYPOINT")
    {
        const auto waypoint = textParameter(instruction, 0, context, scriptName, name);
        const auto radius = realParameter(instruction, 1, context, scriptName, name);
        const auto player = textParameterAllowEmpty(instruction, 2, context, scriptName, name);
        if (!waypoint || !radius || !player ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptMapPresentationAction{
            .command = name == "MAP_REVEAL_AT_WAYPOINT"
                ? ScriptMapPresentationCommand::RevealAtWaypoint
                : ScriptMapPresentationCommand::ShroudAtWaypoint,
            .waypointName = *waypoint,
            .playerName = *player,
            .radius = math::q32_32{*radius},
        });
        return true;
    }
    if (name == "MAP_REVEAL_ALL" || name == "MAP_REVEAL_ALL_PERM" ||
        name == "MAP_REVEAL_ALL_UNDO_PERM" || name == "MAP_SHROUD_ALL")
    {
        const auto player = textParameterAllowEmpty(instruction, 0, context, scriptName, name);
        if (!player || rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                              instruction.serialized)) {
            return false;
        }
        ScriptMapPresentationCommand command = ScriptMapPresentationCommand::RevealAll;
        if (name == "MAP_REVEAL_ALL_PERM") {
            command = ScriptMapPresentationCommand::RevealAllPermanently;
        } else if (name == "MAP_REVEAL_ALL_UNDO_PERM") {
            command = ScriptMapPresentationCommand::UndoRevealAllPermanently;
        } else if (name == "MAP_SHROUD_ALL") {
            command = ScriptMapPresentationCommand::ShroudAll;
        }
        output.emplace_back(ScriptMapPresentationAction{
            .command = command,
            .playerName = *player,
        });
        return true;
    }
    if (name == "MAP_REVEAL_PERMANENTLY_AT_WAYPOINT")
    {
        const auto waypoint = textParameter(instruction, 0, context, scriptName, name);
        const auto radius = realParameter(instruction, 1, context, scriptName, name);
        const auto player = textParameterAllowEmpty(instruction, 2, context, scriptName, name);
        const auto reveal = textParameter(instruction, 3, context, scriptName, name);
        if (!waypoint || !radius || !player || !reveal ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptMapPresentationAction{
            .command = ScriptMapPresentationCommand::RevealPermanentlyAtWaypoint,
            .waypointName = *waypoint,
            .playerName = *player,
            .revealName = *reveal,
            .radius = math::q32_32{*radius},
        });
        return true;
    }
    if (name == "MAP_UNDO_REVEAL_PERMANENTLY_AT_WAYPOINT")
    {
        const auto reveal = textParameter(instruction, 0, context, scriptName, name);
        if (!reveal) return false;
        output.emplace_back(ScriptMapPresentationAction{
            .command = ScriptMapPresentationCommand::UndoRevealPermanentlyAtWaypoint,
            .revealName = *reveal,
        });
        return true;
    }
    if (name == "DISABLE_INPUT" || name == "ENABLE_INPUT")
    {
        output.emplace_back(ScriptUiAction{
            .command = ScriptUiCommand::SetControl,
            .control = ScriptUiControlKind::GameplayInput,
            .enabled = name == "ENABLE_INPUT",
        });
        return true;
    }
    if (name == "ENABLE_SCORING" || name == "DISABLE_SCORING")
    {
        output.emplace_back(ScriptScoreAccumulationPolicyAction{
            .enabled = name == "ENABLE_SCORING",
        });
        return true;
    }
    if (name == "ENABLE_SPECIAL_POWER_DISPLAY" || name == "DISABLE_SPECIAL_POWER_DISPLAY")
    {
        output.emplace_back(ScriptUiAction{
            .command = ScriptUiCommand::SetControl,
            .control = ScriptUiControlKind::SpecialPowerDisplay,
            .enabled = name == "ENABLE_SPECIAL_POWER_DISPLAY",
        });
        return true;
    }
    if (name == "NAMED_HIDE_SPECIAL_POWER_DISPLAY" ||
        name == "NAMED_SHOW_SPECIAL_POWER_DISPLAY")
    {
        const auto object = textParameter(instruction, 0, context, scriptName, name);
        if (!object || rejectDynamicScriptContextSelector(
                context, scriptName, name, *object, instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptObjectPresentationAction{
            .command = ScriptObjectPresentationCommand::SetSpecialPowerDisplayVisible,
            .target = {
                .kind = ScriptObjectPresentationTargetKind::NamedObject,
                .name = *object,
            },
            .specialPowerDisplayVisible =
                name == "NAMED_SHOW_SPECIAL_POWER_DISPLAY",
        });
        return true;
    }
    if (name == "ENABLE_COUNTDOWN_TIMER_DISPLAY" || name == "DISABLE_COUNTDOWN_TIMER_DISPLAY")
    {
        output.emplace_back(ScriptUiAction{
            .command = ScriptUiCommand::SetControl,
            .control = ScriptUiControlKind::NamedTimerDisplay,
            .enabled = name == "ENABLE_COUNTDOWN_TIMER_DISPLAY",
        });
        return true;
    }
    if (name == "DISPLAY_COUNTER" || name == "DISPLAY_COUNTDOWN_TIMER")
    {
        const auto counterName = textParameter(instruction, 0, context, scriptName, name);
        const auto counterText = textParameter(instruction, 1, context, scriptName, name);
        if (!counterName || !counterText) return false;
        output.emplace_back(ScriptUiAction{
            .command = ScriptUiCommand::ShowNamedIndicator,
            .indicatorKind = name == "DISPLAY_COUNTDOWN_TIMER"
                ? ScriptNamedIndicatorKind::Countdown
                : ScriptNamedIndicatorKind::Counter,
            .name = *counterName,
            .text = *counterText,
        });
        return true;
    }
    if (name == "HIDE_COUNTER" || name == "HIDE_COUNTDOWN_TIMER")
    {
        const auto counterName = textParameter(instruction, 0, context, scriptName, name);
        if (!counterName) return false;
        output.emplace_back(ScriptUiAction{
            .command = ScriptUiCommand::HideNamedIndicator,
            .name = *counterName,
        });
        return true;
    }
    if (name == "INGAME_POPUP_MESSAGE")
    {
        const auto text = textParameter(instruction, 0, context, scriptName, name);
        const auto x = integerParameter(instruction, 1, context, scriptName, name);
        const auto y = integerParameter(instruction, 2, context, scriptName, name);
        const auto width = integerParameter(instruction, 3, context, scriptName, name);
        const auto pause = integerParameter(instruction, 4, context, scriptName, name);
        if (!text || !x || !y || !width || !pause) return false;
        output.emplace_back(ScriptUiAction{
            .command = ScriptUiCommand::ShowPopup,
            .text = *text,
            .xPercent = *x,
            .yPercent = *y,
            .width = *width,
            .pauseRequested = *pause != 0,
        });
        return true;
    }
    if (name == "LOCALDEFEAT")
    {
        output.emplace_back(ScriptUiAction{.command = ScriptUiCommand::ShowLocalDefeat});
        return true;
    }
    if (name == "CAMEO_FLASH")
    {
        const auto button = textParameter(instruction, 0, context, scriptName, name);
        const auto seconds = integerParameter(instruction, 1, context, scriptName, name);
        if (!button || !seconds) return false;
        // Drawable::DRAWABLE_FRAMES_PER_FLASH is half a reference second
        // (15 at the original 30 Hz). Preserve that cadence when a modern
        // session freezes a different fixed simulation rate.
        const uint32_t logicFramesPerSecond = std::max(1u, context.options.logicFramesPerSecond);
        const uint32_t kLegacyFramesPerFlash = (logicFramesPerSecond + 1u) / 2u;
        uint32_t flashCount = delaySecondsToTicks(*seconds, context.options) / kLegacyFramesPerFlash;
        if (flashCount % 2u != 0u) ++flashCount;
        // RefCode still calls CommandButton::setFlashCount() for zero or
        // negative durations. Carry that zero through as a stamped, same-name
        // replacement so it cancels an already-active local cameo flash;
        // dropping the action here would incorrectly leave the old flash on.
        output.emplace_back(ScriptUiAction{
            .command = ScriptUiCommand::FlashCameo,
            .name = *button,
            .flashCount = flashCount,
            .framesPerFlash = kLegacyFramesPerFlash,
        });
        return true;
    }
    if (name == "COMMANDBAR_REMOVE_BUTTON_OBJECTTYPE" ||
        name == "COMMANDBAR_ADD_BUTTON_OBJECTTYPE_SLOT")
    {
        // RefCode resolves both authored names only when it executes the
        // action.  Preserve that timing: an unknown Object/CommandButton is
        // a valid no-op, not a compiler error that blocks the whole Script.
        const auto commandButton = textParameter(instruction, 0, context, scriptName, name);
        const auto objectType = textParameter(instruction, 1, context, scriptName, name);
        if (!commandButton || !objectType) return false;
        int32_t oneBasedSlot = 0;
        if (name == "COMMANDBAR_ADD_BUTTON_OBJECTTYPE_SLOT") {
            const auto slot = integerParameter(instruction, 2, context, scriptName, name);
            if (!slot) return false;
            oneBasedSlot = *slot;
        }
        output.emplace_back(ScriptCommandBarOverrideAction{
            .command = name == "COMMANDBAR_REMOVE_BUTTON_OBJECTTYPE"
                ? ScriptCommandBarOverrideCommand::RemoveButtonFromObjectType
                : ScriptCommandBarOverrideCommand::AddButtonToObjectTypeSlot,
            .commandButtonName = *commandButton,
            .objectTypeName = *objectType,
            .oneBasedSlot = oneBasedSlot,
        });
        return true;
    }
    if (name == "DISPLAY_TEXT")
    {
        const auto text = textParameter(instruction, 0, context, scriptName, name);
        if (!text)
            return false;
        output.emplace_back(ScriptDisplayTextAction{.text = *text, .localized = true});
        return true;
    }
    if (name == "DISPLAY_CINEMATIC_TEXT")
    {
        const auto text = textParameter(instruction, 0, context, scriptName, name);
        const auto fontDescriptor = textParameter(instruction, 1, context, scriptName, name);
        const auto seconds = integerParameter(instruction, 2, context, scriptName, name);
        if (!text || !fontDescriptor || !seconds)
            return false;
        // The original multiplies its signed second count by the logic frame
        // rate. A negative visual lifetime has no useful modern presentation
        // meaning, so retain map compatibility by normalizing it to an
        // immediate/zero-tick overlay rather than blocking the whole script.
        output.emplace_back(ScriptDisplayCinematicTextAction{
            .text = *text,
            .fontDescriptor = *fontDescriptor,
            .durationTicks = delaySecondsToTicks(*seconds, context.options),
            .localized = true,
        });
        return true;
    }
    if (name == "SHOW_MILITARY_CAPTION")
    {
        const auto text = textParameter(instruction, 0, context, scriptName, name);
        const auto durationMilliseconds = integerParameter(instruction, 1, context, scriptName, name);
        if (!text || !durationMilliseconds)
            return false;
        // ScriptActions::doMilitaryCaption passes its signed INT directly to
        // InGameUI. A non-positive duration first removes any prior caption
        // and then displays nothing, so normalize it to an explicit local
        // clear rather than rejecting an otherwise valid legacy script.
        output.emplace_back(ScriptMilitaryCaptionAction{
            .text = *text,
            .durationMilliseconds = *durationMilliseconds > 0
                ? static_cast<uint32_t>(*durationMilliseconds)
                : 0u,
            .localized = true,
        });
        return true;
    }
    if (name == "WATER_CHANGE_HEIGHT")
    {
        const auto water = textParameter(instruction, 0, context, scriptName, name);
        const auto height = realParameter(instruction, 1, context, scriptName, name);
        if (!water || !height)
            return false;
        output.emplace_back(
            ScriptWaterAction{.command = ScriptWaterCommand::SetHeight,
                              .waterName = *water,
                              .value = math::q32_32{*height}});
        return true;
    }
    if (name == "WATER_CHANGE_HEIGHT_OVER_TIME")
    {
        const auto water = textParameter(instruction, 0, context, scriptName, name);
        const auto height = realParameter(instruction, 1, context, scriptName, name);
        const auto durationSeconds = realParameter(instruction, 2, context, scriptName, name);
        const auto damagePerSecond = realParameter(instruction, 3, context, scriptName, name);
        if (!water || !height || !durationSeconds || !damagePerSecond)
            return false;
        const auto durationTicks = secondsToTicks(*durationSeconds, context, scriptName,
                                                   instruction.serialized);
        if (!durationTicks)
            return false;
        output.emplace_back(ScriptWaterAction{
            .command = ScriptWaterCommand::SetHeight,
            .waterName = *water,
            .value = math::q32_32{*height},
            .transitionTicks = static_cast<uint32_t>(*durationTicks),
            // Legacy terrain stores the signed authored value but only
            // applies flood damage when it is positive (and water is rising).
            // The modern terrain contract makes that no-damage state explicit
            // as zero, preserving valid negative map content without letting
            // it poison a typed non-negative flood request.
            .damagePerSecond = math::q32_32{
                std::max(*damagePerSecond, 0.0f)},
        });
        return true;
    }
    if (name == "CAMERA_LETTERBOX_BEGIN" || name == "CAMERA_LETTERBOX_END")
    {
        // ScriptActions::doLetterBoxMode has no authored parameters.  The
        // command only changes client presentation/UI state; it must not be
        // misrepresented as a logic-camera transition or a hidden input lock.
        output.emplace_back(ScriptCameraAction{
            .command = ScriptCameraCommand::SetLetterbox,
            .enabled = name == "CAMERA_LETTERBOX_BEGIN",
        });
        return true;
    }
    if (name == "CAMERA_FADE_ADD" || name == "CAMERA_FADE_SUBTRACT" ||
        name == "CAMERA_FADE_SATURATE" || name == "CAMERA_FADE_MULTIPLY")
    {
        // ScriptEngine::setFade retains the raw five authored values and
        // replaces one global fade slot.  In particular, the three frame
        // counts are signed INTs rather than seconds or non-negative modern
        // durations: zero/negative values exercise visible legacy branches.
        const auto minimum = realParameter(instruction, 0, context, scriptName, name);
        const auto maximum = realParameter(instruction, 1, context, scriptName, name);
        const auto increaseFrames = integerParameter(instruction, 2, context, scriptName, name);
        const auto holdFrames = integerParameter(instruction, 3, context, scriptName, name);
        const auto decreaseFrames = integerParameter(instruction, 4, context, scriptName, name);
        if (!minimum || !maximum || !increaseFrames || !holdFrames || !decreaseFrames)
            return false;

        ScriptScreenFadeBlendMode blendMode = ScriptScreenFadeBlendMode::Add;
        if (name == "CAMERA_FADE_SUBTRACT")
            blendMode = ScriptScreenFadeBlendMode::Subtract;
        else if (name == "CAMERA_FADE_SATURATE")
            blendMode = ScriptScreenFadeBlendMode::Saturate;
        else if (name == "CAMERA_FADE_MULTIPLY")
            blendMode = ScriptScreenFadeBlendMode::Multiply;

        output.emplace_back(ScriptScreenFadeAction{
            .blendMode = blendMode,
            .minimumIntensity = *minimum,
            .maximumIntensity = *maximum,
            .increaseFrames = *increaseFrames,
            .holdFrames = *holdFrames,
            .decreaseFrames = *decreaseFrames,
        });
        return true;
    }
    if (name == "CAMERA_BW_MODE_BEGIN" || name == "CAMERA_BW_MODE_END")
    {
        // RefCode's action executor uses `getParameter(0) ? getInt() : 0`.
        // Older authored maps therefore legitimately omit this optional fade
        // length; retain the immediate zero-frame form instead of rejecting
        // the whole ScriptList during modern compilation.
        int32_t transitionFrames = 0;
        if (!instruction.parameters.empty()) {
            const auto authoredFrames = integerParameter(instruction, 0, context, scriptName, name);
            if (!authoredFrames) return false;
            transitionFrames = *authoredFrames;
        }
        // ScriptActions passes this signed INT directly to ScreenBWFilter.
        // Do not turn it into seconds or clamp it: zero/negative counts are
        // immediate renderer-side completion cases in the original filter.
        // End remains an explicit presentation command rather than a local
        // no-op because only the renderer knows whether BW is still its
        // active filter after a motion-blur/filter replacement.
        output.emplace_back(ScriptBlackAndWhiteAction{
            .enabled = name == "CAMERA_BW_MODE_BEGIN",
            .transitionFrames = transitionFrames,
        });
        return true;
    }
    if (name == "CAMERA_MOTION_BLUR")
    {
        const auto zoomIn = integerParameter(instruction, 0, context, scriptName, name);
        const auto saturate = integerParameter(instruction, 1, context, scriptName, name);
        if (!zoomIn || !saturate) return false;
        // ScriptActions selects one of IN/OUT x ALPHA/SATURATE.  Its BOOLEAN
        // parameters are read through getInt(), so preserve the usual legacy
        // non-zero truthiness rather than constraining old maps to 0/1.
        output.emplace_back(ScriptMotionBlurAction{
            .mode = *zoomIn != 0 ? ScriptMotionBlurMode::ZoomIn
                                  : ScriptMotionBlurMode::ZoomOut,
            .saturate = *saturate != 0,
        });
        return true;
    }
    if (name == "CAMERA_MOTION_BLUR_JUMP")
    {
        const auto waypoint = textParameter(instruction, 0, context, scriptName, name);
        const auto saturate = integerParameter(instruction, 1, context, scriptName, name);
        if (!waypoint || !saturate) return false;
        // A missing waypoint is a harmless run-time no-op in RefCode.  Keep
        // the authored identity here and let the session bridge make that
        // check without exposing TerrainLogic to ScriptRuntime.
        output.emplace_back(ScriptMotionBlurAction{
            .mode = ScriptMotionBlurMode::ZoomJump,
            .saturate = *saturate != 0,
            .waypointName = *waypoint,
        });
        return true;
    }
    if (name == "CAMERA_MOTION_BLUR_FOLLOW")
    {
        const auto amount = integerParameter(instruction, 0, context, scriptName, name);
        if (!amount) return false;
        // The original template accidentally declares Parameter[1], but the
        // executor reads Parameter[0] and adds it to FM_VIEW_MB_PAN_ALPHA.
        // Retain that actual wire contract, including signed/zero values.
        output.emplace_back(ScriptMotionBlurAction{
            .mode = ScriptMotionBlurMode::Follow,
            .followAmount = *amount,
        });
        return true;
    }
    if (name == "CAMERA_MOTION_BLUR_END_FOLLOW")
    {
        output.emplace_back(ScriptMotionBlurAction{
            .mode = ScriptMotionBlurMode::EndFollow,
        });
        return true;
    }
    if (name == "CAMERA_ENABLE_SLAVE_MODE")
    {
        // Despite the RefCode parameter name `thingTemplateName`, W3DView
        // passes this text to ScriptEngine::getUnitNamed(). It is therefore a
        // named map Object, paired with an authored W3D camera-bone name.
        const auto object = textParameter(instruction, 0, context, scriptName, name);
        const auto bone = textParameter(instruction, 1, context, scriptName, name);
        if (!object || !bone || object->empty() || bone->empty() ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *object,
                                               instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptCameraSlaveAction{
            .objectName = *object,
            .boneName = *bone,
            .enabled = true,
        });
        return true;
    }
    if (name == "CAMERA_DISABLE_SLAVE_MODE")
    {
        // W3DView only clears its slave flag. It intentionally does not
        // touch the durable logical camera rig or require an Object lookup.
        output.emplace_back(ScriptCameraSlaveAction{.enabled = false});
        return true;
    }
    if (name == "DRAW_SKYBOX_BEGIN" || name == "DRAW_SKYBOX_END")
    {
        // RefCode has no parameters and simply writes W3DWater's durable
        // draw flag. Resolving the new_skybox asset belongs to the renderer,
        // so retain only the authored final state here.
        output.emplace_back(ScriptSkyboxAction{
            .enabled = name == "DRAW_SKYBOX_BEGIN",
        });
        return true;
    }
    if (name == "SCREEN_SHAKE")
    {
        const auto intensity = integerParameter(instruction, 0, context, scriptName, name);
        constexpr int32_t kFirstShakeIntensity =
            static_cast<int32_t>(ScriptScreenShakeIntensity::Subtle);
        constexpr int32_t kShakeIntensityCount =
            static_cast<int32_t>(ScriptScreenShakeIntensity::Count);
        if (!intensity)
            return false;
        // RefCode's editor emits View::CameraShakeType [0, SHAKE_COUNT).
        // Reject malformed mod data rather than treating an arbitrary integer
        // as a zero-strength success that silently changes script coverage.
        if (*intensity < kFirstShakeIntensity || *intensity >= kShakeIntensityCount)
        {
            context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                               "script '" + container::String(scriptName) + "' action '" + name +
                                   "' has an out-of-range shake intensity " +
                                   std::to_string(*intensity),
                               instruction.serialized);
            return false;
        }
        output.emplace_back(ScriptScreenShakeAction{
            .intensity = static_cast<ScriptScreenShakeIntensity>(*intensity),
        });
        return true;
    }
    if (name == "CAMERA_ADD_SHAKER_AT")
    {
        const auto waypoint = textParameter(instruction, 0, context, scriptName, name);
        const auto amplitude = realParameter(instruction, 1, context, scriptName, name);
        const auto durationSeconds = realParameter(instruction, 2, context, scriptName, name);
        const auto radius = realParameter(instruction, 3, context, scriptName, name);
        if (!waypoint || !amplitude || !durationSeconds || !radius ||
            *durationSeconds < 0.0f) {
            return false;
        }
        const auto durationTicks = secondsToTicks(*durationSeconds, context, scriptName,
                                                   instruction.serialized);
        if (!durationTicks) return false;
        output.emplace_back(ScriptLocalizedCameraShakeAction{
            .waypointName = *waypoint,
            .amplitude = *amplitude,
            .durationTicks = static_cast<uint32_t>(*durationTicks),
            .radius = *radius,
        });
        return true;
    }
    if (name == "MOVE_CAMERA_TO")
    {
        const auto waypoint = textParameter(instruction, 0, context, scriptName, name);
        const auto shutterSeconds = realParameter(instruction, 2, context, scriptName, name);
        const auto timing = cameraTimingParameters(instruction, 1, 3, 4, context, scriptName, name);
        if (!waypoint || !shutterSeconds || *shutterSeconds < 0.0f || !timing)
            return false;
        // Shutter is a render-frame path sampling artifact. It does not
        // change durable camera pose or ScriptAction completion semantics, so
        // retain validation but keep it out of the confirmed logic state.
        output.emplace_back(ScriptCameraAction{
            .command = ScriptCameraCommand::MoveTo,
            .waypointName = *waypoint,
            .durationTicks = timing->durationTicks,
            .easeInTicks = timing->easeInTicks,
            .easeOutTicks = timing->easeOutTicks,
        });
        return true;
    }
    if (name == "MOVE_CAMERA_ALONG_WAYPOINT_PATH")
    {
        const auto waypoint = textParameter(instruction, 0, context, scriptName, name);
        const auto shutterSeconds = realParameter(instruction, 2, context, scriptName, name);
        const auto timing = cameraTimingParameters(instruction, 1, 3, 4, context, scriptName, name);
        if (!waypoint || !shutterSeconds || *shutterSeconds < 0.0f || !timing)
            return false;
        output.emplace_back(ScriptCameraAction{
            .command = ScriptCameraCommand::MoveAlongWaypointPath,
            .waypointName = *waypoint,
            .durationTicks = timing->durationTicks,
            .easeInTicks = timing->easeInTicks,
            .easeOutTicks = timing->easeOutTicks,
        });
        return true;
    }
    if (name == "CAMERA_MOD_LOOK_TOWARD")
    {
        const auto waypoint = textParameter(instruction, 0, context, scriptName, name);
        if (!waypoint)
            return false;
        output.emplace_back(ScriptCameraAction{
            .command = ScriptCameraCommand::ModifyLookToward,
            .waypointName = *waypoint,
        });
        return true;
    }
    if (name == "CAMERA_MOD_SET_FINAL_ZOOM" || name == "CAMERA_MOD_SET_FINAL_PITCH")
    {
        const auto finalValue = realParameter(instruction, 0, context, scriptName, name);
        const auto easeIn = realParameter(instruction, 1, context, scriptName, name);
        const auto easeOut = realParameter(instruction, 2, context, scriptName, name);
        if (!finalValue || !easeIn || !easeOut || *easeIn < 0.0f || *easeIn > 1.0f ||
            *easeOut < 0.0f || *easeOut > 1.0f)
        {
            context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                               "script '" + container::String(scriptName) + "' action '" + name +
                                   "' has an invalid ease percentage",
                               instruction.serialized);
            return false;
        }
        output.emplace_back(ScriptCameraAction{
            .command = name == "CAMERA_MOD_SET_FINAL_ZOOM"
                ? ScriptCameraCommand::ModifyFinalZoom
                : ScriptCameraCommand::ModifyFinalPitch,
            .value = *finalValue,
            .secondaryValue = *easeIn,
            .tertiaryValue = *easeOut,
        });
        return true;
    }
    if (name == "CAMERA_MOD_FINAL_LOOK_TOWARD")
    {
        const auto waypoint = textParameter(instruction, 0, context, scriptName, name);
        if (!waypoint)
            return false;
        output.emplace_back(ScriptCameraAction{
            .command = ScriptCameraCommand::ModifyFinalLookToward,
            .waypointName = *waypoint,
        });
        return true;
    }
    if (name == "MOVE_CAMERA_TO_SELECTION")
    {
        // This action has no authored target. RefCode resolves the current
        // client-local selected Drawables when it executes, then only shifts
        // an already active W3D waypoint path's final XY. Keep the immutable
        // action intentionally payload-free; ScriptRuntime must never query
        // selection, ECS or UI in order to compile/run it.
        output.emplace_back(ScriptCameraAction{
            .command = ScriptCameraCommand::MoveToSelection,
        });
        return true;
    }
    if (name == "CAMERA_MOD_FREEZE_ANGLE")
    {
        output.emplace_back(ScriptCameraAction{.command = ScriptCameraCommand::FreezeAngle});
        return true;
    }
    if (name == "CAMERA_MOD_FREEZE_TIME")
    {
        // W3DView arms its next scripted camera movement.  The session owns
        // the resulting simulation-clock gate so the renderer never becomes
        // responsible for pausing TerrainLogic/object simulation.
        output.emplace_back(ScriptCameraAction{
            .command = ScriptCameraCommand::FreezeTimeDuringMotion,
        });
        return true;
    }
    if (name == "SET_VISUAL_SPEED_MULTIPLIER")
    {
        const auto multiplier = integerParameter(instruction, 0, context, scriptName, name);
        if (!multiplier) return false;
        // RefCode writes this signed INT straight to W3DView.  Do not clamp
        // here: <=1 is meaningful as "do not fast-forward" at the modern
        // pacing consumer, and preserving the raw value keeps old maps
        // diagnosable rather than silently normalizing their authoring.
        output.emplace_back(ScriptVisualSpeedAction{.multiplier = *multiplier});
        return true;
    }
    if (name == "CAMERA_MOD_SET_FINAL_SPEED_MULTIPLIER")
    {
        const auto multiplier = integerParameter(instruction, 0, context, scriptName, name);
        if (!multiplier) return false;
        output.emplace_back(ScriptCameraAction{
            .command = ScriptCameraCommand::ModifyFinalSpeedMultiplier,
            .visualSpeedMultiplier = *multiplier,
        });
        return true;
    }
    if (name == "CAMERA_MOD_SET_ROLLING_AVERAGE")
    {
        const auto framesToAverage = integerParameter(instruction, 0, context, scriptName, name);
        if (!framesToAverage) return false;
        // W3DView accepts the signed INT and clamps values below one at the
        // camera boundary. Keep that raw authored value through immutable
        // runtime transport so the modern director owns the compatibility
        // rule instead of the compiler silently changing source data.
        output.emplace_back(ScriptCameraAction{
            .command = ScriptCameraCommand::ModifyRollingAverage,
            .rollingAverageFrames = *framesToAverage,
        });
        return true;
    }
    if (name == "SETUP_CAMERA")
    {
        const auto waypoint = textParameter(instruction, 0, context, scriptName, name);
        const auto zoom = realParameter(instruction, 1, context, scriptName, name);
        const auto pitch = realParameter(instruction, 2, context, scriptName, name);
        const auto lookAtWaypoint = textParameter(instruction, 3, context, scriptName, name);
        if (!waypoint || !zoom || !pitch || !lookAtWaypoint)
            return false;
        output.emplace_back(ScriptCameraAction{
            .command = ScriptCameraCommand::Setup,
            .waypointName = *waypoint,
            .lookAtWaypointName = *lookAtWaypoint,
            .value = *zoom,
            // The original SETUP_CAMERA executes cameraModFinalPitch after
            // cameraModFinalZoom. Preserve both values without inventing an
            // intermediate pose in the immutable compiler.
            .secondaryValue = *pitch,
        });
        return true;
    }
    if (name == "ZOOM_CAMERA" || name == "PITCH_CAMERA")
    {
        const auto value = realParameter(instruction, 0, context, scriptName, name);
        const auto timing = cameraTimingParameters(instruction, 1, 2, 3, context, scriptName, name);
        if (!value || !timing)
            return false;
        output.emplace_back(ScriptCameraAction{
            .command = name == "ZOOM_CAMERA" ? ScriptCameraCommand::Zoom
                                               : ScriptCameraCommand::Pitch,
            .value = *value,
            .durationTicks = timing->durationTicks,
            .easeInTicks = timing->easeInTicks,
            .easeOutTicks = timing->easeOutTicks,
        });
        return true;
    }
    if (name == "ROTATE_CAMERA")
    {
        const auto rotations = realParameter(instruction, 0, context, scriptName, name);
        const auto timing = cameraTimingParameters(instruction, 1, 2, 3, context, scriptName, name);
        if (!rotations || !timing)
            return false;
        output.emplace_back(ScriptCameraAction{
            .command = ScriptCameraCommand::Rotate,
            .value = *rotations,
            .durationTicks = timing->durationTicks,
            .easeInTicks = timing->easeInTicks,
            .easeOutTicks = timing->easeOutTicks,
        });
        return true;
    }
    if (name == "CAMERA_LOOK_TOWARD_WAYPOINT")
    {
        const auto waypoint = textParameter(instruction, 0, context, scriptName, name);
        const auto timing = cameraTimingParameters(instruction, 1, 2, 3, context, scriptName, name);
        const auto reverse = integerParameter(instruction, 4, context, scriptName, name);
        if (!waypoint || !timing || !reverse)
            return false;
        output.emplace_back(ScriptCameraAction{
            .command = ScriptCameraCommand::LookTowardWaypoint,
            .waypointName = *waypoint,
            .durationTicks = timing->durationTicks,
            .easeInTicks = timing->easeInTicks,
            .easeOutTicks = timing->easeOutTicks,
            .reverseRotation = *reverse != 0,
        });
        return true;
    }
    if (name == "CAMERA_LOOK_TOWARD_OBJECT")
    {
        const auto object = textParameter(instruction, 0, context, scriptName, name);
        const auto duration = realParameter(instruction, 1, context, scriptName, name);
        const auto hold = realParameter(instruction, 2, context, scriptName, name);
        const auto easeIn = realParameter(instruction, 3, context, scriptName, name);
        const auto easeOut = realParameter(instruction, 4, context, scriptName, name);
        if (!object || !duration || !hold || !easeIn || !easeOut ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *object,
                                               instruction.serialized)) {
            return false;
        }
        const auto timing = cameraTimingParameters(instruction, 1, 3, 4, context, scriptName, name);
        const auto holdTicks = secondsToTicks(*hold, context, scriptName, instruction.serialized);
        if (!timing || !holdTicks)
            return false;
        output.emplace_back(ScriptCameraAction{
            .command = ScriptCameraCommand::LookTowardNamedObject,
            .objectName = *object,
            .durationTicks = timing->durationTicks,
            .easeInTicks = timing->easeInTicks,
            .easeOutTicks = timing->easeOutTicks,
            .holdTicks = static_cast<uint32_t>(*holdTicks),
        });
        return true;
    }
    if (name == "RESET_CAMERA")
    {
        const auto waypoint = textParameter(instruction, 0, context, scriptName, name);
        const auto timing = cameraTimingParameters(instruction, 1, 2, 3, context, scriptName, name);
        if (!waypoint || !timing)
            return false;
        output.emplace_back(ScriptCameraAction{
            .command = ScriptCameraCommand::Reset,
            .waypointName = *waypoint,
            .durationTicks = timing->durationTicks,
            .easeInTicks = timing->easeInTicks,
            .easeOutTicks = timing->easeOutTicks,
        });
        return true;
    }
    if (name == "CAMERA_SET_DEFAULT")
    {
        const auto pitch = realParameter(instruction, 0, context, scriptName, name);
        const auto angle = realParameter(instruction, 1, context, scriptName, name);
        const auto maxHeight = realParameter(instruction, 2, context, scriptName, name);
        if (!pitch || !angle || !maxHeight)
            return false;
        output.emplace_back(ScriptCameraAction{
            .command = ScriptCameraCommand::SetDefault,
            .value = *pitch,
            .secondaryValue = *angle,
            .tertiaryValue = *maxHeight,
        });
        return true;
    }
    // Both are intentional RefCode no-ops: accepting their authored form
    // improves map compatibility without fabricating presentation state.
    if (name == "CAMERA_MOVE_HOME")
        return true;
    if (name == "CAMERA_SET_AUDIBLE_DISTANCE")
    {
        return realParameter(instruction, 0, context, scriptName, name).has_value();
    }
    if (name == "CAMERA_FOLLOW_NAMED")
    {
        const auto object = textParameter(instruction, 0, context, scriptName, name);
        if (!object ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *object,
                                               instruction.serialized)) {
            return false;
        }
        const bool snap = instruction.parameters.size() > 1 &&
            instruction.parameters[1].integerValue != 0;
        output.emplace_back(ScriptCameraAction{
            .command = ScriptCameraCommand::FollowNamedObject,
            .objectName = *object,
            .enabled = snap,
        });
        return true;
    }
    if (name == "CAMERA_STOP_FOLLOW")
    {
        output.emplace_back(ScriptCameraAction{.command = ScriptCameraCommand::StopFollow});
        return true;
    }
    if (name == "CAMERA_TETHER_NAMED")
    {
        const auto object = textParameter(instruction, 0, context, scriptName, name);
        const auto snap = integerParameter(instruction, 1, context, scriptName, name);
        const auto play = realParameter(instruction, 2, context, scriptName, name);
        if (!object || !snap || !play ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *object,
                                               instruction.serialized)) {
            return false;
        }
        // W3DView stores the third scalar as LOCK_TETHER's local play
        // factor. It is not a timed camera transition, so preserve it raw in
        // the typed scalar slot rather than converting it to ticks.
        output.emplace_back(ScriptCameraAction{
            .command = ScriptCameraCommand::TetherNamedObject,
            .objectName = *object,
            .value = *play,
            .enabled = *snap != 0,
        });
        return true;
    }
    if (name == "CAMERA_STOP_TETHER_NAMED")
    {
        output.emplace_back(ScriptCameraAction{.command = ScriptCameraCommand::StopTether});
        return true;
    }
    return std::nullopt;
}

} // namespace engine::script::legacy::detail
