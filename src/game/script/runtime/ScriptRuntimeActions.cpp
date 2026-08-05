#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "ScriptRuntime.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>



namespace engine::script
{
namespace
{

[[nodiscard]] int32_t saturatedAdd(int32_t lhs, int32_t rhs) noexcept
{
    const int64_t sum = static_cast<int64_t>(lhs) + static_cast<int64_t>(rhs);
    return static_cast<int32_t>(
        std::clamp<int64_t>(sum, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
}

[[nodiscard]] bool finiteVec3(const math::vec3& value) noexcept
{
    return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
}

[[nodiscard]] uint64_t nextEvaluationTick(uint64_t current, uint32_t delay) noexcept
{
    if (delay == 0)
        return current;
    const uint64_t delta = static_cast<uint64_t>(delay);
    return current > std::numeric_limits<uint64_t>::max() - delta ? std::numeric_limits<uint64_t>::max()
                                                           : current + delta;
}

// Runtime state is queried for every scheduled script/group and again for
// enable/disable/call actions. Normal compiler IDs are compact, but the
// public builder intentionally permits sparse uint32 IDs for tools and
// imported content. Match ScriptProgram's bounded dense-index policy: direct
// lookup for compact ranges and an unordered lookup table otherwise, while
// retaining vectors as the only iteration/storage order.
constexpr size_t kMaximumDenseRuntimeStateIndexEntries = 1u << 20;
constexpr size_t kMaximumDenseRuntimeStateSlotsPerEntry = 8;

[[nodiscard]] bool shouldUseDenseRuntimeStateIndex(uint32_t maximumId,
                                                    size_t stateCount) noexcept
{
    if (stateCount == 0)
        return false;
    const uint64_t entries = static_cast<uint64_t>(maximumId) + 1u;
    return entries <= kMaximumDenseRuntimeStateIndexEntries &&
           entries <= static_cast<uint64_t>(stateCount) *
                          kMaximumDenseRuntimeStateSlotsPerEntry;
}
[[nodiscard]] int32_t saturatedWholeSecondsToTicks(
    int32_t seconds, uint32_t ticksPerSecond) noexcept
{
    const int64_t ticks = static_cast<int64_t>(seconds) *
        static_cast<int64_t>(ticksPerSecond);
    return static_cast<int32_t>(std::clamp<int64_t>(
        ticks, std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::max()));
}

[[nodiscard]] bool compare(int64_t lhs, ScriptComparison comparison, int64_t rhs) noexcept
{
    switch (comparison)
    {
    case ScriptComparison::Less:
        return lhs < rhs;
    case ScriptComparison::LessEqual:
        return lhs <= rhs;
    case ScriptComparison::Equal:
        return lhs == rhs;
    case ScriptComparison::GreaterEqual:
        return lhs >= rhs;
    case ScriptComparison::Greater:
        return lhs > rhs;
    case ScriptComparison::NotEqual:
        return lhs != rhs;
    }
    return false;
}

constexpr auto equalAsciiInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] bool isThisPlayerReference(container::StringView value) noexcept
{
    return equalAsciiInsensitive(value, "ThisPlayer") ||
           equalAsciiInsensitive(value, "<This Player>");
}

[[nodiscard]] bool isThisPlayerEnemyReference(
    container::StringView value) noexcept
{
    return equalAsciiInsensitive(value, "<This Player's Enemy>");
}

[[nodiscard]] bool isThisObjectReference(container::StringView value) noexcept
{
    return equalAsciiInsensitive(value, "<This Object>");
}

[[nodiscard]] bool isThisTeamReference(container::StringView value) noexcept
{
    return equalAsciiInsensitive(value, "<This Team>");
}

} // namespace

void ScriptRuntime::executeActions(const container::Vector<ScriptAction>& actions,
                                   ScriptId sourceScript,
                                   ScriptEffectSink& sink,
                                   ScriptRuntimeStepResult& result,
                                   uint32_t callDepth)
{
    for (const ScriptAction& action : actions)
    {
        executeAction(action, sourceScript, sink, result, callDepth);
        // Team death notifications are microtasks, not a once-per-frame
        // aggregate. Drain after each hook action so a synchronous kill runs
        // OnUnitDestroyed before the next authored action, while reentrant
        // drains merely append the next causal generation.
        if (m_teamHookDispatchActive)
            drainTeamUnitDestroyedHookEvents(sink, result, callDepth);
    }
}

void ScriptRuntime::executeAction(const ScriptAction& action,
                                  ScriptId sourceScript,
                                  ScriptEffectSink& sink,
                                  ScriptRuntimeStepResult& result,
                                  uint32_t callDepth)
{
    std::visit(
        [&](const auto& value)
        {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, ScriptNoOpAction>)
            {
                // Retained as an instruction slot for SequentialScript. It
                // intentionally produces no state mutation or effect here.
            }
            else if constexpr (std::is_same_v<Value, ScriptSequentialControlAction>)
            {
                if (!m_context.world) return;
                SequentialTargetKey target;
                target.kind = value.targetKind;
                if (value.targetKind == ScriptSequentialTargetKind::Object)
                {
                    const std::optional<ScriptWorldObjectSnapshot> object =
                        m_context.world->resolveObjectSelector(
                            value.object, m_currentInvocation);
                    if (!object || !object->id || !object->alive) return;
                    target.value = object->id.value;
                }
                else
                {
                    const std::optional<ObjectTeamId> team =
                        m_context.world->resolveTeamSelector(
                            value.team, m_currentInvocation);
                    if (!team) return;
                    target.value = team->value;
                }

                if (value.operation == ScriptSequentialControlOperation::Stop)
                {
                    clearSequential(target);
                    return;
                }
                if (!value.script) return;
                if (value.targetKind == ScriptSequentialTargetKind::Team)
                {
                    // RefCode groupIdle() precedes enqueue.  The authoritative
                    // command bridge clears current Team orders immediately;
                    // the AI runtime observes that stop later in this same
                    // confirmed tick before the runner requires idle.
                    emit(ScriptOrderEffect{
                             .kind = ScriptOrderKind::Stop,
                             .actorSelector = ScriptOrderActorSelector::ScenarioTeam,
                             .scenarioTeam = ObjectTeamId{target.value},
                         },
                         sourceScript, sink, result);
                }
                enqueueSequential(target, value.script, value.remainingRequeues);
            }
            else if constexpr (std::is_same_v<Value, ScriptSequentialWaitAction>)
            {
                // Only executeSequentialScripts may consume this instruction.
                // Reaching it through an ordinary Script branch is the modern
                // conservative equivalent of RefCode's debug crash/no-op.
            }
            else if constexpr (std::is_same_v<Value, ScriptSequentialTimedAction>)
            {
                if (!m_context.world) return;
                SequentialTargetKey target{.kind = value.targetKind};
                ScriptOrderEffect effect{
                    .kind = value.command ==
                                ScriptSequentialTimedCommand::GuardAtCurrentPosition
                        ? ScriptOrderKind::TacticalAttack
                        : ScriptOrderKind::Stop,
                    .tacticalAttackSubtype = value.command ==
                                ScriptSequentialTimedCommand::GuardAtCurrentPosition
                        ? ScriptTacticalAttackSubtype::Guard
                        : ScriptTacticalAttackSubtype::None,
                };
                if (value.targetKind == ScriptSequentialTargetKind::Object)
                {
                    const std::optional<ScriptWorldObjectSnapshot> object =
                        m_context.world->resolveObjectSelector(
                            value.object, m_currentInvocation);
                    if (!object || !object->id || !object->alive) return;
                    if (value.command ==
                            ScriptSequentialTimedCommand::GuardAtCurrentPosition &&
                        !m_context.world->sequentialObjectState(
                            object->id).canGuard) {
                        // RefCode returns before setSequentialTimer() when
                        // the object has no usable AI Guard path.
                        return;
                    }
                    target.value = object->id.value;
                    effect.actorSelector = ScriptOrderActorSelector::NamedObjects;
                    effect.actors.push_back(object->id);
                }
                else
                {
                    const std::optional<ObjectTeamId> team =
                        m_context.world->resolveTeamSelector(
                            value.team, m_currentInvocation);
                    if (!team) return;
                    target.value = team->value;
                    effect.actorSelector = ScriptOrderActorSelector::ScenarioTeam;
                    effect.scenarioTeam = *team;
                }
                if (value.command != ScriptSequentialTimedCommand::DelayOnly)
                    emit(std::move(effect), sourceScript, sink, result);
                setSequentialWaitFrames(target, value.frames);
            }
            else if constexpr (std::is_same_v<Value, ScriptSetTeamCustomStateAction>)
            {
                if (!m_context.world) return;
                const std::optional<ObjectTeamId> team = resolveTeam(value.team);
                if (!team) return;
                emit(ScriptTeamCustomStateEffect{
                         .team = *team,
                         .state = value.state,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptSetFlagAction>)
            {
                if (RuntimeFlagState* state = mutableFlag(value.flagSymbol))
                    state->value = value.value;
            }
            else if constexpr (std::is_same_v<Value, ScriptSetCounterAction>)
            {
                if (ScriptCounterState* state = mutableCounter(value.counterSymbol))
                    state->value = value.value;
            }
            else if constexpr (std::is_same_v<Value, ScriptAdjustCounterAction>)
            {
                if (ScriptCounterState* state = mutableCounter(value.counterSymbol))
                    state->value = saturatedAdd(state->value, value.delta);
            }
            else if constexpr (std::is_same_v<Value, ScriptSetTimerAction>)
            {
                if (ScriptCounterState* state = mutableCounter(value.timerSymbol))
                {
                    state->value = value.durationTicks;
                    state->countdownTimerRunning = true;
                }
            }
            else if constexpr (std::is_same_v<Value, ScriptAdjustTimerAction>)
            {
                if (ScriptCounterState* state = mutableCounter(value.timerSymbol))
                    state->value = saturatedAdd(
                        state->value, value.deltaTicks);
            }
            else if constexpr (std::is_same_v<Value, ScriptSetRandomTimerAction>)
            {
                if (!m_context.random)
                    return;
                if (ScriptCounterState* state = mutableCounter(value.timerSymbol))
                {
                    state->value = std::visit(
                        [this](const auto& range) noexcept -> int32_t
                        {
                            using Range = std::decay_t<decltype(range)>;
                            if constexpr (std::is_same_v<Range, ScriptRandomFrameTimerRange>)
                            {
                                return m_context.random->integerInclusive(
                                    range.minimumDurationTicks, range.maximumDurationTicks);
                            }
                            else
                            {
                                // ScriptEngine::setTimer() calls the integer
                                // GameLogicRandomValue macro even for this
                                // real-authored opcode. The range has already
                                // preserved that float-to-int truncation at
                                // compile time; consume the same integer RNG
                                // stream, then run the legacy float timing
                                // conversion on the selected whole seconds.
                                return saturatedWholeSecondsToTicks(
                                    m_context.random->integerInclusive(
                                        range.minimumSeconds, range.maximumSeconds),
                                    range.ticksPerSecond);
                            }
                        },
                        value.range);
                    state->countdownTimerRunning = true;
                }
            }
            else if constexpr (std::is_same_v<Value, ScriptStopTimerAction>)
            {
                if (ScriptCounterState* state = mutableCounter(value.timerSymbol))
                    state->countdownTimerRunning = false;
            }
            else if constexpr (std::is_same_v<Value, ScriptRestartTimerAction>)
            {
                ScriptCounterState* state = mutableCounter(value.timerSymbol);
                if (state && state->value > 0)
                    state->countdownTimerRunning = true;
            }
            else if constexpr (std::is_same_v<Value, ScriptEnableAction>)
            {
                if (value.target.kind == ScriptTargetKind::Script)
                {
                    if (RuntimeScriptState* state = mutableScriptState(value.target.script))
                        state->enabled = true;
                }
                else if (RuntimeGroupState* state = mutableGroupState(value.target.group))
                {
                    state->enabled = true;
                }
            }
            else if constexpr (std::is_same_v<Value, ScriptDisableAction>)
            {
                if (value.target.kind == ScriptTargetKind::Script)
                {
                    if (RuntimeScriptState* state = mutableScriptState(value.target.script))
                        state->enabled = false;
                }
                else if (RuntimeGroupState* state = mutableGroupState(value.target.group))
                {
                    state->enabled = false;
                }
            }
            else if constexpr (std::is_same_v<Value, ScriptCallSubroutineAction>)
            {
                executeTarget(value.target, sink, result, callDepth);
            }
            else if constexpr (std::is_same_v<Value, ScriptVictoryAction>)
            {
                emit(ScriptVictoryEffect{.mode = value.mode}, sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptDefeatAction>)
            {
                emit(ScriptDefeatEffect{}, sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptDebugMessageAction>)
            {
                emit(ScriptDebugMessageEffect{.text = value.text, .kind = value.kind},
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptDisplayTextAction>)
            {
                emit(ScriptTextEffect{.text = value.text, .localized = value.localized}, sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptDisplayCinematicTextAction>)
            {
                emit(ScriptCinematicTextEffect{
                         .text = value.text,
                         .fontDescriptor = value.fontDescriptor,
                         .durationTicks = value.durationTicks,
                         .localized = value.localized,
                     },
                     sourceScript,
                     sink,
                     result);
            }
            else if constexpr (std::is_same_v<Value, ScriptMilitaryCaptionAction>)
            {
                emit(ScriptMilitaryCaptionEffect{
                         .text = value.text,
                         .durationMilliseconds = value.durationMilliseconds,
                         .localized = value.localized,
                     },
                     sourceScript,
                     sink,
                     result);
            }
            else if constexpr (std::is_same_v<Value, ScriptMovieAction>)
            {
                emit(ScriptMovieEffect{
                         .target = value.target,
                         .movieName = value.movieName,
                     },
                     sourceScript,
                     sink,
                     result);
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayAudioAction>)
            {
                ScriptAudioEffect effect{
                    .eventName = value.eventName,
                    .position = value.position,
                    .waypointName = value.waypointName,
                    .emitterName = value.emitterName,
                    .volumeScale = value.volumeScale,
                    .uninterruptible = value.uninterruptible,
                };
                if (!value.emitterName.empty())
                {
                    // SOUND_PLAY_NAMED in RefCode is a no-op when the
                    // named object does not exist. Do not degrade it to a
                    // non-spatial global sound while a map is loading or an
                    // earlier same-tick effect has removed that object.
                    if (!m_context.world)
                        return;
                    const auto snapshot = resolveObject(value.emitterName);
                    if (!snapshot || !snapshot->alive || !snapshot->id)
                        return;
                    effect.emitter = snapshot->id;
                    effect.position = snapshot->position;
                }
                emit(std::move(effect), sourceScript, sink, result);
                if (!value.subtitleLabel.empty())
                {
                    emit(ScriptSubtitleEffect{
                             .label = value.subtitleLabel,
                             .durationTicks = value.subtitleDurationTicks,
                         },
                         sourceScript, sink, result);
                }
            }
            else if constexpr (std::is_same_v<Value, ScriptMusicAction>)
            {
                // Music replacement/volume is presentation work, but its
                // source-order position is observable: a later script in the
                // same confirmed tick may replace the just-selected track.
                // Emit one detached command rather than resolving an INI
                // MusicTrack or touching an audio device from simulation.
                emit(ScriptMusicEffect{
                         .command = value.command,
                         .trackName = value.trackName,
                         .fadeOut = value.fadeOut,
                         .fadeIn = value.fadeIn,
                         .volume = value.volume,
                     },
                     sourceScript,
                     sink,
                     result);
            }
            else if constexpr (std::is_same_v<Value, ScriptAmbientAudioAction>)
            {
                emit(ScriptAmbientAudioEffect{.paused = value.paused}, sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptTimeControlAction>)
            {
                // This passes through the normal confirmed effect stream so
                // a FREEZE followed by UNFREEZE in one source frame retains
                // its authored ordering at GameSession.  ScriptRuntime itself
                // must keep advancing while frozen.
                emit(ScriptTimeControlEffect{.frozen = value.frozen}, sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptHulkLifetimeOverrideAction>)
            {
                emit(ScriptHulkLifetimeOverrideEffect{
                         .seconds = value.seconds},
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptScoreAccumulationPolicyAction>)
            {
                // This is the legacy GameLogic score-accumulation gate, not
                // a local score-screen control.  Preserve one stamped write
                // per action so same-tick source order remains observable.
                emit(ScriptScoreAccumulationPolicyEffect{.enabled = value.enabled},
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptVisualSpeedAction>)
            {
                // This is a direct tactical-view visual-clock write, not a
                // simulation-delta multiplier.  The bridge/director retain
                // it in confirmed source order and GameLogic decides whether
                // the active match mode may consume it for extra local work.
                emit(ScriptVisualSpeedEffect{.multiplier = value.multiplier},
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptAudioControlAction>)
            {
                // These controls mutate only the client audio policy.  They
                // still cross the confirmed boundary as individually stamped
                // values: PLAY(A), DISABLE(A), and REMOVE(A) are source-order
                // observable in one script frame.
                emit(ScriptAudioControlEffect{
                         .command = value.command,
                         .eventName = value.eventName,
                         .volume = value.volume,
                         .enabled = value.enabled,
                     },
                     sourceScript,
                     sink,
                     result);
            }
            else if constexpr (std::is_same_v<Value, ScriptUiAction>)
            {
                emit(ScriptUiEffect{
                         .command = value.command,
                         .control = value.control,
                         .indicatorKind = value.indicatorKind,
                         .enabled = value.enabled,
                         .name = value.name,
                         .text = value.text,
                         .xPercent = value.xPercent,
                         .yPercent = value.yPercent,
                         .width = value.width,
                         .pauseRequested = value.pauseRequested,
                         .flashCount = value.flashCount,
                         .framesPerFlash = value.framesPerFlash,
                     },
                     sourceScript,
                     sink,
                     result);
            }
            else if constexpr (std::is_same_v<Value, ScriptCommandBarOverrideAction>)
            {
                // The legacy action is a content/UI mutation, not an object
                // order.  Keep names and the raw one-based slot detached
                // until GameSession resolves them through its frozen content
                // snapshot in confirmed source order.
                emit(ScriptCommandBarOverrideEffect{
                         .command = value.command,
                         .commandButtonName = value.commandButtonName,
                         .objectTypeName = value.objectTypeName,
                         .oneBasedSlot = value.oneBasedSlot,
                     },
                     sourceScript,
                     sink,
                     result);
            }
            else if constexpr (std::is_same_v<Value, ScriptClientOptionsAction>)
            {
                emit(ScriptClientOptionsEffect{
                         .command = value.command,
                         .frameRateLimit = value.frameRateLimit,
                         .enabled = value.enabled,
                     },
                     sourceScript,
                     sink,
                     result);
            }
            else if constexpr (std::is_same_v<Value, ScriptMapPresentationAction>)
            {
                emit(ScriptMapPresentationEffect{
                         .command = value.command,
                         .enabled = value.enabled,
                         .position = value.position,
                         .radarEventType = value.radarEventType,
                         .boundaryIndex = value.boundaryIndex,
                         .waypointName = value.waypointName,
                         .playerName = value.playerName,
                         .revealName = value.revealName,
                         .radius = value.radius,
                     },
                     sourceScript,
                     sink,
                     result);
            }
            else if constexpr (std::is_same_v<Value, ScriptObjectPresentationAction>)
            {
                if (!m_context.world)
                    return;

                ScriptObjectPresentationEffect effect{
                    .command = value.command,
                    .target = value.target,
                    .radarEventType = value.radarEventType,
                    .durationSeconds = value.durationSeconds,
                    .flashColor = value.flashColor,
                    .packedColor = value.packedColor,
                    .emoticonDurationSeconds = value.emoticonDurationSeconds,
                    .emoticonName = value.emoticonName,
                    .ambientSoundEnabled = value.ambientSoundEnabled,
                    .specialPowerDisplayVisible = value.specialPowerDisplayVisible,
                };
                if (value.target.kind == ScriptObjectPresentationTargetKind::Team) {
                    const std::optional<ObjectTeamId> team =
                        resolveTeam(value.target.name);
                    if (!team) return;
                    effect.team = *team;
                }
                switch (value.command) {
                case ScriptObjectPresentationCommand::CreateRadarEvent:
                    switch (value.target.kind)
                    {
                    case ScriptObjectPresentationTargetKind::NamedObject:
                    {
                        const std::optional<ScriptWorldObjectSnapshot> object =
                            resolveObject(value.target.name);
                        if (!object || !object->alive || !object->id || !finiteVec3(object->position))
                            return;
                        effect.object = object->id;
                        effect.position = object->position;
                        break;
                    }
                    case ScriptObjectPresentationTargetKind::Team:
                    {
                        const std::optional<math::vec3> position =
                            m_context.world->teamRadarEventPosition(effect.team);
                        if (!position || !finiteVec3(*position))
                            return;
                        effect.position = *position;
                        break;
                    }
                    default:
                        return;
                    }
                    break;
                case ScriptObjectPresentationCommand::Flash:
                    if (value.durationSeconds <= 0) return;
                    if (value.target.kind == ScriptObjectPresentationTargetKind::NamedObject) {
                        const std::optional<ScriptWorldObjectSnapshot> object =
                            resolveObject(value.target.name);
                        if (!object || !object->alive || !object->id) return;
                        effect.object = object->id;
                    } else if (value.target.kind == ScriptObjectPresentationTargetKind::Team) {
                        // Team flash expands its membership only at the bridge
                        // application point. This query merely preserves the
                        // original empty-Team silent no-op without exposing a
                        // vector of ECS members to ScriptRuntime.
                        if (!m_context.world->teamSummary(effect.team).hasObjects) return;
                    } else {
                        return;
                    }
                    break;
                case ScriptObjectPresentationCommand::SetCustomIndicatorColor:
                {
                    if (value.target.kind != ScriptObjectPresentationTargetKind::NamedObject) return;
                    const std::optional<ScriptWorldObjectSnapshot> object =
                        resolveObject(value.target.name);
                    if (!object || !object->alive || !object->id) return;
                    effect.object = object->id;
                    break;
                }
                case ScriptObjectPresentationCommand::SetEmoticon:
                {
                    if (value.target.kind == ScriptObjectPresentationTargetKind::NamedObject) {
                        const std::optional<ScriptWorldObjectSnapshot> object =
                            resolveObject(value.target.name);
                        if (!object || !object->alive || !object->id) return;
                        effect.object = object->id;
                    } else if (value.target.kind == ScriptObjectPresentationTargetKind::Team) {
                        // Team expansion belongs to the session bridge. The
                        // runtime only observes the old silent empty-Team
                        // no-op through a compact summary; it never scans
                        // ECS or retains a competing member list.
                        if (!m_context.world->teamSummary(effect.team).hasObjects) return;
                    } else {
                        return;
                    }
                    break;
                }
                case ScriptObjectPresentationCommand::SetAmbientSoundEnabled:
                case ScriptObjectPresentationCommand::SetSpecialPowerDisplayVisible:
                {
                    if (value.target.kind != ScriptObjectPresentationTargetKind::NamedObject) return;
                    const std::optional<ScriptWorldObjectSnapshot> object =
                        resolveObject(value.target.name);
                    if (!object || !object->alive || !object->id) return;
                    effect.object = object->id;
                    break;
                }
                }
                emit(std::move(effect), sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptForceObjectSelectionAction>)
            {
                // OBJECT_FORCE_SELECT is intentionally unlike named-object
                // presentation actions above: the runtime must not scan a
                // Team, resolve an ObjectId, or inspect this client's
                // LocalSelectionState.  The bridge resolves the authored
                // selector in confirmed source order, then the main-thread
                // local consumer applies the stamped presentation request.
                emit(ScriptForceObjectSelectionEffect{
                         .teamName = value.teamName,
                         .objectTypeName = value.objectTypeName,
                         .centerInView = value.centerInView,
                         .audioEventName = value.audioEventName,
                     },
                     sourceScript,
                     sink,
                     result);
            }
            else if constexpr (std::is_same_v<Value, ScriptViewCompatibilityAction>)
            {
                // These legacy W3D culling controls retain their raw values
                // until the session/render boundary. ScriptRuntime neither
                // owns a terrain window nor performs client visibility work.
                emit(ScriptViewCompatibilityEffect{
                         .command = value.command,
                         .terrainOversizeTiles = value.terrainOversizeTiles,
                         .guardBandX = value.guardBandX,
                         .guardBandY = value.guardBandY,
                     },
                     sourceScript,
                     sink,
                     result);
            }
            else if constexpr (std::is_same_v<Value, ScriptCameraAction>)
            {
                ScriptCameraEffect effect{
                    .command = value.command,
                    .position = value.position,
                    .target = value.target,
                    .waypointName = value.waypointName,
                    .lookAtWaypointName = value.lookAtWaypointName,
                    .objectName = value.objectName,
                    .value = value.value,
                    .secondaryValue = value.secondaryValue,
                    .tertiaryValue = value.tertiaryValue,
                    .rollingAverageFrames = value.rollingAverageFrames,
                    .visualSpeedMultiplier = value.visualSpeedMultiplier,
                    .durationTicks = value.durationTicks,
                    .easeInTicks = value.easeInTicks,
                    .easeOutTicks = value.easeOutTicks,
                    .holdTicks = value.holdTicks,
                    .reverseRotation = value.reverseRotation,
                    .enabled = value.enabled,
                };
                if (!value.objectName.empty() && m_context.world)
                {
                    if (const auto snapshot = resolveObject(value.objectName);
                        snapshot && snapshot->alive && snapshot->id)
                    {
                        effect.object = snapshot->id;
                        if (value.command == ScriptCameraCommand::FollowNamedObject ||
                            value.command == ScriptCameraCommand::TetherNamedObject ||
                            value.command == ScriptCameraCommand::LookTowardNamedObject)
                        {
                            effect.position = snapshot->position;
                        }
                        if (value.command == ScriptCameraCommand::FollowNamedObject ||
                            value.command == ScriptCameraCommand::TetherNamedObject)
                        {
                            effect.target = snapshot->position;
                        }
                    }
                }
                emit(std::move(effect), sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptCameraSlaveAction>)
            {
                ScriptCameraSlaveEffect effect{
                    .objectName = value.objectName,
                    .boneName = value.boneName,
                    .enabled = value.enabled,
                };
                if (value.enabled && m_context.world)
                {
                    if (const auto snapshot = resolveObject(value.objectName);
                        snapshot && snapshot->alive && snapshot->id)
                    {
                        effect.object = snapshot->id;
                    }
                }
                // The request remains value-only even when the object has
                // disappeared since script evaluation. The bridge then clears
                // the prior presentation request instead of incorrectly
                // retaining a stale slave camera.
                emit(std::move(effect), sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptScreenShakeAction>)
            {
                // This deliberately remains a detached presentation impulse.
                // RefCode shakes the current tactical view but does not alter
                // its durable scripted camera motion or simulation state.
                emit(ScriptScreenShakeEffect{.intensity = value.intensity},
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptLocalizedCameraShakeAction>)
            {
                emit(ScriptLocalizedCameraShakeEffect{
                         .waypointName = value.waypointName,
                         .amplitude = value.amplitude,
                         .durationTicks = value.durationTicks,
                         .radius = value.radius,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptScreenFadeAction>)
            {
                // RefCode owns one global fade slot.  Do not fold it into a
                // logic-camera transition or a wall-clock UI animation: the
                // bridge/session advances its authored curve once per
                // confirmed simulation tick before later script actions run.
                emit(ScriptScreenFadeEffect{
                         .blendMode = value.blendMode,
                         .minimumIntensity = value.minimumIntensity,
                         .maximumIntensity = value.maximumIntensity,
                         .increaseFrames = value.increaseFrames,
                         .holdFrames = value.holdFrames,
                         .decreaseFrames = value.decreaseFrames,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptBlackAndWhiteAction>)
            {
                // This is a renderer-owned replacement filter, not a camera
                // transition. Keep both Begin and End as stamped commands:
                // the presentation client alone can tell whether an End still
                // targets BW after another view filter took ownership.
                emit(ScriptBlackAndWhiteEffect{
                         .enabled = value.enabled,
                         .transitionFrames = value.transitionFrames,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptMotionBlurAction>)
            {
                // The old W3D filter owns both its radial/pan frame state and
                // the captured tactical-view color.  Keep the command out of
                // GameCameraDirector and ECS; only a Jump waypoint needs
                // bridge-time map resolution before it reaches presentation.
                emit(ScriptMotionBlurEffect{
                         .mode = value.mode,
                         .saturate = value.saturate,
                         .waypointName = value.waypointName,
                         .followAmount = value.followAmount,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptSkyboxAction>)
            {
                // W3DWater's skybox toggle has no object/ECS dependency. The
                // bridge publishes a durable presentation state; W3D asset
                // lifetime and map material selection remain renderer-owned.
                emit(ScriptSkyboxEffect{.enabled = value.enabled}, sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptTreeSwayAction>)
            {
                // Keep the five authored parameters together.  The bridge
                // stamps one new BreezeInfo generation; individual visual
                // phases are deliberately not ScriptRuntime/simulation state.
                emit(ScriptTreeSwayEffect{
                         .directionRadians = value.directionRadians,
                         .intensityRadians = value.intensityRadians,
                         .leanRadians = value.leanRadians,
                         .periodFrames = value.periodFrames,
                         .randomness = value.randomness,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptWeatherAction>)
            {
                // SHOW_WEATHER only toggles an already loaded Weather.ini
                // effect. The runtime retains no VFS/snow/renderer handle.
                emit(ScriptWeatherEffect{.visible = value.visible}, sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptInfantryLightingAction>)
            {
                // This is a retained world-presentation value rather than a
                // mutable GlobalData write.  Reset stays explicit as an
                // empty optional so a session can return to normal map
                // lighting without ScriptRuntime knowing time-of-day data.
                emit(ScriptInfantryLightingEffect{.overrideScale = value.overrideScale},
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptWaterAction>)
            {
                emit(
                    ScriptWaterEffect{
                        .command = value.command,
                        .waterName = value.waterName,
                        .value = value.value,
                        .transitionTicks = value.transitionTicks,
                        .damagePerSecond = value.damagePerSecond,
                        .enabled = value.enabled,
                    },
                    sourceScript,
                     sink,
                     result);
            }
            else if constexpr (std::is_same_v<
                                   Value,
                                   ScriptFireWeaponFollowingWaypointPathAction>)
            {
                if (!m_context.world || value.waypointPathName.empty())
                    return;
                const std::optional<ScriptWorldObjectSnapshot> object =
                    resolveObject(value.objectName);
                // RefCode silently returns before querying Terrain/WeaponSet
                // when the named Object is absent or effectively dead.
                if (!object || !object->alive || !object->id)
                    return;
                emit(ScriptFireWeaponFollowingWaypointPathEffect{
                         .object = object->id,
                         .waypointPathName = value.waypointPathName,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<
                                   Value,
                                   ScriptCreateReinforcementTeamAction>)
            {
                // Team instance creation and waypoint/content resolution are
                // structural GameSession work.  Preserve source order by
                // emitting the detached request synchronously like object
                // creation, without resolving an existing Team alias here.
                emit(ScriptCreateReinforcementTeamEffect{
                         .teamName = value.teamName,
                         .destinationWaypointName =
                             value.destinationWaypointName,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptBuildTeamAction>)
            {
                emit(ScriptBuildTeamEffect{.teamName = value.teamName},
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<
                                   Value, ScriptGuardSupplyCenterAction>)
            {
                emit(ScriptGuardSupplyCenterEffect{
                         .teamName = value.teamName,
                         .minimumSupplies = value.minimumSupplies,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptRecruitTeamAction>)
            {
                emit(ScriptRecruitTeamEffect{
                         .teamName = value.teamName,
                         .radius = value.radius,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptIssueOrderAction>)
            {
                ScriptOrderEffect effect{
                    .kind = value.kind,
                    .moveRouteSubtype = value.moveRouteSubtype,
                    .tacticalAttackSubtype = value.tacticalAttackSubtype,
                    .actorSelector = value.actorSelector,
                    .teamName = value.teamName,
                    .playerName = value.playerName,
                    .targetTeamName = value.targetTeamName,
                    .targetAreaName = value.targetAreaName,
                    .contentName = value.contentName,
                    .forceAttack = value.forceAttack,
                    .allArmyHunt = value.allArmyHunt,
                    .useTeamCommonTarget = value.useTeamCommonTarget,
                    .disbandAfterStop = value.disbandAfterStop,
                    .queued = value.queued,
                };
                effect.targetPosition = value.targetPosition;
                effect.targetWaypointName = value.targetWaypointName;
                if (!value.targetObjectName.empty())
                {
                    // The original named-object attack actions do not leave
                    // an invalid AI command behind when their victim was
                    // deleted earlier in this confirmed script pass. Resolve
                    // the name at the value boundary and make that case a
                    // silent script no-op, rather than emitting an Attack
                    // effect that the queue must later reject.
                    const auto target = m_context.world
                        ? resolveObject(value.targetObjectName)
                        : std::nullopt;
                    if (target && target->alive && target->id)
                    {
                        effect.targetObject = target->id;
                    }
                    else if (value.kind == ScriptOrderKind::Attack ||
                             value.kind == ScriptOrderKind::SpecialPower ||
                             (value.kind == ScriptOrderKind::TacticalAttack &&
                              value.tacticalAttackSubtype ==
                                  ScriptTacticalAttackSubtype::Guard))
                    {
                        return;
                    }
                }
                if (value.actorSelector == ScriptOrderActorSelector::ScenarioTeam)
                {
                    // Team membership is resolved at bridge admission time,
                    // not here. A legacy compiler cannot know a map's live
                    // ScenarioDefinition/ObjectTeam mapping, and effects
                    // before this one may have changed that membership.
                    if (effect.teamName.empty())
                        return;
                }
                else if (value.actorSelector == ScriptOrderActorSelector::PlayerAssets)
                {
                    // Ownership is live and may have changed earlier in this
                    // same script pass, so the runtime intentionally carries
                    // only the authored player selector to the bridge.
                    if (effect.playerName.empty())
                        return;
                }
                else
                {
                    if (!m_context.world || value.actorNames.empty())
                        return;
                    for (const container::String& actorName : value.actorNames)
                    {
                        if (const auto actor = resolveObject(actorName);
                            actor && actor->alive && actor->id)
                        {
                            effect.actors.push_back(actor->id);
                        }
                    }
                    std::sort(effect.actors.begin(), effect.actors.end());
                    effect.actors.erase(std::unique(effect.actors.begin(), effect.actors.end()), effect.actors.end());
                    if (effect.actors.empty())
                        return;
                }
                emit(std::move(effect), sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptUseCommandButtonAction>)
            {
                ScriptUseCommandButtonEffect effect{
                    .actorSelector = value.actorSelector,
                    .teamName = value.teamName,
                    .buttonName = value.buttonName,
                    .actorPolicy = value.actorPolicy,
                    .actorPercentage = value.actorPercentage,
                    .preselectSourceAndTarget =
                        value.preselectSourceAndTarget,
                    .targetKind = value.targetKind,
                    .targetWaypointName = value.targetWaypointName,
                    .targetFilter = value.targetFilter,
                };
                if (value.targetKind ==
                    ScriptCommandButtonTargetKind::NamedObject) {
                    if (!m_context.world) return;
                    const auto target = resolveObject(value.targetObjectName);
                    // RefCode returns before looking up the button when the
                    // named target no longer exists.
                    if (!target || !target->alive || !target->id) return;
                    effect.targetObject = target->id;
                }
                if (value.targetKind ==
                    ScriptCommandButtonTargetKind::NearestObjectType) {
                    if (const auto list = objectTypeList(value.targetFilter)) {
                        effect.targetObjectTypes.assign(
                            list->begin(), list->end());
                    }
                }
                if (value.actorSelector ==
                    ScriptOrderActorSelector::ScenarioTeam) {
                    if (effect.teamName.empty()) return;
                } else {
                    if (!m_context.world || value.actorNames.size() != 1u)
                        return;
                    const auto actor = resolveObject(value.actorNames.front());
                    if (!actor || !actor->alive || !actor->id) return;
                    effect.actors.push_back(actor->id);
                }
                emit(std::move(effect), sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptFacingAction>)
            {
                ScriptFacingEffect effect{
                    .actorSelector = value.actorSelector,
                    .teamName = value.teamName,
                };
                if (value.targetKind ==
                    ScriptFacingTargetKind::NamedObject) {
                    if (!m_context.world) return;
                    const auto target = resolveObject(value.targetName);
                    if (!target || !target->alive || !target->id) return;
                    effect.targetObject = target->id;
                } else {
                    effect.targetWaypointName = value.targetName;
                }
                if (value.actorSelector ==
                    ScriptOrderActorSelector::ScenarioTeam) {
                    if (effect.teamName.empty()) return;
                } else {
                    if (!m_context.world || value.actorNames.size() != 1u)
                        return;
                    const auto actor = resolveObject(value.actorNames.front());
                    if (!actor || !actor->alive || !actor->id) return;
                    effect.actors.push_back(actor->id);
                }
                emit(std::move(effect), sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptAIBehaviorMutationAction>)
            {
                ScriptAIBehaviorMutationEffect effect{
                    .targetKind = value.targetKind,
                    .mutation = value.mutation,
                    .attackPrioritySet = value.attackPrioritySet,
                    .commandButton = value.commandButton,
                    .attitude = value.attitude,
                };
                if (value.targetKind ==
                    ScriptAIBehaviorTargetKind::NamedObject) {
                    if (!m_context.world) return;
                    const auto object = resolveObject(value.targetName);
                    if (!object || !object->alive || !object->id) return;
                    effect.object = object->id;
                } else {
                    effect.teamName = value.targetName;
                }
                emit(std::move(effect), sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptAttackPriorityMutationAction>)
            {
                ScriptAttackPriorityMutationEffect effect{
                    .mutation = value.mutation,
                    .setName = value.setName,
                    .priority = value.priority,
                };
                if (value.mutation ==
                    ScriptAttackPriorityMutationKind::ObjectType) {
                    if (const auto list = objectTypeList(value.selector))
                        effect.selectors.assign(list->begin(), list->end());
                    else
                        effect.selectors.push_back(value.selector);
                } else if (value.mutation ==
                           ScriptAttackPriorityMutationKind::KindOf) {
                    effect.selectors.push_back(value.selector);
                }
                emit(std::move(effect), sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptStoppingDistanceAction>)
            {
                ScriptStoppingDistanceEffect effect{
                    .targetKind = value.targetKind,
                    .distance = value.distance,
                };
                if (value.targetKind == ScriptStoppingDistanceTargetKind::NamedObject)
                {
                    if (!m_context.world) return;
                    const std::optional<ScriptWorldObjectSnapshot> object =
                        resolveObject(value.targetName);
                    if (!object || !object->alive || !object->id) return;
                    effect.object = object->id;
                }
                else
                {
                    effect.teamName = value.targetName;
                }
                emit(std::move(effect), sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptMoveTowardsNearestAction>)
            {
                ScriptMoveTowardsNearestEffect effect{
                    .actorSelector = value.actorSelector,
                    .teamName = value.teamName,
                    .triggerArea = value.triggerArea,
                };
                const std::optional<container::Span<const container::String>> list =
                    objectTypeList(value.objectType);
                if (list)
                    effect.objectTypes.assign(list->begin(), list->end());
                else
                    effect.objectTypes.push_back(value.objectType);
                if (value.actorSelector == ScriptOrderActorSelector::NamedObjects)
                {
                    if (!m_context.world) return;
                    const std::optional<ScriptWorldObjectSnapshot> actor =
                        resolveObject(value.actorName);
                    if (!actor || !actor->alive || !actor->id) return;
                    effect.actors.push_back(actor->id);
                }
                emit(std::move(effect), sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptSpecialPowerCountdownAction>)
            {
                if (!m_context.world) return;
                const std::optional<ScriptWorldObjectSnapshot> object =
                    resolveObject(value.objectName);
                if (!object || !object->alive || !object->id) return;
                emit(ScriptSpecialPowerCountdownEffect{
                         .operation = value.operation,
                         .object = object->id,
                         .specialPower = value.specialPower,
                         .seconds = value.seconds,
                         .paused = value.paused,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptWarehouseValueAction>)
            {
                if (!m_context.world) return;
                const std::optional<ScriptWorldObjectSnapshot> object =
                    resolveObject(value.objectName);
                if (!object || !object->alive || !object->id) return;
                emit(ScriptWarehouseValueEffect{
                         .object = object->id,
                         .cashValue = value.cashValue,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptCaveIndexAction>)
            {
                if (!m_context.world) return;
                const std::optional<ScriptWorldObjectSnapshot> object =
                    resolveObject(value.objectName);
                if (!object || !object->alive || !object->id) return;
                emit(ScriptCaveIndexEffect{
                         .object = object->id,
                         .caveIndex = value.caveIndex,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptCreateObjectAction>)
            {
                // Object spawning is intentionally not resolved against ECS
                // here. The immutable runtime preserves the authored request;
                // the session bridge resolves Team/waypoint/content only at
                // the stamped effect boundary and uses GameSession's one
                // authoritative ObjectLifecycle path.
                emit(ScriptCreateObjectEffect{
                         .objectName = value.objectName,
                         .templateName = value.templateName,
                         .teamName = value.teamName,
                         .position = value.position,
                         .waypointName = value.waypointName,
                         .rotation = value.rotation,
                     },
                     sourceScript,
                     sink,
                     result);
            }
            else if constexpr (std::is_same_v<Value, ScriptDestroyNamedObjectAction>)
            {
                if (!m_context.world)
                    return;
                const std::optional<ScriptWorldObjectSnapshot> object =
                    resolveObject(value.objectName);
                if (!object || !object->alive || !object->id)
                    return;
                emit(ScriptDestroyObjectEffect{
                         .object = object->id,
                         .objectName = value.objectName,
                         .forceKill = value.forceKill,
                     },
                     sourceScript,
                     sink,
                     result);
            }
            else if constexpr (std::is_same_v<Value, ScriptLifecycleAction>)
            {
                if (value.targetName.empty()) return;
                emit(ScriptLifecycleEffect{
                         .targetKind = value.targetKind,
                         .operation = value.operation,
                         .targetName = value.targetName,
                     },
                     sourceScript,
                     sink,
                     result);
            }
            else if constexpr (std::is_same_v<Value, ScriptContainmentAction>)
            {
                ScriptContainmentEffect effect{
                    .kind = value.kind,
                    .targetName = value.targetName,
                    .evacuationDisposition = value.evacuationDisposition,
                };
                const bool namedTarget =
                    value.kind == ScriptContainmentActionKind::EjectContainerContents ||
                    value.kind == ScriptContainmentActionKind::EjectSpecificStructure ||
                    value.kind == ScriptContainmentActionKind::DetachNamedOccupant ||
                    value.kind == ScriptContainmentActionKind::KillContainerContents ||
                    value.kind ==
                        ScriptContainmentActionKind::SetEvacuationDisposition;
                if (namedTarget)
                {
                    if (!m_context.world) return;
                    const std::optional<ScriptWorldObjectSnapshot> object =
                        resolveObject(value.targetName);
                    if (!object || !object->alive || !object->id) return;
                    effect.namedTarget = object->id;
                }
                else if (value.targetName.empty() &&
                         value.kind != ScriptContainmentActionKind::EjectPlayerStructures)
                {
                    return;
                }
                emit(std::move(effect), sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptContainmentEnterAction>)
            {
                emit(ScriptContainmentEnterEffect{
                         .kind = value.kind,
                         .object = value.object,
                         .team = value.team,
                         .player = value.player,
                         .container = value.container,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptTransferOwnershipAction>)
            {
                ScriptTransferOwnershipEffect effect{
                    .selector = value.selector,
                    .teamName = value.teamName,
                    .targetTeamName = value.targetTeamName,
                    .sourcePlayer = value.sourcePlayer,
                    .targetPlayer = value.targetPlayer,
                };
                if (value.selector == ScriptOwnershipTransferSelector::NamedObject)
                {
                    // Named transfer has the same live-object lookup rule as
                    // the old ScriptActions path: a missing/deleted target is
                    // a harmless no-op, never an ownership request carrying a
                    // stale name.
                    if (!m_context.world)
                        return;
                    const std::optional<ScriptWorldObjectSnapshot> object =
                        resolveObject(value.objectName);
                    if (!object || !object->alive || !object->id)
                        return;
                    effect.object = object->id;
                }
                else if (value.selector == ScriptOwnershipTransferSelector::ScenarioTeam &&
                         value.teamName.empty()) return;
                else if (value.selector == ScriptOwnershipTransferSelector::PlayerAssets &&
                         (value.sourcePlayer.empty() || value.targetPlayer.empty())) return;
                else if (value.selector == ScriptOwnershipTransferSelector::MergeScenarioTeam &&
                         (value.teamName.empty() || value.targetTeamName.empty())) return;
                emit(std::move(effect), sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptDamageAction>)
            {
                ScriptDamageEffect effect{
                    .targetSelector = value.targetSelector,
                    .teamName = value.teamName,
                    .amount = value.amount,
                    .forceKill = value.forceKill,
                };
                if (value.targetSelector == ScriptDamageTargetSelector::NamedObject)
                {
                    // Match ScriptActions::doNamedDamage(): a missing named
                    // object is a no-op, not a queued invalid health request.
                    if (!m_context.world) return;
                    const std::optional<ScriptWorldObjectSnapshot> object =
                        resolveObject(value.objectName);
                    if (!object || !object->alive || !object->id) return;
                    effect.targets.push_back(object->id);
                }
                else if (effect.teamName.empty())
                {
                    return;
                }
                emit(std::move(effect), sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptGrantObjectUpgradeAction>)
            {
                if (!m_context.world)
                    return;
                const std::optional<ScriptWorldObjectSnapshot> object =
                    resolveObject(value.objectName);
                if (!object || !object->alive || !object->id)
                    return;
                emit(ScriptGrantObjectUpgradeEffect{
                         .object = object->id,
                         .upgradeName = value.upgradeName,
                     },
                     sourceScript,
                     sink,
                     result);
            }
            else if constexpr (std::is_same_v<Value, ScriptObjectStateMutationAction>)
            {
                ScriptObjectStateMutationEffect effect{
                    .targetKind = value.targetKind,
                    .mutation = value.mutation,
                    .enabled = value.enabled,
                };
                if (value.targetKind == ScriptObjectStateTargetKind::NamedObject)
                {
                    if (!m_context.world)
                        return;
                    const std::optional<ScriptWorldObjectSnapshot> object =
                        resolveObject(value.targetName);
                    if (!object || !object->alive || !object->id)
                        return;
                    effect.object = object->id;
                }
                else
                {
                    effect.teamName = value.targetName;
                }
                emit(std::move(effect), sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptGlobalObjectAction>)
            {
                emit(ScriptGlobalObjectEffect{.operation = value.operation},
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptBoobyTrapAction>)
            {
                ScriptBoobyTrapEffect effect{
                    .targetKind = value.targetKind,
                    .templateName = value.templateName,
                };
                if (value.targetKind == ScriptObjectStateTargetKind::NamedObject)
                {
                    if (!m_context.world) return;
                    const std::optional<ScriptWorldObjectSnapshot> object =
                        resolveObject(value.targetName);
                    if (!object || !object->alive || !object->id) return;
                    effect.object = object->id;
                }
                else
                {
                    effect.teamName = value.targetName;
                }
                emit(std::move(effect), sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptModifyObjectTypeListAction>)
            {
                auto list = lowerBoundObjectTypeList(value.listName);
                if (value.add)
                {
                    if (list == m_objectTypeLists.end() || list->name != value.listName)
                    {
                        list = m_objectTypeLists.insert(list, ObjectTypeListState{
                            .name = value.listName,
                        });
                    }
                    if (std::find(list->objectTypes.begin(), list->objectTypes.end(),
                                  value.objectType) == list->objectTypes.end())
                    {
                        list->objectTypes.push_back(value.objectType);
                    }
                }
                else if (list != m_objectTypeLists.end() && list->name == value.listName)
                {
                    const auto member = std::find(list->objectTypes.begin(),
                                                  list->objectTypes.end(), value.objectType);
                    if (member != list->objectTypes.end())
                    {
                        list->objectTypes.erase(member);
                        if (list->objectTypes.empty())
                            m_objectTypeLists.erase(list);
                    }
                }
            }
            else if constexpr (std::is_same_v<Value, ScriptSetPlayerCashAction>)
            {
                emit(ScriptPlayerCashEffect{
                         .player = value.player,
                         .value = value.value,
                         .operation = ScriptPlayerCashOperation::Set,
                     },
                     sourceScript,
                     sink,
                     result);
            }
            else if constexpr (std::is_same_v<Value, ScriptAdjustPlayerCashAction>)
            {
                emit(ScriptPlayerCashEffect{
                         .player = value.player,
                         .value = value.delta,
                         .operation = ScriptPlayerCashOperation::Adjust,
                     },
                     sourceScript,
                     sink,
                     result);
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerSellEverythingAction>)
            {
                emit(ScriptPlayerSellEverythingEffect{.player = value.player},
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerRepairStructureAction>)
            {
                emit(ScriptPlayerRepairStructureEffect{
                         .player = value.player,
                         .structure = value.structure,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerBuildUpgradeAction>)
            {
                emit(ScriptPlayerBuildUpgradeEffect{
                         .player = value.player,
                         .upgrade = value.upgrade,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerBuildObjectNearTeamAction>)
            {
                emit(ScriptPlayerBuildObjectNearTeamEffect{
                         .player = value.player,
                         .objectType = value.objectType,
                         .teamName = value.teamName,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerBuildSupplyCenterAction>)
            {
                emit(ScriptPlayerBuildSupplyCenterEffect{
                         .player = value.player,
                         .objectType = value.objectType,
                         .minimumSupplies = value.minimumSupplies,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptSkirmishBuildBuildingAction>)
            {
                emit(ScriptSkirmishBuildBuildingEffect{
                         .objectType = value.objectType,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptSkirmishApproachAction>)
            {
                emit(ScriptSkirmishApproachEffect{
                         .operation = value.operation,
                         .teamName = value.teamName,
                         .pathPrefix = value.pathPrefix,
                         .asTeam = value.asTeam,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptSkirmishPerimeterBuildAction>)
            {
                emit(ScriptSkirmishPerimeterBuildEffect{
                         .objectType = value.objectType,
                         .flank = value.flank,
                         .useFactionBaseDefense =
                             value.useFactionBaseDefense,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<
                                   Value,
                                   ScriptSkirmishFireSpecialPowerAtMostCostAction>)
            {
                emit(ScriptSkirmishFireSpecialPowerAtMostCostEffect{
                         .player = value.player,
                         .specialPower = value.specialPower,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<
                                   Value,
                                   ScriptSkirmishAttackNearestValueGroupAction>)
            {
                emit(ScriptSkirmishAttackNearestValueGroupEffect{
                         .teamName = value.teamName,
                         .comparison = value.comparison,
                         .minimumValue = value.minimumValue,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<
                                   Value,
                                   ScriptSkirmishMostValuableCommandButtonAction>)
            {
                emit(ScriptSkirmishMostValuableCommandButtonEffect{
                         .teamName = value.teamName,
                         .buttonName = value.buttonName,
                         .range = value.range,
                         .allTeamMembers = value.allTeamMembers,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptToppleDirectionAction>)
            {
                // RefCode stores this by script name even when the Object is
                // not alive yet.  Do not resolve it through ScriptWorldQuery
                // here; the session applies the durable override on binding.
                emit(ScriptToppleDirectionEffect{
                         .objectName = value.objectName,
                         .direction = value.direction,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerConstructionAction>)
            {
                emit(ScriptPlayerConstructionEffect{
                         .operation = value.operation,
                         .player = value.player,
                         .factoryType = value.factoryType,
                         .value = value.value,
                         .enabled = value.enabled,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptObjectBuildabilityAction>)
            {
                emit(ScriptObjectBuildabilityEffect{
                         .objectType = value.objectType,
                         .buildability = value.buildability,
                     },
                     sourceScript, sink, result);
            }
            else if constexpr (std::is_same_v<Value, ScriptSetPlayerScienceAvailabilityAction>)
            {
                emit(ScriptPlayerScienceAvailabilityEffect{
                         .player = value.player,
                         .science = value.science,
                         .availability = value.availability,
                     },
                     sourceScript,
                     sink,
                     result);
            }
            else if constexpr (std::is_same_v<Value, ScriptSetPlayerRelationshipAction>)
            {
                emit(ScriptPlayerRelationshipEffect{
                         .sourcePlayer = value.sourcePlayer,
                         .targetPlayer = value.targetPlayer,
                         .relationship = value.relationship,
                     },
                     sourceScript,
                     sink,
                     result);
            }
            else if constexpr (std::is_same_v<Value, ScriptRelationshipOverrideAction>)
            {
                emit(ScriptRelationshipOverrideEffect{
                         .sourceKind = value.sourceKind,
                         .targetKind = value.targetKind,
                         .operation = value.operation,
                         .sourceName = value.sourceName,
                         .targetName = value.targetName,
                         .relationship = value.relationship,
                     },
                     sourceScript,
                     sink,
                     result);
            }
            else if constexpr (std::is_same_v<Value, ScriptGlobalCombatPolicyAction>)
            {
                emit(ScriptGlobalCombatPolicyEffect{
                         .policy = value.policy,
                         .enabled = value.enabled,
                     },
                     sourceScript,
                     sink,
                     result);
            }
            else if constexpr (std::is_same_v<Value, ScriptPlayerProgressionAction>)
            {
                emit(ScriptPlayerProgressionEffect{
                         .operation = value.operation,
                         .player = value.player,
                         .science = value.science,
                         .integerValue = value.integerValue,
                         .realValue = value.realValue,
                     },
                     sourceScript,
                     sink,
                     result);
            }
        },
        action);
}

void ScriptRuntime::emit(ScriptEffectPayload payload,
                         ScriptId sourceScript,
                         ScriptEffectSink& sink,
                         ScriptRuntimeStepResult& result)
{
    ScriptEffect effect{
        .header =
            {
                .confirmedTick = m_currentConfirmedTick,
                .sourceScript = sourceScript,
                .invocation = m_currentInvocation,
                .currentPlayerAlias = m_currentPlayerAlias,
                .ordinal = m_effectOrdinal++,
            },
        .payload = std::move(payload),
    };
    sink.emit(std::move(effect));
    ++result.emittedEffects;
}

} // namespace engine::script
