#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "core/container/container_types.h"
#include "game/object/ai/runtime/AIStateFamilySoAStorage.h"
#include "game/object/ai/runtime/AIStateStep.h"

namespace engine::ai
{

enum class AIHackInternetCommandKind : uint8_t
{
    SetUnpacking,
    ClearUnpacking,
    SetHacking,
    ClearHacking,
    SetPacking,
    ClearPacking,
    DepositCash,
    RecordIncome,
    GrantExperience,
    PublishPresentation,
    ReplayDeferredOrder,
};

struct AIHackInternetCommand final
{
    ObjectId subject = INVALID_OBJECT_ID;
    AIStateRequestId stateRequest;
    AIHackInternetCommandKind kind = AIHackInternetCommandKind::SetUnpacking;
    uint64_t sourceOrderRevision = 0;
    int64_t amount = 0;
    uint64_t confirmedTick = 0;
};

struct AIHackInternetCommandBuffer final
{
    static constexpr size_t Capacity = 8;
    container::Array<AIHackInternetCommand, Capacity> values{};
    size_t count = 0;
    bool overflowed = false;
    [[nodiscard]] constexpr bool hasCapacity(size_t additional) const noexcept
    { return count <= values.size() && additional <= values.size() - count; }
    [[nodiscard]] bool push(const AIHackInternetCommand& value) noexcept
    {
        if (!hasCapacity(1)) { overflowed = true; return false; }
        values[count++] = value;
        return true;
    }
    void clear() noexcept { count = 0; overflowed = false; }
};

struct AIHackInternetStateSoAKernelInput final
{
    uint64_t confirmedTick = 0;
    container::Span<const uint8_t> scheduled{};
    container::Span<const uint8_t> effectivelyDead;
    container::Span<const uint8_t> disabled;
    container::Span<const AIBehaviorProfileHandle> profile;
    container::Span<const uint64_t> profileRevision;
    container::Span<const uint32_t> unpackDurationTicks;
    container::Span<const uint32_t> packDurationTicks;
    container::Span<const uint32_t> payoutPeriodTicks;
    container::Span<const int64_t> payoutAmount;
    container::Span<const int64_t> experienceAmount;
    container::Span<const uint64_t> newestDeferredOrderRevision;
    container::Span<AIHackInternetCommandBuffer> commands;
    container::Span<AIStateStepResult> results;
};

namespace hack_detail
{
[[nodiscard]] constexpr uint64_t addTick(uint64_t tick, uint64_t delta) noexcept
{
    return delta > std::numeric_limits<uint64_t>::max() - tick
               ? std::numeric_limits<uint64_t>::max()
               : tick + delta;
}
[[nodiscard]] inline bool aligned(const AIStateFamilySoAStorage& storage,
                                  const AIHackInternetStateSoAKernelInput& input) noexcept
{
    const size_t n = storage.size();
    return storage.hackInternet().size() == n &&
           (input.scheduled.empty() || input.scheduled.size() == n) &&
           input.effectivelyDead.size() == n && input.disabled.size() == n && input.profile.size() == n &&
           input.profileRevision.size() == n && input.unpackDurationTicks.size() == n &&
           input.packDurationTicks.size() == n && input.payoutPeriodTicks.size() == n &&
           input.payoutAmount.size() == n && input.experienceAmount.size() == n &&
           input.newestDeferredOrderRevision.size() == n && input.commands.size() == n && input.results.size() == n;
}
[[nodiscard]] constexpr bool scheduled(const AIHackInternetStateSoAKernelInput& input, size_t slot) noexcept
{ return input.scheduled.empty() || input.scheduled[slot] != 0; }
[[nodiscard]] constexpr size_t requiredUpdateCommands(AIHackInternetPhase phase,
                                                       uint64_t tick,
                                                       uint64_t deadline,
                                                       bool disabled,
                                                       uint64_t deferred) noexcept
{
    if (deferred != 0 && phase != AIHackInternetPhase::Packing) return 2;
    if (phase == AIHackInternetPhase::Unpacking && tick >= deadline) return 2;
    if (phase == AIHackInternetPhase::Hacking && !disabled && tick >= deadline) return 4;
    if (phase == AIHackInternetPhase::Packing && tick >= deadline) return deferred != 0 ? 2 : 1;
    return 0;
}
inline void emit(AIHackInternetCommandBuffer& output,
                 ObjectId subject,
                 AIStateRequestId request,
                 AIHackInternetCommandKind kind,
                 uint64_t tick,
                 uint64_t revision = 0,
                 int64_t amount = 0) noexcept
{
    static_cast<void>(output.push({.subject = subject, .stateRequest = request, .kind = kind,
                                   .sourceOrderRevision = revision, .amount = amount,
                                   .confirmedTick = tick}));
}
} // namespace hack_detail

[[nodiscard]] inline bool enterHackInternetSoA(AIStateFamilySoAStorage& storage,
                                                const AIHackInternetStateSoAKernelInput& input) noexcept
{
    if (!hack_detail::aligned(storage, input)) return false;
    const auto subjects = storage.subjects(); const auto runtimes = storage.runtimes();
    const auto states = storage.payloadStates(); const auto parameters = storage.parameters();
    auto& columns = storage.hackInternet();
    for (size_t slot=0; slot<storage.size(); ++slot)
    {
        if (!hack_detail::scheduled(input,slot) || runtimes[slot].currentState!=AIStateId::HackInternet) continue;
        if (input.effectivelyDead[slot] || states[slot]!=AIStateId::HackInternet || !subjects[slot] || !columns.request(slot).isValid() ||
            parameters[slot].sourceOrderRevision==0 || !input.profile[slot] || input.profileRevision[slot]==0 ||
            input.payoutPeriodTicks[slot]==0)
            continue;
        if (!input.commands[slot].hasCapacity(1)) return false;
    }
    for (size_t slot=0; slot<storage.size(); ++slot)
    {
        if (!hack_detail::scheduled(input,slot) || runtimes[slot].currentState!=AIStateId::HackInternet) continue;
        if(input.effectivelyDead[slot]){input.results[slot]=AIStateStepResult::transitionTo(AIStateId::Dead);continue;}
        if (states[slot]!=AIStateId::HackInternet || !subjects[slot] || !columns.request(slot).isValid() ||
            parameters[slot].sourceOrderRevision==0 || !input.profile[slot] || input.profileRevision[slot]==0 ||
            input.payoutPeriodTicks[slot]==0) continue;
        columns.setSourceRevision(slot,parameters[slot].sourceOrderRevision);
        columns.setProfile(slot,input.profile[slot],input.profileRevision[slot]);
        columns.setPhase(slot,AIHackInternetPhase::Unpacking);
        columns.setPhaseEndTick(slot,hack_detail::addTick(input.confirmedTick,input.unpackDurationTicks[slot]));
        columns.setNextPayoutTick(slot,0); columns.setDeferredOrderRevision(slot,0);
        hack_detail::emit(input.commands[slot],subjects[slot],columns.request(slot),
                          AIHackInternetCommandKind::SetUnpacking,input.confirmedTick);
        input.results[slot]=AIStateStepResult::continueState();
    }
    return true;
}

[[nodiscard]] inline bool updateHackInternetSoA(AIStateFamilySoAStorage& storage,
                                                 const AIHackInternetStateSoAKernelInput& input) noexcept
{
    if (!hack_detail::aligned(storage,input)) return false;
    const auto subjects=storage.subjects(); const auto runtimes=storage.runtimes();
    const auto states=storage.payloadStates(); auto& columns=storage.hackInternet();
    for(size_t slot=0;slot<storage.size();++slot)
    {
        if(!hack_detail::scheduled(input,slot)||runtimes[slot].currentState!=AIStateId::HackInternet||
           input.effectivelyDead[slot])continue;
        if(states[slot]!=AIStateId::HackInternet)continue;
        const uint64_t incoming=input.newestDeferredOrderRevision[slot];
        const uint64_t deferred=incoming>columns.sourceRevision(slot)?incoming:columns.deferredOrderRevision(slot);
        const size_t required=hack_detail::requiredUpdateCommands(columns.phase(slot),input.confirmedTick,
            columns.phase(slot)==AIHackInternetPhase::Hacking?columns.nextPayoutTick(slot):columns.phaseEndTick(slot),
            input.disabled[slot]!=0,deferred);
        if(!input.commands[slot].hasCapacity(required))return false;
    }
    for(size_t slot=0;slot<storage.size();++slot)
    {
        if(!hack_detail::scheduled(input,slot)||runtimes[slot].currentState!=AIStateId::HackInternet)continue;
        if(input.effectivelyDead[slot]){input.results[slot]=AIStateStepResult::transitionTo(AIStateId::Dead);continue;}
        if(states[slot]!=AIStateId::HackInternet){input.results[slot]=AIStateStepResult::unsupported();continue;}
        const AIStateRequestId request=columns.request(slot); const AIHackInternetPhase phase=columns.phase(slot);
        const uint64_t incoming=input.newestDeferredOrderRevision[slot];
        if(incoming>columns.sourceRevision(slot)&&incoming>columns.deferredOrderRevision(slot))
            columns.setDeferredOrderRevision(slot,incoming);
        const uint64_t deferred=columns.deferredOrderRevision(slot);
        if(deferred!=0&&phase!=AIHackInternetPhase::Packing)
        {
            hack_detail::emit(input.commands[slot],subjects[slot],request,
                phase==AIHackInternetPhase::Hacking?AIHackInternetCommandKind::ClearHacking:
                                                     AIHackInternetCommandKind::ClearUnpacking,input.confirmedTick);
            hack_detail::emit(input.commands[slot],subjects[slot],request,AIHackInternetCommandKind::SetPacking,input.confirmedTick);
            columns.setPhase(slot,AIHackInternetPhase::Packing);
            columns.setPhaseEndTick(slot,hack_detail::addTick(input.confirmedTick,input.packDurationTicks[slot]));
            input.results[slot]=AIStateStepResult::continueState();continue;
        }
        if(phase==AIHackInternetPhase::Unpacking)
        {
            if(input.confirmedTick<columns.phaseEndTick(slot)){input.results[slot]=AIStateStepResult::continueState();continue;}
            hack_detail::emit(input.commands[slot],subjects[slot],request,AIHackInternetCommandKind::ClearUnpacking,input.confirmedTick);
            hack_detail::emit(input.commands[slot],subjects[slot],request,AIHackInternetCommandKind::SetHacking,input.confirmedTick);
            columns.setPhase(slot,AIHackInternetPhase::Hacking);
            columns.setNextPayoutTick(slot,hack_detail::addTick(input.confirmedTick,input.payoutPeriodTicks[slot]));
            input.results[slot]=AIStateStepResult::continueState();continue;
        }
        if(phase==AIHackInternetPhase::Hacking)
        {
            if(input.disabled[slot]||input.confirmedTick<columns.nextPayoutTick(slot))
            {input.results[slot]=AIStateStepResult::continueState();continue;}
            hack_detail::emit(input.commands[slot],subjects[slot],request,AIHackInternetCommandKind::DepositCash,input.confirmedTick,0,input.payoutAmount[slot]);
            hack_detail::emit(input.commands[slot],subjects[slot],request,AIHackInternetCommandKind::RecordIncome,input.confirmedTick,0,input.payoutAmount[slot]);
            hack_detail::emit(input.commands[slot],subjects[slot],request,AIHackInternetCommandKind::GrantExperience,input.confirmedTick,0,input.experienceAmount[slot]);
            hack_detail::emit(input.commands[slot],subjects[slot],request,AIHackInternetCommandKind::PublishPresentation,input.confirmedTick,0,input.payoutAmount[slot]);
            columns.setNextPayoutTick(slot,hack_detail::addTick(input.confirmedTick,input.payoutPeriodTicks[slot]));
            input.results[slot]=AIStateStepResult::continueState();continue;
        }
        if(input.confirmedTick<columns.phaseEndTick(slot)){input.results[slot]=AIStateStepResult::continueState();continue;}
        hack_detail::emit(input.commands[slot],subjects[slot],request,AIHackInternetCommandKind::ClearPacking,input.confirmedTick);
        if(deferred!=0)hack_detail::emit(input.commands[slot],subjects[slot],request,AIHackInternetCommandKind::ReplayDeferredOrder,input.confirmedTick,deferred);
        input.results[slot]=AIStateStepResult::success();
    }
    return true;
}

[[nodiscard]] inline bool canExitHackInternetSoA(const AIStateFamilySoAStorage& storage,
                                                  const AIHackInternetStateSoAKernelInput& input) noexcept
{
    if(!hack_detail::aligned(storage,input))return false;
    const auto states=storage.payloadStates();
    for(size_t slot=0;slot<storage.size();++slot)
        if(hack_detail::scheduled(input,slot)&&states[slot]==AIStateId::HackInternet&&!input.commands[slot].hasCapacity(1))return false;
    return true;
}

[[nodiscard]] inline bool exitHackInternetSoA(AIStateFamilySoAStorage& storage,
                                               const AIHackInternetStateSoAKernelInput& input) noexcept
{
    if(!canExitHackInternetSoA(storage,input))return false;
    const auto subjects=storage.subjects();const auto states=storage.payloadStates();auto& columns=storage.hackInternet();
    for(size_t slot=0;slot<storage.size();++slot)
    {
        if(!hack_detail::scheduled(input,slot)||states[slot]!=AIStateId::HackInternet)continue;
        const AIHackInternetCommandKind kind=columns.phase(slot)==AIHackInternetPhase::Unpacking?
            AIHackInternetCommandKind::ClearUnpacking:columns.phase(slot)==AIHackInternetPhase::Hacking?
            AIHackInternetCommandKind::ClearHacking:AIHackInternetCommandKind::ClearPacking;
        hack_detail::emit(input.commands[slot],subjects[slot],columns.request(slot),kind,input.confirmedTick);
    }
    return true;
}

} // namespace engine::ai
