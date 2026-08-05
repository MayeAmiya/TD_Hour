#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "ScriptRuntime.h"

#include <algorithm>
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

ScriptRuntime::ScriptRuntime(container::SharedPtr<const ScriptProgram> program, ScriptRuntimeContext context)
    : m_program(std::move(program))
    , m_context(context)
{
    reset();
}

void ScriptRuntime::setProgram(container::SharedPtr<const ScriptProgram> program)
{
    m_program = std::move(program);
    m_sidePlayerBindings.clear();
    reset();
}

void ScriptRuntime::setSidePlayerBindings(container::Vector<ScriptSidePlayerBinding> bindings)
{
    bindings.erase(std::remove_if(bindings.begin(), bindings.end(),
        [](const ScriptSidePlayerBinding& binding) {
            return binding.sourceSideOrdinal == INVALID_LEGACY_SIDE_ORDINAL ||
                   !binding.player.isValid();
        }), bindings.end());
    std::stable_sort(bindings.begin(), bindings.end(),
        [](const ScriptSidePlayerBinding& lhs, const ScriptSidePlayerBinding& rhs) {
            return lhs.sourceSideOrdinal < rhs.sourceSideOrdinal;
        });
    bindings.erase(std::unique(bindings.begin(), bindings.end(),
        [](const ScriptSidePlayerBinding& lhs, const ScriptSidePlayerBinding& rhs) {
            return lhs.sourceSideOrdinal == rhs.sourceSideOrdinal;
        }), bindings.end());
    m_sidePlayerBindings = std::move(bindings);
}

void ScriptRuntime::reset()
{
    m_scriptStates.clear();
    m_scriptStateIndex.clear();
    m_sparseScriptStateIndex.clear();
    m_groupStates.clear();
    m_groupStateIndex.clear();
    m_sparseGroupStateIndex.clear();
    m_counters.clear();
    m_counterSymbolsByState.clear();
    m_counterStateIndexBySymbol.clear();
    m_flags.clear();
    m_flagSymbolsByState.clear();
    m_flagStateIndexBySymbol.clear();
    m_objectTypeLists.clear();
    m_objectTypeCountBaselines.clear();
    m_sequentialQueues.clear();
    m_teamHookInstanceStates.clear();
    m_pendingTeamUnitDestroyedHooks.clear();
    m_initialEvaluationScheduleInitialized = false;
    m_hasLastConfirmedTick = false;
    m_lastConfirmedTick = 0;
    m_currentConfirmedTick = 0;
    m_currentInvocation = {};
    m_currentPlayerAlias.clear();
    m_currentDifficultyOverride.reset();
    m_effectOrdinal = 0;
    m_currentTeamHookDeathGeneration = 0;
    m_teamHookDispatchActive = false;
    m_drainingTeamUnitDestroyedHooks = false;

    if (!m_program)
        return;

    // Do not materialize every authored state name here: the legacy runtime
    // treats an untouched flag/counter as absent to callers (while conditions
    // still observe its implicit false/zero value).  Dense lookup slots give
    // compiled code O(1) access once a state is first written.
    m_counterStateIndexBySymbol.resize(m_program->counterSymbolCount());
    m_flagStateIndexBySymbol.resize(m_program->flagSymbolCount());

    m_groupStates.reserve(m_program->groups().size());
    for (const ScriptGroupDefinition& group : m_program->groups())
    {
        m_groupStates.push_back({.id = group.id, .enabled = group.initiallyEnabled});
    }
    m_scriptStates.reserve(m_program->scripts().size());
    for (const ScriptDefinition& script : m_program->scripts())
    {
        m_scriptStates.push_back({.id = script.id, .enabled = script.initiallyEnabled});
    }

    const auto initializeScriptStateIndex = [this]() {
        if (m_scriptStates.empty())
            return;
        const uint32_t maximumId = m_scriptStates.back().id.value;
        if (shouldUseDenseRuntimeStateIndex(maximumId, m_scriptStates.size()))
        {
            m_scriptStateIndex.resize(static_cast<size_t>(maximumId) + 1u);
            for (size_t index = 0; index < m_scriptStates.size(); ++index)
                m_scriptStateIndex[m_scriptStates[index].id.value] = index;
            return;
        }
        m_sparseScriptStateIndex.reserve(m_scriptStates.size());
        for (size_t index = 0; index < m_scriptStates.size(); ++index)
            m_sparseScriptStateIndex.emplace(m_scriptStates[index].id.value, index);
    };
    const auto initializeGroupStateIndex = [this]() {
        if (m_groupStates.empty())
            return;
        const uint32_t maximumId = m_groupStates.back().id.value;
        if (shouldUseDenseRuntimeStateIndex(maximumId, m_groupStates.size()))
        {
            m_groupStateIndex.resize(static_cast<size_t>(maximumId) + 1u);
            for (size_t index = 0; index < m_groupStates.size(); ++index)
                m_groupStateIndex[m_groupStates[index].id.value] = index;
            return;
        }
        m_sparseGroupStateIndex.reserve(m_groupStates.size());
        for (size_t index = 0; index < m_groupStates.size(); ++index)
            m_sparseGroupStateIndex.emplace(m_groupStates[index].id.value, index);
    };
    initializeScriptStateIndex();
    initializeGroupStateIndex();
}

bool ScriptRuntime::isScriptEnabled(ScriptId id) const noexcept
{
    const RuntimeScriptState* state = scriptState(id);
    return state && state->enabled;
}

bool ScriptRuntime::isGroupEnabled(ScriptGroupId id) const noexcept
{
    const RuntimeGroupState* state = groupState(id);
    return state && state->enabled;
}

std::optional<int32_t> ScriptRuntime::counterValue(container::StringView name) const
{
    const ScriptCounterState* state = counter(name);
    return state ? std::optional<int32_t>{state->value} : std::nullopt;
}

std::optional<bool> ScriptRuntime::flagValue(container::StringView name) const
{
    const RuntimeFlagState* state = flag(name);
    return state ? std::optional<bool>{state->value} : std::nullopt;
}

void ScriptRuntime::clearLegacySkirmishTeamBuildingFlags() noexcept
{
    if (!m_program)
        return;

    static constexpr const char* names[] = {
        "USA Team is Building",
        "USA Air Team Is Building",
        "USA Inf Team Is Building",
        "China Team is Building",
        "China Air Team Is Building",
        "China Inf Team Is Building",
        "GLA Team is Building",
        "GLA Inf Team is Building",
    };
    for (const char* name : names)
    {
        const std::optional<ScriptRuntimeSymbolId> symbol =
            m_program->findFlagSymbol(name);
        // Runtime flag storage is deliberately sparse. Do not materialize a
        // never-written flag merely because the legacy compatibility hook ran.
        if (!symbol || !flag(*symbol))
            continue;
        if (RuntimeFlagState* state = mutableFlag(*symbol))
            state->value = false;
    }
}

std::optional<ScriptCounterState> ScriptRuntime::counterState(container::StringView name) const
{
    const ScriptCounterState* state = counter(name);
    return state ? std::optional<ScriptCounterState>{*state} : std::nullopt;
}

container::Span<const ScriptCounterState> ScriptRuntime::counters() const noexcept
{
    return m_counters;
}

std::optional<container::Span<const container::String>> ScriptRuntime::objectTypeList(
    container::StringView name) const noexcept
{
    const auto found = lowerBoundObjectTypeList(name);
    if (found == m_objectTypeLists.end() || found->name != name)
        return std::nullopt;
    return container::Span<const container::String>{found->objectTypes};
}

container::Vector<ScriptRuntime::ObjectTypeListState>::iterator
ScriptRuntime::lowerBoundObjectTypeList(container::StringView name) noexcept
{
    return std::lower_bound(m_objectTypeLists.begin(), m_objectTypeLists.end(), name,
        [](const ObjectTypeListState& state, container::StringView needle) {
            return container::StringView{state.name} < needle;
        });
}

container::Vector<ScriptRuntime::ObjectTypeListState>::const_iterator
ScriptRuntime::lowerBoundObjectTypeList(container::StringView name) const noexcept
{
    return std::lower_bound(m_objectTypeLists.begin(), m_objectTypeLists.end(), name,
        [](const ObjectTypeListState& state, container::StringView needle) {
            return container::StringView{state.name} < needle;
        });
}

ScriptRuntime::RuntimeScriptState* ScriptRuntime::mutableScriptState(ScriptId id) noexcept
{
    if (!id)
        return nullptr;
    if (id.value < m_scriptStateIndex.size())
    {
        const std::optional<size_t>& index = m_scriptStateIndex[id.value];
        return index && *index < m_scriptStates.size() ? &m_scriptStates[*index] : nullptr;
    }
    const auto found = m_sparseScriptStateIndex.find(id.value);
    return found != m_sparseScriptStateIndex.end() && found->second < m_scriptStates.size()
        ? &m_scriptStates[found->second]
        : nullptr;
}

const ScriptRuntime::RuntimeScriptState* ScriptRuntime::scriptState(ScriptId id) const noexcept
{
    if (!id)
        return nullptr;
    if (id.value < m_scriptStateIndex.size())
    {
        const std::optional<size_t>& index = m_scriptStateIndex[id.value];
        return index && *index < m_scriptStates.size() ? &m_scriptStates[*index] : nullptr;
    }
    const auto found = m_sparseScriptStateIndex.find(id.value);
    return found != m_sparseScriptStateIndex.end() && found->second < m_scriptStates.size()
        ? &m_scriptStates[found->second]
        : nullptr;
}

ScriptRuntime::RuntimeGroupState* ScriptRuntime::mutableGroupState(ScriptGroupId id) noexcept
{
    if (!id)
        return nullptr;
    if (id.value < m_groupStateIndex.size())
    {
        const std::optional<size_t>& index = m_groupStateIndex[id.value];
        return index && *index < m_groupStates.size() ? &m_groupStates[*index] : nullptr;
    }
    const auto found = m_sparseGroupStateIndex.find(id.value);
    return found != m_sparseGroupStateIndex.end() && found->second < m_groupStates.size()
        ? &m_groupStates[found->second]
        : nullptr;
}

const ScriptRuntime::RuntimeGroupState* ScriptRuntime::groupState(ScriptGroupId id) const noexcept
{
    if (!id)
        return nullptr;
    if (id.value < m_groupStateIndex.size())
    {
        const std::optional<size_t>& index = m_groupStateIndex[id.value];
        return index && *index < m_groupStates.size() ? &m_groupStates[*index] : nullptr;
    }
    const auto found = m_sparseGroupStateIndex.find(id.value);
    return found != m_sparseGroupStateIndex.end() && found->second < m_groupStates.size()
        ? &m_groupStates[found->second]
        : nullptr;
}

ScriptCounterState* ScriptRuntime::mutableCounter(ScriptRuntimeSymbolId symbol)
{
    if (!m_program || !symbol || symbol.value > m_counterStateIndexBySymbol.size())
        return nullptr;
    std::optional<size_t>& existing = m_counterStateIndexBySymbol[symbol.value - 1];
    if (existing)
        return *existing < m_counters.size() ? &m_counters[*existing] : nullptr;

    const container::StringView name = m_program->counterSymbolName(symbol);
    if (name.empty())
        return nullptr;
    const auto position = std::lower_bound(m_counterSymbolsByState.begin(),
                                           m_counterSymbolsByState.end(), symbol);
    const size_t index = static_cast<size_t>(std::distance(m_counterSymbolsByState.begin(), position));
    m_counterSymbolsByState.insert(position, symbol);
    m_counters.insert(m_counters.begin() + static_cast<std::ptrdiff_t>(index),
                      {.name = container::String(name)});
    for (size_t stateIndex = index; stateIndex < m_counterSymbolsByState.size(); ++stateIndex)
    {
        m_counterStateIndexBySymbol[m_counterSymbolsByState[stateIndex].value - 1] = stateIndex;
    }
    return &m_counters[index];
}

const ScriptCounterState* ScriptRuntime::counter(ScriptRuntimeSymbolId symbol) const noexcept
{
    if (!symbol || symbol.value > m_counterStateIndexBySymbol.size())
        return nullptr;
    const std::optional<size_t>& index = m_counterStateIndexBySymbol[symbol.value - 1];
    return index && *index < m_counters.size() ? &m_counters[*index] : nullptr;
}

const ScriptCounterState* ScriptRuntime::counter(container::StringView name) const
{
    if (!m_program)
        return nullptr;
    const std::optional<ScriptRuntimeSymbolId> symbol = m_program->findCounterSymbol(name);
    return symbol ? counter(*symbol) : nullptr;
}

ScriptRuntime::RuntimeFlagState* ScriptRuntime::mutableFlag(ScriptRuntimeSymbolId symbol)
{
    if (!m_program || !symbol || symbol.value > m_flagStateIndexBySymbol.size())
        return nullptr;
    std::optional<size_t>& existing = m_flagStateIndexBySymbol[symbol.value - 1];
    if (existing)
        return *existing < m_flags.size() ? &m_flags[*existing] : nullptr;

    const container::StringView name = m_program->flagSymbolName(symbol);
    if (name.empty())
        return nullptr;
    const auto position = std::lower_bound(m_flagSymbolsByState.begin(),
                                           m_flagSymbolsByState.end(), symbol);
    const size_t index = static_cast<size_t>(std::distance(m_flagSymbolsByState.begin(), position));
    m_flagSymbolsByState.insert(position, symbol);
    m_flags.insert(m_flags.begin() + static_cast<std::ptrdiff_t>(index),
                   {.name = container::String(name)});
    for (size_t stateIndex = index; stateIndex < m_flagSymbolsByState.size(); ++stateIndex)
    {
        m_flagStateIndexBySymbol[m_flagSymbolsByState[stateIndex].value - 1] = stateIndex;
    }
    return &m_flags[index];
}

const ScriptRuntime::RuntimeFlagState* ScriptRuntime::flag(ScriptRuntimeSymbolId symbol) const noexcept
{
    if (!symbol || symbol.value > m_flagStateIndexBySymbol.size())
        return nullptr;
    const std::optional<size_t>& index = m_flagStateIndexBySymbol[symbol.value - 1];
    return index && *index < m_flags.size() ? &m_flags[*index] : nullptr;
}

const ScriptRuntime::RuntimeFlagState* ScriptRuntime::flag(container::StringView name) const
{
    if (!m_program)
        return nullptr;
    const std::optional<ScriptRuntimeSymbolId> symbol = m_program->findFlagSymbol(name);
    return symbol ? flag(*symbol) : nullptr;
}

PlayerId ScriptRuntime::playerForSourceSide(uint32_t sourceSideOrdinal) const noexcept
{
    if (sourceSideOrdinal == INVALID_LEGACY_SIDE_ORDINAL)
        return INVALID_PLAYER_ID;
    const auto found = std::lower_bound(
        m_sidePlayerBindings.begin(), m_sidePlayerBindings.end(), sourceSideOrdinal,
        [](const ScriptSidePlayerBinding& binding, uint32_t ordinal) {
            return binding.sourceSideOrdinal < ordinal;
        });
    return found != m_sidePlayerBindings.end() && found->sourceSideOrdinal == sourceSideOrdinal
        ? found->player
        : INVALID_PLAYER_ID;
}

std::optional<ScriptDifficulty> ScriptRuntime::difficultyForSourceSide(
    uint32_t sourceSideOrdinal) const noexcept
{
    if (sourceSideOrdinal == INVALID_LEGACY_SIDE_ORDINAL)
        return std::nullopt;
    const auto found = std::lower_bound(
        m_sidePlayerBindings.begin(), m_sidePlayerBindings.end(), sourceSideOrdinal,
        [](const ScriptSidePlayerBinding& binding, uint32_t ordinal) {
            return binding.sourceSideOrdinal < ordinal;
        });
    return found != m_sidePlayerBindings.end() && found->sourceSideOrdinal == sourceSideOrdinal
        ? found->effectiveDifficulty
        : std::nullopt;
}

std::optional<ScriptDifficulty> ScriptRuntime::difficultyForPlayer(
    PlayerId player) const noexcept
{
    if (!player.isValid())
        return std::nullopt;
    // Bindings are retained in source-Side order. RefCode's hook path sets
    // m_currentPlayer to the Team owner, so the first matching Side supplies
    // the same AI difficulty while human/nullopt entries fall back globally.
    const auto found = std::find_if(
        m_sidePlayerBindings.begin(), m_sidePlayerBindings.end(),
        [player](const ScriptSidePlayerBinding& binding) {
            return binding.player == player;
        });
    return found != m_sidePlayerBindings.end()
        ? found->effectiveDifficulty
        : std::nullopt;
}

void ScriptRuntime::decrementTimers() noexcept
{
    for (ScriptCounterState& state : m_counters)
    {
        if (state.countdownTimerRunning && state.value >= 0)
        {
            --state.value;
        }
    }
}

void ScriptRuntime::initializeInitialEvaluationSchedule(uint64_t firstConfirmedTick) noexcept
{
    if (m_initialEvaluationScheduleInitialized || !m_program)
        return;

    // ScriptEngine::checkConditionsForTeamNames() visits every ScriptList in
    // side order, roots before groups, and invokes GameLogicRandomValue only
    // for scripts whose DelayEvalSeconds is positive. Use the immutable
    // execution schedule rather than ScriptProgram's ID-sorted lookup view:
    // compiler IDs happen to be source ordered today, but the schedule is
    // the actual authored-order contract and includes subroutine entries.
    //
    // Focused tools may intentionally run without a session RNG. Preserve
    // their historical deterministic first-tick behavior in that case rather
    // than reaching for a global/std random engine; a real GameSession always
    // supplies its SimulationRandom through ScriptRandomSource.
    m_initialEvaluationScheduleInitialized = true;
    const auto initializeScript = [this, firstConfirmedTick](ScriptId id) noexcept {
        const ScriptDefinition* definition = m_program->findScript(id);
        if (!definition)
            return;
        if (definition->initialEvaluationJitterTicks == 0)
            return;

        RuntimeScriptState* state = mutableScriptState(definition->id);
        if (!state)
            return;

        uint32_t offset = 0;
        if (m_context.random)
        {
            const int32_t maximum = static_cast<int32_t>(definition->initialEvaluationJitterTicks);
            const int32_t sampled = m_context.random->integerInclusive(0, maximum);
            // The source is required to honor the closed interval. Clamp the
            // detached interface defensively so an invalid test/integration
            // source cannot create a wrapped next-evaluation tick.
            offset = static_cast<uint32_t>(std::clamp(sampled, 0, maximum));
        }
        state->nextEvaluationTick = nextEvaluationTick(firstConfirmedTick, offset);
    };
    for (const ScriptListExecutionDefinition& list : m_program->executionLists())
    {
        for (const ScriptId id : list.rootScripts)
            initializeScript(id);
        for (const ScriptGroupId group : list.groups)
        {
            for (const ScriptId id : m_program->groupExecutionOrder(group))
                initializeScript(id);
        }
    }
}

std::optional<ScriptWorldObjectSnapshot> ScriptRuntime::resolveObject(
    container::StringView reference) const
{
    if (!m_context.world || reference.empty()) return std::nullopt;
    const ScriptObjectSelector selector = isThisObjectReference(reference)
        ? ScriptObjectSelector::thisObject()
        : ScriptObjectSelector::named(container::String(reference));
    return m_context.world->resolveObjectSelector(selector, m_currentInvocation);
}

std::optional<ObjectTeamId> ScriptRuntime::resolveTeam(
    container::StringView reference) const
{
    if (!m_context.world || reference.empty()) return std::nullopt;
    const ScriptTeamSelector selector = isThisTeamReference(reference)
        ? ScriptTeamSelector::thisTeam()
        : ScriptTeamSelector::scenarioTeam(container::String(reference));
    return m_context.world->resolveTeamSelector(selector, m_currentInvocation);
}

std::optional<ObjectTeamId> ScriptRuntime::resolveTeam(
    const ScriptTeamSelector& selector) const
{
    if (!m_context.world || !selector.valid()) return std::nullopt;
    return m_context.world->resolveTeamSelector(selector, m_currentInvocation);
}

std::optional<PlayerId> ScriptRuntime::resolvePlayer(container::StringView reference) const
{
    if (!m_context.world || reference.empty())
        return std::nullopt;
    if (isThisPlayerReference(reference))
        return m_currentInvocation.currentPlayer
            ? std::optional<PlayerId>{m_currentInvocation.currentPlayer}
            : std::nullopt;
    if (isThisPlayerEnemyReference(reference))
        return m_currentInvocation.currentPlayer
            ? m_context.world->currentEnemyPlayer(
                  m_currentInvocation.currentPlayer)
            : std::nullopt;
    return m_context.world->findPlayer(reference);
}

} // namespace engine::script
