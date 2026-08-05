#pragma once

#include "game/script/runtime/ScriptProgram.h"

namespace engine::script::detail
{

[[nodiscard]] bool shouldUseDenseGroupScheduleIndex(uint32_t maximumGroupId,
                                                     size_t scheduleCount) noexcept;
void addIssue(container::Vector<ScriptProgramBuildIssue>* issues, container::String message);
[[nodiscard]] bool finiteVec3(const math::vec3& value) noexcept;
[[nodiscard]] bool validTargetShape(const ScriptTarget& target) noexcept;
[[nodiscard]] const ScriptDefinition* findScript(
    container::Span<const ScriptDefinition> scripts,
    ScriptId id) noexcept;
[[nodiscard]] const ScriptGroupDefinition* findGroup(
    container::Span<const ScriptGroupDefinition> groups,
    ScriptGroupId id) noexcept;

[[nodiscard]] bool validCondition(
    const ScriptCondition& condition,
    container::Vector<ScriptProgramBuildIssue>* issues,
    container::StringView scriptName);
[[nodiscard]] bool validActionShape(
    const ScriptAction& action,
    container::Vector<ScriptProgramBuildIssue>* issues,
    container::StringView scriptName);

void normalizeSymbols(container::Vector<container::String>& symbols);
[[nodiscard]] ScriptRuntimeSymbolId symbolId(
    container::Span<const container::String> symbols,
    container::StringView name) noexcept;
void collectRuntimeSymbols(
    const ScriptDefinition& definition,
    container::Vector<container::String>& counters,
    container::Vector<container::String>& flags);
void assignRuntimeSymbols(
    ScriptDefinition& definition,
    container::Span<const container::String> counters,
    container::Span<const container::String> flags);
[[nodiscard]] size_t directEffectCount(const ScriptAction& action) noexcept;
[[nodiscard]] size_t countDirectEffects(const ScriptDefinition& definition) noexcept;

} // namespace engine::script::detail

