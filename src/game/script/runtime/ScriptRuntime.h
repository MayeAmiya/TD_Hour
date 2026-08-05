#pragma once

#include "core/container/hash_containers.h"

#include "ScriptProgram.h"
#include "ScriptEffects.h"
#include "ScriptWorldQuery.h"
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

namespace engine::script {

// Mutation-capable deterministic stream supplied by the owning GameSession.
// ScriptRuntime never falls back to wwmath/std random state: consuming a
// random script action is confirmed simulation work and must be replayable.
class ScriptRandomSource {
public:
    virtual ~ScriptRandomSource() = default;
    [[nodiscard]] virtual int32_t integerInclusive(int32_t lo, int32_t hi) noexcept = 0;
};

struct ScriptRuntimeContext final {
    const ScriptWorldQuery* world = nullptr;
    ScriptRandomSource* random = nullptr;
};

// Installed by ScenarioApplication once per session.  The program keeps a
// raw SidesList ordinal; this small value table supplies the per-session
// PlayerId without making a compiled script program mutable or session-bound.
struct ScriptSidePlayerBinding final {
    uint32_t sourceSideOrdinal = INVALID_LEGACY_SIDE_ORDINAL;
    PlayerId player = INVALID_PLAYER_ID;
    // RefCode evaluates each ScriptList using that side's Player difficulty:
    // AI sides use their own Easy/Normal/Hard setting while human sides fall
    // back to ScriptEngine's global game difficulty. This is immutable match
    // setup data, so keep it with the existing side binding rather than
    // adding a per-condition virtual world query.
    std::optional<ScriptDifficulty> effectiveDifficulty;
};

struct ScriptCounterState final {
    container::String name;
    int32_t value = 0;
    // RefCode represents timers through a counter plus a countdown flag.
    // Keeping those together preserves counter/timer cross-observation while
    // still exposing a plain value-only modern state.
    bool countdownTimerRunning = false;
};

struct ScriptRuntimeStepResult final {
    bool accepted = false;
    bool recursionLimitReached = false;
    uint32_t evaluatedScripts = 0;
    uint32_t emittedEffects = 0;
};

// Deterministic confirmed-tick interpreter for the common ScriptEngine
// control-flow subset. It neither advances from wall-clock time nor writes
// GameSession/ECS/render/audio state directly. All external consequences are
// explicit ScriptEffect values emitted in execution order.
class ScriptRuntime final {
public:
    static constexpr uint32_t kMaximumSubroutineDepth = 64;

    ScriptRuntime() = default;
    explicit ScriptRuntime(container::SharedPtr<const ScriptProgram> program,
                           ScriptRuntimeContext context = {});

    void setProgram(container::SharedPtr<const ScriptProgram> program);
    void setContext(ScriptRuntimeContext context) noexcept { m_context = context; }
    // Binding is data only and accepted before the first confirmed tick.
    // The implementation canonicalizes it to ordinal order and retains the
    // first duplicate just like RefCode's source-order Side lookup.
    void setSidePlayerBindings(container::Vector<ScriptSidePlayerBinding> bindings);
    [[nodiscard]] const container::SharedPtr<const ScriptProgram>& program() const noexcept {
        return m_program;
    }

    void reset();
    void setDifficulty(ScriptDifficulty difficulty) noexcept { m_difficulty = difficulty; }
    [[nodiscard]] ScriptDifficulty difficulty() const noexcept { return m_difficulty; }

    // The caller must invoke this exactly once for each confirmed simulation
    // tick. Duplicate, reversed and skipped ticks are rejected so a client
    // cannot replay one-shot effects or silently age timers differently.
    [[nodiscard]] ScriptRuntimeStepResult advanceConfirmedTick(
        uint64_t confirmedTick, ScriptEffectSink& sink);

    [[nodiscard]] bool isScriptEnabled(ScriptId id) const noexcept;
    [[nodiscard]] bool isGroupEnabled(ScriptGroupId id) const noexcept;
    struct NamedConditionEvaluation final {
        bool value = false;
        bool difficultyAllowed = true;
        uint32_t evaluationDelayTicks = 0;
    };
    // TeamPrototype keeps a private duplicate of its production-condition
    // Script in RefCode. This evaluates the same immutable condition body in
    // an explicit player context without touching the script's automatic
    // enabled/one-shot/runtime schedule; the strategic owner keeps the
    // prototype-local next-evaluation tick.
    [[nodiscard]] std::optional<NamedConditionEvaluation>
    evaluateNamedConditionForPlayer(
        container::StringView name, PlayerId player);
    [[nodiscard]] std::optional<int32_t> counterValue(container::StringView name) const;
    [[nodiscard]] std::optional<bool> flagValue(container::StringView name) const;
    // Retail AISkirmishPlayer clears these eight shared script flags when a
    // queued Team activates or fails its minimum build. Keep the compatibility
    // mutation inside ScriptRuntime, the sole owner of confirmed flag state.
    void clearLegacySkirmishTeamBuildingFlags() noexcept;
    [[nodiscard]] std::optional<ScriptCounterState> counterState(container::StringView name) const;
    [[nodiscard]] container::Span<const ScriptCounterState> counters() const noexcept;
    struct ObjectTypeListState final {
        container::String name;
        container::Vector<container::String> objectTypes;
    };
    [[nodiscard]] std::optional<container::Span<const container::String>> objectTypeList(
        container::StringView name) const noexcept;
    [[nodiscard]] container::Span<const ObjectTypeListState> objectTypeLists() const noexcept {
        return m_objectTypeLists;
    }

private:
    struct RuntimeScriptState final {
        ScriptId id = INVALID_SCRIPT_ID;
        bool enabled = false;
        uint64_t nextEvaluationTick = 0;
    };

    struct RuntimeGroupState final {
        ScriptGroupId id = INVALID_SCRIPT_GROUP_ID;
        bool enabled = false;
    };

    struct RuntimeFlagState final {
        container::String name;
        bool value = false;
    };

    struct ObjectTypeCountBaseline final {
        PlayerId player = INVALID_PLAYER_ID;
        container::String objectType;
        int64_t count = 0;
    };

    struct SequentialTargetKey final {
        ScriptSequentialTargetKind kind = ScriptSequentialTargetKind::Object;
        uint32_t value = 0;

        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return value != 0;
        }
        [[nodiscard]] constexpr auto operator<=>(const SequentialTargetKey&) const noexcept = default;
    };

    struct SequentialEntry final {
        ScriptId script = INVALID_SCRIPT_ID;
        size_t nextInstruction = 0;
        int32_t framesToWait = -1;
        int32_t remainingRequeues = 0;
    };

    struct SequentialQueue final {
        SequentialTargetKey target;
        container::Deque<SequentialEntry> entries;
    };

    struct TeamHookInstanceState final {
        ObjectTeamId team = INVALID_OBJECT_TEAM_ID;
        size_t hookDefinitionIndex = 0;
        uint32_t initialMemberBaseline = 0;
        uint32_t destroyedThresholdCount = 0;
        uint32_t previousAliveCount = 0;
        container::Array<bool, ScriptTeamHookDefinition::kGenericScriptCount>
            shouldAttemptGenericScripts{};
        bool baselineEstablished = false;
        bool destroyedTriggered = false;
        bool previouslySawEnemy = false;
        bool wasIdle = false;
    };

    struct PendingTeamUnitDestroyedHook final {
        ObjectTeamId team = INVALID_OBJECT_TEAM_ID;
        uint32_t generation = 0;
    };

    [[nodiscard]] RuntimeScriptState* mutableScriptState(ScriptId id) noexcept;
    [[nodiscard]] const RuntimeScriptState* scriptState(ScriptId id) const noexcept;
    [[nodiscard]] RuntimeGroupState* mutableGroupState(ScriptGroupId id) noexcept;
    [[nodiscard]] const RuntimeGroupState* groupState(ScriptGroupId id) const noexcept;
    [[nodiscard]] ScriptCounterState* mutableCounter(ScriptRuntimeSymbolId symbol);
    [[nodiscard]] const ScriptCounterState* counter(ScriptRuntimeSymbolId symbol) const noexcept;
    [[nodiscard]] const ScriptCounterState* counter(container::StringView name) const;
    [[nodiscard]] RuntimeFlagState* mutableFlag(ScriptRuntimeSymbolId symbol);
    [[nodiscard]] const RuntimeFlagState* flag(ScriptRuntimeSymbolId symbol) const noexcept;
    [[nodiscard]] const RuntimeFlagState* flag(container::StringView name) const;
    [[nodiscard]] container::Vector<ObjectTypeListState>::iterator lowerBoundObjectTypeList(
        container::StringView name) noexcept;
    [[nodiscard]] container::Vector<ObjectTypeListState>::const_iterator lowerBoundObjectTypeList(
        container::StringView name) const noexcept;
    [[nodiscard]] PlayerId playerForSourceSide(uint32_t sourceSideOrdinal) const noexcept;
    [[nodiscard]] std::optional<ScriptDifficulty> difficultyForSourceSide(
        uint32_t sourceSideOrdinal) const noexcept;
    [[nodiscard]] std::optional<ScriptDifficulty> difficultyForPlayer(
        PlayerId player) const noexcept;
    [[nodiscard]] std::optional<PlayerId> resolvePlayer(container::StringView reference) const;
    [[nodiscard]] std::optional<ScriptWorldObjectSnapshot> resolveObject(
        container::StringView reference) const;
    [[nodiscard]] std::optional<ObjectTeamId> resolveTeam(
        container::StringView reference) const;
    [[nodiscard]] std::optional<ObjectTeamId> resolveTeam(
        const ScriptTeamSelector& selector) const;

    void decrementTimers() noexcept;
    // Applies RefCode's one-time post-load random delay to scripts that have
    // one. It deliberately runs only on the first accepted confirmed tick:
    // the session-owned ScriptRandomSource is then available, while reset()
    // recreates the original map-load boundary.
    void initializeInitialEvaluationSchedule(uint64_t firstConfirmedTick) noexcept;
    void executeAutomaticScripts(ScriptEffectSink& sink, ScriptRuntimeStepResult& result);
    void discoverTeamHookInstances();
    void executeTeamHooks(ScriptEffectSink& sink, ScriptRuntimeStepResult& result);
    void drainObjectHookEvents(ScriptEffectSink& sink,
                               ScriptRuntimeStepResult& result);
    void executeSequentialScripts(ScriptEffectSink& sink, ScriptRuntimeStepResult& result);
    [[nodiscard]] TeamHookInstanceState* mutableTeamHookInstanceState(
        ObjectTeamId team) noexcept;
    [[nodiscard]] const TeamHookInstanceState* teamHookInstanceState(
        ObjectTeamId team) const noexcept;
    TeamHookInstanceState& ensureTeamHookInstanceState(
        ObjectTeamId team, size_t hookDefinitionIndex);
    void executeTeamHookTarget(ScriptTarget target,
                               ObjectTeamId team,
                               PlayerId owner,
                               ScriptEffectSink& sink,
                               ScriptRuntimeStepResult& result,
                               uint32_t callDepth);
    void executeTeamProductionCreateActions(
        ScriptTarget target, ObjectTeamId team, PlayerId owner,
        ScriptEffectSink& sink, ScriptRuntimeStepResult& result,
        uint32_t callDepth, bool bindTeamContext = true);
    // Takes the team identity, not a TeamHookInstanceState reference: the
    // actions it runs can reallocate m_teamHookInstanceStates.
    void executeTeamGenericScripts(const ScriptTeamHookDefinition& hooks,
                                   ObjectTeamId team,
                                   const ScriptWorldTeamHookState& worldState,
                                   ScriptEffectSink& sink,
                                   ScriptRuntimeStepResult& result);
    void drainTeamUnitDestroyedHookEvents(ScriptEffectSink& sink,
                                          ScriptRuntimeStepResult& result,
                                          uint32_t callDepth);
    void executeScriptBody(const ScriptDefinition& definition,
                           RuntimeScriptState& state,
                           bool conditionTeamIteration,
                           ScriptEffectSink& sink,
                           ScriptRuntimeStepResult& result,
                           uint32_t callDepth);
    void executeScript(ScriptId id, bool calledAsSubroutine, ScriptEffectSink& sink,
                       ScriptRuntimeStepResult& result, uint32_t callDepth);
    void executeTarget(ScriptTarget target, ScriptEffectSink& sink,
                       ScriptRuntimeStepResult& result, uint32_t callDepth);
    [[nodiscard]] bool evaluate(const ScriptDefinition& definition) const;
    [[nodiscard]] bool evaluateCondition(const ScriptCondition& condition) const;
    void executeActions(const container::Vector<ScriptAction>& actions, ScriptId sourceScript,
                        ScriptEffectSink& sink, ScriptRuntimeStepResult& result,
                        uint32_t callDepth);
    void executeAction(const ScriptAction& action, ScriptId sourceScript,
                       ScriptEffectSink& sink, ScriptRuntimeStepResult& result,
                       uint32_t callDepth);
    void emit(ScriptEffectPayload payload, ScriptId sourceScript, ScriptEffectSink& sink,
              ScriptRuntimeStepResult& result);
    [[nodiscard]] container::Vector<SequentialQueue>::iterator findSequentialQueue(
        SequentialTargetKey target) noexcept;
    [[nodiscard]] container::Vector<SequentialQueue>::const_iterator findSequentialQueue(
        SequentialTargetKey target) const noexcept;
    void enqueueSequential(SequentialTargetKey target, ScriptId script,
                           int32_t remainingRequeues);
    void clearSequential(SequentialTargetKey target) noexcept;
    void setSequentialWaitFrames(SequentialTargetKey target, int32_t frames) noexcept;
    [[nodiscard]] bool sequentialWaitSatisfied(
        const ScriptSequentialWaitAction& wait) const;

    container::SharedPtr<const ScriptProgram> m_program;
    ScriptRuntimeContext m_context;
    ScriptDifficulty m_difficulty = ScriptDifficulty::Normal;
    // The state vectors remain ID-sorted deterministic storage. These two
    // dense-or-sparse tables are lookup accelerators only: no confirmed
    // iteration observes unordered_map bucket order.
    container::Vector<RuntimeScriptState> m_scriptStates;
    container::Vector<std::optional<size_t>> m_scriptStateIndex;
    container::HashMap<uint32_t, size_t> m_sparseScriptStateIndex;
    container::Vector<RuntimeGroupState> m_groupStates;
    container::Vector<std::optional<size_t>> m_groupStateIndex;
    container::HashMap<uint32_t, size_t> m_sparseGroupStateIndex;
    // State remains sparse just as it was before: an authored name has no
    // externally observable value until an action writes it.  The two dense
    // symbol-index tables merely turn compiled condition/action access into
    // O(1) slot lookup, while the state vectors preserve canonical name order
    // for tooling and deterministic serialization.
    container::Vector<ScriptCounterState> m_counters;
    container::Vector<ScriptRuntimeSymbolId> m_counterSymbolsByState;
    container::Vector<std::optional<size_t>> m_counterStateIndexBySymbol;
    container::Vector<RuntimeFlagState> m_flags;
    container::Vector<ScriptRuntimeSymbolId> m_flagSymbolsByState;
    container::Vector<std::optional<size_t>> m_flagStateIndexBySymbol;
    container::Vector<ObjectTypeListState> m_objectTypeLists;
    container::Vector<SequentialQueue> m_sequentialQueues;
    container::Vector<TeamHookInstanceState> m_teamHookInstanceStates;
    container::Deque<PendingTeamUnitDestroyedHook> m_pendingTeamUnitDestroyedHooks;
    mutable container::Vector<ObjectTypeCountBaseline> m_objectTypeCountBaselines;
    container::Vector<ScriptSidePlayerBinding> m_sidePlayerBindings;
    bool m_initialEvaluationScheduleInitialized = false;
    bool m_hasLastConfirmedTick = false;
    uint64_t m_lastConfirmedTick = 0;
    uint64_t m_currentConfirmedTick = 0;
    ScriptInvocationContext m_currentInvocation;
    container::String m_currentPlayerAlias;
    std::optional<ScriptDifficulty> m_currentDifficultyOverride;
    uint32_t m_effectOrdinal = 0;
    uint32_t m_currentTeamHookDeathGeneration = 0;
    bool m_teamHookDispatchActive = false;
    bool m_drainingTeamUnitDestroyedHooks = false;
};

} // namespace engine::script
