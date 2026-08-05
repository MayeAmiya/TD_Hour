#pragma once

#include <variant>

#include "game/object/ai/runtime/AIStateData.h"
#include "game/object/ai/runtime/AIStateStep.h"

namespace engine::ai
{

[[nodiscard]] inline bool emitFacingCommand(AIStateData& data, const AIStateContext& context, AIStateCommandKind kind)
{
    auto* payload = std::get_if<AIFaceStatePayload>(&data.activePayload);
    if (!payload || !context.subject || !context.services.commands)
        return false;
    if (payload->commandIssued)
        return true;

    const AIStateCommand command{
        .kind = kind,
        .subject = context.subject,
        .request = payload->request,
        .targetObject = data.parameters.goalObject,
        .targetPosition = data.parameters.goalPosition,
        .canTurnInPlace = payload->canTurnInPlace,
        .confirmedTick = context.confirmedTick,
    };
    if (!context.services.commands->push(command))
        return false;
    payload->commandIssued = true;
    return true;
}

[[nodiscard]] inline AIStateStepResult updateFacing(AIStateId state, AIStateData& data, const AIStateContext& context)
{
    if (context.effectivelyDead)
        return AIStateStepResult::transitionTo(AIStateId::Dead);
    const bool objectTarget = state == AIStateId::FaceObject;
    const bool targetValid =
        objectTarget ? static_cast<bool>(data.parameters.goalObject) : data.parameters.hasGoalPosition;
    if (!targetValid)
        return AIStateStepResult::failure();

    const auto* payload = std::get_if<AIFaceStatePayload>(&data.activePayload);
    if (!payload)
        return AIStateStepResult::unsupported();

    const AIFacingFeedback* feedback = context.services.facingFeedback;
    if (feedback && feedback->subject == context.subject && feedback->request == payload->request)
    {
        switch (feedback->status)
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

    if (!emitFacingCommand(
            data, context, objectTarget ? AIStateCommandKind::FaceObject : AIStateCommandKind::FacePosition))
    {
        return AIStateStepResult::unsupported();
    }
    return AIStateStepResult::continueState();
}

[[nodiscard]] inline AIStateStepResult enterFacing(AIStateId state, AIStateData& data, const AIStateContext& context)
{
    auto* payload = std::get_if<AIFaceStatePayload>(&data.activePayload);
    if (!payload)
        return AIStateStepResult::unsupported();
    payload->canTurnInPlace = context.canTurnInPlace;
    return updateFacing(state, data, context);
}

inline void exitFacing(AIStateData& data) noexcept
{
    // Compatibility invariant: AIFaceState::onExit() is empty. Idle clears
    // the locomotor goal during its following update, preserving the original
    // one-update delay.
    static_cast<void>(data);
}

} // namespace engine::ai
