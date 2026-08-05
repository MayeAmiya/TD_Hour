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

// The authored ratio has already been frozen by the Script compiler. For the
// only non-trivial domain (0 < ratio < 1), the unsigned product fits in 64
// bits because baseline is uint32 and the Q32.32 factor is at most 2^32.
[[nodiscard]] uint32_t teamDestroyedRemainingThreshold(
    uint32_t baseline, math::q32_32 destroyedRatio) noexcept
{
    if (baseline == 0)
        return 0;
    const uint32_t maximum = baseline - 1u;
    if (destroyedRatio <= math::q32_32{}) return maximum;
    if (destroyedRatio >= math::q32_32{int32_t{1}}) return 0;
    constexpr uint64_t oneRaw = UINT64_C(1) << 32u;
    const uint64_t remainingFactorRaw =
        oneRaw - static_cast<uint64_t>(destroyedRatio.raw());
    const uint64_t remaining =
        (static_cast<uint64_t>(baseline) * remainingFactorRaw) >> 32u;
    return std::min(static_cast<uint32_t>(remaining), maximum);
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

ScriptRuntime::TeamHookInstanceState* ScriptRuntime::mutableTeamHookInstanceState(
    ObjectTeamId team) noexcept
{
    const auto found = std::lower_bound(
        m_teamHookInstanceStates.begin(), m_teamHookInstanceStates.end(), team,
        [](const TeamHookInstanceState& state, ObjectTeamId value) {
            return state.team < value;
        });
    return found != m_teamHookInstanceStates.end() && found->team == team
        ? &*found : nullptr;
}

const ScriptRuntime::TeamHookInstanceState* ScriptRuntime::teamHookInstanceState(
    ObjectTeamId team) const noexcept
{
    const auto found = std::lower_bound(
        m_teamHookInstanceStates.begin(), m_teamHookInstanceStates.end(), team,
        [](const TeamHookInstanceState& state, ObjectTeamId value) {
            return state.team < value;
        });
    return found != m_teamHookInstanceStates.end() && found->team == team
        ? &*found : nullptr;
}

ScriptRuntime::TeamHookInstanceState& ScriptRuntime::ensureTeamHookInstanceState(
    ObjectTeamId team, size_t hookDefinitionIndex)
{
    const auto found = std::lower_bound(
        m_teamHookInstanceStates.begin(), m_teamHookInstanceStates.end(), team,
        [](const TeamHookInstanceState& state, ObjectTeamId value) {
            return state.team < value;
        });
    if (found != m_teamHookInstanceStates.end() && found->team == team)
        return *found;
    TeamHookInstanceState state{
        .team = team,
        .hookDefinitionIndex = hookDefinitionIndex,
    };
    state.shouldAttemptGenericScripts.fill(true);
    return *m_teamHookInstanceStates.insert(found, std::move(state));
}

void ScriptRuntime::executeTeamHookTarget(
    ScriptTarget target,
    ObjectTeamId team,
    PlayerId owner,
    ScriptEffectSink& sink,
    ScriptRuntimeStepResult& result,
    uint32_t callDepth)
{
    if (!target || !team)
        return;

    const ScriptInvocationContext savedInvocation = m_currentInvocation;
    const container::String savedPlayerAlias = std::move(m_currentPlayerAlias);
    const std::optional<ScriptDifficulty> savedDifficultyOverride =
        m_currentDifficultyOverride;
    // callingTeam mirrors RefCode's runScript(..., thisTeam). runScript clears
    // m_conditionTeam before dispatch, so do not manufacture a condition-Team
    // binding here; a subroutine may establish its own typed iteration later.
    m_currentInvocation = {
        .callingTeam = team,
        .currentPlayer = owner,
        .origin = ScriptInvocationOrigin::TeamHook,
    };
    m_currentPlayerAlias.clear();
    m_currentDifficultyOverride = difficultyForPlayer(owner);
    executeTarget(target, sink, result, callDepth);
    m_currentInvocation = savedInvocation;
    m_currentPlayerAlias = savedPlayerAlias;
    m_currentDifficultyOverride = savedDifficultyOverride;
}

void ScriptRuntime::executeTeamProductionCreateActions(
    ScriptTarget target, ObjectTeamId team, PlayerId owner,
    ScriptEffectSink& sink, ScriptRuntimeStepResult& result,
    uint32_t callDepth, bool bindTeamContext)
{
    if (!target || target.kind != ScriptTargetKind::Script ||
        (bindTeamContext && !team) ||
        callDepth >= kMaximumSubroutineDepth) {
        if (callDepth >= kMaximumSubroutineDepth)
            result.recursionLimitReached = true;
        return;
    }
    const ScriptDefinition* definition = m_program->findScript(target.script);
    if (!definition) return;

    const ScriptInvocationContext savedInvocation = m_currentInvocation;
    const container::String savedPlayerAlias =
        std::move(m_currentPlayerAlias);
    const std::optional<ScriptDifficulty> savedDifficultyOverride =
        m_currentDifficultyOverride;
    m_currentInvocation = {
        .callingTeam = bindTeamContext
            ? team : INVALID_OBJECT_TEAM_ID,
        .currentPlayer = owner,
        .origin = ScriptInvocationOrigin::TeamHook,
    };
    m_currentPlayerAlias.clear();
    m_currentDifficultyOverride = difficultyForPlayer(owner);
    // RefCode calls friend_executeAction(script->getAction()): conditions,
    // enabled state, ELSE and the source Script's one-shot state are bypassed.
    executeActions(definition->thenActions, definition->id,
                   sink, result, callDepth + 1);
    m_currentInvocation = savedInvocation;
    m_currentPlayerAlias = savedPlayerAlias;
    m_currentDifficultyOverride = savedDifficultyOverride;
}

void ScriptRuntime::executeTeamGenericScripts(
    const ScriptTeamHookDefinition& hooks,
    ObjectTeamId team,
    const ScriptWorldTeamHookState& worldState,
    ScriptEffectSink& sink,
    ScriptRuntimeStepResult& result)
{
    if (!worldState.exists || !team)
        return;

    // This function takes the team identity rather than a TeamHookInstanceState
    // reference on purpose: executeActions below drains unit-destroyed hook
    // events, and a drain can insert into the sorted m_teamHookInstanceStates
    // vector and reallocate it.  Any retained reference would dangle, and the
    // one-shot bookkeeping write would land in an unrelated team's state.
    const auto instanceState = [this, team]() -> TeamHookInstanceState* {
        return mutableTeamHookInstanceState(team);
    };
    if (!instanceState())
        return;

    const ScriptInvocationContext savedInvocation = m_currentInvocation;
    const container::String savedPlayerAlias = std::move(m_currentPlayerAlias);
    const std::optional<ScriptDifficulty> savedDifficultyOverride =
        m_currentDifficultyOverride;
    m_currentInvocation = {
        .callingTeam = team,
        .currentPlayer = worldState.owner,
        .origin = ScriptInvocationOrigin::TeamHook,
    };
    m_currentPlayerAlias.clear();
    // Generic conditions themselves bypass difficulty, but a CALL_SUBROUTINE
    // action inherits Team::getControllingPlayer() in RefCode.
    m_currentDifficultyOverride = difficultyForPlayer(worldState.owner);

    for (size_t index = 0; index < hooks.genericScripts.size(); ++index)
    {
        // Re-acquire every iteration: the previous iteration's executeActions
        // may have reallocated the instance vector.
        TeamHookInstanceState* instance = instanceState();
        if (!instance)
            break;
        if (!instance->shouldAttemptGenericScripts[index])
            continue;
        const ScriptTarget target = hooks.genericScripts[index];
        if (!target || target.kind != ScriptTargetKind::Script)
        {
            instance->shouldAttemptGenericScripts[index] = false;
            continue;
        }
        const ScriptDefinition* definition = m_program->findScript(target.script);
        if (!definition)
        {
            instance->shouldAttemptGenericScripts[index] = false;
            continue;
        }

        // Team::updateGenericScripts bypasses ordinary enabled/difficulty/
        // evaluation-delay scheduling. Its Script copy is per Team instance,
        // evaluates conditions every frame, ignores ELSE, and owns one-shot
        // completion independently from the source Script definition.
        ++result.evaluatedScripts;
        if (!evaluate(*definition))
            continue;
        // Record one-shot completion BEFORE running the actions, so a reentrant
        // drain cannot observe this script as still pending.
        if (definition->oneShot)
            instance->shouldAttemptGenericScripts[index] = false;
        executeActions(definition->thenActions, definition->id,
                       sink, result, 0);
    }

    m_currentInvocation = savedInvocation;
    m_currentPlayerAlias = savedPlayerAlias;
    m_currentDifficultyOverride = savedDifficultyOverride;
}

void ScriptRuntime::drainTeamUnitDestroyedHookEvents(
    ScriptEffectSink& sink,
    ScriptRuntimeStepResult& result,
    uint32_t callDepth)
{
    if (!m_teamHookDispatchActive || !m_context.world)
        return;

    const uint32_t nextGeneration = m_drainingTeamUnitDestroyedHooks
        ? (m_currentTeamHookDeathGeneration == std::numeric_limits<uint32_t>::max()
               ? m_currentTeamHookDeathGeneration
               : m_currentTeamHookDeathGeneration + 1u)
        : 0u;
    for (const ScriptWorldTeamUnitDestroyedEvent& event :
         m_context.world->takeTeamUnitDestroyedHookEvents())
    {
        if (event.team)
            m_pendingTeamUnitDestroyedHooks.push_back({event.team, nextGeneration});
    }
    if (m_drainingTeamUnitDestroyedHooks)
        return;

    m_drainingTeamUnitDestroyedHooks = true;
    while (!m_pendingTeamUnitDestroyedHooks.empty())
    {
        const PendingTeamUnitDestroyedHook pending =
            m_pendingTeamUnitDestroyedHooks.front();
        m_pendingTeamUnitDestroyedHooks.pop_front();

        if (pending.generation >= kMaximumSubroutineDepth ||
            callDepth >= kMaximumSubroutineDepth - pending.generation)
        {
            // A finite initial death batch is unlimited; only causally nested
            // generations consume depth. This stops a death hook that causes
            // another death forever without dropping a legitimate large-Team
            // batch delivered by the bridge.
            result.recursionLimitReached = true;
            continue;
        }

        const TeamHookInstanceState* instance =
            teamHookInstanceState(pending.team);
        if (!instance)
        {
            // An earlier automatic action may have created this Team after
            // the tick-start association pass and killed one of its members
            // immediately. Refresh only on that uncommon miss.
            discoverTeamHookInstances();
            instance = teamHookInstanceState(pending.team);
        }
        if (!instance || instance->hookDefinitionIndex >= m_program->teamHooks().size())
            continue;
        const ScriptTeamHookDefinition& hooks =
            m_program->teamHooks()[instance->hookDefinitionIndex];
        const ScriptWorldTeamHookState worldState =
            m_context.world->teamHookState(pending.team);
        m_currentTeamHookDeathGeneration = pending.generation;
        executeTeamHookTarget(hooks.onUnitDestroyed, pending.team,
                              worldState.owner, sink, result,
                              callDepth + pending.generation);
    }
    m_currentTeamHookDeathGeneration = 0;
    m_drainingTeamUnitDestroyedHooks = false;
}

void ScriptRuntime::discoverTeamHookInstances()
{
    if (!m_context.world || !m_program)
        return;
    for (size_t hookIndex = 0; hookIndex < m_program->teamHooks().size(); ++hookIndex)
    {
        const ScriptTeamHookDefinition& hooks = m_program->teamHooks()[hookIndex];
        for (const ObjectTeamId team :
             m_context.world->teamHookInstances(hooks.teamName))
        {
            if (team)
                static_cast<void>(ensureTeamHookInstanceState(team, hookIndex));
        }
    }
}

void ScriptRuntime::executeTeamHooks(
    ScriptEffectSink& sink, ScriptRuntimeStepResult& result)
{
    if (!m_context.world || m_program->teamHooks().empty())
        return;

    struct ScheduledTeamHook final {
        size_t hookDefinitionIndex = 0;
        ObjectTeamId team = INVALID_OBJECT_TEAM_ID;
    };
    container::Vector<ScheduledTeamHook> scheduled;

    // Discover every association before consuming death events. A death may
    // target a later prototype in Program order, and resolving it must not
    // depend on how far the ordinary hook scan has progressed.
    for (size_t hookIndex = 0; hookIndex < m_program->teamHooks().size(); ++hookIndex)
    {
        const ScriptTeamHookDefinition& hooks = m_program->teamHooks()[hookIndex];
        container::Vector<ObjectTeamId> instances =
            m_context.world->teamHookInstances(hooks.teamName);
        for (const ObjectTeamId team : instances)
        {
            if (!team)
                continue;
            TeamHookInstanceState& state =
                ensureTeamHookInstanceState(team, hookIndex);
            // ObjectTeamId is a unique live identity. If a malformed bridge
            // reports it under two prototypes, retain the first association.
            if (state.hookDefinitionIndex == hookIndex)
                scheduled.push_back({hookIndex, team});
        }
    }

    m_teamHookDispatchActive = true;
    drainTeamUnitDestroyedHookEvents(sink, result, 0);

    for (const ScheduledTeamHook entry : scheduled)
    {
        TeamHookInstanceState* instance =
            mutableTeamHookInstanceState(entry.team);
        if (!instance)
            continue;
        const ScriptTeamHookDefinition& hooks =
            m_program->teamHooks()[entry.hookDefinitionIndex];
        ScriptWorldTeamHookState worldState =
            m_context.world->teamHookState(entry.team);
        // RefCode invokes generic Team scripts from Player::update for every
        // instance, including inactive ones, before lifecycle polling.
        executeTeamGenericScripts(hooks, entry.team, worldState, sink, result);
        worldState = m_context.world->teamHookState(entry.team);
        for (uint32_t productionAction = 0;
             worldState.exists && productionAction <
                 worldState.productionActionWithoutTeamCount;
             ++productionAction)
        {
            executeTeamProductionCreateActions(
                hooks.productionCreateActions, entry.team,
                worldState.owner, sink, result, 0, false);
            worldState = m_context.world->teamHookState(entry.team);
        }
        for (uint32_t productionAction = 0;
             worldState.exists &&
             productionAction < worldState.productionActionCount;
             ++productionAction)
        {
            // buildSpecificAITeam executes the production-condition Script's
            // THEN action list immediately after creating the inactive Team;
            // it does not evaluate that Script's condition/ELSE/one-shot.
            executeTeamProductionCreateActions(
                hooks.productionCreateActions, entry.team,
                worldState.owner, sink, result, 0);
            worldState = m_context.world->teamHookState(entry.team);
        }
        if (!worldState.exists || !worldState.active)
            continue;

        // Match Team::update's observable order. Re-query between stages so
        // a synchronously admitted hook effect can affect later stages in the
        // same confirmed tick without Runtime learning any ECS details.
        if (worldState.createdThisTick)
        {
            executeTeamHookTarget(hooks.onCreate, entry.team, worldState.owner,
                                  sink, result, 0);
        }

        worldState = m_context.world->teamHookState(entry.team);
        // Every hook target above can run script actions, and executeActions
        // drains unit-destroyed events after each one — a drain can create a
        // hook instance for a team first seen this tick, which inserts into the
        // sorted m_teamHookInstanceStates vector and reallocates it.  Refresh
        // the instance pointer alongside worldState after any action dispatch.
        instance = mutableTeamHookInstanceState(entry.team);
        if (!instance || !worldState.exists || !worldState.active)
            continue;
        // RefCode establishes OnDestroyed's immutable baseline after
        // OnCreate returns, so members created by that hook participate in
        // the original Team size. A Team first observed without the creation
        // pulse establishes the same baseline at this point.
        if (!instance->baselineEstablished)
        {
            instance->initialMemberBaseline = worldState.totalMemberCount;
            instance->destroyedThresholdCount =
                teamDestroyedRemainingThreshold(
                    worldState.totalMemberCount, hooks.destroyedThreshold);
            instance->previousAliveCount = worldState.totalMemberCount;
            instance->baselineEstablished = true;
        }
        // With no living member RefCode still resets its cached sight bit to
        // false, but deliberately suppresses both EnemySighted and AllClear.
        if (worldState.aliveMemberCount == 0)
        {
            instance->previouslySawEnemy = worldState.seesEnemy;
        }
        else if (worldState.seesEnemy != instance->previouslySawEnemy)
        {
            instance->previouslySawEnemy = worldState.seesEnemy;
            executeTeamHookTarget(
                worldState.seesEnemy ? hooks.onEnemySighted : hooks.onAllClear,
                entry.team, worldState.owner, sink, result, 0);
        }

        worldState = m_context.world->teamHookState(entry.team);
        instance = mutableTeamHookInstanceState(entry.team);
        if (!instance || !worldState.exists || !worldState.active)
            continue;
        const bool aliveCountChanged =
            worldState.aliveMemberCount != instance->previousAliveCount;
        instance->previousAliveCount = worldState.aliveMemberCount;
        if (!instance->destroyedTriggered && aliveCountChanged &&
            worldState.aliveMemberCount <= instance->destroyedThresholdCount)
        {
            instance->destroyedTriggered = true;
            executeTeamHookTarget(hooks.onDestroyed, entry.team,
                                  worldState.owner, sink, result, 0);
        }

        worldState = m_context.world->teamHookState(entry.team);
        instance = mutableTeamHookInstanceState(entry.team);
        if (!instance || !worldState.exists || !worldState.active)
            continue;
        const bool idle = worldState.allAliveAiIdle;
        if (worldState.aliveAiMemberCount != 0 && idle && instance->wasIdle)
            executeTeamHookTarget(hooks.onIdle, entry.team, worldState.owner,
                                  sink, result, 0);
        // Team.cpp stores isIdle even when no alive AI member exists; only
        // the trigger itself requires one. Preserve that subtle transition so
        // an idle AI member joining an otherwise helper-only Team can fire on
        // its first observed tick, just like the original.
        instance = mutableTeamHookInstanceState(entry.team);
        if (!instance)
            continue;
        instance->wasIdle = idle;
    }

    // Capture an event produced by a sink at the tail of the last target even
    // when that target contained no ordinary action instruction.
    drainTeamUnitDestroyedHookEvents(sink, result, 0);
}

} // namespace engine::script
