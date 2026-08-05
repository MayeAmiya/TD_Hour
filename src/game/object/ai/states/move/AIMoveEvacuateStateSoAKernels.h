#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include "core/container/container_types.h"

#include "game/object/ai/states/move/AIMoveStateSoAKernels.h"

namespace engine::ai
{

enum class AIMoveEvacuateCommandKind : uint8_t
{
    Evacuate,
    ActivateTeam,
    SetAllowInvalidPosition,
    AppendPathNode,
    DestroyObject,
};

// Side effects are values, not callbacks. The simulation consumer applies
// them after the state wave commits, in stable slot and command order.
struct AIMoveEvacuateCommand final
{
    ObjectId subject = INVALID_OBJECT_ID;
    AIStateRequestId stateRequest;
    AIMoveEvacuateCommandKind kind = AIMoveEvacuateCommandKind::Evacuate;
    PathHandle path;
    AIFixedPosition position;
    uint32_t layer = 0;
    bool enabled = false;
    uint64_t confirmedTick = 0;
};

using AIMoveEvacuateCommandBuffer = AIPathValueBuffer<AIMoveEvacuateCommand, 8>;

// All spans use the stable AIStateFamilySoAStorage slot. groundedDeleteGoal
// is the order goal with deterministic terrain-ground Z already resolved by
// the caller. No terrain query or world mutation occurs inside the kernel.
struct AIMoveEvacuateStateSoAKernelInput final
{
    uint64_t confirmedTick = 0;
    container::Span<const uint8_t> scheduled{};
    container::Span<const uint8_t> effectivelyDead;
    container::Span<const uint8_t> mobile;
    container::Span<const uint8_t> moveTargetValid;
    container::Span<const uint8_t> hasCurrentLocomotor;
    container::Span<const uint8_t> groundMovement;
    container::Span<const AIFixedPosition> subjectPosition;
    container::Span<const AIFixedPosition> resolvedMoveTarget;
    container::Span<const AIFixedPosition> groundedDeleteGoal;
    container::Span<const uint32_t> ticksPerSecond;
    container::Span<const PathFeedback> pathFeedback;
    container::Span<const MovementFeedback> movementFeedback;
    container::Span<PathRequestBuffer> pathRequests;
    container::Span<MovementCommandBuffer> movementCommands;
    container::Span<AIMoveEvacuateCommandBuffer> commands;
    container::Span<AIStateStepResult> results;
};

namespace detail
{

[[nodiscard]] constexpr bool isEvacuateState(AIStateId state) noexcept
{
    return state == AIStateId::MoveAndEvacuate || state == AIStateId::MoveAndEvacuateAndExit;
}

[[nodiscard]] constexpr bool isMoveEvacuateState(AIStateId state) noexcept
{
    return isEvacuateState(state) || state == AIStateId::MoveAndDelete;
}

[[nodiscard]] inline bool hasAlignedMoveEvacuateSpans(const AIStateFamilySoAStorage& storage,
                                                      const AIMoveEvacuateStateSoAKernelInput& input) noexcept
{
    const size_t count = storage.size();
    return storage.moveEvacuate().size() == count &&
           (input.scheduled.empty() || input.scheduled.size() == count) && input.effectivelyDead.size() == count &&
           input.mobile.size() == count && input.moveTargetValid.size() == count &&
           input.hasCurrentLocomotor.size() == count && input.groundMovement.size() == count &&
           input.subjectPosition.size() == count &&
           input.resolvedMoveTarget.size() == count && input.groundedDeleteGoal.size() == count &&
           input.ticksPerSecond.size() == count && input.pathFeedback.size() == count &&
           input.movementFeedback.size() == count && input.pathRequests.size() == count &&
           input.movementCommands.size() == count && input.commands.size() == count && input.results.size() == count;
}

[[nodiscard]] constexpr bool moveEvacuateScheduled(const AIMoveEvacuateStateSoAKernelInput& input, size_t slot) noexcept
{
    return input.scheduled.empty() || input.scheduled[slot] != 0;
}

template <typename Buffer>
[[nodiscard]] constexpr bool canAppend(const Buffer& destination, const Buffer& staged) noexcept
{
    return destination.count <= destination.values.size() &&
           staged.count <= destination.values.size() - destination.count;
}

template <typename Buffer>
inline void appendStaged(Buffer& destination, const Buffer& staged) noexcept
{
    for (size_t index = 0; index < staged.count; ++index)
        static_cast<void>(destination.push(staged.values[index]));
}

[[nodiscard]] inline bool stageCommand(AIMoveEvacuateCommandBuffer& output,
                                       ObjectId subject,
                                       const AIMoveToStatePayload& payload,
                                       AIMoveEvacuateCommandKind kind,
                                       uint64_t confirmedTick,
                                       PathHandle path = {},
                                       AIFixedPosition position = {},
                                       uint32_t layer = 0,
                                       bool enabled = false) noexcept
{
    return output.push({
        .subject = subject,
        .stateRequest = payload.request,
        .kind = kind,
        .path = path,
        .position = position,
        .layer = layer,
        .enabled = enabled,
        .confirmedTick = confirmedTick,
    });
}

[[nodiscard]] inline AIStateStepResult advanceMoveEvacuatePath(ObjectId subject,
                                                               const AIStateParameters& parameters,
                                                               const AIMoveEvacuateStateSoAKernelInput& input,
                                                               size_t slot,
                                                               AIMoveToStatePayload& candidate,
                                                               PathRequestBuffer& stagedPaths,
                                                               MovementCommandBuffer& stagedMovement) noexcept
{
    const PathCorrelation expected = moveStateSoACorrelation(subject, candidate);
    if (candidate.phase == AIMoveToPhase::WaitingForPath)
    {
        if (!candidate.pathRequestIssued)
        {
            return emitMoveStateSoAPathRequest(
                       stagedPaths, subject, input.subjectPosition[slot], parameters,
                       candidate, PathRequestKind::New,
                       !moveStateSoAFact(input.groundMovement[slot]))
                       ? AIStateStepResult::continueState()
                       : AIStateStepResult::unsupported();
        }

        const PathFeedback& feedback = input.pathFeedback[slot];
        if (!(feedback.correlation == expected) || feedback.status == PathFeedbackStatus::Pending ||
            feedback.status == PathFeedbackStatus::Delayed)
        {
            return AIStateStepResult::continueState();
        }
        if (feedback.status == PathFeedbackStatus::Ready)
        {
            if (!feedback.path)
                return AIStateStepResult::unsupported();
            candidate.pathRequestIssued = false;
            candidate.path = feedback.path;
            candidate.adjustedGoal = feedback.adjustedGoal;
            candidate.adjustedLayer = feedback.adjustedLayer;
            candidate.phase = AIMoveToPhase::FollowingPath;
            return emitMoveStateSoAInstall(stagedMovement, subject, input.confirmedTick,
                                           candidate, parameters.ignoredObstacle)
                       ? AIStateStepResult::continueState()
                       : AIStateStepResult::unsupported();
        }

        candidate.pathRequestIssued = false;
        return feedback.status == PathFeedbackStatus::Unsupported ? AIStateStepResult::unsupported()
                                                                  : AIStateStepResult::failure();
    }

    const MovementFeedback& feedback = input.movementFeedback[slot];
    if (!(feedback.correlation == expected))
        return AIStateStepResult::continueState();

    switch (feedback.status)
    {
    case MovementFeedbackStatus::Started:
    case MovementFeedbackStatus::Moving:
        return AIStateStepResult::continueState();
    case MovementFeedbackStatus::Completed:
        return AIStateStepResult::success();
    case MovementFeedbackStatus::Cancelled:
        return AIStateStepResult::failure();
    case MovementFeedbackStatus::Unsupported:
        return AIStateStepResult::unsupported();
    case MovementFeedbackStatus::Blocked:
    {
        const uint32_t maximum = std::numeric_limits<uint32_t>::max();
        const uint32_t threshold = input.ticksPerSecond[slot] > maximum / 2 ? maximum : input.ticksPerSecond[slot] * 2;
        if (feedback.blockedTicks < threshold)
            return AIStateStepResult::continueState();
        break;
    }
    case MovementFeedbackStatus::Stuck:
        break;
    }

    return beginMoveStateSoARepath(
               stagedPaths, subject, input.subjectPosition[slot], parameters,
               candidate, !moveStateSoAFact(input.groundMovement[slot]))
               ? AIStateStepResult::continueState()
               : AIStateStepResult::unsupported();
}

[[nodiscard]] inline bool stageTerminalCommands(AIMoveEvacuateCommandBuffer& staged,
                                                ObjectId subject,
                                                AIStateId state,
                                                const AIMoveToStatePayload& payload,
                                                uint64_t confirmedTick) noexcept
{
    if (isEvacuateState(state))
    {
        return stageCommand(staged, subject, payload, AIMoveEvacuateCommandKind::Evacuate, confirmedTick) &&
               stageCommand(staged, subject, payload, AIMoveEvacuateCommandKind::ActivateTeam, confirmedTick);
    }
    return stageCommand(staged, subject, payload, AIMoveEvacuateCommandKind::DestroyObject, confirmedTick);
}

} // namespace detail

// A false return means the input is not slot-aligned; nothing is modified.
[[nodiscard]] inline bool enterMoveEvacuateStateSoA(AIStateFamilySoAStorage& storage,
                                                    const AIMoveEvacuateStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedMoveEvacuateSpans(storage, input))
        return false;

    const auto subjects = storage.subjects();
    auto runtimes = storage.runtimes();
    const auto payloadStates = storage.payloadStates();
    const auto sequences = storage.activationSequences();
    auto parameters = storage.parameters();
    auto& moveTo = storage.moveTo();
    auto& columns = storage.moveEvacuate();

    for (const size_t slot : storage.executionSlots())
    {
        const AIStateId state = runtimes[slot].currentState;
        if (!detail::moveEvacuateScheduled(input, slot) || !detail::isMoveEvacuateState(state))
            continue;
        if (payloadStates[slot] != state || !subjects[slot] || sequences[slot] == 0)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        AIStateParameters parameter = parameters[slot];
        AIMoveToStatePayload candidate{AIStateRequestId{runtimes[slot].enteredTick, sequences[slot]}};
        candidate.sourceOrderRevision = parameter.sourceOrderRevision;
        candidate.adjustDestinations = detail::isEvacuateState(state);
        if (parameter.sourceOrderRevision == 0)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        const auto commitEnteredPayload = [&]() noexcept
        {
            moveTo.store(slot, candidate);
            columns.setOrigin(slot, input.subjectPosition[slot]);
            columns.setAppendDeleteGoal(slot, state == AIStateId::MoveAndDelete);
            AIStateMachine::setLock(runtimes[slot], AIStateMachineLock::ExternalTransitionsBlocked);
        };
        if (input.effectivelyDead[slot] != 0)
        {
            commitEnteredPayload();
            input.results[slot] = AIStateStepResult::transitionTo(AIStateId::Dead);
            continue;
        }
        if (input.mobile[slot] == 0)
        {
            commitEnteredPayload();
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        if (parameter.goalObject)
        {
            if (input.moveTargetValid[slot] == 0)
            {
                commitEnteredPayload();
                input.results[slot] = AIStateStepResult::failure();
                continue;
            }
            parameter.goalPosition = input.resolvedMoveTarget[slot];
            parameter.hasGoalPosition = true;
        }
        else if (!parameter.hasGoalPosition)
        {
            commitEnteredPayload();
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        candidate.resolvedGoal = parameter.goalPosition;

        PathRequestBuffer staged;
        if (!detail::emitMoveStateSoAPathRequest(
                staged, subjects[slot], input.subjectPosition[slot], parameter,
                candidate, PathRequestKind::New,
                !detail::moveStateSoAFact(input.groundMovement[slot])) ||
            !detail::canAppend(input.pathRequests[slot], staged))
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        detail::appendStaged(input.pathRequests[slot], staged);
        parameters[slot] = parameter;
        commitEnteredPayload();
        input.results[slot] = AIStateStepResult::continueState();
    }
    return true;
}

[[nodiscard]] inline bool updateMoveEvacuateStateSoA(AIStateFamilySoAStorage& storage,
                                                     const AIMoveEvacuateStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedMoveEvacuateSpans(storage, input))
        return false;

    const auto subjects = storage.subjects();
    const auto runtimes = storage.runtimes();
    const auto payloadStates = storage.payloadStates();
    const auto parameters = storage.parameters();
    auto& moveTo = storage.moveTo();
    auto& columns = storage.moveEvacuate();

    for (const size_t slot : storage.executionSlots())
    {
        const AIStateId state = runtimes[slot].currentState;
        if (!detail::moveEvacuateScheduled(input, slot) || !detail::isMoveEvacuateState(state))
            continue;
        if (input.effectivelyDead[slot] != 0)
        {
            input.results[slot] = AIStateStepResult::transitionTo(AIStateId::Dead);
            continue;
        }
        if (payloadStates[slot] != state)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        AIMoveToStatePayload candidate = moveTo.load(slot);
        const AIStateParameters& parameter = parameters[slot];

        PathRequestBuffer stagedPaths;
        MovementCommandBuffer stagedMovement;
        AIMoveEvacuateCommandBuffer stagedCommands;
        bool appendDeleteGoal = columns.appendDeleteGoal(slot);

        if (state == AIStateId::MoveAndDelete)
        {
            if (input.hasCurrentLocomotor[slot] != 0)
            {
                static_cast<void>(detail::stageCommand(stagedCommands,
                                                       subjects[slot],
                                                       candidate,
                                                       AIMoveEvacuateCommandKind::SetAllowInvalidPosition,
                                                       input.confirmedTick,
                                                       {},
                                                       {},
                                                       0,
                                                       true));
            }
            // Legacy appends only on a later update, after the asynchronous
            // path has become installed and the AI is no longer waiting.
            if (appendDeleteGoal && candidate.phase == AIMoveToPhase::FollowingPath && candidate.path)
            {
                static_cast<void>(detail::stageCommand(stagedCommands,
                                                       subjects[slot],
                                                       candidate,
                                                       AIMoveEvacuateCommandKind::AppendPathNode,
                                                       input.confirmedTick,
                                                       candidate.path,
                                                       input.groundedDeleteGoal[slot],
                                                       0));
                appendDeleteGoal = false;
            }
        }

        AIStateStepResult result =
            input.mobile[slot] == 0
                ? AIStateStepResult::failure()
                : detail::advanceMoveEvacuatePath(
                      subjects[slot], parameter, input, slot, candidate, stagedPaths, stagedMovement);
        if (result.kind == AIStateStepKind::Success || result.kind == AIStateStepKind::Failure)
        {
            static_cast<void>(
                detail::stageTerminalCommands(stagedCommands, subjects[slot], state, candidate, input.confirmedTick));
        }

        if (!detail::canAppend(input.pathRequests[slot], stagedPaths) ||
            !detail::canAppend(input.movementCommands[slot], stagedMovement) ||
            !detail::canAppend(input.commands[slot], stagedCommands))
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        detail::appendStaged(input.pathRequests[slot], stagedPaths);
        detail::appendStaged(input.movementCommands[slot], stagedMovement);
        detail::appendStaged(input.commands[slot], stagedCommands);
        moveTo.store(slot, candidate);
        columns.setAppendDeleteGoal(slot, appendDeleteGoal);
        input.results[slot] = result;
    }
    return true;
}

// Exit cleanup is all-or-nothing across the scheduled batch. This preserves
// the old request correlation until Cancel + EndMovement can both be emitted.
[[nodiscard]] inline bool canExitMoveEvacuateStateSoA(const AIStateFamilySoAStorage& storage,
                                                      const AIMoveEvacuateStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedMoveEvacuateSpans(storage, input))
        return false;
    const auto states = storage.payloadStates();
    const auto& moveTo = storage.moveTo();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::moveEvacuateScheduled(input, slot) || !detail::isMoveEvacuateState(states[slot]))
            continue;
        if (moveTo.load(slot).pathRequestIssued &&
            input.pathRequests[slot].count >= input.pathRequests[slot].values.size())
            return false;
        if (input.movementCommands[slot].count >= input.movementCommands[slot].values.size())
            return false;
    }
    return true;
}

[[nodiscard]] inline bool exitMoveEvacuateStateSoA(AIStateFamilySoAStorage& storage,
                                                   const AIMoveEvacuateStateSoAKernelInput& input) noexcept
{
    if (!canExitMoveEvacuateStateSoA(storage, input))
        return false;

    const auto subjects = storage.subjects();
    auto runtimes = storage.runtimes();
    const auto states = storage.payloadStates();
    auto parameters = storage.parameters();
    auto& moveTo = storage.moveTo();
    const auto& columns = storage.moveEvacuate();
    for (const size_t slot : storage.executionSlots())
    {
        const AIStateId state = states[slot];
        if (!detail::moveEvacuateScheduled(input, slot) || !detail::isMoveEvacuateState(state))
            continue;

        AIMoveToStatePayload payload = moveTo.load(slot);
        if (payload.pathRequestIssued)
        {
            static_cast<void>(detail::emitMoveStateSoAPathRequest(input.pathRequests[slot],
                                                                  subjects[slot],
                                                                  input.subjectPosition[slot],
                                                                  parameters[slot],
                                                                  payload,
                                                                  PathRequestKind::Cancel,
                                                                  !detail::moveStateSoAFact(
                                                                      input.groundMovement[slot])));
        }
        static_cast<void>(input.movementCommands[slot].push({
            .correlation = detail::moveStateSoACorrelation(subjects[slot], payload),
            .kind = MovementCommandKind::EndMovement,
            .path = payload.path,
            .clearGoal = payload.adjustDestinations,
            .preserveUltraAccurateFinalPosition = true,
            .confirmedTick = input.confirmedTick,
        }));
        if (detail::isEvacuateState(state))
        {
            parameters[slot].goalPosition = columns.origin(slot);
            parameters[slot].hasGoalPosition = true;
        }
        moveTo.store(slot, payload);
        AIStateMachine::setLock(runtimes[slot], AIStateMachineLock::Unlocked);
    }
    return true;
}

} // namespace engine::ai
