#include "core/container/container_types.h"
#include "ScriptProgram.h"
#include "ScriptProgramValidationInternal.h"

#include <algorithm>

namespace engine::script
{

using namespace detail;

container::Span<const ScriptGroupDefinition> ScriptProgram::groups() const noexcept
{
    return m_groupsById;
}

container::Span<const ScriptDefinition> ScriptProgram::scripts() const noexcept
{
    return m_scriptsById;
}

container::Span<const ScriptListExecutionDefinition> ScriptProgram::executionLists() const noexcept
{
    return m_executionLists;
}

container::Span<const ScriptTeamHookDefinition> ScriptProgram::teamHooks() const noexcept
{
    return m_teamHooks;
}

container::Span<const ScriptObjectHookDefinition> ScriptProgram::objectHooks() const noexcept
{
    return m_objectHooks;
}

const ScriptObjectHookDefinition* ScriptProgram::findObjectHook(
    uint32_t sourceSideOrdinal,
    uint32_t sourceBuildListOrdinal) const noexcept
{
    const auto found = std::find_if(
        m_objectHooks.begin(), m_objectHooks.end(),
        [sourceSideOrdinal, sourceBuildListOrdinal](
            const ScriptObjectHookDefinition& hook) noexcept {
            return hook.sourceSideOrdinal == sourceSideOrdinal &&
                hook.sourceBuildListOrdinal == sourceBuildListOrdinal;
        });
    return found == m_objectHooks.end() ? nullptr : &*found;
}

container::Span<const ScriptId> ScriptProgram::rootExecutionOrder() const noexcept
{
    return m_rootExecutionOrder;
}

container::Span<const ScriptGroupId> ScriptProgram::groupTraversalOrder() const noexcept
{
    return m_groupExecutionOrder;
}

std::optional<size_t> ScriptProgram::findGroupScheduleIndex(ScriptGroupId group) const noexcept
{
    if (!group)
        return std::nullopt;

    if (group.value < m_groupScheduleIndex.size())
        return m_groupScheduleIndex[group.value];

    const auto found = m_sparseGroupScheduleIndex.find(group.value);
    return found == m_sparseGroupScheduleIndex.end() ? std::nullopt
                                                      : std::optional<size_t>{found->second};
}

container::Span<const ScriptId> ScriptProgram::groupExecutionOrder(ScriptGroupId group) const noexcept
{
    const std::optional<size_t> index = findGroupScheduleIndex(group);
    return index && *index < m_groupSchedules.size() ? m_groupSchedules[*index].scriptIds
                                                       : container::Span<const ScriptId>{};
}

const ScriptDefinition* ScriptProgram::findScript(ScriptId id) const noexcept
{
    return detail::findScript(m_scriptsById, id);
}

const ScriptDefinition* ScriptProgram::findScriptByName(
    container::StringView name) const noexcept
{
    const auto found = std::find_if(
        m_scriptsById.begin(), m_scriptsById.end(),
        [name](const ScriptDefinition& script) noexcept {
            return script.name == name;
        });
    return found == m_scriptsById.end() ? nullptr : &*found;
}

const ScriptGroupDefinition* ScriptProgram::findGroup(ScriptGroupId id) const noexcept
{
    return detail::findGroup(m_groupsById, id);
}

std::optional<ScriptRuntimeSymbolId> ScriptProgram::findCounterSymbol(container::StringView name) const noexcept
{
    const ScriptRuntimeSymbolId id = symbolId(m_counterSymbols, name);
    return id ? std::optional<ScriptRuntimeSymbolId>{id} : std::nullopt;
}

std::optional<ScriptRuntimeSymbolId> ScriptProgram::findFlagSymbol(container::StringView name) const noexcept
{
    const ScriptRuntimeSymbolId id = symbolId(m_flagSymbols, name);
    return id ? std::optional<ScriptRuntimeSymbolId>{id} : std::nullopt;
}

container::StringView ScriptProgram::counterSymbolName(ScriptRuntimeSymbolId id) const noexcept
{
    const size_t index = id ? static_cast<size_t>(id.value - 1) : m_counterSymbols.size();
    return index < m_counterSymbols.size() ? container::StringView{m_counterSymbols[index]} : container::StringView{};
}

container::StringView ScriptProgram::flagSymbolName(ScriptRuntimeSymbolId id) const noexcept
{
    const size_t index = id ? static_cast<size_t>(id.value - 1) : m_flagSymbols.size();
    return index < m_flagSymbols.size() ? container::StringView{m_flagSymbols[index]} : container::StringView{};
}

} // namespace engine::script
