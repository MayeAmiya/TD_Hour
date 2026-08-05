#pragma once

#include "game/script/runtime/ScriptHookDefinitions.h"

namespace engine::script
{

// Script action values.  Effects stay typed all the way through the runtime;
// no action stores a GameSession, terrain, renderer, audio, Entity or raw
// object pointer.
struct ScriptNoOpAction final {};

enum class ScriptSequentialTargetKind : uint8_t
{
    Object,
    Team,
};

enum class ScriptSequentialControlOperation : uint8_t
{
    Start,
    Stop,
};

// The interpreter owns sequence control.  Start resolves an authored target
// to one stable ObjectId/ObjectTeamId before enqueueing; Stop clears that
// target's entire FIFO. `remainingRequeues == -1` is the legacy infinite-loop
// sentinel, while zero means one execution with no requeue.
struct ScriptSequentialControlAction final
{
    ScriptSequentialControlOperation operation = ScriptSequentialControlOperation::Start;
    ScriptSequentialTargetKind targetKind = ScriptSequentialTargetKind::Object;
    ScriptObjectSelector object;
    ScriptTeamSelector team;
    ScriptId script = INVALID_SCRIPT_ID;
    int32_t remainingRequeues = 0;
};

enum class ScriptSequentialWaitKind : uint8_t
{
    CommandButtonAllReady,
    CommandButtonPartiallyReady,
    TeamNotContainedAll,
    TeamNotContainedPartial,
};

// These four opcodes are illegal as ordinary actions in RefCode.  They are
// retained as real instruction slots and interpreted only by the Sequential
// runner, which retries the same PC until the predicate succeeds.
struct ScriptSequentialWaitAction final
{
    ScriptSequentialWaitKind kind = ScriptSequentialWaitKind::CommandButtonAllReady;
    ScriptTeamSelector team;
    container::String commandButton;
};

enum class ScriptSequentialTimedCommand : uint8_t
{
    // TEAM_SPIN_FOR_FRAMECOUNT is misnamed in shipped RefCode: it only sets
    // the sequential timer and never issues a rotation/facing command.
    DelayOnly,
    Idle,
    GuardAtCurrentPosition,
};

// Timed sequential actions optionally issue one command and then write the
// active target runner's countdown. The command remains an ordinary stamped
// order effect; only the countdown belongs to ScriptRuntime.
struct ScriptSequentialTimedAction final
{
    ScriptSequentialTargetKind targetKind = ScriptSequentialTargetKind::Object;
    ScriptObjectSelector object;
    ScriptTeamSelector team;
    int32_t frames = 0;
    ScriptSequentialTimedCommand command = ScriptSequentialTimedCommand::Idle;
};

struct ScriptSetTeamCustomStateAction final
{
    ScriptTeamSelector team;
    container::String state;
};

struct ScriptSetFlagAction final
{
    container::String flag;
    ScriptRuntimeSymbolId flagSymbol = INVALID_SCRIPT_RUNTIME_SYMBOL_ID;
    bool value = false;
};

struct ScriptSetCounterAction final
{
    container::String counter;
    ScriptRuntimeSymbolId counterSymbol = INVALID_SCRIPT_RUNTIME_SYMBOL_ID;
    int32_t value = 0;
};

struct ScriptAdjustCounterAction final
{
    container::String counter;
    ScriptRuntimeSymbolId counterSymbol = INVALID_SCRIPT_RUNTIME_SYMBOL_ID;
    int32_t delta = 0;
};

struct ScriptSetTimerAction final
{
    container::String timer;
    ScriptRuntimeSymbolId timerSymbol = INVALID_SCRIPT_RUNTIME_SYMBOL_ID;
    // Confirmed simulation ticks, never a wall-clock duration. This is
    // deliberately signed: RefCode accepts a negative authored frame count,
    // leaves the countdown running and consequently makes TIMER_EXPIRED true
    // immediately. Do not silently clamp that content at the program edge.
    int32_t durationTicks = 0;
};

// ADD_TO_MSEC_TIMER and SUB_FROM_MSEC_TIMER are quantized by the legacy
// compiler after applying SUB's sign and before the immutable Program is
// installed. Runtime never carries or reconverts the authored Real value.
struct ScriptAdjustTimerAction final
{
    container::String timer;
    ScriptRuntimeSymbolId timerSymbol = INVALID_SCRIPT_RUNTIME_SYMBOL_ID;
    int32_t deltaTicks = 0;
};

// SET_RANDOM_TIMER samples integral frame endpoints. Although its two
// SET_RANDOM_MSEC_TIMER parameters are authored as Reals, RefCode calls the
// integer GameLogicRandomValue macro, so C++ truncates those endpoints to
// integral seconds before consuming the RNG. Keep that historical quirk
// explicit instead of silently substituting a continuous real distribution.
struct ScriptRandomFrameTimerRange final
{
    int32_t minimumDurationTicks = 0;
    int32_t maximumDurationTicks = 0;
};

struct ScriptRandomSecondTimerRange final
{
    int32_t minimumSeconds = 0;
    int32_t maximumSeconds = 0;
    uint32_t ticksPerSecond = 30;
};

struct ScriptSetRandomTimerAction final
{
    container::String timer;
    ScriptRuntimeSymbolId timerSymbol = INVALID_SCRIPT_RUNTIME_SYMBOL_ID;
    std::variant<ScriptRandomFrameTimerRange, ScriptRandomSecondTimerRange> range;
};

struct ScriptStopTimerAction final
{
    container::String timer;
    ScriptRuntimeSymbolId timerSymbol = INVALID_SCRIPT_RUNTIME_SYMBOL_ID;
};

struct ScriptRestartTimerAction final
{
    container::String timer;
    ScriptRuntimeSymbolId timerSymbol = INVALID_SCRIPT_RUNTIME_SYMBOL_ID;
};

struct ScriptEnableAction final
{
    ScriptTarget target;
};

struct ScriptDisableAction final
{
    ScriptTarget target;
};

struct ScriptCallSubroutineAction final
{
    ScriptTarget target;
};

// FREEZE_TIME / UNFREEZE_TIME control authoritative world advancement while
// ScriptRuntime continues evaluating confirmed script ticks.
struct ScriptTimeControlAction final
{
    bool frozen = false;
};

// Map-scoped authoritative lifetime policy. The authored REAL is quantized
// before ScriptProgram installation; a negative value is the legacy off
// sentinel.
struct ScriptHulkLifetimeOverrideAction final
{
    math::q32_32 seconds{int32_t{-1}};
};

// ENABLE_SCORING / DISABLE_SCORING gate ScoreKeeper accumulation and are not
// ScoreScreen or UI visibility commands.
struct ScriptScoreAccumulationPolicyAction final
{
    bool enabled = true;
};

// VICTORY/DEFEAT are global ScriptActions, not player-targeted mutations.
// QUICKVICTORY uses the same terminal state but a distinct legacy end timer;
// retain that presentation/scheduling intent instead of collapsing it during
// compilation.
enum class ScriptMissionEndMode : uint8_t
{
    Normal,
    Quick,
};

struct ScriptVictoryAction final
{
    ScriptMissionEndMode mode = ScriptMissionEndMode::Normal;
};

struct ScriptDefeatAction final
{
};


} // namespace engine::script
