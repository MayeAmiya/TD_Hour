#pragma once

#include <cstdint>

#include "core/container/container_types.h"
#include "game/object/ai/contracts/AIStateCommands.h"
#include "game/object/ai/runtime/AIStateTypes.h"

namespace engine::ai
{
enum class AIDockPurpose : uint8_t { Dock, Repair };
enum class AIDockPhase : uint8_t
{ Inactive, Approach, WaitForClearance, AdvancePosition, MoveToEntry, MoveToDock,
  ProcessDock, MoveToExit, MoveToRally, Completed };
enum class AIDockMoveStage : uint8_t { None, Queue, Entry, Dock, Exit, Rally };
enum class AIDockRequestKind : uint8_t
{ None, ReserveApproach, PollClearance, AdvanceApproach, QueryEntryPosition, QueryDockPosition,
  ProcessAction, QueryExitPosition, QueryRallyPosition, BeginMove, NotifyApproachReached,
  NotifyEnterReached, NotifyDockReached, NotifyExitReached, EndMove, CancelDock, RestorePathing };

struct AIDockToken final
{
    ObjectId subject = INVALID_OBJECT_ID;
    ObjectId dock = INVALID_OBJECT_ID;
    AIStateRequestId stateRequest{};
    AIDockPurpose purpose = AIDockPurpose::Dock;
    [[nodiscard]] constexpr bool isValid() const noexcept
    { return subject && dock && stateRequest.isValid(); }
    constexpr auto operator<=>(const AIDockToken&) const noexcept = default;
};

struct AIDockStateSoAColumns final
{
    container::Span<ObjectId> tokenSubjects; container::Span<ObjectId> tokenDocks;
    container::Span<uint64_t> tokenIssuedTicks; container::Span<uint32_t> tokenRequestSequences;
    container::Span<AIDockPurpose> purposes; container::Span<AIDockPhase> phases;
    container::Span<uint32_t> phaseRevisions; container::Span<uint32_t> exchangeSequences;
    container::Span<AIDockRequestKind> pendingRequests; container::Span<int32_t> approachPositions;
    container::Span<uint64_t> clearanceEnterTicks; container::Span<uint64_t> nextActionTicks;
    container::Span<uint32_t> actionDelayTicks; container::Span<ObjectId> drones;
    container::Span<uint8_t> movementActive;
};

struct AIDockStatePayload final
{
    AIDockToken token; AIDockPhase phase=AIDockPhase::Inactive; uint32_t phaseRevision=0;
    uint32_t exchangeSequence=0; AIDockRequestKind pendingRequest=AIDockRequestKind::None;
    int32_t approachPosition=-1; uint64_t clearanceEnterTick=0; uint64_t nextActionTick=0;
    uint32_t actionDelayTicks=0; ObjectId drone=INVALID_OBJECT_ID; bool movementActive=false;
    constexpr bool operator==(const AIDockStatePayload&) const noexcept = default;
};
} // namespace engine::ai
