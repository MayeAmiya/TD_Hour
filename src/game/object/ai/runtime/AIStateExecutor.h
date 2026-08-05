#pragma once

#include <limits>

#include "game/object/ai/runtime/AIStateDescriptor.h"
#include "game/object/ai/runtime/AIStateDispatch.h"

namespace engine::ai
{

enum class AIStateExecutionStatus : uint8_t
{
    Running,
    Sleeping,
    Blocked,
    Transitioned,
    Unsupported,
    Rejected,
    TransitionLimitExceeded,
};

struct AIStateExecutionReport final
{
    AIStateExecutionStatus status = AIStateExecutionStatus::Running;
    AIStateTransition lastCommittedTransition{};
    uint8_t transitionCount = 0;
    bool hasTransition = false;

    [[nodiscard]] const AIStateTransition* lastTransition() const noexcept
    {
        return hasTransition ? &lastCommittedTransition : nullptr;
    }
};

class AIStateExecutor final
{
public:
    [[nodiscard]] static AIStateExecutionReport initialize(AIStateMachineRuntime& runtime,
                                                           AIStateData& data,
                                                           AIStateId defaultState,
                                                           const AIStateContext& context)
    {
        AIStateExecutionReport report;
        const auto transition = AIStateMachine::initialize(runtime, defaultState, context.confirmedTick);
        if (!transition)
        {
            report.status = AIStateExecutionStatus::Rejected;
            return report;
        }
        return processTransition(runtime, data, context, *transition, report);
    }

    [[nodiscard]] static AIStateExecutionReport setState(
        AIStateMachineRuntime& runtime,
        AIStateData& data,
        AIStateId state,
        const AIStateContext& context,
        AIStateTransitionAuthority authority = AIStateTransitionAuthority::External,
        bool reenter = false)
    {
        AIStateExecutionReport report;
        const auto transition = AIStateMachine::setState(runtime, state, context.confirmedTick, authority, reenter);
        if (!transition)
        {
            report.status = runtime.transitionLimitExceeded ? AIStateExecutionStatus::TransitionLimitExceeded
                                                            : AIStateExecutionStatus::Rejected;
            return report;
        }
        return processTransition(runtime, data, context, *transition, report);
    }

    [[nodiscard]] static AIStateExecutionReport update(AIStateMachineRuntime& runtime,
                                                       AIStateData& data,
                                                       const AIStateContext& context)
    {
        AIStateExecutionReport report;
        if (!runtime.initialized || !isValidState(runtime.currentState))
        {
            report.status = AIStateExecutionStatus::Rejected;
            return report;
        }

        if (runtime.temporaryActive && context.confirmedTick >= runtime.temporaryEndTickExclusive)
        {
            const auto expired = AIStateMachine::expireTemporaryState(runtime, context.confirmedTick);
            if (!expired)
            {
                report.status = runtime.transitionLimitExceeded ? AIStateExecutionStatus::TransitionLimitExceeded
                                                                : AIStateExecutionStatus::Rejected;
                return report;
            }
            report = processTransition(runtime, data, context, *expired, report);
            if (report.status == AIStateExecutionStatus::Unsupported ||
                report.status == AIStateExecutionStatus::TransitionLimitExceeded)
            {
                return report;
            }
        }

        if (AIStateMachine::isSleeping(runtime, context.confirmedTick))
        {
            report.status = AIStateExecutionStatus::Sleeping;
            return report;
        }

        if (runtime.wakeTick != 0)
            AIStateMachine::wake(runtime, AIWakeReason::Deadline);
        return processStep(runtime, data, context, dispatchStateUpdate(runtime.currentState, data, context), report);
    }

private:
    static void appendTransition(AIStateExecutionReport& report, const AIStateTransition& transition) noexcept
    {
        report.lastCommittedTransition = transition;
        report.hasTransition = true;
        if (report.transitionCount < AIStateMachineRuntime::MaximumTransitionsPerTick)
            ++report.transitionCount;
    }

    [[nodiscard]] static AIStateStepResult beginCommittedState(AIStateMachineRuntime& runtime,
                                                               AIStateData& data,
                                                               const AIStateContext& context,
                                                               const AIStateTransition& transition)
    {
        if (isValidState(transition.from))
            dispatchStateExit(transition.from, data, transition.exitReason, context);

        if (transition.from == AIStateId::Dead && transition.to != AIStateId::Dead &&
            runtime.lock == AIStateMachineLock::Terminal)
        {
            AIStateMachine::setLock(runtime, AIStateMachineLock::Unlocked);
        }

        data.activate(transition.to, context.confirmedTick);
        const AIStateDescriptor* descriptor = descriptorFor(transition.to);
        if (descriptor && descriptor->terminal)
            AIStateMachine::setLock(runtime, AIStateMachineLock::Terminal);
        return dispatchStateEnter(transition.to, data, context);
    }

    [[nodiscard]] static AIStateExecutionReport processTransition(AIStateMachineRuntime& runtime,
                                                                  AIStateData& data,
                                                                  const AIStateContext& context,
                                                                  const AIStateTransition& transition,
                                                                  AIStateExecutionReport report)
    {
        appendTransition(report, transition);
        report.status = AIStateExecutionStatus::Transitioned;
        return processStep(runtime, data, context, beginCommittedState(runtime, data, context, transition), report);
    }

    [[nodiscard]] static AIStateExecutionReport processStep(AIStateMachineRuntime& runtime,
                                                            AIStateData& data,
                                                            const AIStateContext& context,
                                                            AIStateStepResult step,
                                                            AIStateExecutionReport report)
    {
        while (true)
        {
            switch (step.kind)
            {
            case AIStateStepKind::Continue:
                report.status = report.transitionCount == 0 ? AIStateExecutionStatus::Running
                                                            : AIStateExecutionStatus::Transitioned;
                return report;
            case AIStateStepKind::SleepUntil:
                AIStateMachine::sleepUntil(runtime, step.wakeTick);
                report.status = AIStateExecutionStatus::Sleeping;
                return report;
            case AIStateStepKind::Blocked:
                AIStateMachine::sleepUntil(runtime, std::numeric_limits<uint64_t>::max());
                report.status = AIStateExecutionStatus::Blocked;
                return report;
            case AIStateStepKind::Unsupported:
                report.status = AIStateExecutionStatus::Unsupported;
                return report;
            case AIStateStepKind::Transition:
            {
                const AIStateTransitionAuthority authority = step.target == AIStateId::Dead
                                                                 ? AIStateTransitionAuthority::Terminal
                                                                 : AIStateTransitionAuthority::Internal;
                const auto transition =
                    AIStateMachine::setState(runtime, step.target, context.confirmedTick, authority);
                if (!transition)
                {
                    report.status = runtime.transitionLimitExceeded ? AIStateExecutionStatus::TransitionLimitExceeded
                                                                    : AIStateExecutionStatus::Rejected;
                    return report;
                }
                appendTransition(report, *transition);
                step = beginCommittedState(runtime, data, context, *transition);
                break;
            }
            case AIStateStepKind::Success:
            case AIStateStepKind::Failure:
            {
                const AIStateDescriptor* descriptor = descriptorFor(runtime.currentState);
                if (!descriptor)
                {
                    report.status = AIStateExecutionStatus::Unsupported;
                    return report;
                }
                const auto transition = AIStateMachine::complete(
                    runtime,
                    step.kind == AIStateStepKind::Success ? AIStateOutcome::Success : AIStateOutcome::Failure,
                    descriptor->successState,
                    descriptor->failureState,
                    context.confirmedTick);
                if (!transition)
                {
                    report.status = runtime.transitionLimitExceeded ? AIStateExecutionStatus::TransitionLimitExceeded
                                                                    : AIStateExecutionStatus::Rejected;
                    return report;
                }
                appendTransition(report, *transition);
                step = beginCommittedState(runtime, data, context, *transition);
                break;
            }
            }
        }
    }
};

} // namespace engine::ai
