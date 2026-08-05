#pragma once

#include <cstdint>
#include <limits>
#include <variant>

#include "game/object/ai/runtime/AIStateData.h"
#include "game/object/ai/runtime/AIStateStep.h"

namespace engine::ai
{

[[nodiscard]] constexpr uint64_t saturatingTickAdd(uint64_t tick, uint64_t delta) noexcept
{
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    return delta > maximum - tick ? maximum : tick + delta;
}

[[nodiscard]] inline AIStateStepResult enterIdle(AIStateData& data, const AIStateContext& context)
{
    auto* payload = std::get_if<AIIdleStatePayload>(&data.activePayload);
    if (!payload)
        return AIStateStepResult::unsupported();
    payload->nextTargetScanTick = saturatingTickAdd(
        context.confirmedTick, context.forceIdleBeforeAcquireTicks);
    return context.effectivelyDead ? AIStateStepResult::transitionTo(AIStateId::Dead)
                                   : AIStateStepResult::continueState();
}

[[nodiscard]] inline AIStateStepResult updateIdle(AIStateData& data, const AIStateContext& context)
{
    if (context.effectivelyDead)
        return AIStateStepResult::transitionTo(AIStateId::Dead);

    auto* payload = std::get_if<AIIdleStatePayload>(&data.activePayload);
    if (!payload)
        return AIStateStepResult::unsupported();
    if (!context.idleAutoAcquireEnabled)
        return AIStateStepResult::sleepUntil(std::numeric_limits<uint64_t>::max());
    if (context.confirmedTick < payload->nextTargetScanTick)
        return AIStateStepResult::sleepUntil(payload->nextTargetScanTick);

    const uint64_t interval = context.idleTargetScanIntervalTicks == 0
                                  ? uint64_t{1}
                                  : static_cast<uint64_t>(context.idleTargetScanIntervalTicks);
    payload->nextTargetScanTick = saturatingTickAdd(context.confirmedTick, interval);
    return context.idleTargetAvailable ? AIStateStepResult::transitionTo(AIStateId::AttackObject)
                                       : AIStateStepResult::sleepUntil(payload->nextTargetScanTick);
}

[[nodiscard]] inline AIStateStepResult enterWait(AIStateData& data, const AIStateContext& context)
{
    const auto* payload = std::get_if<AIWaitStatePayload>(&data.activePayload);
    if (!payload)
        return AIStateStepResult::unsupported();
    return context.confirmedTick >= payload->endTick ? AIStateStepResult::success()
                                                     : AIStateStepResult::sleepUntil(payload->endTick);
}

[[nodiscard]] inline AIStateStepResult updateWait(AIStateData& data, const AIStateContext& context)
{
    if (context.effectivelyDead)
        return AIStateStepResult::transitionTo(AIStateId::Dead);
    return enterWait(data, context);
}

[[nodiscard]] inline AIStateStepResult enterBusy(AIStateData& data, const AIStateContext& context)
{
    if (!std::holds_alternative<AIBusyStatePayload>(data.activePayload))
        return AIStateStepResult::unsupported();
    return context.effectivelyDead ? AIStateStepResult::transitionTo(AIStateId::Dead)
                                   : AIStateStepResult::continueState();
}

[[nodiscard]] inline AIStateStepResult updateBusy(AIStateData& data, const AIStateContext& context)
{
    if (!std::holds_alternative<AIBusyStatePayload>(data.activePayload))
        return AIStateStepResult::unsupported();
    return context.effectivelyDead ? AIStateStepResult::transitionTo(AIStateId::Dead)
                                   : AIStateStepResult::sleepUntil(std::numeric_limits<uint64_t>::max());
}

[[nodiscard]] inline AIStateStepResult enterDead(AIStateData& data, const AIStateContext&)
{
    return std::holds_alternative<AIDeadStatePayload>(data.activePayload)
               ? AIStateStepResult::sleepUntil(std::numeric_limits<uint64_t>::max())
               : AIStateStepResult::unsupported();
}

[[nodiscard]] inline AIStateStepResult updateDead(AIStateData& data, const AIStateContext& context)
{
    return enterDead(data, context);
}

inline void exitCoreState(AIStateData&, AIStateId, AIStateExitReason) noexcept
{
}

} // namespace engine::ai
