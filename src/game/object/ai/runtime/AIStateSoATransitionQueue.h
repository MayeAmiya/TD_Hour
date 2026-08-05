#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "core/container/container_types.h"
#include "game/object/ai/runtime/AIStateDescriptor.h"
#include "game/object/ai/runtime/AIStateFamilySoAStorage.h"

namespace engine::ai
{

enum class AIStateSoATransitionOperation : uint8_t
{
    Direct,
    CompleteSuccess,
    CompleteFailure,
    TemporaryExpired,
    BeginTemporary,
};

struct AIStateSoATransitionRequest final
{
    size_t slot = 0;
    ObjectId subject = INVALID_OBJECT_ID;
    AIStateId expectedState = AIStateId::Invalid;
    uint64_t expectedRevision = 0;
    AIStateSoATransitionOperation operation = AIStateSoATransitionOperation::Direct;
    AIStateId target = AIStateId::Invalid;
    uint64_t temporaryDurationTicks = 0;
    // Specialized entry feedback uses the caller's requested activation tick
    // as part of its correlation. The transition still commits at the next
    // shadow phase; this value exists to validate/save that exact identity.
    uint64_t correlationIssuedTick = 0;
    AIStateTransitionAuthority authority = AIStateTransitionAuthority::External;
    bool reenter = false;
    bool terminalPriority = false;
};

enum class AIStateSoATransitionResult : uint8_t
{
    Pending,
    Committed,
    SupersededByTerminal,
    ConflictingNormalRequests,
    ConflictingTerminalRequests,
    InvalidSlot,
    SubjectMismatch,
    Uninitialized,
    InvalidTarget,
    StaleState,
    StaleRevision,
    RejectedByLock,
    NoChange,
    TransitionBudgetExceeded,
};

enum class AIStateSoAPayloadCommitMode : uint8_t
{
    ActivateImmediately,
    DeferUntilAfterExit,
};

struct AIStateSoATransitionEntry final
{
    AIStateSoATransitionRequest request;
    AIStateSoATransitionResult result = AIStateSoATransitionResult::Pending;
    size_t insertionOrdinal = 0;
    AIStateTransition committedTransition;
    bool hasCommittedTransition = false;
    bool payloadActivated = false;
};

struct AIStateSoATransitionCommitReport final
{
    size_t requested = 0;
    size_t committed = 0;
    size_t superseded = 0;
    size_t conflicts = 0;
    size_t rejected = 0;
};

// A queue owns no memory. The caller must keep the entry span alive and call
// clear() before reusing it after commit(). Equal sort keys retain push order.
class AIStateSoATransitionQueue final
{
public:
    explicit AIStateSoATransitionQueue(container::Span<AIStateSoATransitionEntry> entries) noexcept
        : m_entries(entries)
    {
    }

    [[nodiscard]] bool push(const AIStateSoATransitionRequest& request) noexcept
    {
        if (m_sealed || m_count == m_entries.size())
            return false;
        AIStateSoATransitionEntry entry;
        entry.request = request;
        entry.insertionOrdinal = m_count;
        if (m_count != 0 && less(entry, m_entries[m_count - 1]))
            m_ordered = false;
        m_entries[m_count] = entry;
        ++m_count;
        return true;
    }

    void clear() noexcept
    {
        m_count = 0;
        m_sealed = false;
        m_ordered = true;
        m_report = {};
    }

    [[nodiscard]] size_t size() const noexcept
    {
        return m_count;
    }
    [[nodiscard]] size_t capacity() const noexcept
    {
        return m_entries.size();
    }
    [[nodiscard]] bool empty() const noexcept
    {
        return m_count == 0;
    }
    [[nodiscard]] bool sealed() const noexcept
    {
        return m_sealed;
    }

    [[nodiscard]] container::Span<AIStateSoATransitionEntry> entries() noexcept
    {
        return m_entries.first(m_count);
    }
    [[nodiscard]] container::Span<const AIStateSoATransitionEntry> entries() const noexcept
    {
        return m_entries.first(m_count);
    }

    [[nodiscard]] AIStateSoATransitionCommitReport commit(
        AIStateFamilySoAStorage& storage,
        uint64_t confirmedTick,
        AIStateSoAPayloadCommitMode payloadMode = AIStateSoAPayloadCommitMode::ActivateImmediately) noexcept
    {
        if (m_sealed)
            return m_report;
        m_sealed = true;
        deterministicSort();

        const container::Span<const ObjectId> subjects = storage.subjects();
        container::Span<AIStateMachineRuntime> runtimes = storage.runtimes();
        for (AIStateSoATransitionEntry& entry : entries())
        {
            const AIStateSoATransitionRequest& request = entry.request;
            if (request.slot >= runtimes.size())
            {
                entry.result = AIStateSoATransitionResult::InvalidSlot;
                continue;
            }
            if (request.subject != subjects[request.slot])
            {
                entry.result = AIStateSoATransitionResult::SubjectMismatch;
                continue;
            }

            const AIStateMachineRuntime& runtime = runtimes[request.slot];
            if (!runtime.initialized)
                entry.result = AIStateSoATransitionResult::Uninitialized;
            else if ((request.operation == AIStateSoATransitionOperation::Direct ||
                      request.operation == AIStateSoATransitionOperation::BeginTemporary) &&
                     !isValidState(request.target))
                entry.result = AIStateSoATransitionResult::InvalidTarget;
            else if ((request.operation == AIStateSoATransitionOperation::CompleteSuccess ||
                      request.operation == AIStateSoATransitionOperation::CompleteFailure) &&
                     descriptorFor(request.expectedState) == nullptr)
                entry.result = AIStateSoATransitionResult::InvalidTarget;
            else if (runtime.currentState != request.expectedState)
                entry.result = AIStateSoATransitionResult::StaleState;
            else if (runtime.revision != request.expectedRevision)
                entry.result = AIStateSoATransitionResult::StaleRevision;
        }

        resolveConflicts();

        for (AIStateSoATransitionEntry& entry : entries())
        {
            if (entry.result != AIStateSoATransitionResult::Pending)
                continue;

            const AIStateSoATransitionRequest& request = entry.request;
            AIStateMachineRuntime& runtime = runtimes[request.slot];
            if (!AIStateMachine::canTransition(runtime, request.authority))
            {
                entry.result = AIStateSoATransitionResult::RejectedByLock;
                continue;
            }
            if (request.operation == AIStateSoATransitionOperation::Direct && runtime.currentState == request.target &&
                !request.reenter)
            {
                entry.result = AIStateSoATransitionResult::NoChange;
                continue;
            }

            std::optional<AIStateTransition> transition;
            if (request.operation == AIStateSoATransitionOperation::Direct)
            {
                transition = AIStateMachine::setState(
                    runtime, request.target, confirmedTick, request.authority, request.reenter);
            }
            else if (request.operation == AIStateSoATransitionOperation::BeginTemporary)
            {
                transition = AIStateMachine::setTemporaryState(
                    runtime, request.target, confirmedTick, request.temporaryDurationTicks);
            }
            else if (request.operation == AIStateSoATransitionOperation::CompleteSuccess ||
                     request.operation == AIStateSoATransitionOperation::CompleteFailure)
            {
                const AIStateDescriptor* descriptor = descriptorFor(request.expectedState);
                transition = AIStateMachine::complete(
                    runtime,
                    request.operation == AIStateSoATransitionOperation::CompleteSuccess ? AIStateOutcome::Success
                                                                                        : AIStateOutcome::Failure,
                    descriptor->successState,
                    descriptor->failureState,
                    confirmedTick);
            }
            else
            {
                transition = AIStateMachine::expireTemporaryState(runtime, confirmedTick);
            }
            if (!transition)
            {
                entry.result = runtime.transitionLimitExceeded ? AIStateSoATransitionResult::TransitionBudgetExceeded
                                                               : AIStateSoATransitionResult::NoChange;
                continue;
            }
            entry.committedTransition = *transition;
            entry.hasCommittedTransition = true;
            if (payloadMode == AIStateSoAPayloadCommitMode::ActivateImmediately)
            {
                applyPostExitLockChanges(runtime, *transition);
                storage.activate(request.slot, transition->to, confirmedTick);
                applyTargetLock(runtime, transition->to);
                entry.payloadActivated = true;
            }
            entry.result = AIStateSoATransitionResult::Committed;
        }

        buildReport();
        return m_report;
    }

    // Called after old-state exit kernels when commit() used deferred payload
    // activation. Returns the number of newly activated target payloads.
    [[nodiscard]] size_t activateCommittedPayloads(AIStateFamilySoAStorage& storage) noexcept
    {
        if (!m_sealed)
            return 0;
        size_t activated = 0;
        for (AIStateSoATransitionEntry& entry : entries())
        {
            if (entry.result != AIStateSoATransitionResult::Committed || !entry.hasCommittedTransition ||
                entry.payloadActivated)
            {
                continue;
            }
            AIStateMachineRuntime& runtime = storage.runtimes()[entry.request.slot];
            applyPostExitLockChanges(runtime, entry.committedTransition);
            storage.activate(entry.request.slot, entry.committedTransition.to, entry.committedTransition.confirmedTick);
            applyTargetLock(runtime, entry.committedTransition.to);
            entry.payloadActivated = true;
            ++activated;
        }
        return activated;
    }

    // Recovers a deferred commit after its caller-owned queue has gone away
    // (for example when a bounded Move exit could not emit cleanup). The
    // runtime names the target while payloadStates() names the old state. A
    // same-state reenter additionally requires the caller's persistent
    // deferred marker, passed as reenter, because the two IDs are equal.
    [[nodiscard]] static bool activateDeferredSlot(AIStateFamilySoAStorage& storage,
                                                   size_t slot,
                                                   bool reenter = false) noexcept
    {
        if (slot >= storage.size())
            return false;
        AIStateMachineRuntime& runtime = storage.runtimes()[slot];
        const AIStateId from = storage.payloadStates()[slot];
        const AIStateId to = runtime.currentState;
        if (!runtime.initialized || !isValidState(from) || !isValidState(to) || (from == to && !reenter))
            return false;

        const AIStateTransition transition{
            .from = from,
            .to = to,
            .confirmedTick = runtime.enteredTick,
            .revision = runtime.revision,
        };
        applyPostExitLockChanges(runtime, transition);
        storage.activate(slot, to, runtime.enteredTick);
        applyTargetLock(runtime, to);
        return true;
    }

private:
    static void applyPostExitLockChanges(AIStateMachineRuntime& runtime, const AIStateTransition& transition) noexcept
    {
        if (transition.from == AIStateId::Dead && transition.to != AIStateId::Dead &&
            runtime.lock == AIStateMachineLock::Terminal)
        {
            AIStateMachine::setLock(runtime, AIStateMachineLock::Unlocked);
        }
    }

    static void applyTargetLock(AIStateMachineRuntime& runtime, AIStateId target) noexcept
    {
        const AIStateDescriptor* descriptor = descriptorFor(target);
        if (descriptor && descriptor->terminal)
            AIStateMachine::setLock(runtime, AIStateMachineLock::Terminal);
    }

    [[nodiscard]] static bool less(const AIStateSoATransitionEntry& left,
                                   const AIStateSoATransitionEntry& right) noexcept
    {
        if (left.request.subject != right.request.subject)
            return left.request.subject < right.request.subject;
        if (left.request.terminalPriority != right.request.terminalPriority)
            return left.request.terminalPriority;
        if (left.request.slot != right.request.slot)
            return left.request.slot < right.request.slot;
        return left.insertionOrdinal < right.insertionOrdinal;
    }

    void siftDown(size_t root, size_t count) noexcept
    {
        while (true)
        {
            const size_t left = root * 2 + 1;
            if (left >= count)
                return;
            size_t largest = left;
            const size_t right = left + 1;
            if (right < count && less(m_entries[largest], m_entries[right]))
                largest = right;
            if (!less(m_entries[root], m_entries[largest]))
                return;
            std::swap(m_entries[root], m_entries[largest]);
            root = largest;
        }
    }

    void deterministicSort() noexcept
    {
        if (m_ordered)
            return;
        // In-place heap sort keeps commit O(N log N), deterministic and free
        // of hidden scratch allocation. insertionOrdinal preserves push order
        // for otherwise equal keys.
        for (size_t start = m_count / 2; start != 0; --start)
            siftDown(start - 1, m_count);
        for (size_t end = m_count; end > 1; --end)
        {
            std::swap(m_entries[0], m_entries[end - 1]);
            siftDown(0, end - 1);
        }
        m_ordered = true;
    }

    void resolveConflicts() noexcept
    {
        size_t groupBegin = 0;
        while (groupBegin < m_count)
        {
            size_t groupEnd = groupBegin + 1;
            while (groupEnd < m_count && m_entries[groupEnd].request.subject == m_entries[groupBegin].request.subject)
            {
                ++groupEnd;
            }

            size_t normalCount = 0;
            size_t terminalCount = 0;
            for (size_t index = groupBegin; index < groupEnd; ++index)
            {
                const AIStateSoATransitionEntry& entry = m_entries[index];
                if (entry.result != AIStateSoATransitionResult::Pending)
                    continue;
                if (entry.request.terminalPriority)
                    ++terminalCount;
                else
                    ++normalCount;
            }

            for (size_t index = groupBegin; index < groupEnd; ++index)
            {
                AIStateSoATransitionEntry& entry = m_entries[index];
                if (entry.result != AIStateSoATransitionResult::Pending)
                    continue;
                if (entry.request.terminalPriority && terminalCount > 1)
                    entry.result = AIStateSoATransitionResult::ConflictingTerminalRequests;
                else if (!entry.request.terminalPriority && terminalCount != 0)
                    entry.result = AIStateSoATransitionResult::SupersededByTerminal;
                else if (!entry.request.terminalPriority && normalCount > 1)
                    entry.result = AIStateSoATransitionResult::ConflictingNormalRequests;
            }

            groupBegin = groupEnd;
        }
    }

    void buildReport() noexcept
    {
        m_report.requested = m_count;
        for (const AIStateSoATransitionEntry& entry : entries())
        {
            if (entry.result == AIStateSoATransitionResult::Committed)
                ++m_report.committed;
            else if (entry.result == AIStateSoATransitionResult::SupersededByTerminal)
                ++m_report.superseded;
            else
            {
                ++m_report.rejected;
                if (entry.result == AIStateSoATransitionResult::ConflictingNormalRequests ||
                    entry.result == AIStateSoATransitionResult::ConflictingTerminalRequests)
                {
                    ++m_report.conflicts;
                }
            }
        }
    }

    container::Span<AIStateSoATransitionEntry> m_entries;
    size_t m_count = 0;
    bool m_sealed = false;
    bool m_ordered = true;
    AIStateSoATransitionCommitReport m_report;
};

} // namespace engine::ai
