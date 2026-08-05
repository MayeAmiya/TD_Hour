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

container::Vector<ScriptRuntime::SequentialQueue>::iterator
ScriptRuntime::findSequentialQueue(SequentialTargetKey target) noexcept
{
    return std::find_if(m_sequentialQueues.begin(), m_sequentialQueues.end(),
        [target](const SequentialQueue& queue) { return queue.target == target; });
}

container::Vector<ScriptRuntime::SequentialQueue>::const_iterator
ScriptRuntime::findSequentialQueue(SequentialTargetKey target) const noexcept
{
    return std::find_if(m_sequentialQueues.begin(), m_sequentialQueues.end(),
        [target](const SequentialQueue& queue) { return queue.target == target; });
}

void ScriptRuntime::enqueueSequential(SequentialTargetKey target,
                                      ScriptId script,
                                      int32_t remainingRequeues)
{
    if (!target || !script) return;
    auto queue = findSequentialQueue(target);
    if (queue == m_sequentialQueues.end())
    {
        m_sequentialQueues.push_back({.target = target});
        queue = std::prev(m_sequentialQueues.end());
    }
    queue->entries.push_back({
        .script = script,
        .remainingRequeues = remainingRequeues,
    });
}

void ScriptRuntime::clearSequential(SequentialTargetKey target) noexcept
{
    const auto queue = findSequentialQueue(target);
    if (queue != m_sequentialQueues.end())
        m_sequentialQueues.erase(queue);
}

void ScriptRuntime::setSequentialWaitFrames(
    SequentialTargetKey target, int32_t frames) noexcept
{
    const auto queue = findSequentialQueue(target);
    if (queue != m_sequentialQueues.end() && !queue->entries.empty())
        queue->entries.front().framesToWait = frames;
}

bool ScriptRuntime::sequentialWaitSatisfied(
    const ScriptSequentialWaitAction& wait) const
{
    if (!m_context.world) return false;
    const std::optional<ObjectTeamId> team =
        m_context.world->resolveTeamSelector(wait.team, m_currentInvocation);
    switch (wait.kind)
    {
    case ScriptSequentialWaitKind::CommandButtonAllReady:
        return team && m_context.world->teamCommandButtonReady(
                           *team, wait.commandButton, true);
    case ScriptSequentialWaitKind::CommandButtonPartiallyReady:
        return team && m_context.world->teamCommandButtonReady(
                           *team, wait.commandButton, false);
    case ScriptSequentialWaitKind::TeamNotContainedAll:
        return !team || !m_context.world->teamContained(*team, true);
    case ScriptSequentialWaitKind::TeamNotContainedPartial:
        return !team || !m_context.world->teamContained(*team, false);
    }
    return false;
}

void ScriptRuntime::executeSequentialScripts(
    ScriptEffectSink& sink,
    ScriptRuntimeStepResult& result)
{
    if (!m_context.world) return;
    constexpr uint32_t kMaximumProgressPassesPerTarget = 21;

    size_t queueIndex = 0;
    while (queueIndex < m_sequentialQueues.size())
    {
        const SequentialTargetKey target = m_sequentialQueues[queueIndex].target;
        uint32_t progressPasses = 0;
        bool yielded = false;
        while (!yielded && progressPasses < kMaximumProgressPassesPerTarget)
        {
            auto queue = findSequentialQueue(target);
            if (queue == m_sequentialQueues.end()) break;
            if (queue->entries.empty())
            {
                m_sequentialQueues.erase(queue);
                break;
            }

            const ScriptSequentialAuthorityState authority =
                target.kind == ScriptSequentialTargetKind::Object
                    ? m_context.world->sequentialObjectState(ObjectId{target.value})
                    : m_context.world->sequentialTeamState(ObjectTeamId{target.value});
            const bool canUseTarget = target.kind == ScriptSequentialTargetKind::Team ||
                                      authority.hasAI;
            if (!authority.exists || authority.effectivelyDead)
            {
                clearSequential(target);
                break;
            }
            if (!canUseTarget)
            {
                yielded = true;
                break;
            }

            SequentialEntry& entry = queue->entries.front();
            if (entry.framesToWait > 0)
            {
                --entry.framesToWait;
                yielded = true;
                break;
            }
            if (entry.framesToWait < 0 && !authority.idle)
            {
                yielded = true;
                break;
            }

            const ScriptDefinition* definition = m_program->findScript(entry.script);
            if (!definition)
            {
                queue->entries.pop_front();
                ++progressPasses;
                continue;
            }
            if (entry.nextInstruction >= definition->thenActions.size())
            {
                SequentialEntry completed = entry;
                queue->entries.pop_front();
                if (completed.remainingRequeues != 0)
                {
                    if (completed.remainingRequeues > 0)
                        --completed.remainingRequeues;
                    completed.nextInstruction = 0;
                    completed.framesToWait = -1;
                    queue->entries.push_back(completed);
                }
                if (queue->entries.empty())
                    m_sequentialQueues.erase(queue);
                ++progressPasses;
                continue;
            }

            const ScriptAction& instruction =
                definition->thenActions[entry.nextInstruction];
            const ScriptInvocationContext savedInvocation = m_currentInvocation;
            const container::String savedPlayerAlias = std::move(m_currentPlayerAlias);
            m_currentInvocation = {
                .conditionTeam = target.kind == ScriptSequentialTargetKind::Team
                    ? ObjectTeamId{target.value}
                    : INVALID_OBJECT_TEAM_ID,
                .conditionObject = target.kind == ScriptSequentialTargetKind::Object
                    ? ObjectId{target.value}
                    : INVALID_OBJECT_ID,
                .currentPlayer = authority.currentPlayer,
                .origin = target.kind == ScriptSequentialTargetKind::Object
                    ? ScriptInvocationOrigin::SequentialObject
                    : ScriptInvocationOrigin::SequentialTeam,
            };
            m_currentPlayerAlias.clear();

            if (const auto* wait = std::get_if<ScriptSequentialWaitAction>(&instruction))
            {
                if (sequentialWaitSatisfied(*wait))
                    ++entry.nextInstruction;
                else
                    yielded = true;
            }
            else
            {
                ++entry.nextInstruction;
                entry.framesToWait = -1;
                executeAction(instruction, definition->id, sink, result, 0);
            }

            m_currentInvocation = savedInvocation;
            m_currentPlayerAlias = savedPlayerAlias;
            ++progressPasses;
            if (yielded) break;

            // The instruction may have stopped or replaced this target's
            // FIFO. Reacquire by stable key before observing post-action AI.
            queue = findSequentialQueue(target);
            if (queue == m_sequentialQueues.end()) break;
            if (!queue->entries.empty() && queue->entries.front().framesToWait > 0)
            {
                --queue->entries.front().framesToWait;
                yielded = true;
                break;
            }
            const ScriptSequentialAuthorityState after =
                target.kind == ScriptSequentialTargetKind::Object
                    ? m_context.world->sequentialObjectState(ObjectId{target.value})
                    : m_context.world->sequentialTeamState(ObjectTeamId{target.value});
            if (!after.exists || after.effectivelyDead)
            {
                clearSequential(target);
                break;
            }
            if (!after.idle)
                yielded = true;
        }

        const auto current = findSequentialQueue(target);
        if (current == m_sequentialQueues.end())
        {
            // Erasure shifts the next insertion-order queue into this slot.
            continue;
        }
        queueIndex = static_cast<size_t>(
            std::distance(m_sequentialQueues.begin(), current)) + 1u;
    }
}

} // namespace engine::script
