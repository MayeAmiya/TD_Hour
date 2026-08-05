#pragma once

#include <cstddef>
#include <cstdint>

#include "core/container/container_types.h"
#include "game/object/ai/runtime/AIStateSoAMultiwaveExecutor.h"

namespace engine::ai
{

enum class ObjectAIShadowBatchStatus : uint8_t
{
    Success,
    NotInitialized,
    InvalidCapacity,
    CapacityExceeded,
};

struct ObjectAIShadowBatchConfig final
{
    uint32_t idleTargetScanIntervalTicks = 3;
    uint32_t forceIdleBeforeAcquireTicks = 1;
    int64_t followPathCellSizeRaw = 0;
    uint32_t maximumSkippedPathPoints = 1024;
    uint32_t maximumWaypointHops = 1024;
    uint32_t repulsorWaitFrames = 10;
    int64_t attackContactExtraDistanceRaw = 0;
    uint32_t dockTicksPerSecond = 30;
    uint32_t guardEnemyScanIntervalTicks = 15;
    uint32_t guardReturnScanIntervalTicks = 30;
    uint32_t guardChaseDurationTicks = 300;
    int64_t guardAnchorMoveThresholdRaw = 0;
    uint32_t tacticalEnemyScanIntervalTicks = 30;
    AIPathSequenceResolver pathSequences;
    AIWaypointGraphResolver waypointGraph;
};

// Mutable, slot-aligned facts and previous-tick feedback. The owner keeps the
// backing allocations stable; callers only populate these spans before the AI
// phase and never lend ECS component addresses to a state kernel.
struct ObjectAIShadowBatchColumns final
{
    container::Span<uint8_t> scheduled;
    container::Span<uint8_t> effectivelyDead;
    container::Span<uint8_t> idleAutoAcquireEnabled;
    container::Span<uint8_t> idleTargetAvailable;
    container::Span<uint32_t> idleTargetScanIntervalTicks;
    container::Span<uint8_t> faceTargetValid;
    container::Span<AIFacingFeedback> facingFeedback;
    container::Span<uint8_t> canTurnInPlace;

    container::Span<uint8_t> mobile;
    container::Span<uint8_t> moveTargetValid;
    container::Span<uint8_t> hasCurrentLocomotor;
    container::Span<uint8_t> groundMovement;
    container::Span<uint8_t> projectile;
    container::Span<uint8_t> jetAI;
    container::Span<uint8_t> canBeRepulsed;
    container::Span<AIFixedPosition> subjectPositions;
    container::Span<AIFixedPosition> resolvedMoveTargets;
    container::Span<AIFixedPosition> groupOffsets;
    container::Span<AIFixedPosition> wanderOffsets;
    container::Span<AIFixedPosition> groundedDeleteGoals;
    container::Span<uint32_t> ticksPerSecond;
    container::Span<ObjectId> closestRepulsors;
    container::Span<int32_t> wanderOffsetXCells;
    container::Span<int32_t> wanderOffsetYCells;
    container::Span<int64_t> wanderCellSizeRaw;
    container::Span<int64_t> pathfindCellSizeRaw;
    container::Span<uint32_t> branchChoice;
    container::Span<AITeamHandle> teamProgressTeam;
    container::Span<AIWaypointHandle> teamProgressCurrent;
    container::Span<uint64_t> teamProgressRevision;
    container::Span<PathFeedback> pathFeedback;
    container::Span<MovementFeedback> movementFeedback;

    container::Span<ObjectId> containmentGoalObjects;
    container::Span<AIContainmentFeedbackBuffer> containmentFeedback;

    container::Span<uint8_t> disabled;
    container::Span<AIBehaviorProfileHandle> behaviorProfiles;
    container::Span<uint64_t> behaviorProfileRevisions;
    container::Span<uint32_t> hackUnpackDurationTicks;
    container::Span<uint32_t> hackPackDurationTicks;
    container::Span<uint32_t> hackPayoutPeriodTicks;
    container::Span<int64_t> hackPayoutAmount;
    container::Span<int64_t> hackExperienceAmount;
    container::Span<uint64_t> hackNewestDeferredOrderRevision;

    container::Span<uint8_t> constructionComplete;
    container::Span<uint8_t> hasAmmo;
    container::Span<uint8_t> attackMoodAllowed;
    container::Span<uint8_t> attackExitConditionSatisfied;
    container::Span<uint64_t> sourceOrderRevisions;
    container::Span<uint64_t> weaponRevisions;
    container::Span<ObjectId> attackGoalObjects;
    container::Span<AIFixedPosition> attackGoalPositions;
    container::Span<uint8_t> attackGoalPositionValid;
    container::Span<AIAttackFeedbackBuffer> attackFeedback;

    container::Span<ObjectId> dockGoalObjects;
    container::Span<AIDockFeedbackBuffer> dockFeedback;

    container::Span<ObjectId> insertionGoalObjects;
    container::Span<AIFixedPosition> insertionGoalPositions;
    container::Span<uint8_t> insertionGoalPositionValid;
    container::Span<AIInsertionMotionFeedbackBuffer> insertionMotionFeedback;
    container::Span<AIInsertionContainmentFeedbackBuffer> insertionContainmentFeedback;
    container::Span<AIInsertionOperationFeedbackBuffer> insertionOperationFeedback;

    container::Span<uint8_t> enterGuard;
    container::Span<uint8_t> guardWithoutPursuit;
    container::Span<uint8_t> flyingOnly;
    container::Span<uint8_t> tracksAnchor;
    container::Span<uint8_t> contained;
    container::Span<AIFixedPosition> currentAnchors;
    container::Span<ObjectId> initialNemesis;
    container::Span<ObjectId> priorityNemesis;
    container::Span<ObjectId> aggressors;
    container::Span<ObjectId> crates;
    container::Span<AIFixedPosition> cratePositions;
    container::Span<uint8_t> cratePositionValid;

    container::Span<ObjectId> nearestTunnels;
    container::Span<int64_t> guardRangeRaw;
    container::Span<int64_t> visionRangeRaw;
    container::Span<uint32_t> initialScanJitter;
    container::Span<AIGuardFeedbackBuffer> guardFeedback;

    container::Span<uint8_t> allWeaponsOutOfAmmo;
    container::Span<uint8_t> allArmyHunt;
    container::Span<uint8_t> useTeamCommonTarget;
    container::Span<AITargetCollectionHandle> targetCollections;
    container::Span<uint64_t> targetCollectionRevisions;
    container::Span<AIAttackAreaHandle> attackAreas;
    container::Span<uint64_t> attackAreaRevisions;
    container::Span<AISquadTargetSelection> squadSelections;
    container::Span<AITacticalAttackQueryFeedbackBuffer> tacticalQueryFeedback;
    container::Span<AITacticalAttackChildFeedbackBuffer> tacticalChildFeedback;

    container::Span<AIStateStepResult> opportunityMovementResults;
    container::Span<AIOpportunityAttackMoveQueryFeedbackBuffer> opportunityQueryFeedback;
    container::Span<AIOpportunityAttackMoveChildFeedbackBuffer> opportunityChildFeedback;
};

// Per-slot bounded sinks. Each element is itself a fixed-capacity buffer, so
// kernels can append during a multiwave pass without growing heap storage.
struct ObjectAIShadowBatchOutputs final
{
    container::Span<AIStateCommandBuffer> facingCommands;
    container::Span<PathRequestBuffer> pathRequests;
    container::Span<MovementCommandBuffer> movementCommands;
    container::Span<AIMoveEvacuateCommandBuffer> moveEvacuateCommands;
    container::Span<AIWaypointTeamProgressBuffer> teamProgressRequests;
    container::Span<AIWaypointCompletionBuffer> waypointCompletions;
    container::Span<AIContainmentCommandBuffer> containmentCommands;
    container::Span<AIHackInternetCommandBuffer> hackCommands;
    container::Span<AIAttackCommandBuffer> attackCommands;
    container::Span<AIDockRequestBuffer> dockRequests;
    container::Span<AIInsertionMotionCommandBuffer> insertionMotionCommands;
    container::Span<AIInsertionContainmentCommandBuffer> insertionContainmentCommands;
    container::Span<AIInsertionOperationCommandBuffer> insertionOperationCommands;
    container::Span<AIInsertionEffectCommandBuffer> insertionEffectCommands;
    container::Span<AIGuardTacticalCommandBuffer> guardTacticalCommands;
    container::Span<AIGuardInteractionCommandBuffer> guardInteractionCommands;
    container::Span<AITacticalAttackQueryCommandBuffer> tacticalQueryCommands;
    container::Span<AITacticalAttackChildCommandBuffer> tacticalChildCommands;
    container::Span<AIOpportunityAttackMoveQueryCommandBuffer> opportunityQueryCommands;
    container::Span<AIOpportunityAttackMoveChildCommandBuffer> opportunityChildCommands;
};

// Fixed-capacity owner for one production SoA page's shadow inputs, feedback
// inboxes, command sinks, and multiwave scratch. initialize() is the only path
// that changes backing capacities; all tick APIs are allocation-free.
class ObjectAIShadowBatch final
{
public:
    static constexpr size_t TransitionRequestsPerSlot = 3;
    static constexpr size_t TransitionEntriesPerSlot = TransitionRequestsPerSlot + 1;

    [[nodiscard]] ObjectAIShadowBatchStatus initialize(
        size_t capacity,
        const ObjectAIShadowBatchConfig& config = {});

    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] size_t capacity() const noexcept;

    [[nodiscard]] ObjectAIShadowBatchColumns columns() noexcept;

    [[nodiscard]] ObjectAIShadowBatchOutputs outputs() noexcept;

    [[nodiscard]] ObjectAIShadowBatchStatus stageTransitionRequest(
        const AIStateSoATransitionRequest& request);

    [[nodiscard]] container::Span<AIStateSoATransitionRequest> transitionRequests() noexcept;

    [[nodiscard]] container::Span<const AIStateSoATransitionRequest> transitionRequests() const noexcept;

    void setPathSequenceResolver(AIPathSequenceResolver resolver) noexcept;

    void setWaypointGraphResolver(AIWaypointGraphResolver resolver) noexcept;

    void setPathfindCellSizeRaw(int64_t value) noexcept;

    [[nodiscard]] bool alignedWith(const AIStateFamilySoAStorage& storage) const noexcept;

    [[nodiscard]] AIStateSoAMultiwaveInput input(AIStateFamilySoAStorage& storage,
                                                  uint64_t confirmedTick) noexcept;

    [[nodiscard]] AIStateSoAMultiwaveInput input(
        AIStateFamilySoAStorage& storage,
        uint64_t confirmedTick,
        container::Span<const AIStateSoATransitionRequest> requests) noexcept;

    [[nodiscard]] AIStateSoAMultiwaveInput makeInput(AIStateFamilySoAStorage& storage,
                                                      uint64_t confirmedTick) noexcept;

    [[nodiscard]] AIStateSoAMultiwaveScratch scratch() noexcept;

    [[nodiscard]] AIStateSoAMultiwaveScratch makeScratch() noexcept;

    // True when at least one slot is due, a transition request is staged, or
    // a previously committed transition still owns a deferred exit retry.
    [[nodiscard]] bool hasRunnableWork() const noexcept;

    // Clears values emitted during a completed/abandoned shadow tick. exitMask
    // is deliberately retained: the multiwave executor uses it to retry a
    // transactional exit that was blocked after runtime commit.
    void clearTransientOutputs(bool clearTransitionRequests = true) noexcept;

    // Feedback is an input inbox and therefore has a separate visibility
    // boundary from output discard. Scalar feedback uses value reset; bounded
    // family feedback retains its allocation-free fixed arrays and clears count.
    void clearFeedback() noexcept;

    void clearDeferredExitState() noexcept;

private:
    template <typename Value>
    static void assign(container::Vector<Value>& values, size_t capacity)
    {
        values.assign(capacity, Value{});
    }

    template <typename Buffer>
    static void clearBuffers(container::Vector<Buffer>& buffers) noexcept
    {
        for (Buffer& buffer : buffers)
            buffer.clear();
    }

    [[nodiscard]] static AIPathSequenceQuery missingPathSequence(
        const void*, AIPathSequenceHandle, uint64_t, uint32_t) noexcept;

    [[nodiscard]] static AIWaypointQuery missingWaypointNode(
        const void*, AIWaypointHandle, uint64_t) noexcept;

    [[nodiscard]] static AIWaypointLinkQuery missingWaypointLink(
        const void*, AIWaypointHandle, uint64_t, uint32_t) noexcept;

    void installFallbackResolvers() noexcept;

    ObjectAIShadowBatchConfig m_config;
    size_t m_capacity = 0;
    bool m_initialized = false;

    container::Vector<uint8_t> m_scheduled;
    container::Vector<uint8_t> m_effectivelyDead;
    container::Vector<uint8_t> m_idleAutoAcquireEnabled;
    container::Vector<uint8_t> m_idleTargetAvailable;
    container::Vector<uint32_t> m_idleTargetScanIntervalTicks;
    container::Vector<uint8_t> m_faceTargetValid;
    container::Vector<AIFacingFeedback> m_facingFeedback;
    container::Vector<uint8_t> m_canTurnInPlace;
    container::Vector<uint8_t> m_mobile;
    container::Vector<uint8_t> m_moveTargetValid;
    container::Vector<uint8_t> m_hasCurrentLocomotor;
    container::Vector<uint8_t> m_groundMovement;

    container::Vector<uint8_t> m_projectile;
    container::Vector<uint8_t> m_jetAI;
    container::Vector<uint8_t> m_canBeRepulsed;
    container::Vector<AIFixedPosition> m_subjectPositions;
    container::Vector<AIFixedPosition> m_resolvedMoveTargets;
    container::Vector<AIFixedPosition> m_groupOffsets;
    container::Vector<AIFixedPosition> m_wanderOffsets;
    container::Vector<AIFixedPosition> m_groundedDeleteGoals;
    container::Vector<uint32_t> m_ticksPerSecond;
    container::Vector<ObjectId> m_closestRepulsors;
    container::Vector<int32_t> m_wanderOffsetXCells;
    container::Vector<int32_t> m_wanderOffsetYCells;
    container::Vector<int64_t> m_wanderCellSizeRaw;
    container::Vector<int64_t> m_pathfindCellSizeRaw;
    container::Vector<uint32_t> m_branchChoice;
    container::Vector<AITeamHandle> m_teamProgressTeam;
    container::Vector<AIWaypointHandle> m_teamProgressCurrent;
    container::Vector<uint64_t> m_teamProgressRevision;
    container::Vector<PathFeedback> m_pathFeedback;
    container::Vector<MovementFeedback> m_movementFeedback;
    container::Vector<ObjectId> m_containmentGoalObjects;
    container::Vector<AIContainmentFeedbackBuffer> m_containmentFeedback;
    container::Vector<uint8_t> m_disabled;
    container::Vector<AIBehaviorProfileHandle> m_behaviorProfiles;
    container::Vector<uint64_t> m_behaviorProfileRevisions;
    container::Vector<uint32_t> m_hackUnpackDurationTicks;
    container::Vector<uint32_t> m_hackPackDurationTicks;
    container::Vector<uint32_t> m_hackPayoutPeriodTicks;
    container::Vector<int64_t> m_hackPayoutAmount;
    container::Vector<int64_t> m_hackExperienceAmount;
    container::Vector<uint64_t> m_hackNewestDeferredOrderRevision;
    container::Vector<uint8_t> m_constructionComplete;
    container::Vector<uint8_t> m_hasAmmo;
    container::Vector<uint8_t> m_attackMoodAllowed;
    container::Vector<uint8_t> m_attackExitConditionSatisfied;
    container::Vector<uint64_t> m_sourceOrderRevisions;
    container::Vector<uint64_t> m_weaponRevisions;
    container::Vector<ObjectId> m_attackGoalObjects;
    container::Vector<AIFixedPosition> m_attackGoalPositions;
    container::Vector<uint8_t> m_attackGoalPositionValid;
    container::Vector<AIAttackFeedbackBuffer> m_attackFeedback;
    container::Vector<ObjectId> m_dockGoalObjects;
    container::Vector<AIDockFeedbackBuffer> m_dockFeedback;
    container::Vector<ObjectId> m_insertionGoalObjects;
    container::Vector<AIFixedPosition> m_insertionGoalPositions;
    container::Vector<uint8_t> m_insertionGoalPositionValid;
    container::Vector<AIInsertionMotionFeedbackBuffer> m_insertionMotionFeedback;
    container::Vector<AIInsertionContainmentFeedbackBuffer> m_insertionContainmentFeedback;
    container::Vector<AIInsertionOperationFeedbackBuffer> m_insertionOperationFeedback;
    container::Vector<uint8_t> m_enterGuard;
    container::Vector<uint8_t> m_guardWithoutPursuit;
    container::Vector<uint8_t> m_flyingOnly;
    container::Vector<uint8_t> m_tracksAnchor;
    container::Vector<uint8_t> m_contained;
    container::Vector<AIFixedPosition> m_currentAnchors;
    container::Vector<ObjectId> m_initialNemesis;
    container::Vector<ObjectId> m_priorityNemesis;
    container::Vector<ObjectId> m_aggressors;
    container::Vector<ObjectId> m_crates;
    container::Vector<AIFixedPosition> m_cratePositions;
    container::Vector<uint8_t> m_cratePositionValid;
    container::Vector<ObjectId> m_nearestTunnels;
    container::Vector<int64_t> m_guardRangeRaw;
    container::Vector<int64_t> m_visionRangeRaw;
    container::Vector<uint32_t> m_initialScanJitter;
    container::Vector<AIGuardFeedbackBuffer> m_guardFeedback;
    container::Vector<uint8_t> m_allWeaponsOutOfAmmo;
    container::Vector<uint8_t> m_allArmyHunt;
    container::Vector<uint8_t> m_useTeamCommonTarget;
    container::Vector<AITargetCollectionHandle> m_targetCollections;
    container::Vector<uint64_t> m_targetCollectionRevisions;
    container::Vector<AIAttackAreaHandle> m_attackAreas;
    container::Vector<uint64_t> m_attackAreaRevisions;
    container::Vector<AISquadTargetSelection> m_squadSelections;
    container::Vector<AITacticalAttackQueryFeedbackBuffer> m_tacticalQueryFeedback;
    container::Vector<AITacticalAttackChildFeedbackBuffer> m_tacticalChildFeedback;
    container::Vector<uint8_t> m_tacticalAttackChildMask;
    container::Vector<AIStateId> m_tacticalAttackChildStates;
    container::Vector<ObjectId> m_tacticalAttackChildTargets;
    container::Vector<AIFixedPosition> m_tacticalAttackChildTargetPositions;
    container::Vector<uint8_t> m_tacticalAttackChildTargetPositionValid;
    container::Vector<AIStateStepResult> m_tacticalAttackChildResults;
    container::Vector<AIStateStepResult> m_opportunityMovementResults;
    container::Vector<AIOpportunityAttackMoveQueryFeedbackBuffer> m_opportunityQueryFeedback;
    container::Vector<AIOpportunityAttackMoveChildFeedbackBuffer> m_opportunityChildFeedback;
    container::Vector<uint8_t> m_opportunityAttackChildMask;
    container::Vector<AIStateId> m_opportunityAttackChildStates;
    container::Vector<ObjectId> m_opportunityAttackChildTargets;
    container::Vector<AIFixedPosition> m_opportunityAttackChildTargetPositions;
    container::Vector<uint8_t> m_opportunityAttackChildTargetPositionValid;
    container::Vector<AIStateStepResult> m_opportunityAttackChildResults;

    container::Vector<AIStateCommandBuffer> m_facingCommands;
    container::Vector<PathRequestBuffer> m_pathRequests;
    container::Vector<MovementCommandBuffer> m_movementCommands;
    container::Vector<AIMoveEvacuateCommandBuffer> m_moveEvacuateCommands;
    container::Vector<AIWaypointTeamProgressBuffer> m_teamProgressRequests;
    container::Vector<AIWaypointCompletionBuffer> m_waypointCompletions;
    container::Vector<AIContainmentCommandBuffer> m_containmentCommands;
    container::Vector<AIHackInternetCommandBuffer> m_hackCommands;
    container::Vector<AIAttackCommandBuffer> m_attackCommands;
    container::Vector<AIDockRequestBuffer> m_dockRequests;
    container::Vector<AIInsertionMotionCommandBuffer> m_insertionMotionCommands;
    container::Vector<AIInsertionContainmentCommandBuffer> m_insertionContainmentCommands;
    container::Vector<AIInsertionOperationCommandBuffer> m_insertionOperationCommands;
    container::Vector<AIInsertionEffectCommandBuffer> m_insertionEffectCommands;
    container::Vector<AIGuardTacticalCommandBuffer> m_guardTacticalCommands;
    container::Vector<AIGuardInteractionCommandBuffer> m_guardInteractionCommands;
    container::Vector<uint8_t> m_guardMoveChildMask;
    container::Vector<uint8_t> m_guardMoveChildTargetValid;
    container::Vector<AIFixedPosition> m_guardMoveChildTargets;
    container::Vector<AIStateStepResult> m_guardMoveChildResults;
    container::Vector<uint8_t> m_guardAttackChildMask;
    container::Vector<AIStateId> m_guardAttackChildStates;
    container::Vector<ObjectId> m_guardAttackChildTargets;
    container::Vector<AIFixedPosition> m_guardAttackChildTargetPositions;
    container::Vector<uint8_t> m_guardAttackChildTargetPositionValid;
    container::Vector<AIStateStepResult> m_guardAttackChildResults;
    container::Vector<AITacticalAttackQueryCommandBuffer> m_tacticalQueryCommands;
    container::Vector<AITacticalAttackChildCommandBuffer> m_tacticalChildCommands;
    container::Vector<AIOpportunityAttackMoveQueryCommandBuffer> m_opportunityQueryCommands;

    container::Vector<AIOpportunityAttackMoveChildCommandBuffer> m_opportunityChildCommands;

    container::Vector<AIStateSoATransitionRequest> m_transitionRequests;
    container::Vector<AIStateStepResult> m_results;
    container::Vector<uint8_t> m_actionMask;
    container::Vector<uint8_t> m_exitMask;
    container::Vector<uint8_t> m_enterMask;
    container::Vector<AIStateSoATransitionEntry> m_transitionEntries;
};

} // namespace engine::ai
