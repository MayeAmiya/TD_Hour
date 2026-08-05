#pragma once

#include <cstdint>
#include <limits>
#include <optional>

#include "game/object/ai/runtime/AIStateTypes.h"

namespace engine::ai
{

struct AIStateMachineRuntime final
{
    static constexpr uint8_t MaximumTransitionsPerTick = 8;

    AIStateId currentState = AIStateId::Invalid;
    AIStateId previousState = AIStateId::Invalid;
    AIStateId defaultState = AIStateId::Idle;
    AIStateId temporaryResumeState = AIStateId::Invalid;

    uint64_t enteredTick = 0;
    uint64_t wakeTick = 0;
    uint64_t temporaryEndTickExclusive = 0;
    uint64_t revision = 0;
    uint64_t transitionBudgetTick = 0;

    AISubstateDomain substateDomain = AISubstateDomain::None;
    AISubstateId substate = INVALID_AI_SUBSTATE;
    AIStateMachineLock lock = AIStateMachineLock::Unlocked;
    AIWakeReason lastWakeReason = AIWakeReason::None;
    AIStateTransitionReason lastTransitionReason =
        AIStateTransitionReason::Initialize;
    uint8_t transitionsThisTick = 0;

    bool initialized = false;
    bool temporaryActive = false;
    bool transitionLimitExceeded = false;
};

static_assert(sizeof(AIStateMachineRuntime) <= 64);

struct AIStateTransition final
{
    AIStateId from = AIStateId::Invalid;
    AIStateId to = AIStateId::Invalid;
    AIStateTransitionReason reason = AIStateTransitionReason::Explicit;
    AIStateExitReason exitReason = AIStateExitReason::Transition;
    uint64_t confirmedTick = 0;
    uint64_t revision = 0;
    bool reentered = false;
};

class AIStateMachine final
{
public:
    [[nodiscard]] static std::optional<AIStateTransition> initialize(AIStateMachineRuntime& runtime,
                                                                     AIStateId defaultState,
                                                                     uint64_t confirmedTick) noexcept
    {
        if (!isValidState(defaultState))
            return std::nullopt;

        runtime = {};
        runtime.defaultState = defaultState;
        runtime.initialized = true;
        return commit(runtime,
                      defaultState,
                      AIStateTransitionReason::Initialize,
                      AIStateExitReason::Transition,
                      confirmedTick,
                      true);
    }

    [[nodiscard]] static bool canTransition(const AIStateMachineRuntime& runtime,
                                            AIStateTransitionAuthority authority) noexcept
    {
        switch (runtime.lock)
        {
        case AIStateMachineLock::Unlocked:
            return true;
        case AIStateMachineLock::ExternalTransitionsBlocked:
            return authority != AIStateTransitionAuthority::External;
        case AIStateMachineLock::Terminal:
            return authority == AIStateTransitionAuthority::Terminal;
        }
        return false;
    }

    [[nodiscard]] static std::optional<AIStateTransition> setState(
        AIStateMachineRuntime& runtime,
        AIStateId target,
        uint64_t confirmedTick,
        AIStateTransitionAuthority authority = AIStateTransitionAuthority::External,
        bool reenter = false) noexcept
    {
        if (!runtime.initialized || !isValidState(target) || !canTransition(runtime, authority))
            return std::nullopt;

        const AIStateExitReason exitReason = authority == AIStateTransitionAuthority::Terminal
                                                 ? AIStateExitReason::Terminal
                                             : runtime.temporaryActive ? AIStateExitReason::Interrupted
                                             : reenter                 ? AIStateExitReason::Reenter
                                                                       : AIStateExitReason::Transition;
        auto transition =
            commit(runtime, target, AIStateTransitionReason::Explicit, exitReason, confirmedTick, reenter);
        if (transition)
            clearTemporary(runtime);
        return transition;
    }

    [[nodiscard]] static std::optional<AIStateTransition> reset(
        AIStateMachineRuntime& runtime,
        uint64_t confirmedTick,
        AIStateTransitionAuthority authority = AIStateTransitionAuthority::External) noexcept
    {
        if (!runtime.initialized || !isValidState(runtime.defaultState) || !canTransition(runtime, authority))
            return std::nullopt;

        auto transition = commit(runtime,
                                 runtime.defaultState,
                                 AIStateTransitionReason::Reset,
                                 AIStateExitReason::Reset,
                                 confirmedTick,
                                 true);
        if (transition)
            clearTemporary(runtime);
        return transition;
    }

    [[nodiscard]] static std::optional<AIStateTransition> complete(AIStateMachineRuntime& runtime,
                                                                   AIStateOutcome outcome,
                                                                   AIStateId successState,
                                                                   AIStateId failureState,
                                                                   uint64_t confirmedTick) noexcept
    {
        if (!runtime.initialized || (outcome != AIStateOutcome::Success && outcome != AIStateOutcome::Failure) ||
            !canTransition(runtime, AIStateTransitionAuthority::Internal))
        {
            return std::nullopt;
        }

        if (runtime.temporaryActive)
        {
            const AIStateId resumeState = runtime.temporaryResumeState;
            if (!isValidState(resumeState))
                return std::nullopt;
            auto transition = commit(runtime,
                                     resumeState,
                                     AIStateTransitionReason::TemporaryCompleted,
                                     AIStateExitReason::TemporaryCompleted,
                                     confirmedTick,
                                     true);
            if (transition)
                clearTemporary(runtime);
            return transition;
        }

        const bool succeeded = outcome == AIStateOutcome::Success;
        const AIStateId target = succeeded ? successState : failureState;
        if (!isValidState(target))
            return std::nullopt;
        return commit(runtime,
                      target,
                      succeeded ? AIStateTransitionReason::Success : AIStateTransitionReason::Failure,
                      AIStateExitReason::Transition,
                      confirmedTick,
                      true);
    }

    [[nodiscard]] static std::optional<AIStateTransition> setTemporaryState(AIStateMachineRuntime& runtime,
                                                                            AIStateId target,
                                                                            uint64_t confirmedTick,
                                                                            uint64_t durationTicks) noexcept
    {
        if (!runtime.initialized || !isValidState(runtime.currentState) || !isValidState(target) ||
            !canTransition(runtime, AIStateTransitionAuthority::Internal))
        {
            return std::nullopt;
        }

        const AIStateId resumeState = runtime.temporaryActive ? runtime.temporaryResumeState : runtime.currentState;
        auto transition = commit(
            runtime, target, AIStateTransitionReason::Temporary, AIStateExitReason::Interrupted, confirmedTick, true);
        if (!transition)
            return std::nullopt;

        runtime.temporaryResumeState = resumeState;
        runtime.temporaryEndTickExclusive = saturatingAdd(confirmedTick, durationTicks);
        runtime.temporaryActive = true;
        return transition;
    }

    [[nodiscard]] static std::optional<AIStateTransition> expireTemporaryState(AIStateMachineRuntime& runtime,
                                                                               uint64_t confirmedTick) noexcept
    {
        if (!runtime.initialized || !runtime.temporaryActive || confirmedTick < runtime.temporaryEndTickExclusive ||
            !canTransition(runtime, AIStateTransitionAuthority::Internal))
        {
            return std::nullopt;
        }

        const AIStateId resumeState = runtime.temporaryResumeState;
        if (!isValidState(resumeState))
            return std::nullopt;
        auto transition = commit(runtime,
                                 resumeState,
                                 AIStateTransitionReason::TemporaryExpired,
                                 AIStateExitReason::TemporaryExpired,
                                 confirmedTick,
                                 true);
        if (transition)
            clearTemporary(runtime);
        return transition;
    }

    static void setLock(AIStateMachineRuntime& runtime, AIStateMachineLock lock) noexcept
    {
        runtime.lock = lock;
    }

    static void setSubstate(AIStateMachineRuntime& runtime, AISubstateDomain domain, AISubstateId substate) noexcept
    {
        runtime.substateDomain = domain;
        runtime.substate = domain == AISubstateDomain::None ? INVALID_AI_SUBSTATE : substate;
    }

    static void clearSubstate(AIStateMachineRuntime& runtime) noexcept
    {
        runtime.substateDomain = AISubstateDomain::None;
        runtime.substate = INVALID_AI_SUBSTATE;
    }

    static void sleepUntil(AIStateMachineRuntime& runtime, uint64_t wakeTick) noexcept
    {
        runtime.wakeTick = wakeTick;
        runtime.lastWakeReason = AIWakeReason::None;
    }

    static void wake(AIStateMachineRuntime& runtime, AIWakeReason reason = AIWakeReason::ExternalCommand) noexcept
    {
        runtime.wakeTick = 0;
        runtime.lastWakeReason = reason;
    }

    [[nodiscard]] static bool isSleeping(const AIStateMachineRuntime& runtime, uint64_t confirmedTick) noexcept
    {
        return runtime.wakeTick != 0 && confirmedTick < runtime.wakeTick;
    }

private:
    [[nodiscard]] static constexpr uint64_t saturatingAdd(uint64_t value, uint64_t delta) noexcept
    {
        const uint64_t maximum = std::numeric_limits<uint64_t>::max();
        return delta > maximum - value ? maximum : value + delta;
    }

    static void clearTemporary(AIStateMachineRuntime& runtime) noexcept
    {
        runtime.temporaryResumeState = AIStateId::Invalid;
        runtime.temporaryEndTickExclusive = 0;
        runtime.temporaryActive = false;
    }

    [[nodiscard]] static bool consumeTransitionBudget(AIStateMachineRuntime& runtime, uint64_t confirmedTick) noexcept
    {
        if (runtime.transitionBudgetTick != confirmedTick)
        {
            runtime.transitionBudgetTick = confirmedTick;
            runtime.transitionsThisTick = 0;
            runtime.transitionLimitExceeded = false;
        }
        if (runtime.transitionsThisTick >= AIStateMachineRuntime::MaximumTransitionsPerTick)
        {
            runtime.transitionLimitExceeded = true;
            return false;
        }
        ++runtime.transitionsThisTick;
        return true;
    }

    [[nodiscard]] static std::optional<AIStateTransition> commit(AIStateMachineRuntime& runtime,
                                                                 AIStateId target,
                                                                 AIStateTransitionReason reason,
                                                                 AIStateExitReason exitReason,
                                                                 uint64_t confirmedTick,
                                                                 bool reenter) noexcept
    {
        if (!isValidState(target) || (runtime.currentState == target && !reenter) ||
            !consumeTransitionBudget(runtime, confirmedTick))
        {
            return std::nullopt;
        }

        const AIStateId from = runtime.currentState;
        runtime.previousState = from;
        runtime.currentState = target;
        runtime.enteredTick = confirmedTick;
        runtime.wakeTick = 0;
        runtime.lastTransitionReason = reason;
        clearSubstate(runtime);
        ++runtime.revision;

        return AIStateTransition{
            .from = from,
            .to = target,
            .reason = reason,
            .exitReason = exitReason,
            .confirmedTick = confirmedTick,
            .revision = runtime.revision,
            .reentered = from == target,
        };
    }
};

} // namespace engine::ai
