#include "LegacyScriptCompilerInternal.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>
#include <variant>

namespace engine::script::legacy::detail
{

void addScriptName(CompileContext& context, container::String name, ScriptId id, bool subroutine)
{
    if (name.empty())
        return;
    if (findScriptByName(context, name))
    {
        // Script names are global lookup aliases in the original engine.
        // Multiple Side-owned lists may reuse one while every definition
        // remains scheduled in its own list; only explicit name lookup uses
        // the first definition in Side order.
        return;
    }
    const size_t sourceOrderIndex = context.scriptsBySourceOrder.size();
    context.scriptsBySourceOrder.push_back({.name = std::move(name), .id = id, .isSubroutine = subroutine});
    context.scriptNameIndex.emplace(context.scriptsBySourceOrder.back().name, sourceOrderIndex);
}

void addGroupName(
    CompileContext& context, container::String name, ScriptGroupId id, bool subroutine)
{
    if (name.empty())
        return;
    if (findGroupByName(context, name))
    {
        // Group names are only lookup aliases. Different Side-owned
        // ScriptLists routinely reuse names such as "Team Scripts"; the
        // original engine keeps every group and resolves the first alias in
        // Side order without diagnosing the later definitions.
        return;
    }
    const size_t sourceOrderIndex = context.groupsBySourceOrder.size();
    context.groupsBySourceOrder.push_back({.name = std::move(name), .id = id, .isSubroutine = subroutine});
    context.groupNameIndex.emplace(context.groupsBySourceOrder.back().name, sourceOrderIndex);
}

[[nodiscard]] ScriptTarget resolveTeamHookTarget(
    CompileContext& context,
    container::StringView teamName, container::StringView fieldName,
    container::StringView targetName, LegacySourceRange source)
{
    if (targetName.empty() || uppercaseAscii(targetName) == "NONE")
        return {};

    // ScriptEngine::runScript() resolves a Group first. A non-subroutine
    // Group therefore shadows a same-named Script instead of falling through
    // to it; preserve that observable lookup rule here.
    if (const NamedGroup* group = findGroupByName(context, targetName))
    {
        if (group->isSubroutine)
            return ScriptTarget::groupTarget(group->id);
        context.diagnostic(
            LegacyScriptCompileDiagnosticSeverity::Warning,
            "legacy Team '" + container::String(teamName) + "' hook field '" +
                container::String(fieldName) + "' targets non-subroutine group '" +
                container::String(targetName) + "'; hook target omitted",
            source);
        return {};
    }
    if (const NamedScript* script = findScriptByName(context, targetName))
    {
        if (script->isSubroutine && script->runnable)
            return ScriptTarget::scriptTarget(script->id);
        context.diagnostic(
            LegacyScriptCompileDiagnosticSeverity::Warning,
            "legacy Team '" + container::String(teamName) + "' hook field '" +
                container::String(fieldName) + "' targets " +
                (script->isSubroutine ? "blocked subroutine script '"
                                      : "non-subroutine script '") +
                container::String(targetName) + "'; hook target omitted",
            source);
        return {};
    }

    // Legacy Team dictionaries can retain stale editor hook names. The
    // original stores the string and runScript() becomes a no-op when lookup
    // eventually fails. Since the compiled Program is immutable, omitting
    // the unresolved target here is the same observable behavior and is not
    // a content-compilation fault.
    return {};
}

[[nodiscard]] ScriptTarget resolveTeamGenericScriptTarget(
    CompileContext& context, container::StringView teamName,
    container::StringView fieldName, container::StringView targetName,
    LegacySourceRange source)
{
    if (targetName.empty() || uppercaseAscii(targetName) == "NONE")
        return {};
    if (const NamedScript* script = findScriptByName(context, targetName))
    {
        if (script->runnable)
            return ScriptTarget::scriptTarget(script->id);
        context.diagnostic(
            LegacyScriptCompileDiagnosticSeverity::Warning,
            "legacy Team '" + container::String(teamName) +
                "' generic hook field '" + container::String(fieldName) +
                "' targets blocked Script '" + container::String(targetName) +
                "'; generic hook target omitted",
            source);
        return {};
    }

    const NamedGroup* group = findGroupByName(context, targetName);
    context.diagnostic(
        LegacyScriptCompileDiagnosticSeverity::Warning,
        "legacy Team '" + container::String(teamName) + "' generic hook field '" +
            container::String(fieldName) + "' targets " +
            (group ? "a Script Group instead of a Script '"
                   : "unknown Script '") +
            container::String(targetName) + "'; generic hook target omitted",
        source);
    return {};
}

void appendTeamHooks(const LegacyMapScriptSource& source,
                     ScriptProgramBuilder& builder,
                     CompileContext& context)
{
    container::HashMap<container::String, bool, ScriptNameHash,
                       ScriptNameEqual> seenTeams;
    for (const LegacySidesListSource& sidesList : source.sidesLists())
    {
        for (const LegacyTeamSource& sourceTeam : sidesList.teams)
        {
            const LegacyDictionaryEntry* nameProperty =
                findDictionaryProperty(sourceTeam.properties, "teamName");
            const LegacySourceRange teamSource = nameProperty &&
                    nameProperty->serialized.size != 0
                ? nameProperty->serialized
                : sidesList.serialized;
            const container::String teamName = teamStringProperty(
                sourceTeam, "teamName", context, "<unnamed>", teamSource);
            if (teamName.empty())
            {
                context.diagnostic(
                    LegacyScriptCompileDiagnosticSeverity::Warning,
                    "legacy Team hook definition has an empty teamName; ignored",
                    teamSource);
                continue;
            }
            const container::String teamKey = uppercaseAscii(teamName);
            if (seenTeams.find(teamKey) != seenTeams.end())
            {
                context.diagnostic(
                    LegacyScriptCompileDiagnosticSeverity::Warning,
                    "duplicate legacy Team hook definition '" + teamName +
                        "'; original first-match definition is retained",
                    teamSource);
                continue;
            }
            seenTeams.emplace(teamKey, true);

            const auto hook = [&](container::StringView fieldName)
            {
                const LegacyDictionaryEntry* property =
                    findDictionaryProperty(sourceTeam.properties, fieldName);
                const LegacySourceRange fieldSource = property &&
                        property->serialized.size != 0
                    ? property->serialized
                    : teamSource;
                const container::String targetName = teamStringProperty(
                    sourceTeam, fieldName, context, teamName, fieldSource);
                return resolveTeamHookTarget(
                    context, teamName, fieldName, targetName, fieldSource);
            };

            container::Array<ScriptTarget,
                             ScriptTeamHookDefinition::kGenericScriptCount>
                genericScripts{};
            for (size_t index = 0; index < genericScripts.size(); ++index)
            {
                const container::String fieldName =
                    "teamGenericScriptHook" + std::to_string(index);
                const LegacyDictionaryEntry* property =
                    findDictionaryProperty(sourceTeam.properties, fieldName);
                const LegacySourceRange fieldSource = property &&
                        property->serialized.size != 0
                    ? property->serialized
                    : teamSource;
                const container::String targetName = teamStringProperty(
                    sourceTeam, fieldName, context, teamName, fieldSource);
                genericScripts[index] = resolveTeamGenericScriptTarget(
                    context, teamName, fieldName, targetName, fieldSource);
            }

            ScriptTarget productionCreateActions;
            if (teamBoolProperty(
                    sourceTeam, "teamExecutesActionsOnCreate", context,
                    teamName, teamSource)) {
                const LegacyDictionaryEntry* property =
                    findDictionaryProperty(
                        sourceTeam.properties, "teamProductionCondition");
                const LegacySourceRange fieldSource = property &&
                        property->serialized.size != 0
                    ? property->serialized : teamSource;
                const container::String targetName = teamStringProperty(
                    sourceTeam, "teamProductionCondition", context,
                    teamName, fieldSource);
                productionCreateActions = resolveTeamGenericScriptTarget(
                    context, teamName, "teamProductionCondition",
                    targetName, fieldSource);
            }

            ScriptTeamHookDefinition definition{
                .teamName = teamName,
                .productionCreateActions = productionCreateActions,
                .onCreate = hook("teamOnCreateScript"),
                .onIdle = hook("teamOnIdleScript"),
                .onEnemySighted = hook("teamEnemySightedScript"),
                .onAllClear = hook("teamAllClearScript"),
                .onDestroyed = hook("teamOnDestroyedScript"),
                .onUnitDestroyed = hook("teamOnUnitDestroyedScript"),
                .genericScripts = std::move(genericScripts),
                .destroyedThreshold = math::q32_32{teamRealProperty(
                    sourceTeam, "teamDestroyedThreshold", context,
                    teamName, teamSource)},
            };
            if (!builder.addTeamHook(std::move(definition)))
            {
                context.diagnostic(
                    LegacyScriptCompileDiagnosticSeverity::Error,
                    "ScriptProgramBuilder rejected legacy Team hook definition '" +
                        teamName + "'",
                    teamSource);
            }
        }
    }
}

[[nodiscard]] ScriptTarget resolveObjectHookTarget(
    CompileContext& context, uint32_t sourceSideOrdinal,
    uint32_t sourceBuildListOrdinal, container::StringView structureName,
    container::StringView targetName, LegacySourceRange source)
{
    if (targetName.empty() || uppercaseAscii(targetName) == "NONE")
        return {};

    const container::String sourceDescription =
        "legacy BuildList Object hook at Side " +
        std::to_string(sourceSideOrdinal) + ", entry " +
        std::to_string(sourceBuildListOrdinal) +
        (structureName.empty()
             ? container::String{}
             : " ('" + container::String(structureName) + "')");

    // ScriptEngine::runObjectScript() resolves Group before Script.  A
    // non-subroutine Group shadows a same-named Script and must not fall
    // through. Group active state remains a runtime concern because scripts
    // may enable or disable the Group before construction completes.
    if (const NamedGroup* group = findGroupByName(context, targetName))
    {
        if (group->isSubroutine)
            return ScriptTarget::groupTarget(group->id);
        context.diagnostic(
            LegacyScriptCompileDiagnosticSeverity::Warning,
            sourceDescription + " targets non-subroutine Group '" +
                container::String(targetName) + "'; Object hook omitted",
            source);
        return {};
    }
    if (const NamedScript* script = findScriptByName(context, targetName))
    {
        if (script->isSubroutine && script->runnable)
            return ScriptTarget::scriptTarget(script->id);
        context.diagnostic(
            LegacyScriptCompileDiagnosticSeverity::Warning,
            sourceDescription + " targets " +
                (script->isSubroutine ? "blocked subroutine Script '"
                                      : "non-subroutine Script '") +
                container::String(targetName) + "'; Object hook omitted",
            source);
        return {};
    }

    context.diagnostic(
        LegacyScriptCompileDiagnosticSeverity::Warning,
        sourceDescription + " targets unknown Script/Group '" +
            container::String(targetName) + "'; Object hook omitted",
        source);
    return {};
}

void appendObjectHooks(const LegacyMapScriptSource& source,
                       ScriptProgramBuilder& builder,
                       CompileContext& context)
{
    uint32_t sourceSideOrdinal = 0;
    for (const LegacySidesListSource& sidesList : source.sidesLists())
    {
        for (const LegacySideSource& side : sidesList.sides)
        {
            for (size_t buildListOrdinal = 0;
                 buildListOrdinal < side.buildList.size();
                 ++buildListOrdinal)
            {
                const LegacyBuildListEntrySource& entry =
                    side.buildList[buildListOrdinal];
                if (entry.script.empty() || uppercaseAscii(entry.script) == "NONE")
                    continue;

                const uint32_t sourceBuildListOrdinal =
                    static_cast<uint32_t>(buildListOrdinal);
                const ScriptTarget target = resolveObjectHookTarget(
                    context, sourceSideOrdinal, sourceBuildListOrdinal,
                    entry.buildingName, entry.script, entry.serialized);
                if (!target)
                    continue;

                if (!builder.addObjectHook({
                        .sourceSideOrdinal = sourceSideOrdinal,
                        .sourceBuildListOrdinal = sourceBuildListOrdinal,
                        .structureName = entry.buildingName,
                        .onBuilt = target,
                    }))
                {
                    context.diagnostic(
                        LegacyScriptCompileDiagnosticSeverity::Error,
                        "ScriptProgramBuilder rejected legacy BuildList Object hook for Side " +
                            std::to_string(sourceSideOrdinal) + ", entry " +
                            std::to_string(sourceBuildListOrdinal),
                        entry.serialized);
                }
            }
            ++sourceSideOrdinal;
        }
    }
}


LegacyScriptCompileResult compileLegacyScript(
    const LegacyMapScriptSource& source,
    const LegacyScriptCompileOptions& options)
{
    LegacyScriptCompileResult result;
    CompileContext context{.options = options, .result = result};

    container::Vector<ScriptGroupDefinition> groups;
    container::Vector<SourceScriptBinding> scripts;
    container::Vector<ScriptListExecutionDefinition> executionLists;
    uint32_t nextGroupId = 1;
    uint32_t nextScriptId = 1;

    // RefCode's SidesList reader assigns the i-th ScriptList inside a
    // PlayerScriptsList to the i-th Side (SidesList.cpp:303-315). Script
    // lookup and update then walk those side-owned lists in order
    // (ScriptEngine.cpp:5573-5588, 6287-6328). Preserve both the owning Side
    // alias and root/group order before compiling name-resolving instructions.
    const container::Span<const LegacyPlayerScriptsListSource> playerScriptLists = source.playerScriptLists();
    container::Vector<bool> consumedPlayerScriptLists(playerScriptLists.size(), false);

    const auto appendScriptList = [&](const LegacyScriptListSource& list,
                                      container::String currentPlayerAlias,
                                      uint32_t sourceSideOrdinal)
    {
        ScriptListExecutionDefinition executionList;
        executionList.currentPlayerAlias = std::move(currentPlayerAlias);
        executionList.sourceSideOrdinal = sourceSideOrdinal;
        executionList.rootScripts.reserve(list.scripts.size());
        executionList.groups.reserve(list.groups.size());

        for (const LegacyScriptSource& sourceScript : list.scripts)
        {
            const ScriptId id{nextScriptId++};
            container::String name = sourceScript.name;
            if (name.empty())
                name = "__legacy_script_" + std::to_string(id.value);
            scripts.push_back(
                {.source = &sourceScript, .id = id, .group = INVALID_SCRIPT_GROUP_ID, .programName = name});
            addScriptName(context, name, id, sourceScript.subroutine);
            executionList.rootScripts.push_back(id);
        }

        for (const LegacyScriptGroupSource& sourceGroup : list.groups)
        {
            const ScriptGroupId groupId{nextGroupId++};
            container::String groupName = sourceGroup.name;
            if (groupName.empty())
            {
                groupName = "__legacy_group_" + std::to_string(groupId.value);
                context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                                   "legacy script group had an empty name; assigned " + groupName,
                                   sourceGroup.serialized);
            }
            groups.push_back({.id = groupId,
                              .name = groupName,
                              .initiallyEnabled = sourceGroup.active,
                              .isSubroutine = sourceGroup.subroutine});
            addGroupName(context, groupName, groupId, sourceGroup.subroutine);
            executionList.groups.push_back(groupId);
            for (const LegacyScriptSource& sourceScript : sourceGroup.scripts)
            {
                const ScriptId id{nextScriptId++};
                container::String name = sourceScript.name;
                if (name.empty())
                    name = "__legacy_script_" + std::to_string(id.value);
                scripts.push_back({.source = &sourceScript, .id = id, .group = groupId, .programName = name});
                addScriptName(context, name, id, sourceScript.subroutine);
            }
        }
        executionLists.push_back(std::move(executionList));
    };

    const auto appendPlayerScriptsList = [&](const LegacyPlayerScriptsListSource& playerScripts,
                                             container::Span<const LegacySideSource> sides,
                                             uint32_t firstSourceSideOrdinal)
    {
        for (size_t listIndex = 0; listIndex < playerScripts.playerLists.size(); ++listIndex)
        {
            const LegacyScriptListSource& list = playerScripts.playerLists[listIndex];
            if (!sides.empty() && listIndex >= sides.size())
            {
                // RefCode deletes lists beyond the Side count instead of
                // executing them against an arbitrary player.
                context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                                   "PlayerScriptsList contains a ScriptList without a matching SidesList Side; ignored",
                                   list.serialized);
                continue;
            }

            container::String currentPlayerAlias;
            if (!sides.empty())
            {
                if (const std::optional<container::String> alias = sidePlayerAlias(sides[listIndex]))
                {
                    currentPlayerAlias = *alias;
                }
                else if (!list.scripts.empty() || !list.groups.empty())
                {
                    context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                                       "SidesList Side has scripts but no PlayerName; ThisPlayer actions will be diagnosed at runtime",
                                       list.serialized);
                }
            }
            const uint32_t sourceSideOrdinal = sides.empty()
                ? INVALID_LEGACY_SIDE_ORDINAL
                : firstSourceSideOrdinal + static_cast<uint32_t>(listIndex);
            appendScriptList(list, std::move(currentPlayerAlias), sourceSideOrdinal);
        }
    };

    uint32_t nextSourceSideOrdinal = 0;
    for (const LegacySidesListSource& sidesList : source.sidesLists())
    {
        for (const uint32_t playerScriptsIndex : sidesList.playerScriptsListIndices)
        {
            if (playerScriptsIndex >= playerScriptLists.size())
            {
                context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Error,
                                   "SidesList references a missing PlayerScriptsList index " +
                                       std::to_string(playerScriptsIndex),
                                   sidesList.serialized);
                continue;
            }
            if (consumedPlayerScriptLists[playerScriptsIndex])
            {
                context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Error,
                                   "PlayerScriptsList is attached to more than one SidesList",
                                   sidesList.serialized);
                continue;
            }
            consumedPlayerScriptLists[playerScriptsIndex] = true;
            appendPlayerScriptsList(playerScriptLists[playerScriptsIndex], sidesList.sides,
                                    nextSourceSideOrdinal);
        }
        nextSourceSideOrdinal += static_cast<uint32_t>(sidesList.sides.size());
    }

    // Standalone .scb/tool PlayerScriptsList data has no owning SidesList.
    // RefCode consumes it in file order, so retain it after explicitly-bound
    // map lists rather than silently dropping authored scripts.
    for (size_t index = 0; index < playerScriptLists.size(); ++index)
    {
        if (!consumedPlayerScriptLists[index])
            appendPlayerScriptsList(playerScriptLists[index], {}, INVALID_LEGACY_SIDE_ORDINAL);
    }

    ScriptProgramBuilder builder;
    for (ScriptGroupDefinition& group : groups)
    {
        if (!builder.addGroup(std::move(group)))
        {
            context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Error,
                               "ScriptProgramBuilder rejected a legacy group");
        }
    }
    for (ScriptListExecutionDefinition& executionList : executionLists)
    {
        if (!builder.addExecutionList(std::move(executionList)))
        {
            context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Error,
                               "ScriptProgramBuilder rejected a legacy ScriptList execution schedule");
        }
    }

    for (const SourceScriptBinding& binding : scripts)
    {
        ++result.sourceScriptCount;
        const LegacyScriptSource& sourceScript = *binding.source;
        ScriptDefinition definition{
            .id = binding.id,
            .name = binding.programName,
            .group = binding.group,
            .initiallyEnabled = sourceScript.active,
            .oneShot = sourceScript.oneShot,
            .isSubroutine = sourceScript.subroutine,
            .difficulties = {.easy = sourceScript.easy, .normal = sourceScript.normal, .hard = sourceScript.hard},
            .evaluationDelayTicks = delaySecondsToTicks(sourceScript.delayEvaluationSeconds, options),
            // ScriptEngine::checkConditionsForTeamNames() seeds delayed
            // scripts once with GameLogicRandomValue(0, 2 *
            // LOGICFRAMES_PER_SECOND). Store the already-selected session
            // frame rate in the Program so ScriptRuntime never consults a
            // global timing constant.
            .initialEvaluationJitterTicks = sourceScript.delayEvaluationSeconds > 0
                ? static_cast<uint32_t>(std::min<uint64_t>(
                    static_cast<uint64_t>(options.logicFramesPerSecond) * 2u,
                    static_cast<uint64_t>(std::numeric_limits<int32_t>::max())))
                : 0u,
            .conditionTeamCandidates = conditionTeamCandidates(sourceScript),
            .hasElseBranch = !sourceScript.falseActions.empty(),
        };

        const bool conditionsSupported = compileConditions(sourceScript, definition.anyOf, context, definition.name);
        const bool thenSupported =
            compileActions(sourceScript.actions, definition.thenActions, context, definition.name);
        const bool elseSupported =
            compileActions(sourceScript.falseActions, definition.elseActions, context, definition.name);
        const bool supported = conditionsSupported && thenSupported && elseSupported;
        if (!supported && options.blockScriptsWithUnsupportedInstructions)
        {
            // Conservatively retain an addressable, inert definition. This
            // keeps later enable/disable/call targets structurally valid while
            // preventing a partial replay from firing a victory or mutation.
            definition.anyOf = {{.allOf = {ScriptAlwaysFalseCondition{}}}};
            definition.conditionTeamCandidates.clear();
            definition.thenActions.clear();
            definition.hasElseBranch = false;
            definition.elseActions.clear();
            definition.oneShot = false;
            ++result.blockedScriptCount;
            setScriptRunnable(context, binding.id, false);
            context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Warning,
                               "script '" + definition.name +
                                   "' was blocked until all of its legacy instructions are implemented",
                               sourceScript.serialized);
        }
        else
        {
            ++result.runnableScriptCount;
        }
        if (!builder.addScript(std::move(definition)))
        {
            context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Error,
                               "ScriptProgramBuilder rejected legacy script '" + binding.programName + "'",
                               sourceScript.serialized);
        }
    }

    // Every Script/Group name index and runnable Script decision is complete
    // at this point. Resolve event bindings once now. Team gameplay events
    // remain a GameSession/ScriptRuntime responsibility; BuildList Object
    // completion remains a future Player/Skirmish AI producer responsibility.
    appendTeamHooks(source, builder, context);
    appendObjectHooks(source, builder, context);

    container::Vector<ScriptProgramBuildIssue> buildIssues;
    result.program = builder.finalize(&buildIssues);
    for (const ScriptProgramBuildIssue& issue : buildIssues)
    {
        context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Error, "ScriptProgram validation: " + issue.message);
    }
    if (!result.program)
    {
        context.diagnostic(LegacyScriptCompileDiagnosticSeverity::Error,
                           "Legacy script compilation could not freeze a ScriptProgram");
    }
    return result;
}
} // namespace engine::script::legacy::detail
