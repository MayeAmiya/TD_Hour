#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "core/container/container_types.h"
#include "game/object/ai/states/core/AICoreStates.h"
#include "game/object/ai/runtime/AIStateFamilySoAStorage.h"

namespace engine::ai
{

struct AICoreStateSoAKernelInput final
{
    uint64_t confirmedTick = 0;
    uint32_t idleTargetScanIntervalTicks = 3;
    uint32_t forceIdleBeforeAcquireTicks = 1;
    container::Span<const uint32_t> idleTargetScanIntervalTicksBySlot;
    // Empty means every slot in this family pass is scheduled. A populated
    // mask lets the wake scheduler skip sleeping slots without compacting.
    container::Span<const uint8_t> scheduled{};
    container::Span<const uint8_t> effectivelyDead;
    container::Span<const uint8_t> idleAutoAcquireEnabled;
    container::Span<const uint8_t> idleTargetAvailable;
    container::Span<AIStateStepResult> results;
};

namespace detail
{

[[nodiscard]] inline bool hasAlignedCoreStateSoASpans(const AIStateFamilySoAStorage& storage,
                                                      const AICoreStateSoAKernelInput& input) noexcept
{
    const size_t count = storage.size();
    return (input.scheduled.empty() || input.scheduled.size() == count) && input.effectivelyDead.size() == count &&
           input.idleAutoAcquireEnabled.size() == count && input.idleTargetAvailable.size() == count &&
           input.results.size() == count;
}

[[nodiscard]] constexpr bool coreStateSoAScheduled(const AICoreStateSoAKernelInput& input, size_t slot) noexcept
{
    return input.scheduled.empty() || input.scheduled[slot] != 0;
}

[[nodiscard]] constexpr bool coreStateSoAFact(uint8_t value) noexcept
{
    return value != 0;
}

} // namespace detail

// A false return means the caller-provided spans were not slot-aligned with
// storage. In that case no payload or result slot is written.
[[nodiscard]] inline bool enterIdleSoA(AIStateFamilySoAStorage& storage,
                                       const AICoreStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedCoreStateSoASpans(storage, input))
        return false;

    const auto runtimes = storage.runtimes();
    const auto payloadStates = storage.payloadStates();
    auto payloads = storage.idle();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::coreStateSoAScheduled(input, slot))
            continue;
        if (runtimes[slot].currentState != AIStateId::Idle)
            continue;
        if (payloadStates[slot] != AIStateId::Idle)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        payloads[slot].nextTargetScanTick = saturatingTickAdd(
            input.confirmedTick, input.forceIdleBeforeAcquireTicks);
        input.results[slot] = detail::coreStateSoAFact(input.effectivelyDead[slot])
                                  ? AIStateStepResult::transitionTo(AIStateId::Dead)
                                  : AIStateStepResult::continueState();
    }
    return true;
}

[[nodiscard]] inline bool updateIdleSoA(AIStateFamilySoAStorage& storage,
                                        const AICoreStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedCoreStateSoASpans(storage, input))
        return false;

    const auto runtimes = storage.runtimes();
    const auto payloadStates = storage.payloadStates();
    auto payloads = storage.idle();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::coreStateSoAScheduled(input, slot))
            continue;
        if (runtimes[slot].currentState != AIStateId::Idle)
            continue;
        if (detail::coreStateSoAFact(input.effectivelyDead[slot]))
        {
            input.results[slot] = AIStateStepResult::transitionTo(AIStateId::Dead);
            continue;
        }
        if (payloadStates[slot] != AIStateId::Idle)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        if (!detail::coreStateSoAFact(input.idleAutoAcquireEnabled[slot]))
        {
            input.results[slot] = AIStateStepResult::sleepUntil(std::numeric_limits<uint64_t>::max());
            continue;
        }

        AIIdleStatePayload& payload = payloads[slot];
        if (input.confirmedTick < payload.nextTargetScanTick)
        {
            input.results[slot] = AIStateStepResult::sleepUntil(payload.nextTargetScanTick);
            continue;
        }

        const uint32_t authoredInterval =
            input.idleTargetScanIntervalTicksBySlot.empty()
            ? input.idleTargetScanIntervalTicks
            : input.idleTargetScanIntervalTicksBySlot[slot];
        const uint64_t interval = authoredInterval == 0
                                      ? uint64_t{1}
                                      : static_cast<uint64_t>(authoredInterval);
        payload.nextTargetScanTick = saturatingTickAdd(input.confirmedTick, interval);
        input.results[slot] = detail::coreStateSoAFact(input.idleTargetAvailable[slot])
                                  ? AIStateStepResult::transitionTo(AIStateId::AttackObject)
                                  : AIStateStepResult::sleepUntil(payload.nextTargetScanTick);
    }
    return true;
}

[[nodiscard]] inline bool enterWaitSoA(AIStateFamilySoAStorage& storage,
                                       const AICoreStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedCoreStateSoASpans(storage, input))
        return false;

    const auto runtimes = storage.runtimes();
    const auto payloadStates = storage.payloadStates();
    const auto payloads = storage.wait();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::coreStateSoAScheduled(input, slot))
            continue;
        if (runtimes[slot].currentState != AIStateId::Wait)
            continue;
        if (payloadStates[slot] != AIStateId::Wait)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        input.results[slot] = input.confirmedTick >= payloads[slot].endTick
                                  ? AIStateStepResult::success()
                                  : AIStateStepResult::sleepUntil(payloads[slot].endTick);
    }
    return true;
}

[[nodiscard]] inline bool updateWaitSoA(AIStateFamilySoAStorage& storage,
                                        const AICoreStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedCoreStateSoASpans(storage, input))
        return false;

    const auto runtimes = storage.runtimes();
    const auto payloadStates = storage.payloadStates();
    const auto payloads = storage.wait();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::coreStateSoAScheduled(input, slot))
            continue;
        if (runtimes[slot].currentState != AIStateId::Wait)
            continue;
        if (detail::coreStateSoAFact(input.effectivelyDead[slot]))
        {
            input.results[slot] = AIStateStepResult::transitionTo(AIStateId::Dead);
            continue;
        }
        if (payloadStates[slot] != AIStateId::Wait)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        input.results[slot] = input.confirmedTick >= payloads[slot].endTick
                                  ? AIStateStepResult::success()
                                  : AIStateStepResult::sleepUntil(payloads[slot].endTick);
    }
    return true;
}

[[nodiscard]] inline bool enterBusySoA(AIStateFamilySoAStorage& storage,
                                       const AICoreStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedCoreStateSoASpans(storage, input))
        return false;

    const auto runtimes = storage.runtimes();
    const auto payloadStates = storage.payloadStates();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::coreStateSoAScheduled(input, slot))
            continue;
        if (runtimes[slot].currentState != AIStateId::Busy)
            continue;
        if (payloadStates[slot] != AIStateId::Busy)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        input.results[slot] = detail::coreStateSoAFact(input.effectivelyDead[slot])
                                  ? AIStateStepResult::transitionTo(AIStateId::Dead)
                                  : AIStateStepResult::continueState();
    }
    return true;
}

[[nodiscard]] inline bool updateBusySoA(AIStateFamilySoAStorage& storage,
                                        const AICoreStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedCoreStateSoASpans(storage, input))
        return false;

    const auto runtimes = storage.runtimes();
    const auto payloadStates = storage.payloadStates();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::coreStateSoAScheduled(input, slot))
            continue;
        if (runtimes[slot].currentState != AIStateId::Busy)
            continue;
        if (payloadStates[slot] != AIStateId::Busy)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        input.results[slot] = detail::coreStateSoAFact(input.effectivelyDead[slot])
                                  ? AIStateStepResult::transitionTo(AIStateId::Dead)
                                  : AIStateStepResult::sleepUntil(std::numeric_limits<uint64_t>::max());
    }
    return true;
}

[[nodiscard]] inline bool enterDeadSoA(AIStateFamilySoAStorage& storage,
                                       const AICoreStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedCoreStateSoASpans(storage, input))
        return false;

    const auto runtimes = storage.runtimes();
    const auto payloadStates = storage.payloadStates();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::coreStateSoAScheduled(input, slot))
            continue;
        if (runtimes[slot].currentState != AIStateId::Dead)
            continue;
        input.results[slot] = payloadStates[slot] == AIStateId::Dead
                                  ? AIStateStepResult::sleepUntil(std::numeric_limits<uint64_t>::max())
                                  : AIStateStepResult::unsupported();
    }
    return true;
}

[[nodiscard]] inline bool updateDeadSoA(AIStateFamilySoAStorage& storage,
                                        const AICoreStateSoAKernelInput& input) noexcept
{
    return enterDeadSoA(storage, input);
}

} // namespace engine::ai
