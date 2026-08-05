#include "core/container/container_types.h"
#include "ScriptProgramValidationInternal.h"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace engine::script
{
namespace detail
{


// A dense table is the cheapest lookup for normal compiler-assigned group
// IDs, but each optional<size_t> consumes substantially more space than a
// schedule itself.  Keep the direct table bounded and reasonably occupied;
// very sparse/high uint32_t IDs use ScriptProgram's compact hash fallback.
constexpr size_t kMaximumDenseGroupScheduleIndexEntries = 1u << 20;
constexpr size_t kMaximumDenseGroupScheduleSlotsPerGroup = 8;

[[nodiscard]] bool shouldUseDenseGroupScheduleIndex(uint32_t maximumGroupId,
                                                     size_t scheduleCount) noexcept
{
    if (scheduleCount == 0)
        return false;

    const uint64_t indexEntryCount = static_cast<uint64_t>(maximumGroupId) + 1u;
    if (indexEntryCount > kMaximumDenseGroupScheduleIndexEntries)
        return false;

    // `indexEntryCount` is capped above, so this ceiling division cannot
    // overflow.  It expresses indexEntryCount <= scheduleCount * density
    // without multiplying an untrusted size_t count.
    const uint64_t minimumScheduleCount =
        (indexEntryCount + kMaximumDenseGroupScheduleSlotsPerGroup - 1u) /
        kMaximumDenseGroupScheduleSlotsPerGroup;
    return static_cast<uint64_t>(scheduleCount) >= minimumScheduleCount;
}

void addIssue(container::Vector<ScriptProgramBuildIssue>* issues, container::String message)
{
    if (issues)
        issues->push_back({.message = std::move(message)});
}

[[nodiscard]] bool finiteVec3(const math::vec3& value) noexcept
{
    return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
}

[[nodiscard]] bool validTargetShape(const ScriptTarget& target) noexcept
{
    switch (target.kind)
    {
    case ScriptTargetKind::Script:
        return static_cast<bool>(target.script) && !target.group;
    case ScriptTargetKind::Group:
        return static_cast<bool>(target.group) && !target.script;
    }
    return false;
}

[[nodiscard]] const ScriptDefinition* findScript(container::Span<const ScriptDefinition> scripts, ScriptId id) noexcept
{
    const auto found =
        std::lower_bound(scripts.begin(),
                         scripts.end(),
                         id,
                         [](const ScriptDefinition& definition, ScriptId needle) { return definition.id < needle; });
    return found != scripts.end() && found->id == id ? &*found : nullptr;
}

[[nodiscard]] const ScriptGroupDefinition* findGroup(container::Span<const ScriptGroupDefinition> groups,
                                                     ScriptGroupId id) noexcept
{
    const auto found = std::lower_bound(groups.begin(),
                                        groups.end(),
                                        id,
                                        [](const ScriptGroupDefinition& definition, ScriptGroupId needle)
                                        { return definition.id < needle; });
    return found != groups.end() && found->id == id ? &*found : nullptr;
}

} // namespace detail

using namespace detail;

bool ScriptProgramBuilder::addGroup(ScriptGroupDefinition definition)
{
    if (m_finalized)
        return false;
    m_groups.push_back(std::move(definition));
    return true;
}

bool ScriptProgramBuilder::addScript(ScriptDefinition definition)
{
    if (m_finalized)
        return false;
    m_scripts.push_back(std::move(definition));
    return true;
}

bool ScriptProgramBuilder::addExecutionList(ScriptListExecutionDefinition definition)
{
    if (m_finalized)
        return false;
    m_executionLists.push_back(std::move(definition));
    return true;
}

bool ScriptProgramBuilder::addTeamHook(ScriptTeamHookDefinition definition)
{
    if (m_finalized)
        return false;
    m_teamHooks.push_back(std::move(definition));
    return true;
}

bool ScriptProgramBuilder::addObjectHook(ScriptObjectHookDefinition definition)
{
    if (m_finalized)
        return false;
    m_objectHooks.push_back(std::move(definition));
    return true;
}

container::SharedPtr<const ScriptProgram> ScriptProgramBuilder::finalize(container::Vector<ScriptProgramBuildIssue>* issues)
{
    container::Vector<ScriptProgramBuildIssue> privateIssues;
    if (!issues)
        issues = &privateIssues;
    issues->clear();
    if (m_finalized)
    {
        addIssue(issues, "ScriptProgramBuilder has already been finalized");
        return {};
    }

    container::Vector<ScriptGroupDefinition> groupsById = m_groups;
    std::sort(groupsById.begin(),
              groupsById.end(),
              [](const ScriptGroupDefinition& lhs, const ScriptGroupDefinition& rhs) { return lhs.id < rhs.id; });
    for (size_t index = 0; index < groupsById.size(); ++index)
    {
        const ScriptGroupDefinition& group = groupsById[index];
        if (!group.id)
        {
            addIssue(issues, "script group has invalid ID zero");
        }
        if (group.name.empty())
        {
            addIssue(issues, "script group has an empty name");
        }
        if (index != 0 && group.id == groupsById[index - 1].id)
        {
            addIssue(issues, "script program contains duplicate group IDs");
        }
    }

    container::Vector<ScriptDefinition> scriptsById = m_scripts;
    std::sort(scriptsById.begin(),
              scriptsById.end(),
              [](const ScriptDefinition& lhs, const ScriptDefinition& rhs) { return lhs.id < rhs.id; });
    for (size_t index = 0; index < scriptsById.size(); ++index)
    {
        const ScriptDefinition& script = scriptsById[index];
        const container::StringView scriptName =
            script.name.empty() ? container::StringView{"<unnamed>"} : container::StringView{script.name};
        if (!script.id)
        {
            addIssue(issues, "script '" + container::String(scriptName) + "' has invalid ID zero");
        }
        if (script.name.empty())
        {
            addIssue(issues, "script has an empty name");
        }
        if (index != 0 && script.id == scriptsById[index - 1].id)
        {
            addIssue(issues, "script program contains duplicate script IDs");
        }
        if (script.group && !findGroup(groupsById, script.group))
        {
            addIssue(issues, "script '" + container::String(scriptName) + "' references an unknown group");
        }
        if (script.initialEvaluationJitterTicks != 0 && script.evaluationDelayTicks == 0)
        {
            addIssue(issues,
                     "script '" + container::String(scriptName) +
                         "' has an initial evaluation jitter without an evaluation delay");
        }
        if (script.initialEvaluationJitterTicks >
            static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
        {
            addIssue(issues,
                     "script '" + container::String(scriptName) +
                         "' has an initial evaluation jitter outside ScriptRandomSource's Int range");
        }
        if (std::any_of(script.conditionTeamCandidates.begin(),
                        script.conditionTeamCandidates.end(),
                        [](const ScriptTeamSelector& selector) {
                            return !selector.valid();
                        }))
        {
            addIssue(issues,
                     "script '" + container::String(scriptName) +
                         "' has an invalid condition Team candidate");
        }
        for (const ScriptAndClause& clause : script.anyOf)
        {
            for (const ScriptCondition& condition : clause.allOf)
            {
                static_cast<void>(validCondition(condition, issues, scriptName));
            }
        }
        for (const ScriptAction& action : script.thenActions)
        {
            static_cast<void>(validActionShape(action, issues, scriptName));
        }
        for (const ScriptAction& action : script.elseActions)
        {
            static_cast<void>(validActionShape(action, issues, scriptName));
        }
    }

    const auto validateTarget = [&](const ScriptTarget& target, bool requiresSubroutine, container::StringView scriptName)
    {
        if (!validTargetShape(target))
            return;
        if (target.kind == ScriptTargetKind::Script)
        {
            const ScriptDefinition* targetScript = findScript(scriptsById, target.script);
            if (!targetScript)
            {
                addIssue(issues, "script '" + container::String(scriptName) + "' targets an unknown script");
            }
            else if (requiresSubroutine && !targetScript->isSubroutine)
            {
                addIssue(issues, "script '" + container::String(scriptName) + "' calls a script that is not a subroutine");
            }
            return;
        }
        const ScriptGroupDefinition* targetGroup = findGroup(groupsById, target.group);
        if (!targetGroup)
        {
            addIssue(issues, "script '" + container::String(scriptName) + "' targets an unknown group");
        }
        else if (requiresSubroutine && !targetGroup->isSubroutine)
        {
            addIssue(issues, "script '" + container::String(scriptName) + "' calls a group that is not a subroutine");
        }
    };
    for (const ScriptDefinition& script : scriptsById)
    {
        const container::StringView scriptName =
            script.name.empty() ? container::StringView{"<unnamed>"} : container::StringView{script.name};
        const auto validateActions = [&](const container::Vector<ScriptAction>& actions)
        {
            for (const ScriptAction& action : actions)
            {
                std::visit(
                    [&](const auto& value)
                    {
                        using Value = std::decay_t<decltype(value)>;
                        if constexpr (std::is_same_v<Value, ScriptEnableAction> ||
                                      std::is_same_v<Value, ScriptDisableAction>)
                        {
                            validateTarget(value.target, false, scriptName);
                        }
                        else if constexpr (std::is_same_v<Value, ScriptCallSubroutineAction>)
                        {
                            validateTarget(value.target, true, scriptName);
                        }
                        else if constexpr (std::is_same_v<Value, ScriptSequentialControlAction>)
                        {
                            if (value.operation == ScriptSequentialControlOperation::Start &&
                                !findScript(scriptsById, value.script))
                            {
                                addIssue(issues,
                                         "script '" + container::String(scriptName) +
                                             "' starts an unknown sequential script");
                            }
                        }
                    },
                    action);
            }
        };
        validateActions(script.thenActions);
        validateActions(script.elseActions);
    }

    for (const ScriptTeamHookDefinition& hooks : m_teamHooks)
    {
        if (hooks.teamName.empty())
        {
            addIssue(issues, "script Team hook definition has an empty Team name");
        }
        const auto validateHook = [&](const ScriptTarget& target,
                                      container::StringView hookName)
        {
            if (!target)
                return;
            if (!validTargetShape(target))
            {
                addIssue(issues,
                         "script Team '" + hooks.teamName + "' hook '" +
                             container::String(hookName) +
                             "' has an invalid target shape");
                return;
            }
            validateTarget(target, true,
                           "Team '" + hooks.teamName + "' hook '" +
                               container::String(hookName) + "'");
        };
        if (hooks.productionCreateActions)
        {
            if (!validTargetShape(hooks.productionCreateActions) ||
                hooks.productionCreateActions.kind !=
                    ScriptTargetKind::Script)
            {
                addIssue(issues,
                         "script Team '" + hooks.teamName +
                             "' production-create action has an invalid target");
            }
            else
            {
                validateTarget(
                    hooks.productionCreateActions, false,
                    "Team '" + hooks.teamName +
                        "' production-create action");
            }
        }
        validateHook(hooks.onCreate, "OnCreate");
        validateHook(hooks.onIdle, "OnIdle");
        validateHook(hooks.onEnemySighted, "EnemySighted");
        validateHook(hooks.onAllClear, "AllClear");
        validateHook(hooks.onDestroyed, "OnDestroyed");
        validateHook(hooks.onUnitDestroyed, "OnUnitDestroyed");
        for (size_t index = 0; index < hooks.genericScripts.size(); ++index)
        {
            const ScriptTarget& target = hooks.genericScripts[index];
            if (!target)
                continue;
            if (!validTargetShape(target))
            {
                addIssue(issues,
                         "script Team '" + hooks.teamName + "' generic hook " +
                             std::to_string(index) +
                             " has an invalid target shape");
                continue;
            }
            if (target.kind != ScriptTargetKind::Script)
            {
                addIssue(issues,
                         "script Team '" + hooks.teamName + "' generic hook " +
                             std::to_string(index) + " targets a Group");
                continue;
            }
            validateTarget(
                target, false,
                "Team '" + hooks.teamName + "' generic hook " +
                    std::to_string(index));
        }
    }

    container::Vector<std::pair<uint32_t, uint32_t>> objectHookSourceKeys;
    objectHookSourceKeys.reserve(m_objectHooks.size());
    for (const ScriptObjectHookDefinition& hook : m_objectHooks)
    {
        const std::pair sourceKey{hook.sourceSideOrdinal,
                                  hook.sourceBuildListOrdinal};
        const auto sourceKeyPosition = std::lower_bound(
            objectHookSourceKeys.begin(), objectHookSourceKeys.end(), sourceKey);
        if (hook.sourceSideOrdinal == INVALID_LEGACY_SIDE_ORDINAL ||
            hook.sourceBuildListOrdinal == INVALID_LEGACY_BUILD_LIST_ORDINAL)
        {
            addIssue(issues, "script Object hook has an invalid BuildList source position");
        }
        else if (sourceKeyPosition != objectHookSourceKeys.end() &&
                 *sourceKeyPosition == sourceKey)
        {
            addIssue(issues, "script program contains duplicate Object-hook BuildList source positions");
        }
        else
        {
            objectHookSourceKeys.insert(sourceKeyPosition, sourceKey);
        }

        if (!hook.onBuilt)
        {
            addIssue(issues, "script Object hook has no OnBuilt target");
        }
        else if (!validTargetShape(hook.onBuilt))
        {
            addIssue(issues, "script Object hook has an invalid OnBuilt target shape");
        }
        else
        {
            const container::String hookName = hook.structureName.empty()
                ? "BuildList Object hook"
                : "BuildList Object hook '" + hook.structureName + "'";
            validateTarget(hook.onBuilt, true, hookName);
        }
    }

    // RefCode's update loop is nested by player ScriptList.  An explicit
    // schedule retains that boundary; if callers use the small hand-authored
    // builder API without one, retain the historical single-list behavior.
    container::Vector<ScriptListExecutionDefinition> executionLists = m_executionLists;
    if (executionLists.empty())
    {
        ScriptListExecutionDefinition implicitList;
        implicitList.rootScripts.reserve(m_scripts.size());
        implicitList.groups.reserve(m_groups.size());
        for (const ScriptDefinition& script : m_scripts)
        {
            if (!script.group)
                implicitList.rootScripts.push_back(script.id);
        }
        for (const ScriptGroupDefinition& group : m_groups)
        {
            implicitList.groups.push_back(group.id);
        }
        executionLists.push_back(std::move(implicitList));
    }

    container::Vector<ScriptId> scheduledRoots;
    container::Vector<ScriptGroupId> scheduledGroups;
    for (size_t listIndex = 0; listIndex < executionLists.size(); ++listIndex)
    {
        const ScriptListExecutionDefinition& list = executionLists[listIndex];
        for (const ScriptId id : list.rootScripts)
        {
            const ScriptDefinition* script = findScript(scriptsById, id);
            if (!script)
            {
                addIssue(issues,
                         "script execution list " + std::to_string(listIndex) + " references an unknown root script");
            }
            else if (script->group)
            {
                addIssue(issues,
                         "script execution list " + std::to_string(listIndex) +
                             " schedules a grouped script as a root");
            }
            if (std::find(scheduledRoots.begin(), scheduledRoots.end(), id) != scheduledRoots.end())
            {
                addIssue(issues, "script program schedules a root script more than once");
            }
            else
            {
                scheduledRoots.push_back(id);
            }
        }
        for (const ScriptGroupId id : list.groups)
        {
            if (!findGroup(groupsById, id))
            {
                addIssue(issues,
                         "script execution list " + std::to_string(listIndex) + " references an unknown script group");
            }
            if (std::find(scheduledGroups.begin(), scheduledGroups.end(), id) != scheduledGroups.end())
            {
                addIssue(issues, "script program schedules a group more than once");
            }
            else
            {
                scheduledGroups.push_back(id);
            }
        }
    }
    for (const ScriptDefinition& script : scriptsById)
    {
        if (!script.group && std::find(scheduledRoots.begin(), scheduledRoots.end(), script.id) == scheduledRoots.end())
        {
            addIssue(issues, "script '" + script.name + "' is missing from every root execution list");
        }
    }
    for (const ScriptGroupDefinition& group : groupsById)
    {
        if (std::find(scheduledGroups.begin(), scheduledGroups.end(), group.id) == scheduledGroups.end())
        {
            addIssue(issues, "script group '" + group.name + "' is missing from every execution list");
        }
    }

    if (!issues->empty())
        return {};

    // Names are authored strings, but they are immutable once the program is
    // finalized.  Canonicalize their state-bearing subset once here so every
    // subsequent confirmed tick can use a dense integer slot.  Sorting gives
    // the IDs a deterministic value independent of parser allocation order.
    container::Vector<container::String> counterSymbols;
    container::Vector<container::String> flagSymbols;
    size_t effectReserveHint = 0;
    for (const ScriptDefinition& script : scriptsById)
    {
        collectRuntimeSymbols(script, counterSymbols, flagSymbols);
        effectReserveHint += countDirectEffects(script);
    }
    normalizeSymbols(counterSymbols);
    normalizeSymbols(flagSymbols);
    if (counterSymbols.size() > std::numeric_limits<uint32_t>::max() ||
        flagSymbols.size() > std::numeric_limits<uint32_t>::max())
    {
        addIssue(issues, "script program contains more runtime symbols than its canonical ID space permits");
        return {};
    }
    for (ScriptDefinition& script : scriptsById)
    {
        assignRuntimeSymbols(script, counterSymbols, flagSymbols);
    }

    // ScriptProgram's constructor is private so only this builder can create
    // a frozen instance. `make_shared` cannot access a private constructor
    // through std's construction machinery, hence the explicit owned value.
    auto program = container::SharedPtr<ScriptProgram>(new ScriptProgram());
    program->m_groupsById = std::move(groupsById);
    program->m_scriptsById = std::move(scriptsById);
    program->m_executionLists = std::move(executionLists);
    program->m_teamHooks = std::move(m_teamHooks);
    program->m_objectHooks = std::move(m_objectHooks);
    program->m_counterSymbols = std::move(counterSymbols);
    program->m_flagSymbols = std::move(flagSymbols);
    program->m_effectReserveHint = effectReserveHint;
    program->m_groupExecutionOrder.reserve(m_groups.size());
    program->m_groupSchedules.reserve(m_groups.size());
    for (const ScriptGroupDefinition& group : m_groups)
    {
        program->m_groupExecutionOrder.push_back(group.id);
        program->m_groupSchedules.push_back({.group = group.id});
    }

    if (!program->m_groupSchedules.empty())
    {
        const auto maximumSchedule = std::max_element(
            program->m_groupSchedules.begin(),
            program->m_groupSchedules.end(),
            [](const ScriptProgram::GroupSchedule& lhs, const ScriptProgram::GroupSchedule& rhs)
            { return lhs.group.value < rhs.group.value; });
        const uint32_t maximumGroupId = maximumSchedule->group.value;
        if (shouldUseDenseGroupScheduleIndex(maximumGroupId, program->m_groupSchedules.size()))
        {
            program->m_groupScheduleIndex.resize(static_cast<size_t>(maximumGroupId) + 1u);
            for (size_t index = 0; index < program->m_groupSchedules.size(); ++index)
            {
                program->m_groupScheduleIndex[program->m_groupSchedules[index].group.value] = index;
            }
        }
        else
        {
            program->m_sparseGroupScheduleIndex.reserve(program->m_groupSchedules.size());
            for (size_t index = 0; index < program->m_groupSchedules.size(); ++index)
            {
                program->m_sparseGroupScheduleIndex.emplace(
                    program->m_groupSchedules[index].group.value, index);
            }
        }
    }

    for (const ScriptDefinition& script : m_scripts)
    {
        if (!script.group)
        {
            program->m_rootExecutionOrder.push_back(script.id);
            continue;
        }
        const std::optional<size_t> scheduleIndex = program->findGroupScheduleIndex(script.group);
        // Group existence was validated above. Keep this defensive branch so
        // malformed builder state cannot create a partially scheduled script.
        if (scheduleIndex && *scheduleIndex < program->m_groupSchedules.size())
        {
            program->m_groupSchedules[*scheduleIndex].scriptIds.push_back(script.id);
        }
    }

    m_finalized = true;
    return program;
}

} // namespace engine::script
