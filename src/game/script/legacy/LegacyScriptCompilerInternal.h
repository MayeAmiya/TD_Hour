#pragma once

#include "core/container/hash_containers.h"
#include "LegacyScriptCompiler.h"

#include <optional>

namespace engine::script::legacy::detail
{

struct NamedScript final
{
    container::String name;
    ScriptId id = INVALID_SCRIPT_ID;
    bool isSubroutine = false;
    bool runnable = true;
};

struct NamedGroup final
{
    container::String name;
    ScriptGroupId id = INVALID_SCRIPT_GROUP_ID;
    bool isSubroutine = false;
};

struct SourceScriptBinding final
{
    const LegacyScriptSource* source = nullptr;
    ScriptId id = INVALID_SCRIPT_ID;
    ScriptGroupId group = INVALID_SCRIPT_GROUP_ID;
    container::String programName;
};

struct ScriptNameHash final
{
    using is_transparent = void;

    [[nodiscard]] size_t operator()(container::StringView value) const noexcept;
    [[nodiscard]] size_t operator()(const container::String& value) const noexcept;
};

struct ScriptNameEqual final
{
    using is_transparent = void;

    [[nodiscard]] bool operator()(container::StringView left,
                                  container::StringView right) const noexcept;
};

struct CompileContext final
{
    const LegacyScriptCompileOptions& options;
    LegacyScriptCompileResult& result;
    container::Vector<NamedScript> scriptsBySourceOrder;
    container::HashMap<container::String, size_t, ScriptNameHash, ScriptNameEqual>
        scriptNameIndex;
    container::Vector<NamedGroup> groupsBySourceOrder;
    container::HashMap<container::String, size_t, ScriptNameHash, ScriptNameEqual>
        groupNameIndex;

    void diagnostic(LegacyScriptCompileDiagnosticSeverity severity,
                    container::String message,
                    LegacySourceRange source = {});
};

struct LegacyCameraTiming final
{
    uint32_t durationTicks = 0;
    uint32_t easeInTicks = 0;
    uint32_t easeOutTicks = 0;
};

[[nodiscard]] std::optional<container::StringView>
legacyZeroHourKindOfName(int32_t ordinal) noexcept;
[[nodiscard]] container::String uppercaseAscii(container::StringView value);
[[nodiscard]] bool isDynamicScriptContextSelector(container::StringView value) noexcept;
[[nodiscard]] ScriptObjectSelector objectSelector(container::String value);
[[nodiscard]] ScriptTeamSelector teamSelector(container::String value);
[[nodiscard]] bool isUnsupportedDynamicPlayerSelector(container::StringView value) noexcept;
bool rejectDynamicScriptContextSelector(CompileContext& context,
                                        container::StringView scriptName,
                                        container::StringView actionName,
                                        container::StringView selector,
                                        const LegacySourceRange& source);
bool rejectUnsupportedDynamicPlayerSelector(CompileContext& context,
                                            container::StringView scriptName,
                                            container::StringView instructionName,
                                            container::StringView selector,
                                            const LegacySourceRange& source);
[[nodiscard]] std::optional<container::String> sidePlayerAlias(const LegacySideSource& side);
[[nodiscard]] const LegacyDictionaryEntry* findDictionaryProperty(
    const container::Vector<LegacyDictionaryEntry>& properties,
    container::StringView name);
[[nodiscard]] container::String teamStringProperty(
    const LegacyTeamSource& team, container::StringView propertyName,
    CompileContext& context, container::StringView teamName,
    LegacySourceRange fallbackSource);
[[nodiscard]] float teamRealProperty(
    const LegacyTeamSource& team, container::StringView propertyName,
    CompileContext& context, container::StringView teamName,
    LegacySourceRange fallbackSource);
[[nodiscard]] bool teamBoolProperty(
    const LegacyTeamSource& team, container::StringView propertyName,
    CompileContext& context, container::StringView teamName,
    LegacySourceRange fallbackSource);
[[nodiscard]] container::String instructionName(
    const LegacyScriptInstructionSource& instruction, bool action,
    CompileContext& context, container::StringView scriptName);
[[nodiscard]] const LegacyScriptParameter* parameterAt(
    const LegacyScriptInstructionSource& instruction, size_t index,
    CompileContext& context, container::StringView scriptName,
    container::StringView instructionLabel);
[[nodiscard]] std::optional<container::String> textParameter(
    const LegacyScriptInstructionSource& instruction, size_t index,
    CompileContext& context, container::StringView scriptName,
    container::StringView instructionLabel);
[[nodiscard]] std::optional<container::String> textParameterAllowEmpty(
    const LegacyScriptInstructionSource& instruction, size_t index,
    CompileContext& context, container::StringView scriptName,
    container::StringView instructionLabel);
[[nodiscard]] std::optional<int32_t> integerParameter(
    const LegacyScriptInstructionSource& instruction, size_t index,
    CompileContext& context, container::StringView scriptName,
    container::StringView instructionLabel);
[[nodiscard]] std::optional<float> realParameter(
    const LegacyScriptInstructionSource& instruction, size_t index,
    CompileContext& context, container::StringView scriptName,
    container::StringView instructionLabel);
[[nodiscard]] std::optional<math::vec3> coordinateParameter(
    const LegacyScriptInstructionSource& instruction, size_t index,
    CompileContext& context, container::StringView scriptName,
    container::StringView instructionLabel);
[[nodiscard]] std::optional<ScriptComparison> comparisonParameter(
    const LegacyScriptInstructionSource& instruction, size_t index,
    CompileContext& context, container::StringView scriptName,
    container::StringView instructionLabel);
[[nodiscard]] std::optional<ScriptPlayerRelationship> playerRelationshipParameter(
    const LegacyScriptInstructionSource& instruction, size_t index,
    CompileContext& context, container::StringView scriptName,
    container::StringView instructionLabel);
[[nodiscard]] std::optional<ScriptScienceAvailability> scienceAvailabilityParameter(
    const LegacyScriptInstructionSource& instruction, size_t index,
    CompileContext& context, container::StringView scriptName,
    container::StringView instructionLabel);
[[nodiscard]] std::optional<uint8_t> teamAreaSurfacesParameter(
    const LegacyScriptInstructionSource& instruction, CompileContext& context,
    container::StringView scriptName,
    container::StringView instructionLabel);
[[nodiscard]] std::optional<int32_t> secondsToTicks(
    float seconds, CompileContext& context, container::StringView scriptName,
    LegacySourceRange source);
[[nodiscard]] std::optional<int32_t> signedSecondsToTicks(
    float seconds, CompileContext& context, container::StringView scriptName,
    LegacySourceRange source);
[[nodiscard]] std::optional<int32_t> randomTimerSecondsEndpoint(
    float seconds, CompileContext& context, container::StringView scriptName,
    LegacySourceRange source);
[[nodiscard]] std::optional<LegacyCameraTiming> cameraTimingParameters(
    const LegacyScriptInstructionSource& instruction, size_t durationIndex,
    size_t easeInIndex, size_t easeOutIndex, CompileContext& context,
    container::StringView scriptName, container::StringView instructionLabel);
[[nodiscard]] int32_t saturatedNegate(int32_t value) noexcept;
[[nodiscard]] uint32_t delaySecondsToTicks(
    int32_t seconds, const LegacyScriptCompileOptions& options);
[[nodiscard]] const NamedScript* findScriptByName(
    const CompileContext& context, container::StringView name) noexcept;
void setScriptRunnable(CompileContext& context, ScriptId id, bool runnable) noexcept;
[[nodiscard]] const NamedGroup* findGroupByName(
    const CompileContext& context, container::StringView name) noexcept;

[[nodiscard]] bool compileConditions(
    const LegacyScriptSource& source, container::Vector<ScriptAndClause>& output,
    CompileContext& context, container::StringView scriptName);
[[nodiscard]] container::Vector<ScriptTeamSelector> conditionTeamCandidates(
    const LegacyScriptSource& source);

[[nodiscard]] std::optional<bool> compileControlAndPresentationAction(
    const LegacyScriptInstructionSource& instruction,
    container::Vector<ScriptAction>& output, CompileContext& context,
    container::StringView scriptName, const container::String& name);
[[nodiscard]] std::optional<bool> compileObjectAndTeamAction(
    const LegacyScriptInstructionSource& instruction,
    container::Vector<ScriptAction>& output, CompileContext& context,
    container::StringView scriptName, const container::String& name);
[[nodiscard]] std::optional<bool> compilePlayerAndSequentialAction(
    const LegacyScriptInstructionSource& instruction,
    container::Vector<ScriptAction>& output, CompileContext& context,
    container::StringView scriptName, const container::String& name);
[[nodiscard]] bool compileActions(
    const container::Vector<LegacyScriptInstructionSource>& sourceActions,
    container::Vector<ScriptAction>& output, CompileContext& context,
    container::StringView scriptName);

[[nodiscard]] LegacyScriptCompileResult compileLegacyScript(
    const LegacyMapScriptSource& source,
    const LegacyScriptCompileOptions& options);

} // namespace engine::script::legacy::detail
