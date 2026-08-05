#include "game/object/ai/runtime/ObjectAIShadowBatch.h"
#include <algorithm>
#include <limits>

namespace engine::ai
{

[[nodiscard]] ObjectAIShadowBatchStatus ObjectAIShadowBatch::initialize(
    size_t capacity,
    const ObjectAIShadowBatchConfig& config)
{
    if (capacity > std::numeric_limits<size_t>::max() / TransitionEntriesPerSlot)
        return ObjectAIShadowBatchStatus::InvalidCapacity;

    m_config = config;
    installFallbackResolvers();

    assign(m_scheduled, capacity);
    assign(m_effectivelyDead, capacity);
    assign(m_idleAutoAcquireEnabled, capacity);
    assign(m_idleTargetAvailable, capacity);
    m_idleTargetScanIntervalTicks.assign(
        capacity, std::max<uint32_t>(1u, config.idleTargetScanIntervalTicks));
    assign(m_faceTargetValid, capacity);
    assign(m_facingFeedback, capacity);
    assign(m_canTurnInPlace, capacity);
    assign(m_mobile, capacity);
    assign(m_moveTargetValid, capacity);
    assign(m_hasCurrentLocomotor, capacity);
    assign(m_groundMovement, capacity);
    assign(m_projectile, capacity);
    assign(m_jetAI, capacity);
    assign(m_canBeRepulsed, capacity);
    assign(m_subjectPositions, capacity);
    assign(m_resolvedMoveTargets, capacity);
    assign(m_groupOffsets, capacity);
    assign(m_wanderOffsets, capacity);
    assign(m_groundedDeleteGoals, capacity);
    m_ticksPerSecond.assign(capacity, uint32_t{30});
    assign(m_closestRepulsors, capacity);
    assign(m_wanderOffsetXCells, capacity);
    assign(m_wanderOffsetYCells, capacity);
    assign(m_wanderCellSizeRaw, capacity);
    assign(m_pathfindCellSizeRaw, capacity);
    assign(m_branchChoice, capacity);
    assign(m_teamProgressTeam, capacity);
    assign(m_teamProgressCurrent, capacity);
    assign(m_teamProgressRevision, capacity);
    assign(m_pathFeedback, capacity);
    assign(m_movementFeedback, capacity);
    assign(m_containmentGoalObjects, capacity);
    assign(m_containmentFeedback, capacity);
    assign(m_disabled, capacity);
    assign(m_behaviorProfiles, capacity);
    assign(m_behaviorProfileRevisions, capacity);
    assign(m_hackUnpackDurationTicks, capacity);
    assign(m_hackPackDurationTicks, capacity);
    assign(m_hackPayoutPeriodTicks, capacity);
    assign(m_hackPayoutAmount, capacity);
    assign(m_hackExperienceAmount, capacity);
    assign(m_hackNewestDeferredOrderRevision, capacity);
    assign(m_constructionComplete, capacity);
    assign(m_hasAmmo, capacity);
    assign(m_attackMoodAllowed, capacity);
    assign(m_attackExitConditionSatisfied, capacity);
    assign(m_sourceOrderRevisions, capacity);
    assign(m_weaponRevisions, capacity);
    assign(m_attackGoalObjects, capacity);
    assign(m_attackGoalPositions, capacity);
    assign(m_attackGoalPositionValid, capacity);
    assign(m_attackFeedback, capacity);
    assign(m_dockGoalObjects, capacity);

    assign(m_dockFeedback, capacity);
    assign(m_insertionGoalObjects, capacity);
    assign(m_insertionGoalPositions, capacity);
    assign(m_insertionGoalPositionValid, capacity);
    assign(m_insertionMotionFeedback, capacity);
    assign(m_insertionContainmentFeedback, capacity);
    assign(m_insertionOperationFeedback, capacity);
    assign(m_enterGuard, capacity);
    assign(m_guardWithoutPursuit, capacity);
    assign(m_flyingOnly, capacity);
    assign(m_tracksAnchor, capacity);
    assign(m_contained, capacity);
    assign(m_currentAnchors, capacity);
    assign(m_initialNemesis, capacity);
    assign(m_priorityNemesis, capacity);
    assign(m_aggressors, capacity);
    assign(m_crates, capacity);
    assign(m_cratePositions, capacity);
    assign(m_cratePositionValid, capacity);
    assign(m_nearestTunnels, capacity);
    assign(m_guardRangeRaw, capacity);
    assign(m_visionRangeRaw, capacity);
    assign(m_initialScanJitter, capacity);
    assign(m_guardFeedback, capacity);
    assign(m_allWeaponsOutOfAmmo, capacity);
    assign(m_allArmyHunt, capacity);
    assign(m_useTeamCommonTarget, capacity);
    assign(m_targetCollections, capacity);
    assign(m_targetCollectionRevisions, capacity);
    assign(m_attackAreas, capacity);
    assign(m_attackAreaRevisions, capacity);
    assign(m_squadSelections, capacity);
    assign(m_tacticalQueryFeedback, capacity);
    assign(m_tacticalChildFeedback, capacity);
    assign(m_tacticalAttackChildMask, capacity);
    assign(m_tacticalAttackChildStates, capacity);
    assign(m_tacticalAttackChildTargets, capacity);
    assign(m_tacticalAttackChildTargetPositions, capacity);
    assign(m_tacticalAttackChildTargetPositionValid, capacity);
    m_tacticalAttackChildResults.assign(capacity, AIStateStepResult::continueState());
    m_opportunityMovementResults.assign(capacity, AIStateStepResult::continueState());
    assign(m_opportunityQueryFeedback, capacity);
    assign(m_opportunityChildFeedback, capacity);
    assign(m_opportunityAttackChildMask, capacity);
    assign(m_opportunityAttackChildStates, capacity);
    assign(m_opportunityAttackChildTargets, capacity);
    assign(m_opportunityAttackChildTargetPositions, capacity);
    assign(m_opportunityAttackChildTargetPositionValid, capacity);
    m_opportunityAttackChildResults.assign(capacity, AIStateStepResult::continueState());

    assign(m_facingCommands, capacity);
    assign(m_pathRequests, capacity);
    assign(m_movementCommands, capacity);
    assign(m_moveEvacuateCommands, capacity);
    assign(m_teamProgressRequests, capacity);
    assign(m_waypointCompletions, capacity);
    assign(m_containmentCommands, capacity);
    assign(m_hackCommands, capacity);
    assign(m_attackCommands, capacity);
    assign(m_dockRequests, capacity);
    assign(m_insertionMotionCommands, capacity);
    assign(m_insertionContainmentCommands, capacity);
    assign(m_insertionOperationCommands, capacity);
    assign(m_insertionEffectCommands, capacity);
    assign(m_guardTacticalCommands, capacity);
    assign(m_guardInteractionCommands, capacity);
    assign(m_guardMoveChildMask, capacity);
    assign(m_guardMoveChildTargetValid, capacity);
    assign(m_guardMoveChildTargets, capacity);
    m_guardMoveChildResults.assign(
        capacity, AIStateStepResult::continueState());
    assign(m_guardAttackChildMask, capacity);
    assign(m_guardAttackChildStates, capacity);
    assign(m_guardAttackChildTargets, capacity);
    assign(m_guardAttackChildTargetPositions, capacity);
    assign(m_guardAttackChildTargetPositionValid, capacity);
    m_guardAttackChildResults.assign(
        capacity, AIStateStepResult::continueState());
    assign(m_tacticalQueryCommands, capacity);
    assign(m_tacticalChildCommands, capacity);
    assign(m_opportunityQueryCommands, capacity);
    assign(m_opportunityChildCommands, capacity);

    m_results.assign(capacity, AIStateStepResult::continueState());
    assign(m_actionMask, capacity);
    assign(m_exitMask, capacity);
    assign(m_enterMask, capacity);
    assign(m_transitionEntries, capacity * TransitionEntriesPerSlot);
    m_transitionRequests.clear();
    m_transitionRequests.reserve(capacity * TransitionRequestsPerSlot);
    m_capacity = capacity;
    m_initialized = true;
    return ObjectAIShadowBatchStatus::Success;
}

void ObjectAIShadowBatch::setPathSequenceResolver(AIPathSequenceResolver resolver) noexcept
{

    m_config.pathSequences = resolver;
    installFallbackResolvers();
}

void ObjectAIShadowBatch::setWaypointGraphResolver(AIWaypointGraphResolver resolver) noexcept
{
    m_config.waypointGraph = resolver;
    installFallbackResolvers();
}

void ObjectAIShadowBatch::setPathfindCellSizeRaw(int64_t value) noexcept
{
    m_config.followPathCellSizeRaw = value > 0 ? value : 0;
    std::fill(
        m_pathfindCellSizeRaw.begin(), m_pathfindCellSizeRaw.end(),
        m_config.followPathCellSizeRaw);
}

[[nodiscard]] AIPathSequenceQuery ObjectAIShadowBatch::missingPathSequence(
    const void*, AIPathSequenceHandle, uint64_t, uint32_t) noexcept
{
    return {.status = AIPathSequenceQueryStatus::Missing, .point = {}};
}

[[nodiscard]] AIWaypointQuery ObjectAIShadowBatch::missingWaypointNode(
    const void*, AIWaypointHandle, uint64_t) noexcept
{
    return {.status = AIWaypointQueryStatus::Missing, .node = {}};
}

[[nodiscard]] AIWaypointLinkQuery ObjectAIShadowBatch::missingWaypointLink(
    const void*, AIWaypointHandle, uint64_t, uint32_t) noexcept
{
    return {.status = AIWaypointQueryStatus::Missing, .target = {}};
}

void ObjectAIShadowBatch::installFallbackResolvers() noexcept
{
    if (m_config.pathSequences.queryPoint == nullptr)
        m_config.pathSequences.queryPoint = &missingPathSequence;
    if (m_config.waypointGraph.queryNode == nullptr)
        m_config.waypointGraph.queryNode = &missingWaypointNode;
    if (m_config.waypointGraph.queryLink == nullptr)
        m_config.waypointGraph.queryLink = &missingWaypointLink;
}

} // namespace engine::ai
