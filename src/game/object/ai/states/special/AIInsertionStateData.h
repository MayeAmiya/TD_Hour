#pragma once
#include <compare>
#include <cstdint>
#include "core/container/container_types.h"
#include "game/object/ai/contracts/AIStateCommands.h"
namespace engine::ai
{
struct AIInsertionOperationHandle final
{uint64_t value=0;[[nodiscard]]constexpr bool isValid()const noexcept{return value!=0;}
 explicit constexpr operator bool()const noexcept{return isValid();}constexpr auto operator<=>(const AIInsertionOperationHandle&)const noexcept=default;};

// CombatDrop is owned by a concrete ChinookAI module.  A generation by itself
// is not an operation identity because every module advances its own counter
// from the same initial value.  Keep the module ordinal in the upper half so
// Poll/OrderChildRappel/Cancel can never bind to a sibling module that happens
// to be on the same generation.
struct AIInsertionOperationIdentity final
{
 uint32_t ownerIndex=0;uint32_t generation=0;bool valid=false;
 [[nodiscard]]explicit constexpr operator bool()const noexcept{return valid;}
 constexpr auto operator<=>(const AIInsertionOperationIdentity&)const noexcept=default;
};

[[nodiscard]] constexpr AIInsertionOperationHandle makeAIInsertionOperationHandle(
    uint32_t ownerIndex,uint32_t generation) noexcept
{
 if(generation==0||ownerIndex==UINT32_MAX)return {};
 return {((static_cast<uint64_t>(ownerIndex)+1u)<<32)|generation};
}

[[nodiscard]] constexpr AIInsertionOperationIdentity decodeAIInsertionOperationHandle(
    AIInsertionOperationHandle handle) noexcept
{
 const uint32_t ownerOrdinal=static_cast<uint32_t>(handle.value>>32);
 const uint32_t generation=static_cast<uint32_t>(handle.value);
 if(ownerOrdinal==0||generation==0)return {};
 return {.ownerIndex=ownerOrdinal-1u,.generation=generation,.valid=true};
}
enum class AIRappelInsertionPhase:uint8_t{Inactive,Descending,AwaitingBuildingResolution,Landed};
enum class AICombatDropInsertionPhase:uint8_t
{
 Inactive,
 ApproachWaitingForPath,
 ApproachFollowingPath,
 BeginPending,
 PollPending,
 Finished
};
struct AIInsertionStateSoAColumns final
{container::Span<uint64_t> requestTick;container::Span<uint32_t> requestSequence;container::Span<ObjectId> rappelTarget;
 container::Span<uint8_t> rappelTargetIsBuilding;container::Span<int64_t> rappelDestinationZRaw;container::Span<int64_t> rappelSpeedRaw;
 container::Span<AIRappelInsertionPhase> rappelPhase;container::Span<AIInsertionOperationHandle> combatDropOperation;
 container::Span<uint32_t> combatDropNextEventSequence;container::Span<AICombatDropInsertionPhase> combatDropPhase;
 container::Span<PathHandle> combatDropPath;container::Span<uint64_t> combatDropSourceOrderRevision;
 container::Span<uint32_t> combatDropPathGeneration;container::Span<int64_t> combatDropOldPreferredHeightRaw;
 container::Span<uint8_t> combatDropPathRequestIssued;container::Span<uint8_t> combatDropApproachConfigured;};
struct AIInsertionStatePayload final
{AIStateRequestId request;ObjectId rappelTarget=INVALID_OBJECT_ID;bool rappelTargetIsBuilding=false;
 int64_t rappelDestinationZRaw=0;int64_t rappelSpeedRaw=0;AIRappelInsertionPhase rappelPhase=AIRappelInsertionPhase::Inactive;
 AIInsertionOperationHandle combatDropOperation;uint32_t combatDropNextEventSequence=0;
 AICombatDropInsertionPhase combatDropPhase=AICombatDropInsertionPhase::Inactive;
 PathHandle combatDropPath;uint64_t combatDropSourceOrderRevision=0;uint32_t combatDropPathGeneration=1;
 int64_t combatDropOldPreferredHeightRaw=0;bool combatDropPathRequestIssued=false;
 bool combatDropApproachConfigured=false;
 constexpr bool operator==(const AIInsertionStatePayload&)const noexcept=default;};
}
