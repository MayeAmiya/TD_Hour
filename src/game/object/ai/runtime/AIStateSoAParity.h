#pragma once

#include <cstddef>
#include <cstdint>

#include "core/container/container_types.h"
#include "game/object/ai/runtime/AIStateFamilySoAStorage.h"

namespace engine::ai
{

// Ordinals are mapped into a fixed multiword diagnostic set. The old single
// uint64_t representation silently capped payload coverage at 64 fields;
// reserving four words lets migration add state-specific diagnostics without
// changing the snapshot/parity ABI again.
enum class AIStateParityMismatch : uint16_t
{
    None = 0,
    RuntimeCurrentState,
    RuntimePreviousState,
    RuntimeDefaultState,
    RuntimeTemporaryResumeState,
    RuntimeEnteredTick,
    RuntimeWakeTick,
    RuntimeTemporaryEndTickExclusive,
    RuntimeRevision,
    RuntimeTransitionBudgetTick,
    RuntimeSubstateDomain,
    RuntimeSubstate,
    RuntimeLock,
    RuntimeLastWakeReason,
    RuntimeLastTransitionReason,
    RuntimeTransitionsThisTick,
    RuntimeInitialized,
    RuntimeTemporaryActive,
    RuntimeTransitionLimitExceeded,
    ParameterWaitEndTick,
    ParameterGoalObject,
    ParameterGoalPositionX,
    ParameterGoalPositionY,
    ParameterGoalPositionZ,
    ParameterIgnoredObstacle,
    ParameterSourceOrderRevision,
    ParameterPathSurfaceMask,
    ParameterArrivalRadiusRaw,
    ParameterHasGoalPosition,
    ParameterAdjustDestinations,
    PayloadState,
    ActivationSequence,
    PayloadTag,
    IdleNextTargetScanTick,
    WaitEndTick,
    BusyEnteredTick,
    DeadEnteredTick,
    FaceRequestIssuedTick,
    FaceRequestSequence,
    FaceCommandIssued,
    FaceCanTurnInPlace,
    MoveToRequestIssuedTick,
    MoveToRequestSequence,
    MoveToResolvedGoalX,
    MoveToResolvedGoalY,
    MoveToResolvedGoalZ,
    MoveToAdjustedGoalX,
    MoveToAdjustedGoalY,
    MoveToAdjustedGoalZ,
    MoveToPath,
    MoveToSourceOrderRevision,
    MoveToGeneration,
    MoveToAdjustedLayer,
    MoveToPhase,
    MoveToPathRequestIssued,
    MoveToAdjustDestinations,
    SlotOutOfRange,
    ParameterPathSequence,
    ParameterPathSequenceRevision,
    ParameterWaypoint,
    ParameterWaypointGraphRevision,
    ParameterWaypointTeam,
    ParameterWaypointGroupOffsetX,
    ParameterWaypointGroupOffsetY,
    ParameterWaypointGroupOffsetZ,
    ParameterWaypointGroupSpeed,
    ParameterExistingPath,
    FollowPathPayload,
    WaypointPathPayload,
    MoveOutOfWayPayload,
    ApproachPathPayload,
    PickUpCratePayload,
    WanderPanicPayload,
    MoveEvacuatePayload,
    ContainmentPayload,
    HackInternetPayload,
    AttackPayload,
    DockPayload,
    InsertionPayload,
    GuardPayload,
    TacticalAttackPayload,
    OpportunityAttackMovePayload,
    Count,
};

struct AIStateParityResult final
{
    static constexpr size_t WordCount = 4;
    container::Array<uint64_t, WordCount> words{};

    [[nodiscard]] constexpr bool matches() const noexcept
    {
        for (uint64_t word : words)
            if (word != 0)
                return false;
        return true;
    }
    [[nodiscard]] constexpr bool has(AIStateParityMismatch mismatch) const noexcept
    {
        const size_t ordinal = static_cast<size_t>(mismatch);
        return ordinal < WordCount * 64 && (words[ordinal / 64] & (uint64_t{1} << (ordinal % 64))) != 0;
    }
    constexpr void add(AIStateParityMismatch mismatch) noexcept
    {
        const size_t ordinal = static_cast<size_t>(mismatch);
        if (mismatch != AIStateParityMismatch::None && ordinal < WordCount * 64)
            words[ordinal / 64] |= uint64_t{1} << (ordinal % 64);
    }
    constexpr void merge(const AIStateParityResult& other) noexcept
    {
        for (size_t index = 0; index < WordCount; ++index)
            words[index] |= other.words[index];
    }
};

static_assert(static_cast<size_t>(AIStateParityMismatch::Count) < AIStateParityResult::WordCount * 64);

namespace detail
{

// Kept as a declared white-box seam for state-family coverage and snapshot
// validation contracts.
[[nodiscard]] bool payloadTagMatches(const AIStateData& data) noexcept;

} // namespace detail

[[nodiscard]] AIStateParityResult compareAIStateMachineRuntime(const AIStateMachineRuntime& oracle,
                                                               const AIStateMachineRuntime& candidate) noexcept;

[[nodiscard]] AIStateParityResult compareAIStateDataToSoASlot(const AIStateData& oracle,
                                                              const AIStateFamilySoAStorage& storage,
                                                              size_t slot) noexcept;

[[nodiscard]] AIStateParityResult compareAIStateExecutorParity(const AIStateMachineRuntime& oracleRuntime,
                                                               const AIStateData& oracleData,
                                                               const AIStateFamilySoAStorage& storage,
                                                               size_t slot) noexcept;

enum class AIStateSoABridgeError : uint8_t
{
    None = 0,
    SlotOutOfRange = uint8_t{1} << 0,
    PayloadTagMismatch = uint8_t{1} << 1,
    PayloadMetadataRejected = uint8_t{1} << 2,
};

struct AIStateSoABridgeResult final
{
    uint8_t bits = 0;

    [[nodiscard]] constexpr bool succeeded() const noexcept { return bits == 0; }
    [[nodiscard]] constexpr bool has(AIStateSoABridgeError error) const noexcept
    {
        return (bits & static_cast<uint8_t>(error)) != 0;
    }
    constexpr void add(AIStateSoABridgeError error) noexcept
    {
        bits |= static_cast<uint8_t>(error);
    }
};

[[nodiscard]] AIStateSoABridgeResult writeAIStateDataToSoASlot(const AIStateData& oracle,
                                                              AIStateFamilySoAStorage& storage,
                                                              size_t slot) noexcept;

[[nodiscard]] AIStateSoABridgeResult rebuildAIStateDataFromSoASlot(const AIStateFamilySoAStorage& storage,
                                                                  size_t slot,
                                                                  AIStateData& output) noexcept;

} // namespace engine::ai
