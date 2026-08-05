#pragma once

#include "game/script/runtime/ScriptAction.h"
#include "game/script/runtime/ScriptConditions.h"
#include "game/script/runtime/ScriptHookDefinitions.h"

namespace engine::script
{

struct ScriptGroupDefinition final
{
    ScriptGroupId id = INVALID_SCRIPT_GROUP_ID;
    container::String name;
    bool initiallyEnabled = true;
    bool isSubroutine = false;
};

struct ScriptDefinition final
{
    ScriptId id = INVALID_SCRIPT_ID;
    container::String name;
    // Invalid group means a root ScriptList entry. Root scripts are evaluated
    // before normal groups, exactly as the original ScriptEngine does.
    ScriptGroupId group = INVALID_SCRIPT_GROUP_ID;
    bool initiallyEnabled = true;
    bool oneShot = true;
    bool isSubroutine = false;
    ScriptDifficultyMask difficulties{};
    // Zero means evaluate on every confirmed tick.
    uint32_t evaluationDelayTicks = 0;
    // RefCode gives every script with a positive DelayEvalSeconds one
    // deterministic load-time offset in [0, 2 * LOGICFRAMES_PER_SECOND].
    // Keep that bound with the immutable definition rather than deriving it
    // from a process-global frame rate at runtime. Zero means no initial
    // random stagger (and must accompany a zero evaluation delay).
    uint32_t initialEvaluationJitterTicks = 0;
    // RefCode scans every TEAM-typed condition parameter, chooses the first
    // multi-instance prototype (or the last singleton if there is no multi),
    // and then evaluates once per live instance.  Program creation has no
    // ScenarioDefinition, so preserve source-order typed candidates and let
    // the session bridge make that prototype decision authoritatively.
    container::Vector<ScriptTeamSelector> conditionTeamCandidates;
    container::Vector<ScriptAndClause> anyOf;
    container::Vector<ScriptAction> thenActions;
    // Do not infer this from `elseActions.empty()`: legacy NO_OP is a real
    // false-branch action.  A one-shot script with an ELSE containing only
    // NO_OP must still deactivate after that false evaluation.
    bool hasElseBranch = false;
    container::Vector<ScriptAction> elseActions;
};

// One source ScriptList schedule.  RefCode does not run every top-level
// script in the map and then every group in the map: it visits one player's
// ScriptList at a time, executes that list's roots, and only then traverses
// that same list's groups.  Keeping this as authored data is necessary when
// an earlier side enables or disables a later side's scripts in the same
// confirmed tick.
struct ScriptListExecutionDefinition final
{
    // RefCode sets ScriptEngine::m_currentPlayer to the Side that owns this
    // ScriptList before executing it. Keep its authored alias, not a live
    // PlayerId: Scenario application resolves map-side aliases later and a
    // ScriptProgram remains immutable/reusable across sessions.  The raw
    // alias remains useful for diagnostics and first-match object owners.
    container::String currentPlayerAlias;
    // Absolute CkMp SidesList position.  This is the authoritative context
    // for ThisPlayer after scenario application; playerName itself is not
    // unique in all original campaign maps.
    uint32_t sourceSideOrdinal = INVALID_LEGACY_SIDE_ORDINAL;
    container::Vector<ScriptId> rootScripts;
    container::Vector<ScriptGroupId> groups;
};

struct ScriptProgramBuildIssue final
{
    container::String message;
};

// A frozen authored program. Mutability is deliberately isolated in
// ScriptProgramBuilder and ScriptRuntime, so a running session never observes
// a parser/editor changing script definitions underneath it.
class ScriptProgram final
{
public:
    [[nodiscard]] container::Span<const ScriptGroupDefinition> groups() const noexcept;
    [[nodiscard]] container::Span<const ScriptDefinition> scripts() const noexcept;
    [[nodiscard]] container::Span<const ScriptListExecutionDefinition> executionLists() const noexcept;
    // Preserves SidesList/Team source order. Team lifecycle ordering is
    // observable when two hooks mutate the same script state in one tick.
    [[nodiscard]] container::Span<const ScriptTeamHookDefinition> teamHooks() const noexcept;
    // Preserves SidesList/Side/BuildList source order for deterministic
    // matching by the future production-domain event producer.
    [[nodiscard]] container::Span<const ScriptObjectHookDefinition> objectHooks() const noexcept;
    [[nodiscard]] const ScriptObjectHookDefinition* findObjectHook(
        uint32_t sourceSideOrdinal,
        uint32_t sourceBuildListOrdinal) const noexcept;
    // Flat authored lookup views retained for diagnostics and tooling.  The
    // automatic runtime schedule is executionLists(), not these flattened
    // lists, because RefCode resets the root/group traversal at each
    // player's ScriptList boundary.
    [[nodiscard]] container::Span<const ScriptId> rootExecutionOrder() const noexcept;
    // Authored ScriptGroup traversal order, distinct from the canonical
    // ID-sorted `groups()` lookup view.
    [[nodiscard]] container::Span<const ScriptGroupId> groupTraversalOrder() const noexcept;
    [[nodiscard]] container::Span<const ScriptId> groupExecutionOrder(ScriptGroupId group) const noexcept;
    [[nodiscard]] const ScriptDefinition* findScript(ScriptId id) const noexcept;
    [[nodiscard]] const ScriptDefinition* findScriptByName(
        container::StringView name) const noexcept;
    [[nodiscard]] const ScriptGroupDefinition* findGroup(ScriptGroupId id) const noexcept;

    // These tables are sorted by spelling and frozen with the program.  They
    // exist both for the O(1) runtime-slot path and for infrequent tooling /
    // diagnostics which still address state by authored name.
    [[nodiscard]] size_t counterSymbolCount() const noexcept { return m_counterSymbols.size(); }
    [[nodiscard]] size_t flagSymbolCount() const noexcept { return m_flagSymbols.size(); }
    [[nodiscard]] std::optional<ScriptRuntimeSymbolId> findCounterSymbol(container::StringView name) const noexcept;
    [[nodiscard]] std::optional<ScriptRuntimeSymbolId> findFlagSymbol(container::StringView name) const noexcept;
    [[nodiscard]] container::StringView counterSymbolName(ScriptRuntimeSymbolId id) const noexcept;
    [[nodiscard]] container::StringView flagSymbolName(ScriptRuntimeSymbolId id) const noexcept;
    // A capacity hint, not a semantic limit.  It is the direct number of
    // effect-producing actions in the program; callers may emit more through
    // subroutine reuse, but this avoids repeated vector growth for ordinary
    // map scripts without preallocating an unbounded recursion budget.
    [[nodiscard]] size_t effectReserveHint() const noexcept { return m_effectReserveHint; }

private:
    friend class ScriptProgramBuilder;

    ScriptProgram() = default;

    struct GroupSchedule final
    {
        ScriptGroupId group = INVALID_SCRIPT_GROUP_ID;
        container::Vector<ScriptId> scriptIds;
    };

    // Group IDs usually come from the legacy compiler as a compact sequence,
    // so this table turns runtime scheduling into one bounds check and one
    // indexed read.  Arbitrary program authors can still supply a sparse
    // uint32_t ID space; those entries live in the compact hash fallback
    // rather than forcing an allocation up to the largest authored ID. Both
    // are lookup-only accelerators: m_groupExecutionOrder remains the sole
    // source of authored group traversal order.
    [[nodiscard]] std::optional<size_t> findGroupScheduleIndex(ScriptGroupId group) const noexcept;

    container::Vector<ScriptGroupDefinition> m_groupsById;
    container::Vector<ScriptGroupId> m_groupExecutionOrder;
    container::Vector<ScriptDefinition> m_scriptsById;
    container::Vector<ScriptId> m_rootExecutionOrder;
    container::Vector<GroupSchedule> m_groupSchedules;
    container::Vector<std::optional<size_t>> m_groupScheduleIndex;
    container::HashMap<uint32_t, size_t> m_sparseGroupScheduleIndex;
    container::Vector<ScriptListExecutionDefinition> m_executionLists;
    container::Vector<ScriptTeamHookDefinition> m_teamHooks;
    container::Vector<ScriptObjectHookDefinition> m_objectHooks;
    container::Vector<container::String> m_counterSymbols;
    container::Vector<container::String> m_flagSymbols;
    size_t m_effectReserveHint = 0;
};

// Builder input order is authored execution order. Definition lookups in the
// frozen Program are sorted vectors. Its private group-schedule index may use
// a hash fallback for sparse IDs, but it never supplies iteration order or
// pointer identity to confirmed simulation.
class ScriptProgramBuilder final
{
public:
    [[nodiscard]] bool addGroup(ScriptGroupDefinition definition);
    [[nodiscard]] bool addScript(ScriptDefinition definition);

    // Adds one authored ScriptList schedule.  When no explicit schedules are
    // supplied, finalize() creates a single backward-compatible schedule
    // containing all roots followed by all groups in add order.
    [[nodiscard]] bool addExecutionList(ScriptListExecutionDefinition definition);
    [[nodiscard]] bool addTeamHook(ScriptTeamHookDefinition definition);
    [[nodiscard]] bool addObjectHook(ScriptObjectHookDefinition definition);

    // On validation failure the builder remains editable, making parse
    // diagnostics recoverable without leaking a half-frozen Program.
    [[nodiscard]] container::SharedPtr<const ScriptProgram> finalize(container::Vector<ScriptProgramBuildIssue>* issues = nullptr);

    [[nodiscard]] bool isFinalized() const noexcept
    {
        return m_finalized;
    }

private:
    container::Vector<ScriptGroupDefinition> m_groups;
    container::Vector<ScriptDefinition> m_scripts;
    container::Vector<ScriptListExecutionDefinition> m_executionLists;
    container::Vector<ScriptTeamHookDefinition> m_teamHooks;
    container::Vector<ScriptObjectHookDefinition> m_objectHooks;
    bool m_finalized = false;
};

} // namespace engine::script
