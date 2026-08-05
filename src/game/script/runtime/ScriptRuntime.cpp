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

ScriptRuntimeStepResult ScriptRuntime::advanceConfirmedTick(uint64_t confirmedTick, ScriptEffectSink& sink)
{
    ScriptRuntimeStepResult result;
    if (!m_program)
        return result;
    if (m_hasLastConfirmedTick)
    {
        // Never silently coalesce a gap: scripts may change flags and emit
        // one-shot effects on every confirmed tick, so replaying only the
        // final state would be nondeterministic and observably wrong.
        if (m_lastConfirmedTick == std::numeric_limits<uint64_t>::max() || confirmedTick != m_lastConfirmedTick + 1u)
        {
            return result;
        }
    }

    m_hasLastConfirmedTick = true;
    m_lastConfirmedTick = confirmedTick;
    m_currentConfirmedTick = confirmedTick;
    m_effectOrdinal = 0;
    result.accepted = true;

    initializeInitialEvaluationSchedule(confirmedTick);
    sink.reserveEffects(m_program->effectReserveHint());

    // RefCode decrements shared countdown counters before condition scans.
    // A timer therefore reaches -1 and remains expired until an action sets
    // or stops it, while regular counter conditions see the same value.
    decrementTimers();
    if (m_context.world)
    {
        if (m_program->teamHooks().empty())
        {
            // GameSession publishes death notifications independently from
            // whether this particular map authored a Team hook. Consume the
            // empty-program journal so a long match cannot retain it forever.
            static_cast<void>(m_context.world->takeTeamUnitDestroyedHookEvents());
        }
        else
        {
            // Establish stable TeamId -> hook-definition associations before
            // automatic scripts run. A lethal automatic action can therefore
            // dispatch OnUnitDestroyed before the next authored action.
            discoverTeamHookInstances();
            m_teamHookDispatchActive = true;
            drainTeamUnitDestroyedHookEvents(sink, result, 0);
        }
        // BuildList completion is a domain-published event, not a polled
        // script condition. Consume it at the start of the next script phase
        // so object-attached subroutines run before ordinary automatic scans.
        drainObjectHookEvents(sink, result);
    }
    executeAutomaticScripts(sink, result);
    executeTeamHooks(sink, result);
    executeSequentialScripts(sink, result);
    // Sequential instructions run after lifecycle polling but may still kill
    // Team members. Keep the hook microtask window open through that runner so
    // those notifications are not delayed to the next confirmed tick.
    if (m_teamHookDispatchActive)
    {
        drainTeamUnitDestroyedHookEvents(sink, result, 0);
        m_teamHookDispatchActive = false;
    }
    return result;
}

void ScriptRuntime::drainObjectHookEvents(
    ScriptEffectSink& sink, ScriptRuntimeStepResult& result)
{
    if (!m_context.world)
        return;

    container::Vector<ScriptWorldObjectHookEvent> events =
        m_context.world->takeObjectHookEvents();
    if (events.empty() || m_program->objectHooks().empty())
        return;

    for (const ScriptWorldObjectHookEvent& event : events)
    {
        if (!event.object ||
            m_context.world->objectState(event.object) !=
                ScriptWorldNamedObjectState::Alive)
        {
            continue;
        }
        const ScriptObjectHookDefinition* hook = m_program->findObjectHook(
            event.sourceSideOrdinal, event.sourceBuildListOrdinal);
        if (!hook || !hook->onBuilt)
            continue;

        const ScriptInvocationContext savedInvocation = m_currentInvocation;
        const container::String savedPlayerAlias =
            std::move(m_currentPlayerAlias);
        const std::optional<ScriptDifficulty> savedDifficultyOverride =
            m_currentDifficultyOverride;
        const PlayerId owner = playerForSourceSide(hook->sourceSideOrdinal);
        m_currentInvocation = {
            .callingObject = event.object,
            .currentPlayer = owner,
            .origin = ScriptInvocationOrigin::ObjectHook,
        };
        m_currentPlayerAlias.clear();
        m_currentDifficultyOverride =
            difficultyForSourceSide(hook->sourceSideOrdinal);
        executeTarget(hook->onBuilt, sink, result, 0);
        if (m_teamHookDispatchActive)
            drainTeamUnitDestroyedHookEvents(sink, result, 0);
        m_currentInvocation = savedInvocation;
        m_currentPlayerAlias = savedPlayerAlias;
        m_currentDifficultyOverride = savedDifficultyOverride;
    }
}

void ScriptRuntime::executeAutomaticScripts(ScriptEffectSink& sink, ScriptRuntimeStepResult& result)
{
    // RefCode's update() walks one player's ScriptList at a time: roots
    // first, then that same list's normal groups.  Flattening every root
    // before every group changes same-tick ENABLE_SCRIPT/DISABLE_SCRIPT
    // behavior across sides, so retain the list boundary in the immutable
    // Program schedule.
    const ScriptInvocationContext savedInvocation = m_currentInvocation;
    const container::String savedPlayerAlias = std::move(m_currentPlayerAlias);
    const std::optional<ScriptDifficulty> savedDifficultyOverride = m_currentDifficultyOverride;
    for (const ScriptListExecutionDefinition& list : m_program->executionLists())
    {
        m_currentPlayerAlias = list.currentPlayerAlias;
        m_currentInvocation = {
            .currentPlayer = playerForSourceSide(list.sourceSideOrdinal),
            .origin = ScriptInvocationOrigin::Automatic,
        };
        m_currentDifficultyOverride = difficultyForSourceSide(list.sourceSideOrdinal);
        for (const ScriptId id : list.rootScripts)
        {
            executeScript(id, false, sink, result, 0);
        }
        for (const ScriptGroupId groupId : list.groups)
        {
            const ScriptGroupDefinition* group = m_program->findGroup(groupId);
            const RuntimeGroupState* state = groupState(groupId);
            if (!group || !state || !state->enabled || group->isSubroutine)
                continue;
            for (const ScriptId id : m_program->groupExecutionOrder(groupId))
            {
                executeScript(id, false, sink, result, 0);
            }
        }
    }
    m_currentInvocation = savedInvocation;
    m_currentPlayerAlias = savedPlayerAlias;
    m_currentDifficultyOverride = savedDifficultyOverride;
}

void ScriptRuntime::executeScript(
    ScriptId id, bool calledAsSubroutine, ScriptEffectSink& sink, ScriptRuntimeStepResult& result, uint32_t callDepth)
{
    const ScriptDefinition* definition = m_program->findScript(id);
    RuntimeScriptState* state = mutableScriptState(id);
    if (!definition || !state || !state->enabled)
        return;
    if (!calledAsSubroutine && definition->isSubroutine)
        return;
    const ScriptDifficulty effectiveDifficulty = m_currentDifficultyOverride.value_or(m_difficulty);
    if (!definition->difficulties.includes(effectiveDifficulty) ||
        m_currentConfirmedTick < state->nextEvaluationTick)
    {
        return;
    }
    state->nextEvaluationTick = nextEvaluationTick(m_currentConfirmedTick, definition->evaluationDelayTicks);
    ++result.evaluatedScripts;

    const ObjectTeamId savedConditionTeam = m_currentInvocation.conditionTeam;
    if (definition->conditionTeamCandidates.empty())
    {
        executeScriptBody(*definition, *state, false, sink, result, callDepth);
        return;
    }
    const ScriptWorldTeamInvocationSet invocationSet = m_context.world
        ? m_context.world->selectConditionTeamInvocation(
              definition->conditionTeamCandidates)
        : ScriptWorldTeamInvocationSet{};
    if (invocationSet.prototypeExists && !invocationSet.instances.empty())
    {
        // The source Team-instance list is copied by the bridge before any
        // action runs.  Script-created instances therefore begin with the
        // next ordinary script evaluation, avoiding iterator invalidation and
        // matching the legacy prototype-list traversal boundary.
        for (const ObjectTeamId team : invocationSet.instances)
        {
            m_currentInvocation.conditionTeam = team;
            executeScriptBody(*definition, *state, true, sink, result, callDepth);
        }
    }
    else
    {
        m_currentInvocation.conditionTeam = INVALID_OBJECT_TEAM_ID;
        executeScriptBody(*definition, *state, false, sink, result, callDepth);
    }
    m_currentInvocation.conditionTeam = savedConditionTeam;
}

void ScriptRuntime::executeScriptBody(
    const ScriptDefinition& definition,
    RuntimeScriptState& state,
    bool conditionTeamIteration,
    ScriptEffectSink& sink,
    ScriptRuntimeStepResult& result,
    uint32_t callDepth)
{
    const bool conditionResult = evaluate(definition);
    const bool hasElseBranch = definition.hasElseBranch || !definition.elseActions.empty();
    if (conditionResult)
    {
        executeActions(definition.thenActions, definition.id, sink, result, callDepth);
    }
    else if (hasElseBranch)
    {
        executeActions(definition.elseActions, definition.id, sink, result, callDepth);
    }

    // RefCode's Team-instance branch has a long-standing asymmetry: a true
    // one-shot disables the Script, but an authored ELSE does not.  The
    // ordinary/no-instance path disables for either executed branch.
    if (definition.oneShot &&
        (conditionResult || (!conditionTeamIteration && hasElseBranch)))
    {
        state.enabled = false;
    }
}

void ScriptRuntime::executeTarget(ScriptTarget target,
                                  ScriptEffectSink& sink,
                                  ScriptRuntimeStepResult& result,
                                  uint32_t callDepth)
{
    if (callDepth >= kMaximumSubroutineDepth)
    {
        result.recursionLimitReached = true;
        return;
    }
    if (target.kind == ScriptTargetKind::Script)
    {
        const ScriptDefinition* definition = m_program->findScript(target.script);
        if (!definition || !definition->isSubroutine)
            return;
        executeScript(target.script, true, sink, result, callDepth + 1);
        return;
    }

    const ScriptGroupDefinition* group = m_program->findGroup(target.group);
    const RuntimeGroupState* groupRuntimeState = groupState(target.group);
    if (!group || !groupRuntimeState || !groupRuntimeState->enabled || !group->isSubroutine)
        return;
    for (const ScriptId id : m_program->groupExecutionOrder(target.group))
    {
        // RefCode's executeScripts() still skips individual subroutine
        // entries while a subroutine group is running.
        const ScriptDefinition* definition = m_program->findScript(id);
        if (definition && !definition->isSubroutine)
        {
            executeScript(id, true, sink, result, callDepth + 1);
        }
    }
}

} // namespace engine::script
