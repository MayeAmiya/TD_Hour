#pragma once

#include "game/object/ai/states/core/AICoreStates.h"
#include "game/object/ai/states/core/AIFacingStates.h"
#include "game/object/ai/states/move/AIMoveStates.h"
#include "game/object/ai/runtime/AIStateMachine.h"

namespace engine::ai
{

[[nodiscard]] constexpr bool hasStateBehavior(AIStateId state) noexcept
{
    switch (state)
    {
    case AIStateId::Idle:
    case AIStateId::Wait:
    case AIStateId::Busy:
    case AIStateId::Dead:
    case AIStateId::FaceObject:
    case AIStateId::FacePosition:
    case AIStateId::MoveTo:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] inline AIStateStepResult dispatchStateEnter(AIStateId state,
                                                          AIStateData& data,
                                                          const AIStateContext& context)
{
    switch (state)
    {
    case AIStateId::Idle:
        return enterIdle(data, context);
    case AIStateId::Wait:
        return enterWait(data, context);
    case AIStateId::Busy:
        return enterBusy(data, context);
    case AIStateId::Dead:
        return enterDead(data, context);
    case AIStateId::FaceObject:
    case AIStateId::FacePosition:
        return enterFacing(state, data, context);
    case AIStateId::MoveTo:
        return enterMoveTo(data, context);
    default:
        return AIStateStepResult::unsupported();
    }
}

[[nodiscard]] inline AIStateStepResult dispatchStateUpdate(AIStateId state,
                                                           AIStateData& data,
                                                           const AIStateContext& context)
{
    switch (state)
    {
    case AIStateId::Idle:
        return updateIdle(data, context);
    case AIStateId::Wait:
        return updateWait(data, context);
    case AIStateId::Busy:
        return updateBusy(data, context);
    case AIStateId::Dead:
        return updateDead(data, context);
    case AIStateId::FaceObject:
    case AIStateId::FacePosition:
        return updateFacing(state, data, context);
    case AIStateId::MoveTo:
        return updateMoveTo(data, context);
    default:
        return AIStateStepResult::unsupported();
    }
}

inline void dispatchStateExit(AIStateId state,
                              AIStateData& data,
                              AIStateExitReason reason,
                              const AIStateContext& context) noexcept
{
    switch (state)
    {
    case AIStateId::Idle:
    case AIStateId::Wait:
    case AIStateId::Busy:
    case AIStateId::Dead:
        exitCoreState(data, state, reason);
        break;
    case AIStateId::FaceObject:
    case AIStateId::FacePosition:
        exitFacing(data);
        break;
    case AIStateId::MoveTo:
        exitMoveTo(data, context);
        break;
    default:
        break;
    }
}

} // namespace engine::ai
