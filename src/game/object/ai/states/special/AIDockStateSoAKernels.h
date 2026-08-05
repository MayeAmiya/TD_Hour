#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "core/container/container_types.h"
#include "game/object/ai/runtime/AIStateStep.h"
#include "game/object/ai/states/special/AIDockStateData.h"

namespace engine::ai
{

// GetRepaired is a generic-unit alias for Dock.  The purpose survives the
// alias so the world-side adapter can apply repair-specific eligibility and
// action semantics without forking this state machine.

// These are the eight states, in legacy transition order, from AIDock.cpp.

// Approach and AdvancePosition are two state-machine phases but one movement
// role.  The phase revision below prevents old Approach feedback from being
// accepted by a later AdvancePosition request.

// Stable for the complete docking activation.  In particular, phase changes
// never change this token; state-request replacement and slot reuse do.

// Every request/feedback exchange has a unique correlation while retaining
// the stable activation token.  This rejects feedback from an earlier phase,
// an earlier queue advance, or an earlier poll in the same phase.
struct AIDockCorrelation final
{
    AIDockToken token{};
    AIDockPhase phase = AIDockPhase::Inactive;
    AIDockMoveStage moveStage = AIDockMoveStage::None;
    uint32_t phaseRevision = 0;
    uint32_t exchangeSequence = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return token.isValid() && phase != AIDockPhase::Inactive && phase != AIDockPhase::Completed &&
               phaseRevision != 0 && exchangeSequence != 0;
    }
    constexpr auto operator<=>(const AIDockCorrelation&) const noexcept = default;
};


struct AIDockRequest final
{
    AIDockCorrelation correlation{};
    AIDockRequestKind kind = AIDockRequestKind::None;
    AIFixedPosition position{};
    int32_t approachPosition = -1;
    uint64_t confirmedTick = 0;
    bool ignoreDockObstacle = false;
    bool allowPathThroughUnits = false;
    bool adjustDestination = true;
};

struct AIDockRequestBuffer final
{
    static constexpr size_t Capacity = 8;
    container::Array<AIDockRequest, Capacity> values{};
    size_t count = 0;
    bool overflowed = false;

    [[nodiscard]] constexpr bool hasCapacity(size_t additional) const noexcept
    {
        return !overflowed && count <= values.size() && additional <= values.size() - count;
    }

    [[nodiscard]] bool push(const AIDockRequest& request) noexcept
    {
        if (!hasCapacity(1))
        {
            overflowed = true;
            return false;
        }
        values[count++] = request;
        return true;
    }

    void clear() noexcept
    {
        count = 0;
        overflowed = false;
    }
};

enum class AIDockFeedbackStatus : uint8_t
{
    None,
    Pending,
    Accepted,
    Denied,
    DockMissing,
    DockClosed,
    ClearanceWaiting,
    ClearToAdvance,
    ClearToEnter,
    MovementMoving,
    MovementSucceeded,
    MovementFailed,
    ActionContinue,
    ActionComplete,
    NoRally,
    Unsupported,
};

struct AIDockFeedback final
{
    AIDockCorrelation correlation{};
    AIDockRequestKind request = AIDockRequestKind::None;
    AIDockFeedbackStatus status = AIDockFeedbackStatus::None;
    AIFixedPosition position{};
    int32_t approachPosition = -1;
    uint32_t actionDelayTicks = 0;
    ObjectId drone = INVALID_OBJECT_ID;
    bool allowPassthrough = false;
};

struct AIDockFeedbackBuffer final
{
    static constexpr size_t Capacity = 8;
    container::Array<AIDockFeedback, Capacity> values{};
    size_t count = 0;
    bool overflowed = false;

    [[nodiscard]] bool push(const AIDockFeedback& feedback) noexcept
    {
        if (overflowed || count >= values.size())
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

// Deliberately field-level columns: no retained Object/DockUpdate pointers and
// no per-slot aggregate payload.  A future shared family storage can map each
// span to a dense ECS column without changing this protocol.
struct AIDockStateSoAKernelInput final
{
    uint64_t confirmedTick = 0;
    uint32_t ticksPerSecond = 30;
    container::Span<const uint8_t> scheduled{};
    AIExecutionSlotRange executionSlots{};
    container::Span<const AIStateId> activeStates;
    container::Span<const ObjectId> subjects;
    container::Span<const ObjectId> goalObjects;
    container::Span<const AIDockFeedbackBuffer> feedback;
    container::Span<AIDockRequestBuffer> requests;
    container::Span<AIStateStepResult> results;
};

namespace dock_detail
{

[[nodiscard]] constexpr bool isDockAlias(AIStateId state) noexcept
{
    return state == AIStateId::Dock || state == AIStateId::GetRepaired;
}

[[nodiscard]] constexpr AIDockPurpose purposeFor(AIStateId state) noexcept
{
    return state == AIStateId::GetRepaired ? AIDockPurpose::Repair : AIDockPurpose::Dock;
}

[[nodiscard]] constexpr AIDockMoveStage moveStageFor(AIDockPhase phase) noexcept
{
    switch (phase)
    {
    case AIDockPhase::Approach:
    case AIDockPhase::AdvancePosition:
        return AIDockMoveStage::Queue;
    case AIDockPhase::MoveToEntry:
        return AIDockMoveStage::Entry;
    case AIDockPhase::MoveToDock:
        return AIDockMoveStage::Dock;
    case AIDockPhase::MoveToExit:
        return AIDockMoveStage::Exit;
    case AIDockPhase::MoveToRally:
        return AIDockMoveStage::Rally;
    default:
        return AIDockMoveStage::None;
    }
}

[[nodiscard]] inline bool hasAlignedInputSpans(size_t count,const AIDockStateSoAKernelInput& input) noexcept
{
    const bool aligned =
        (input.scheduled.empty() || input.scheduled.size() == count) && input.subjects.size() == count &&
        input.activeStates.size()==count&&input.goalObjects.size() == count && input.feedback.size() == count &&
        input.requests.size() == count && input.results.size() == count && input.ticksPerSecond != 0;
    if (!aligned)return false;
    // Fixed-capacity production pages may contain inactive holes and their
    // physical slot order is not the ObjectId execution order after reuse.
    // Validate only scheduled subjects here; ObjectAIRuntime owns global
    // uniqueness and stable ordering. Keep adjacent duplicate rejection for
    // standalone kernel callers without imposing a sort on hot SoA slots.
    ObjectId previousScheduled = INVALID_OBJECT_ID;
    for(size_t slot=0;slot<count;++slot)
    {
        if(!input.scheduled.empty() && input.scheduled[slot]==0)continue;
        if(!input.subjects[slot] || input.subjects[slot]==previousScheduled)return false;
        previousScheduled=input.subjects[slot];
    }
    return true;
}

[[nodiscard]] inline bool hasAlignedSpans(const AIDockStateSoAColumns& columns,
                                          const AIDockStateSoAKernelInput& input) noexcept
{
    const size_t count = input.activeStates.size();
    const bool aligned = hasAlignedInputSpans(count,input)&&columns.tokenSubjects.size() == count &&
        columns.tokenDocks.size() == count && columns.tokenIssuedTicks.size() == count &&
        columns.tokenRequestSequences.size() == count && columns.purposes.size() == count &&
        columns.phases.size() == count && columns.phaseRevisions.size() == count &&
        columns.exchangeSequences.size() == count && columns.pendingRequests.size() == count &&
        columns.approachPositions.size() == count && columns.clearanceEnterTicks.size() == count &&
        columns.nextActionTicks.size() == count && columns.actionDelayTicks.size() == count &&
        columns.drones.size() == count && columns.movementActive.size() == count;
    return aligned;
}

[[nodiscard]] constexpr bool scheduled(const AIDockStateSoAKernelInput& input, size_t slot) noexcept
{
    return input.scheduled.empty() || input.scheduled[slot] != 0;
}

[[nodiscard]] constexpr uint32_t nextSequence(uint32_t value) noexcept
{
    return value == std::numeric_limits<uint32_t>::max() ? 1U : value + 1U;
}

[[nodiscard]] constexpr uint64_t saturatedAdd(uint64_t lhs, uint64_t rhs) noexcept
{
    return rhs > std::numeric_limits<uint64_t>::max() - lhs ? std::numeric_limits<uint64_t>::max() : lhs + rhs;
}

[[nodiscard]] constexpr uint64_t clearanceDeadline(uint64_t entered, uint32_t ticksPerSecond) noexcept
{
    constexpr uint64_t Seconds = 30;
    const uint64_t ticks = Seconds * static_cast<uint64_t>(ticksPerSecond);
    return saturatedAdd(entered, ticks);
}

[[nodiscard]] constexpr AIDockToken token(const AIDockStateSoAColumns& columns, size_t slot) noexcept
{
    return {
        .subject = columns.tokenSubjects[slot],
        .dock = columns.tokenDocks[slot],
        .stateRequest = {.issuedTick = columns.tokenIssuedTicks[slot], .sequence = columns.tokenRequestSequences[slot]},
        .purpose = columns.purposes[slot],
    };
}

[[nodiscard]] constexpr AIDockCorrelation pendingCorrelation(const AIDockStateSoAColumns& columns, size_t slot) noexcept
{
    return {
        .token = token(columns, slot),
        .phase = columns.phases[slot],
        .moveStage = columns.pendingRequests[slot] == AIDockRequestKind::BeginMove ? moveStageFor(columns.phases[slot])
                                                                                   : AIDockMoveStage::None,
        .phaseRevision = columns.phaseRevisions[slot],
        .exchangeSequence = columns.exchangeSequences[slot],
    };
}

[[nodiscard]] inline const AIDockFeedback* relevantFeedback(const AIDockFeedbackBuffer& buffer,
                                                            const AIDockStateSoAColumns& columns,
                                                            size_t slot) noexcept
{
    if (columns.pendingRequests[slot] == AIDockRequestKind::None)
        return nullptr;
    const AIDockCorrelation expected = pendingCorrelation(columns, slot);
    for (size_t index = 0; index < buffer.count; ++index)
    {
        const AIDockFeedback& candidate = buffer.values[index];
        if (candidate.request == columns.pendingRequests[slot] && candidate.correlation == expected)
            return &candidate;
    }
    return nullptr;
}

[[nodiscard]] constexpr bool activationMatches(const AIDockStateSoAColumns& columns,
                                               const AIDockStateSoAKernelInput& input,
                                               size_t slot) noexcept
{
    if (!isDockAlias(input.activeStates[slot]))
        return false;
    const AIDockToken active = token(columns, slot);
    return active.subject == input.subjects[slot] && active.dock == input.goalObjects[slot] &&
           active.stateRequest.isValid() && active.purpose == purposeFor(input.activeStates[slot]);
}

inline void setPhase(AIDockStateSoAColumns& columns, size_t slot, AIDockPhase phase) noexcept
{
    columns.phases[slot] = phase;
    columns.phaseRevisions[slot] = nextSequence(columns.phaseRevisions[slot]);
    columns.pendingRequests[slot] = AIDockRequestKind::None;
    columns.movementActive[slot] = 0;
}

[[nodiscard]] inline AIDockCorrelation nextCorrelation(AIDockStateSoAColumns& columns,
                                                       size_t slot,
                                                       AIDockRequestKind kind) noexcept
{
    columns.exchangeSequences[slot] = nextSequence(columns.exchangeSequences[slot]);
    return {
        .token = token(columns, slot),
        .phase = columns.phases[slot],
        .moveStage = kind == AIDockRequestKind::BeginMove ? moveStageFor(columns.phases[slot]) : AIDockMoveStage::None,
        .phaseRevision = columns.phaseRevisions[slot],
        .exchangeSequence = columns.exchangeSequences[slot],
    };
}

inline void emit(AIDockStateSoAColumns& columns,
                 const AIDockStateSoAKernelInput& input,
                 size_t slot,
                 AIDockRequestKind kind,
                 bool awaitsFeedback,
                 AIFixedPosition position = {},
                 bool allowPassthrough = false) noexcept
{
    const AIDockCorrelation correlation = nextCorrelation(columns, slot, kind);
    AIDockRequest request{
        .correlation = correlation,
        .kind = kind,
        .position = position,
        .approachPosition = columns.approachPositions[slot],
        .confirmedTick = input.confirmedTick,
    };
    if (kind == AIDockRequestKind::BeginMove)
    {
        const AIDockMoveStage stage = correlation.moveStage;
        request.ignoreDockObstacle =
            allowPassthrough &&
            (stage == AIDockMoveStage::Entry || stage == AIDockMoveStage::Dock || stage == AIDockMoveStage::Exit);
        request.allowPathThroughUnits = allowPassthrough && stage == AIDockMoveStage::Dock;
        request.adjustDestination =
            !(allowPassthrough && (stage == AIDockMoveStage::Dock || stage == AIDockMoveStage::Exit));
        columns.movementActive[slot] = 1;
    }
    static_cast<void>(input.requests[slot].push(request));
    columns.pendingRequests[slot] = awaitsFeedback ? kind : AIDockRequestKind::None;
}

[[nodiscard]] constexpr bool positionRequest(AIDockRequestKind request) noexcept
{
    return request == AIDockRequestKind::ReserveApproach || request == AIDockRequestKind::AdvanceApproach ||
           request == AIDockRequestKind::QueryEntryPosition || request == AIDockRequestKind::QueryDockPosition ||
           request == AIDockRequestKind::QueryExitPosition || request == AIDockRequestKind::QueryRallyPosition;
}

[[nodiscard]] constexpr bool semanticallyValidFeedback(AIDockRequestKind request, AIDockFeedbackStatus status) noexcept
{
    if (status == AIDockFeedbackStatus::Pending || status == AIDockFeedbackStatus::Denied ||
        status == AIDockFeedbackStatus::DockMissing || status == AIDockFeedbackStatus::DockClosed ||
        status == AIDockFeedbackStatus::Unsupported)
        return true;
    if (status == AIDockFeedbackStatus::Accepted)
        return positionRequest(request);
    if (status == AIDockFeedbackStatus::MovementMoving || status == AIDockFeedbackStatus::MovementSucceeded ||
        status == AIDockFeedbackStatus::MovementFailed)
        return request == AIDockRequestKind::BeginMove;
    if (status == AIDockFeedbackStatus::ClearanceWaiting || status == AIDockFeedbackStatus::ClearToAdvance ||
        status == AIDockFeedbackStatus::ClearToEnter)
        return request == AIDockRequestKind::PollClearance;
    if (status == AIDockFeedbackStatus::ActionContinue || status == AIDockFeedbackStatus::ActionComplete)
        return request == AIDockRequestKind::ProcessAction;
    if (status == AIDockFeedbackStatus::NoRally)
        return request == AIDockRequestKind::QueryRallyPosition;
    return false;
}

[[nodiscard]] constexpr size_t requestsForFeedback(AIDockPhase phase,
                                                   AIDockRequestKind request,
                                                   AIDockFeedbackStatus status) noexcept
{
    if (!semanticallyValidFeedback(request, status))
        return 0;
    if (status == AIDockFeedbackStatus::Accepted)
    {
        switch (phase)
        {
        case AIDockPhase::Approach:
        case AIDockPhase::AdvancePosition:
        case AIDockPhase::MoveToEntry:
        case AIDockPhase::MoveToDock:
        case AIDockPhase::MoveToExit:
        case AIDockPhase::MoveToRally:
            return 1; // BeginMove.
        default:
            return 0;
        }
    }
    if (status == AIDockFeedbackStatus::MovementSucceeded)
    {
        switch (phase)
        {
        case AIDockPhase::Approach:
        case AIDockPhase::AdvancePosition:
        case AIDockPhase::MoveToEntry:
        case AIDockPhase::MoveToExit:
            return 2; // notification plus the next awaited request.
        case AIDockPhase::MoveToDock:
            return 1; // onDockReached; ProcessDock observes its delay first.
        default:
            return 0;
        }
    }
    if (status == AIDockFeedbackStatus::MovementFailed)
        return phase == AIDockPhase::MoveToEntry || phase == AIDockPhase::MoveToDock ? 1 : 0;
    if (phase == AIDockPhase::WaitForClearance)
    {
        if (status == AIDockFeedbackStatus::ClearanceWaiting || status == AIDockFeedbackStatus::ClearToAdvance ||
            status == AIDockFeedbackStatus::ClearToEnter)
            return 1;
    }
    if (phase == AIDockPhase::ProcessDock &&
        (status == AIDockFeedbackStatus::ActionComplete || status == AIDockFeedbackStatus::DockClosed))
        return 1;
    if ((phase == AIDockPhase::MoveToEntry || phase == AIDockPhase::MoveToDock || phase == AIDockPhase::ProcessDock) &&
        (status == AIDockFeedbackStatus::Denied || status == AIDockFeedbackStatus::DockMissing ||
         status == AIDockFeedbackStatus::DockClosed))
        return 1;
    return 0;
}

[[nodiscard]] constexpr bool terminalFailure(AIDockFeedbackStatus status) noexcept
{
    return status == AIDockFeedbackStatus::Denied || status == AIDockFeedbackStatus::DockMissing ||
           status == AIDockFeedbackStatus::Unsupported;
}

inline void clearSlot(AIDockStateSoAColumns& columns, size_t slot) noexcept
{
    columns.tokenSubjects[slot] = INVALID_OBJECT_ID;
    columns.tokenDocks[slot] = INVALID_OBJECT_ID;
    columns.tokenIssuedTicks[slot] = 0;
    columns.tokenRequestSequences[slot] = 0;
    columns.purposes[slot] = AIDockPurpose::Dock;
    columns.phases[slot] = AIDockPhase::Inactive;
    columns.phaseRevisions[slot] = 0;
    columns.exchangeSequences[slot] = 0;
    columns.pendingRequests[slot] = AIDockRequestKind::None;
    columns.approachPositions[slot] = -1;
    columns.clearanceEnterTicks[slot] = 0;
    columns.nextActionTicks[slot] = 0;
    columns.actionDelayTicks[slot] = 0;
    columns.drones[slot] = INVALID_OBJECT_ID;
    columns.movementActive[slot] = 0;
}

} // namespace dock_detail

[[nodiscard]] inline bool enterDockStateSoA(AIDockStateSoAColumns columns,
                                            const AIDockStateSoAKernelInput& input) noexcept
{
    using namespace dock_detail;
    if (!hasAlignedSpans(columns, input))
        return false;

    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || !isDockAlias(input.activeStates[slot]))
            continue;
        const AIStateRequestId request{columns.tokenIssuedTicks[slot],columns.tokenRequestSequences[slot]};
        if (columns.phases[slot] != AIDockPhase::Inactive || !request.isValid() ||
            !input.goalObjects[slot] || !input.requests[slot].hasCapacity(1))
            return false;
    }

    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || !isDockAlias(input.activeStates[slot]))
            continue;
        columns.tokenSubjects[slot] = input.subjects[slot];
        columns.tokenDocks[slot] = input.goalObjects[slot];
        columns.purposes[slot] = purposeFor(input.activeStates[slot]);
        columns.phases[slot] = AIDockPhase::Approach;
        columns.phaseRevisions[slot] = 1;
        columns.exchangeSequences[slot] = 0;
        columns.pendingRequests[slot] = AIDockRequestKind::None;
        columns.approachPositions[slot] = -1;
        columns.clearanceEnterTicks[slot] = 0;
        columns.nextActionTicks[slot] = 0;
        columns.actionDelayTicks[slot] = 0;
        columns.drones[slot] = INVALID_OBJECT_ID;
        columns.movementActive[slot] = 0;
        emit(columns, input, slot, AIDockRequestKind::ReserveApproach, true);
        input.results[slot] = AIStateStepResult::continueState();
    }
    return true;
}

[[nodiscard]] inline bool updateDockStateSoA(AIDockStateSoAColumns columns,
                                             const AIDockStateSoAKernelInput& input) noexcept
{
    using namespace dock_detail;
    if (!hasAlignedSpans(columns, input))
        return false;

    for (const AIDockFeedbackBuffer& buffer : input.feedback)
    {
        if (buffer.overflowed || buffer.count > AIDockFeedbackBuffer::Capacity)
            return false;
    }

    // Preflight every slot before mutating any slot.  Request-buffer pressure
    // therefore cannot produce a half-applied multi-unit confirmed tick.
    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || !isDockAlias(input.activeStates[slot]))
            continue;
        if (!activationMatches(columns, input, slot) || columns.phases[slot] == AIDockPhase::Inactive ||
            columns.phases[slot] == AIDockPhase::Completed)
            continue;

        size_t required = 0;
        const AIDockFeedback* event = relevantFeedback(input.feedback[slot], columns, slot);
        if (event)
        {
            required = requestsForFeedback(columns.phases[slot], columns.pendingRequests[slot], event->status);
            if (columns.phases[slot] == AIDockPhase::WaitForClearance &&
                event->status == AIDockFeedbackStatus::ClearanceWaiting &&
                input.confirmedTick > clearanceDeadline(columns.clearanceEnterTicks[slot], input.ticksPerSecond))
                required = 0;
        }
        else if (columns.phases[slot] == AIDockPhase::ProcessDock &&
                 columns.pendingRequests[slot] == AIDockRequestKind::None &&
                 input.confirmedTick >= columns.nextActionTicks[slot])
        {
            required = 1;
        }
        if (!input.requests[slot].hasCapacity(required))
            return false;
    }

    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || !isDockAlias(input.activeStates[slot]))
            continue;
        if (!activationMatches(columns, input, slot) || columns.phases[slot] == AIDockPhase::Inactive)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        if (columns.phases[slot] == AIDockPhase::Completed)
        {
            input.results[slot] = AIStateStepResult::success();
            continue;
        }

        const AIDockFeedback* event = relevantFeedback(input.feedback[slot], columns, slot);
        if (!event)
        {
            if (columns.phases[slot] == AIDockPhase::WaitForClearance &&
                input.confirmedTick > clearanceDeadline(columns.clearanceEnterTicks[slot], input.ticksPerSecond))
            {
                input.results[slot] = AIStateStepResult::failure();
                continue;
            }
            if (columns.phases[slot] == AIDockPhase::ProcessDock &&
                columns.pendingRequests[slot] == AIDockRequestKind::None &&
                input.confirmedTick >= columns.nextActionTicks[slot])
                emit(columns, input, slot, AIDockRequestKind::ProcessAction, true);
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }

        const AIDockPhase phase = columns.phases[slot];
        const AIDockRequestKind answered = columns.pendingRequests[slot];
        columns.pendingRequests[slot] = AIDockRequestKind::None;
        if (event->drone)
            columns.drones[slot] = event->drone;

        if (!semanticallyValidFeedback(answered, event->status))
        {
            columns.pendingRequests[slot] = answered;
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        if (event->status == AIDockFeedbackStatus::Pending || event->status == AIDockFeedbackStatus::MovementMoving)
        {
            columns.pendingRequests[slot] = answered;
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }

        if (event->status == AIDockFeedbackStatus::Accepted)
        {
            if (answered == AIDockRequestKind::ReserveApproach || answered == AIDockRequestKind::AdvanceApproach)
                columns.approachPositions[slot] = event->approachPosition;
            if (phase == AIDockPhase::MoveToDock)
                columns.actionDelayTicks[slot] = event->actionDelayTicks;
            emit(columns, input, slot, AIDockRequestKind::BeginMove, true, event->position, event->allowPassthrough);
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }

        if (event->status == AIDockFeedbackStatus::MovementSucceeded)
        {
            columns.movementActive[slot] = 0;
            switch (phase)
            {
            case AIDockPhase::Approach:
            case AIDockPhase::AdvancePosition:
                emit(columns, input, slot, AIDockRequestKind::NotifyApproachReached, false);
                setPhase(columns, slot, AIDockPhase::WaitForClearance);
                columns.clearanceEnterTicks[slot] = input.confirmedTick;
                emit(columns, input, slot, AIDockRequestKind::PollClearance, true);
                break;
            case AIDockPhase::MoveToEntry:
                emit(columns, input, slot, AIDockRequestKind::NotifyEnterReached, false);
                columns.approachPositions[slot] = -1;
                setPhase(columns, slot, AIDockPhase::MoveToDock);
                emit(columns, input, slot, AIDockRequestKind::QueryDockPosition, true);
                break;
            case AIDockPhase::MoveToDock:
                emit(columns, input, slot, AIDockRequestKind::NotifyDockReached, false);
                setPhase(columns, slot, AIDockPhase::ProcessDock);
                columns.nextActionTicks[slot] = saturatedAdd(input.confirmedTick, columns.actionDelayTicks[slot]);
                break;
            case AIDockPhase::MoveToExit:
                emit(columns, input, slot, AIDockRequestKind::NotifyExitReached, false);
                setPhase(columns, slot, AIDockPhase::MoveToRally);
                emit(columns, input, slot, AIDockRequestKind::QueryRallyPosition, true);
                break;
            case AIDockPhase::MoveToRally:
                setPhase(columns, slot, AIDockPhase::Completed);
                input.results[slot] = AIStateStepResult::success();
                continue;
            default:
                input.results[slot] = AIStateStepResult::unsupported();
                continue;
            }
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }

        if (event->status == AIDockFeedbackStatus::MovementFailed)
        {
            columns.movementActive[slot] = 0;
            if (phase == AIDockPhase::MoveToEntry || phase == AIDockPhase::MoveToDock)
            {
                setPhase(columns, slot, AIDockPhase::MoveToExit);
                emit(columns, input, slot, AIDockRequestKind::QueryExitPosition, true);
                input.results[slot] = AIStateStepResult::continueState();
            }
            else
            {
                input.results[slot] = AIStateStepResult::failure();
            }
            continue;
        }

        if (phase == AIDockPhase::WaitForClearance)
        {
            if (event->status == AIDockFeedbackStatus::ClearanceWaiting)
            {
                if (input.confirmedTick > clearanceDeadline(columns.clearanceEnterTicks[slot], input.ticksPerSecond))
                    input.results[slot] = AIStateStepResult::failure();
                else
                {
                    emit(columns, input, slot, AIDockRequestKind::PollClearance, true);
                    input.results[slot] = AIStateStepResult::continueState();
                }
                continue;
            }
            if (event->status == AIDockFeedbackStatus::ClearToAdvance)
            {
                setPhase(columns, slot, AIDockPhase::AdvancePosition);
                emit(columns, input, slot, AIDockRequestKind::AdvanceApproach, true);
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
            if (event->status == AIDockFeedbackStatus::ClearToEnter)
            {
                setPhase(columns, slot, AIDockPhase::MoveToEntry);
                emit(columns, input, slot, AIDockRequestKind::QueryEntryPosition, true);
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
        }

        if (phase == AIDockPhase::ProcessDock)
        {
            if (event->status == AIDockFeedbackStatus::ActionContinue)
            {
                const uint32_t delay =
                    event->actionDelayTicks != 0 ? event->actionDelayTicks : columns.actionDelayTicks[slot];
                columns.actionDelayTicks[slot] = delay;
                columns.nextActionTicks[slot] = saturatedAdd(input.confirmedTick, delay);
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
            if (event->status == AIDockFeedbackStatus::ActionComplete ||
                event->status == AIDockFeedbackStatus::DockClosed)
            {
                setPhase(columns, slot, AIDockPhase::MoveToExit);
                emit(columns, input, slot, AIDockRequestKind::QueryExitPosition, true);
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
        }

        if (phase == AIDockPhase::MoveToRally && event->status == AIDockFeedbackStatus::NoRally)
        {
            setPhase(columns, slot, AIDockPhase::Completed);
            input.results[slot] = AIStateStepResult::success();
            continue;
        }

        if ((phase == AIDockPhase::MoveToEntry || phase == AIDockPhase::MoveToDock ||
             phase == AIDockPhase::ProcessDock) &&
            (event->status == AIDockFeedbackStatus::Denied || event->status == AIDockFeedbackStatus::DockMissing ||
             event->status == AIDockFeedbackStatus::DockClosed))
        {
            setPhase(columns, slot, AIDockPhase::MoveToExit);
            emit(columns, input, slot, AIDockRequestKind::QueryExitPosition, true);
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }

        if (event->status == AIDockFeedbackStatus::Unsupported)
            input.results[slot] = AIStateStepResult::unsupported();
        else if (terminalFailure(event->status) || event->status == AIDockFeedbackStatus::DockClosed)
            input.results[slot] = AIStateStepResult::failure();
        else
            input.results[slot] = AIStateStepResult::unsupported();
    }
    return true;
}

[[nodiscard]] inline bool canExitDockStateSoA(const AIDockStateSoAColumns& columns,
                                              const AIDockStateSoAKernelInput& input) noexcept
{
    using namespace dock_detail;
    if (!hasAlignedSpans(columns, input))
        return false;
    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || columns.phases[slot] == AIDockPhase::Inactive)
            continue;
        const size_t required = 2 + static_cast<size_t>(columns.movementActive[slot] != 0);
        if (!token(columns, slot).isValid() || !input.requests[slot].hasCapacity(required))
            return false;
    }
    return true;
}

// Mirrors AIDockState::onExit + AIDockMachine::halt as one bounded
// transaction.  If any slot lacks capacity, no request or column is changed.
[[nodiscard]] inline bool exitDockStateSoA(AIDockStateSoAColumns columns,
                                           const AIDockStateSoAKernelInput& input) noexcept
{
    using namespace dock_detail;
    if (!canExitDockStateSoA(columns, input))
        return false;
    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || columns.phases[slot] == AIDockPhase::Inactive)
            continue;
        if (columns.movementActive[slot])
            emit(columns, input, slot, AIDockRequestKind::EndMove, false);
        emit(columns, input, slot, AIDockRequestKind::CancelDock, false);
        emit(columns, input, slot, AIDockRequestKind::RestorePathing, false);
        clearSlot(columns, slot);
    }
    return true;
}

} // namespace engine::ai
