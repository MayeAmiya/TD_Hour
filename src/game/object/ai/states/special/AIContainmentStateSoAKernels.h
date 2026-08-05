#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

#include "core/container/container_types.h"
#include "game/object/ai/runtime/AIStateStep.h"

namespace engine::ai
{

// This staging protocol deliberately owns neither Object nor ContainModule
// pointers. The caller resolves world state, submits correlated feedback, and
// applies the bounded commands after a successful kernel transaction.
struct AIContainmentCorrelation final
{
    ObjectId subject = INVALID_OBJECT_ID;
    AIStateRequestId stateRequest{};
    AIStateId state = AIStateId::Invalid;

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return subject && stateRequest.isValid() &&
               (state == AIStateId::Enter || state == AIStateId::Exit || state == AIStateId::ExitInstantly);
    }
    constexpr auto operator<=>(const AIContainmentCorrelation&) const noexcept = default;
};

enum class AIContainmentDoor : uint8_t
{
    None,
    Door1,
    Door2,
    Door3,
    Door4,
};

enum class AIContainmentFeedbackKind : uint8_t
{
    None,
    GoalMissing,
    EnterReady,
    EnterDenied,
    EnterHeld,
    EnterMoving,
    EnterMovementSucceeded,
    EnterMovementFailed,
    EnterMovementUnsupported,
    ExitEntryReady,
    ExitWait,
    ExitBusy,
    ExitReady,
    ExitNoInterface,
    ExitNoDoor,
    ExitContainerDead,
    Unsupported,
    // Session accepted the typed ExitViaDoor transaction and the containment
    // edge was removed. Keeping this distinct from ExitReady prevents the AI
    // state from declaring success before the confirmed detach actually ran.
    ExitSucceeded,
};

struct AIContainmentFeedback final
{
    AIContainmentCorrelation correlation{};
    AIContainmentFeedbackKind kind = AIContainmentFeedbackKind::None;
    ObjectId goal = INVALID_OBJECT_ID;
    AIFixedPosition goalPosition{};
    AIContainmentDoor reservedDoor = AIContainmentDoor::None;
    bool goalHasContain = false;
    bool goalHasExitInterface = false;
    bool goalContainedByOther = false;
    bool goalAboveTerrain = false;
    bool subjectAboveTerrain = false;
    bool verticalOverlap = true;
    bool enemy = false;
    bool attackPossible = false;
    bool closeEnough = false;
};

struct AIContainmentFeedbackBuffer final
{
    static constexpr size_t Capacity = 4;
    container::Array<AIContainmentFeedback, Capacity> values{};
    size_t count = 0;
    bool overflowed = false;

    [[nodiscard]] bool push(const AIContainmentFeedback& feedback) noexcept
    {
        if (count >= values.size())
        {
            overflowed = true;
            return false;
        }
        values[count++] = feedback;
        return true;
    }

    void clear() noexcept
    {
        count = 0;
        overflowed = false;
    }
};

enum class AIContainmentCommandKind : uint8_t
{
    SetWantsToEnter,
    SetWantsToExit,
    SetWantsNeither,
    BeginEnterMovement,
    RefreshEnterMovementGoal,
    EndEnterMovement,
    AttackGoal,
    ForceIntoContain,
    ExitViaDoor,
};

struct AIContainmentCommand final
{
    AIContainmentCorrelation correlation{};
    AIContainmentCommandKind kind = AIContainmentCommandKind::SetWantsNeither;
    ObjectId goal = INVALID_OBJECT_ID;
    AIFixedPosition goalPosition{};
    AIContainmentDoor door = AIContainmentDoor::None;
    // Begin/EndEnterMovement map the inherited move state's obstacle and
    // locomotor policy changes across this pointer-free boundary.
    bool ignoreGoalObstacle = false;
    bool allowInvalidPosition = false;
    bool adjustDestination = false;
    uint64_t confirmedTick = 0;
};

struct AIContainmentCommandBuffer final
{
    static constexpr size_t Capacity = 4;
    container::Array<AIContainmentCommand, Capacity> values{};
    size_t count = 0;
    bool overflowed = false;

    [[nodiscard]] constexpr bool hasCapacity(size_t additional) const noexcept
    {
        return count <= values.size() && additional <= values.size() - count;
    }

    [[nodiscard]] bool push(const AIContainmentCommand& command) noexcept
    {
        if (!hasCapacity(1))
        {
            overflowed = true;
            return false;
        }
        values[count++] = command;
        return true;
    }

    void clear() noexcept
    {
        count = 0;
        overflowed = false;
    }
};

// The arrays behind this view remain caller-owned. They are the minimal
// per-activation payload that a future shared containment-family storage must
// expose; this slice does not modify shared storage.
struct AIContainmentStateSoAColumns final
{
    container::Span<uint64_t> requestTick;
    container::Span<uint32_t> requestSequence;
    container::Span<ObjectId> trackedGoal;
    container::Span<ObjectId> entryToClear;
    container::Span<AIContainmentPhase> phase;
};

struct AIContainmentStateSoAKernelInput final
{
    uint64_t confirmedTick = 0;
    container::Span<const uint8_t> scheduled{};
    AIExecutionSlotRange executionSlots{};
    container::Span<const AIStateId> activeStates;
    container::Span<const ObjectId> subjects;
    container::Span<const ObjectId> goalObjects;
    container::Span<const AIContainmentFeedbackBuffer> feedback;
    container::Span<AIContainmentCommandBuffer> commands;
    container::Span<AIStateStepResult> results;
};

namespace containment_detail
{

[[nodiscard]] inline bool hasAlignedSpans(const AIContainmentStateSoAColumns& columns,
                                          const AIContainmentStateSoAKernelInput& input) noexcept
{
    const size_t count = input.activeStates.size();
    return (input.scheduled.empty() || input.scheduled.size() == count) && input.subjects.size() == count &&
           input.goalObjects.size() == count && input.feedback.size() == count &&
           input.commands.size() == count && input.results.size() == count && columns.trackedGoal.size() == count &&
           columns.entryToClear.size() == count && columns.phase.size() == count &&
           columns.requestTick.size() == count && columns.requestSequence.size() == count;
}

[[nodiscard]] constexpr bool scheduled(const AIContainmentStateSoAKernelInput& input, size_t slot) noexcept
{
    return input.scheduled.empty() || input.scheduled[slot] != 0;
}

[[nodiscard]] constexpr AIContainmentCorrelation correlation(const AIContainmentStateSoAColumns& columns,
                                                             const AIContainmentStateSoAKernelInput& input,
                                                             size_t slot,
                                                             AIStateId state) noexcept
{
    return {.subject = input.subjects[slot],
            .stateRequest = {columns.requestTick[slot], columns.requestSequence[slot]},
            .state = state};
}

[[nodiscard]] inline const AIContainmentFeedback* relevantFeedback(const AIContainmentFeedbackBuffer& buffer,
                                                                   const AIContainmentCorrelation& expected,
                                                                   ObjectId expectedGoal) noexcept
{
    const size_t count = buffer.count < buffer.values.size() ? buffer.count : buffer.values.size();
    for (size_t index = 0; index < count; ++index)
    {
        const AIContainmentFeedback& candidate = buffer.values[index];
        if (candidate.correlation == expected && candidate.goal == expectedGoal)
            return &candidate;
    }
    return nullptr;
}

[[nodiscard]] inline bool canAppend(const AIContainmentStateSoAKernelInput& input, size_t slot, size_t count) noexcept
{
    return input.commands[slot].hasCapacity(count);
}

inline void emit(const AIContainmentStateSoAKernelInput& input,
                 size_t slot,
                 const AIContainmentCorrelation& correlationValue,
                 AIContainmentCommandKind kind,
                 ObjectId goal,
                 AIFixedPosition goalPosition = {},
                 AIContainmentDoor door = AIContainmentDoor::None) noexcept
{
    AIContainmentCommand command{
        .correlation = correlationValue,
        .kind = kind,
        .goal = goal,
        .goalPosition = goalPosition,
        .door = door,
        .confirmedTick = input.confirmedTick,
    };
    if (kind == AIContainmentCommandKind::BeginEnterMovement)
    {
        command.ignoreGoalObstacle = true;
        command.allowInvalidPosition = true;
        command.adjustDestination = false;
    }
    // Capacity was checked for the complete batch before any push.
    static_cast<void>(input.commands[slot].push(command));
}

[[nodiscard]] constexpr bool isEnterUpdateFeedback(AIContainmentFeedbackKind kind) noexcept
{
    return kind == AIContainmentFeedbackKind::EnterDenied || kind == AIContainmentFeedbackKind::EnterHeld ||
           kind == AIContainmentFeedbackKind::EnterMoving ||
           kind == AIContainmentFeedbackKind::EnterMovementSucceeded ||
           kind == AIContainmentFeedbackKind::EnterMovementFailed ||
           kind == AIContainmentFeedbackKind::EnterMovementUnsupported;
}

[[nodiscard]] constexpr size_t enterUpdateCommandCount(const AIContainmentFeedback& feedback) noexcept
{
    if (feedback.goalContainedByOther && feedback.goalAboveTerrain && !feedback.subjectAboveTerrain)
        return 0;
    if (feedback.kind == AIContainmentFeedbackKind::EnterDenied)
        return feedback.enemy && feedback.attackPossible ? 2 : 1;
    if (feedback.kind == AIContainmentFeedbackKind::EnterMovementSucceeded && feedback.verticalOverlap &&
        feedback.closeEnough && feedback.goalHasContain)
        return 2;
    return isEnterUpdateFeedback(feedback.kind) ? 1 : 0;
}

} // namespace containment_detail

[[nodiscard]] inline bool enterContainmentEnterStateSoA(AIContainmentStateSoAColumns columns,
                                                        const AIContainmentStateSoAKernelInput& input) noexcept
{
    using namespace containment_detail;
    if (!hasAlignedSpans(columns, input))
        return false;

    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::Enter)
            continue;
        const AIContainmentCorrelation expected = correlation(columns, input, slot, AIStateId::Enter);
        if (!expected.isValid() || !input.goalObjects[slot])
            continue;
        const AIContainmentFeedback* event = relevantFeedback(input.feedback[slot], expected, input.goalObjects[slot]);
        if (event && event->kind == AIContainmentFeedbackKind::EnterReady)
        {
            const size_t required = 1 + static_cast<size_t>(event->goalHasContain);
            if (!canAppend(input, slot, required))
                return false;
        }
    }

    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::Enter)
            continue;
        const AIContainmentCorrelation expected = correlation(columns, input, slot, AIStateId::Enter);
        if (!expected.isValid() || !input.goalObjects[slot])
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        const AIContainmentFeedback* event = relevantFeedback(input.feedback[slot], expected, input.goalObjects[slot]);
        if (!event)
        {
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }
        if (event->kind == AIContainmentFeedbackKind::GoalMissing ||
            event->kind == AIContainmentFeedbackKind::EnterDenied)
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        if (event->kind != AIContainmentFeedbackKind::EnterReady)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        columns.trackedGoal[slot] = event->goal;
        columns.entryToClear[slot] = event->goalHasContain ? event->goal : INVALID_OBJECT_ID;
        columns.phase[slot] = AIContainmentPhase::EnterActive;
        if (event->goalHasContain)
            emit(input, slot, expected, AIContainmentCommandKind::SetWantsToEnter, event->goal);
        emit(input, slot, expected, AIContainmentCommandKind::BeginEnterMovement, event->goal, event->goalPosition);
        input.results[slot] = AIStateStepResult::continueState();
    }
    return true;
}

[[nodiscard]] inline bool updateContainmentEnterStateSoA(AIContainmentStateSoAColumns columns,
                                                         const AIContainmentStateSoAKernelInput& input) noexcept
{
    using namespace containment_detail;
    if (!hasAlignedSpans(columns, input))
        return false;

    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::Enter ||
            columns.phase[slot] != AIContainmentPhase::EnterActive)
            continue;
        const auto expected = correlation(columns, input, slot, AIStateId::Enter);
        if (!expected.isValid() || !columns.trackedGoal[slot])
            continue;
        const AIContainmentFeedback* event =
            relevantFeedback(input.feedback[slot], expected, columns.trackedGoal[slot]);
        if (event && !canAppend(input, slot, enterUpdateCommandCount(*event)))
            return false;
    }

    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::Enter)
            continue;
        const auto expected = correlation(columns, input, slot, AIStateId::Enter);
        if (!expected.isValid() || !columns.trackedGoal[slot] || columns.phase[slot] != AIContainmentPhase::EnterActive)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        const AIContainmentFeedback* event =
            relevantFeedback(input.feedback[slot], expected, columns.trackedGoal[slot]);
        if (!event)
        {
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }
        if (event->kind == AIContainmentFeedbackKind::GoalMissing)
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        if (!isEnterUpdateFeedback(event->kind))
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        if (event->goalContainedByOther && event->goalAboveTerrain && !event->subjectAboveTerrain)
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }

        emit(input,
             slot,
             expected,
             AIContainmentCommandKind::RefreshEnterMovementGoal,
             event->goal,
             event->goalPosition);
        if (event->kind == AIContainmentFeedbackKind::EnterDenied)
        {
            if (event->enemy && event->attackPossible)
            {
                emit(input, slot, expected, AIContainmentCommandKind::AttackGoal, event->goal);
                input.results[slot] = AIStateStepResult::continueState();
            }
            else
            {
                input.results[slot] = AIStateStepResult::failure();
            }
            continue;
        }
        if (event->kind == AIContainmentFeedbackKind::EnterHeld)
        {
            input.results[slot] = AIStateStepResult::success();
            continue;
        }
        if (event->kind == AIContainmentFeedbackKind::EnterMoving)
        {
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }
        if (event->kind == AIContainmentFeedbackKind::EnterMovementFailed)
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        if (event->kind == AIContainmentFeedbackKind::EnterMovementUnsupported)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        if (!event->verticalOverlap)
        {
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }
        if (event->closeEnough && event->goalHasContain)
            emit(input, slot, expected, AIContainmentCommandKind::ForceIntoContain, event->goal);
        input.results[slot] = AIStateStepResult::success();
    }
    return true;
}

[[nodiscard]] inline bool enterContainmentExitStateSoA(AIContainmentStateSoAColumns columns,
                                                       const AIContainmentStateSoAKernelInput& input) noexcept
{
    using namespace containment_detail;
    if (!hasAlignedSpans(columns, input))
        return false;

    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::Exit)
            continue;
        const auto expected = correlation(columns, input, slot, AIStateId::Exit);
        if (!expected.isValid() || !input.goalObjects[slot])
            continue;
        const auto* event = relevantFeedback(input.feedback[slot], expected, input.goalObjects[slot]);
        if (event && event->kind == AIContainmentFeedbackKind::ExitEntryReady &&
            !canAppend(input, slot, static_cast<size_t>(event->goalHasContain)))
            return false;
    }

    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::Exit)
            continue;
        const auto expected = correlation(columns, input, slot, AIStateId::Exit);
        if (!expected.isValid() || !input.goalObjects[slot])
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        const auto* event = relevantFeedback(input.feedback[slot], expected, input.goalObjects[slot]);
        if (!event)
        {
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }
        if (event->kind == AIContainmentFeedbackKind::GoalMissing)
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        if (event->kind != AIContainmentFeedbackKind::ExitEntryReady)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        columns.trackedGoal[slot] = event->goal;
        columns.entryToClear[slot] = event->goalHasContain ? event->goal : INVALID_OBJECT_ID;
        columns.phase[slot] = AIContainmentPhase::ExitActive;
        if (event->goalHasContain)
            emit(input, slot, expected, AIContainmentCommandKind::SetWantsToExit, event->goal);
        input.results[slot] = AIStateStepResult::continueState();
    }
    return true;
}

[[nodiscard]] inline bool updateContainmentExitStateSoA(AIContainmentStateSoAColumns columns,
                                                        const AIContainmentStateSoAKernelInput& input) noexcept
{
    using namespace containment_detail;
    if (!hasAlignedSpans(columns, input))
        return false;

    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::Exit ||
            columns.phase[slot] != AIContainmentPhase::ExitActive)
            continue;
        const auto expected = correlation(columns, input, slot, AIStateId::Exit);
        if (!expected.isValid() || !columns.trackedGoal[slot])
            continue;
        const auto* event = relevantFeedback(input.feedback[slot], expected, columns.trackedGoal[slot]);
        if (event && event->kind == AIContainmentFeedbackKind::ExitReady &&
            event->reservedDoor != AIContainmentDoor::None && !canAppend(input, slot, 1))
            return false;
    }

    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::Exit)
            continue;
        const auto expected = correlation(columns, input, slot, AIStateId::Exit);
        if (!expected.isValid() || !columns.trackedGoal[slot] ||
            (columns.phase[slot] != AIContainmentPhase::ExitActive &&
             columns.phase[slot] != AIContainmentPhase::ExitCommandIssued))
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        // ExitViaDoor is confirmed by the Session transaction drain. Do not
        // declare success merely because the command was emitted: a closed
        // dock, a destroyed host, or an exit admission failure must leave the
        // rider in a retryable/failing state rather than stranded inside the
        // container with its AI state silently cleared.
        if (columns.phase[slot] == AIContainmentPhase::ExitCommandIssued)
        {
            const auto* event = relevantFeedback(
                input.feedback[slot], expected, columns.trackedGoal[slot]);
            if (!event) {
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
            switch (event->kind)
            {
            case AIContainmentFeedbackKind::ExitSucceeded:
                input.results[slot] = AIStateStepResult::success();
                break;
            case AIContainmentFeedbackKind::ExitWait:
            case AIContainmentFeedbackKind::ExitBusy:
                // OpenContain starts AI_EXIT for every passenger. More than
                // one rider can therefore observe a free door in the same
                // update, while TransportContain::ExitDelay accepts only the
                // first confirmed detach. Losing that deterministic race is
                // not an exit failure: return to the polling phase and wait
                // until the shared door becomes available, just as the
                // original AIExit state does.
                columns.phase[slot] = AIContainmentPhase::ExitActive;
                input.results[slot] = AIStateStepResult::continueState();
                break;
            case AIContainmentFeedbackKind::GoalMissing:
            case AIContainmentFeedbackKind::ExitNoInterface:
            case AIContainmentFeedbackKind::ExitNoDoor:
            case AIContainmentFeedbackKind::ExitContainerDead:
                input.results[slot] = AIStateStepResult::failure();
                break;
            default:
                input.results[slot] = AIStateStepResult::unsupported();
                break;
            }
            continue;
        }
        const auto* event = relevantFeedback(input.feedback[slot], expected, columns.trackedGoal[slot]);
        if (!event)
        {
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }
        switch (event->kind)
        {
        case AIContainmentFeedbackKind::GoalMissing:
        case AIContainmentFeedbackKind::ExitNoInterface:
        case AIContainmentFeedbackKind::ExitNoDoor:
        case AIContainmentFeedbackKind::ExitContainerDead:
            input.results[slot] = AIStateStepResult::failure();
            break;
        case AIContainmentFeedbackKind::ExitWait:
        case AIContainmentFeedbackKind::ExitBusy:
            input.results[slot] = AIStateStepResult::continueState();
            break;
        case AIContainmentFeedbackKind::ExitReady:
            if (event->reservedDoor == AIContainmentDoor::None)
            {
                input.results[slot] = AIStateStepResult::unsupported();
                break;
            }
            emit(input, slot, expected, AIContainmentCommandKind::ExitViaDoor, event->goal, {}, event->reservedDoor);
            columns.phase[slot] = AIContainmentPhase::ExitCommandIssued;
            input.results[slot] = AIStateStepResult::continueState();
            break;
        case AIContainmentFeedbackKind::Unsupported:
            input.results[slot] = AIStateStepResult::unsupported();
            break;
        default:
            input.results[slot] = AIStateStepResult::unsupported();
            break;
        }
    }
    return true;
}

[[nodiscard]] inline bool enterContainmentExitInstantlyStateSoA(AIContainmentStateSoAColumns columns,
                                                                const AIContainmentStateSoAKernelInput& input) noexcept
{
    using namespace containment_detail;
    if (!hasAlignedSpans(columns, input))
        return false;

    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::ExitInstantly)
            continue;
        const auto expected = correlation(columns, input, slot, AIStateId::ExitInstantly);
        if (!expected.isValid() || !input.goalObjects[slot])
            continue;
        const auto* event = relevantFeedback(input.feedback[slot], expected, input.goalObjects[slot]);
        if (event && event->kind == AIContainmentFeedbackKind::ExitEntryReady)
        {
            const size_t required =
                static_cast<size_t>(event->goalHasContain) + static_cast<size_t>(event->goalHasExitInterface);
            if (!canAppend(input, slot, required))
                return false;
        }
    }

    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::ExitInstantly)
            continue;
        const auto expected = correlation(columns, input, slot, AIStateId::ExitInstantly);
        if (!expected.isValid() || !input.goalObjects[slot])
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        const auto* event = relevantFeedback(input.feedback[slot], expected, input.goalObjects[slot]);
        if (!event)
        {
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }
        if (event->kind == AIContainmentFeedbackKind::GoalMissing)
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        if (event->kind != AIContainmentFeedbackKind::ExitEntryReady)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        columns.trackedGoal[slot] = event->goal;
        columns.entryToClear[slot] = event->goalHasContain ? event->goal : INVALID_OBJECT_ID;
        columns.phase[slot] = AIContainmentPhase::ExitActive;
        if (event->goalHasContain)
            emit(input, slot, expected, AIContainmentCommandKind::SetWantsToExit, event->goal);
        if (!event->goalHasExitInterface)
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        // GeneralsMD intentionally bypasses reservation and always chooses DOOR_1 here.
        emit(input, slot, expected, AIContainmentCommandKind::ExitViaDoor, event->goal, {}, AIContainmentDoor::Door1);
        columns.phase[slot] = AIContainmentPhase::ExitInstantlyCommandIssued;
        input.results[slot] = AIStateStepResult::continueState();
    }
    return true;
}

[[nodiscard]] inline bool updateContainmentExitInstantlyStateSoA(AIContainmentStateSoAColumns columns,
                                                                 const AIContainmentStateSoAKernelInput& input) noexcept
{
    using namespace containment_detail;
    if (!hasAlignedSpans(columns, input))
        return false;
    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::ExitInstantly)
            continue;
        const auto expected = correlation(columns, input, slot, AIStateId::ExitInstantly);
        input.results[slot] =
            expected.isValid() && columns.phase[slot] == AIContainmentPhase::ExitInstantlyCommandIssued
                ? AIStateStepResult::success()
                : AIStateStepResult::unsupported();
    }
    return true;
}

[[nodiscard]] inline bool canExitContainmentStateSoA(const AIContainmentStateSoAColumns& columns,
                                                     const AIContainmentStateSoAKernelInput& input,
                                                     AIStateId state) noexcept
{
    using namespace containment_detail;
    if (!hasAlignedSpans(columns, input) ||
        (state != AIStateId::Enter && state != AIStateId::Exit && state != AIStateId::ExitInstantly))
        return false;
    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != state ||
            columns.phase[slot] == AIContainmentPhase::Inactive)
            continue;
        if (!correlation(columns, input, slot, state).isValid())
            return false;
        size_t required = static_cast<size_t>(columns.entryToClear[slot].isValid());
        if (state == AIStateId::Enter && columns.phase[slot] == AIContainmentPhase::EnterActive)
            ++required;
        if (!canAppend(input, slot, required))
            return false;
    }
    return true;
}

[[nodiscard]] inline bool exitContainmentStateSoA(AIContainmentStateSoAColumns columns,
                                                  const AIContainmentStateSoAKernelInput& input,
                                                  AIStateId state) noexcept
{
    using namespace containment_detail;
    if (!canExitContainmentStateSoA(columns, input, state))
        return false;
    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != state ||
            columns.phase[slot] == AIContainmentPhase::Inactive)
            continue;
        const auto expected = correlation(columns, input, slot, state);
        if (state == AIStateId::Enter && columns.phase[slot] == AIContainmentPhase::EnterActive)
            emit(input, slot, expected, AIContainmentCommandKind::EndEnterMovement, columns.trackedGoal[slot]);
        if (columns.entryToClear[slot])
            emit(input, slot, expected, AIContainmentCommandKind::SetWantsNeither, columns.entryToClear[slot]);
        columns.trackedGoal[slot] = INVALID_OBJECT_ID;
        columns.entryToClear[slot] = INVALID_OBJECT_ID;
        columns.phase[slot] = AIContainmentPhase::Inactive;
    }
    return true;
}

} // namespace engine::ai
