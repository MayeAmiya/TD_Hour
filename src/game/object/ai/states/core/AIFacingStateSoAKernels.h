#pragma once

#include <cstddef>
#include <cstdint>

#include "core/container/container_types.h"
#include "game/object/ai/runtime/AIStateFamilySoAStorage.h"
#include "game/object/ai/runtime/AIStateStep.h"

namespace engine::ai
{

struct AIFacingStateSoAKernelInput final
{
    uint64_t confirmedTick = 0;
    // Empty schedules every slot. Non-empty masks preserve stable slot
    // identity without requiring a compacted scratch batch.
    container::Span<const uint8_t> scheduled{};
    container::Span<const uint8_t> effectivelyDead;
    container::Span<const uint8_t> faceTargetValid;
    container::Span<const AIFacingFeedback> facingFeedback;
    // Sampled into the Face SoA columns only by an enter kernel.
    container::Span<const uint8_t> canTurnInPlace;
    container::Span<AIStateStepResult> results;

    // Exactly one command destination is required. Per-slot buffers isolate
    // producer capacity; sharedCommands is the bounded batch-sink option.
    container::Span<AIStateCommandBuffer> commandBuffers{};
    AIStateCommandBuffer* sharedCommands = nullptr;
};

namespace detail
{

[[nodiscard]] constexpr bool facingStateSoAFact(uint8_t value) noexcept
{
    return value != 0;
}

[[nodiscard]] constexpr bool facingStateSoAScheduled(const AIFacingStateSoAKernelInput& input, size_t slot) noexcept
{
    return input.scheduled.empty() || facingStateSoAFact(input.scheduled[slot]);
}

[[nodiscard]] constexpr bool isFacingStateSoAState(AIStateId state) noexcept
{
    return state == AIStateId::FaceObject || state == AIStateId::FacePosition;
}

[[nodiscard]] inline bool hasAlignedFacingStateSoASpans(const AIStateFamilySoAStorage& storage,
                                                        const AIFacingStateSoAKernelInput& input) noexcept
{
    const size_t count = storage.size();
    const bool hasPerSlotBuffers = !input.commandBuffers.empty();
    const bool hasSharedBuffer = input.sharedCommands != nullptr;
    return (input.scheduled.empty() || input.scheduled.size() == count) && input.effectivelyDead.size() == count &&
           input.faceTargetValid.size() == count && input.facingFeedback.size() == count &&
           input.canTurnInPlace.size() == count && input.results.size() == count &&
           (hasPerSlotBuffers != hasSharedBuffer) && (!hasPerSlotBuffers || input.commandBuffers.size() == count);
}

[[nodiscard]] inline AIStateCommandBuffer& facingStateSoACommandSink(const AIFacingStateSoAKernelInput& input,
                                                                     size_t slot) noexcept
{
    return input.commandBuffers.empty() ? *input.sharedCommands : input.commandBuffers[slot];
}

[[nodiscard]] inline bool facingStateSoATargetValid(AIStateId state,
                                                    const AIStateParameters& parameters,
                                                    uint8_t callerTargetValid) noexcept
{
    return state == AIStateId::FaceObject
               ? static_cast<bool>(parameters.goalObject) && facingStateSoAFact(callerTargetValid)
               : parameters.hasGoalPosition;
}

[[nodiscard]] inline AIStateStepResult stepFacingStateSoASlot(AIStateFamilySoAStorage& storage,
                                                              AIStateId state,
                                                              const AIFacingStateSoAKernelInput& input,
                                                              size_t slot) noexcept
{
    if (facingStateSoAFact(input.effectivelyDead[slot]))
        return AIStateStepResult::transitionTo(AIStateId::Dead);

    const auto parameters = storage.parameters();
    if (!facingStateSoATargetValid(state, parameters[slot], input.faceTargetValid[slot]))
        return AIStateStepResult::failure();

    if (storage.payloadStates()[slot] != state)
        return AIStateStepResult::unsupported();

    auto& columns = storage.face();
    AIFaceStatePayload payload = columns.load(slot);
    const ObjectId subject = storage.subjects()[slot];
    const AIFacingFeedback& feedback = input.facingFeedback[slot];
    if (feedback.subject == subject && feedback.request == payload.request)
    {
        switch (feedback.status)
        {
        case AIFacingFeedbackStatus::Completed:
            return AIStateStepResult::success();
        case AIFacingFeedbackStatus::TargetLost:
            return AIStateStepResult::failure();
        case AIFacingFeedbackStatus::Unsupported:
            return AIStateStepResult::unsupported();
        case AIFacingFeedbackStatus::None:
        case AIFacingFeedbackStatus::Pending:
            break;
        }
    }

    if (!payload.commandIssued)
    {
        const AIStateCommand command{
            .kind = state == AIStateId::FaceObject ? AIStateCommandKind::FaceObject : AIStateCommandKind::FacePosition,
            .subject = subject,
            .request = payload.request,
            .targetObject = parameters[slot].goalObject,
            .targetPosition = parameters[slot].goalPosition,
            .canTurnInPlace = payload.canTurnInPlace,
            .confirmedTick = input.confirmedTick,
        };
        if (!facingStateSoACommandSink(input, slot).push(command))
            return AIStateStepResult::unsupported();
        payload.commandIssued = true;
        columns.store(slot, payload);
    }
    return AIStateStepResult::continueState();
}

} // namespace detail

// False means state or caller-owned spans were invalid. Validation is atomic:
// no result, payload, or command buffer is touched in that case.
[[nodiscard]] inline bool enterFacingSoA(AIStateFamilySoAStorage& storage,
                                         AIStateId state,
                                         const AIFacingStateSoAKernelInput& input) noexcept
{
    if (!detail::isFacingStateSoAState(state) || !detail::hasAlignedFacingStateSoASpans(storage, input))
        return false;

    const auto runtimes = storage.runtimes();
    const auto payloadStates = storage.payloadStates();
    auto& columns = storage.face();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::facingStateSoAScheduled(input, slot) || runtimes[slot].currentState != state)
            continue;
        // AIFacingStates::enterFacing checks the payload before sampling or
        // evaluating death/target facts.
        if (payloadStates[slot] != state)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        AIFaceStatePayload payload = columns.load(slot);
        payload.canTurnInPlace = detail::facingStateSoAFact(input.canTurnInPlace[slot]);
        columns.store(slot, payload);
        input.results[slot] = detail::stepFacingStateSoASlot(storage, state, input, slot);
    }
    return true;
}

[[nodiscard]] inline bool updateFacingSoA(AIStateFamilySoAStorage& storage,
                                          AIStateId state,
                                          const AIFacingStateSoAKernelInput& input) noexcept
{
    if (!detail::isFacingStateSoAState(state) || !detail::hasAlignedFacingStateSoASpans(storage, input))
        return false;

    const auto runtimes = storage.runtimes();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::facingStateSoAScheduled(input, slot) || runtimes[slot].currentState != state)
            continue;
        input.results[slot] = detail::stepFacingStateSoASlot(storage, state, input, slot);
    }
    return true;
}

[[nodiscard]] inline bool enterFaceObjectSoA(AIStateFamilySoAStorage& storage,
                                             const AIFacingStateSoAKernelInput& input) noexcept
{
    return enterFacingSoA(storage, AIStateId::FaceObject, input);
}

[[nodiscard]] inline bool updateFaceObjectSoA(AIStateFamilySoAStorage& storage,
                                              const AIFacingStateSoAKernelInput& input) noexcept
{
    return updateFacingSoA(storage, AIStateId::FaceObject, input);
}

[[nodiscard]] inline bool enterFacePositionSoA(AIStateFamilySoAStorage& storage,
                                               const AIFacingStateSoAKernelInput& input) noexcept
{
    return enterFacingSoA(storage, AIStateId::FacePosition, input);
}

[[nodiscard]] inline bool updateFacePositionSoA(AIStateFamilySoAStorage& storage,
                                                const AIFacingStateSoAKernelInput& input) noexcept
{
    return updateFacingSoA(storage, AIStateId::FacePosition, input);
}

inline void exitFacingSoA(AIStateFamilySoAStorage&) noexcept
{
    // Compatibility invariant: AIFaceState::onExit() is empty.
}

inline void exitFaceObjectSoA(AIStateFamilySoAStorage& storage) noexcept
{
    exitFacingSoA(storage);
}

inline void exitFacePositionSoA(AIStateFamilySoAStorage& storage) noexcept
{
    exitFacingSoA(storage);
}

} // namespace engine::ai
