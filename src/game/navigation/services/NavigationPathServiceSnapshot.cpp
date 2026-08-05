#include "NavigationPathService.h"

namespace engine::navigation
{

void NavigationPathService::setNavigationRevision(NavigationRevision revision) noexcept
{
    m_navigationRevision = revision;
}

NavigationRevision NavigationPathService::navigationRevision() const noexcept
{
    return m_navigationRevision;
}

bool NavigationPathService::poll(const engine::ai::PathCorrelation& correlation,
                                 uint64_t confirmedTick,
                                 NavigationRevision expectedRevision,
                                 NavigationAdapterFeedback& output,
                                 PathRepository* repository) noexcept
{
    for (size_t index = 0; index < m_feedbackCount; ++index)
    {
        StoredFeedback& stored = m_feedback[index];
        if (!(stored.value.feedback.correlation == correlation) || confirmedTick < stored.visibleTick)
            continue;
        output = stored.value;
        if (!expectedRevision || expectedRevision != stored.value.navigationRevision)
        {
            releaseFeedbackPath(index, repository);
            // The search result was valid for its published grid, but a
            // confirmed topology revision landed before the AI owner polled
            // it.  This invalidates the frozen path, not the Move order.  Use
            // the same retry contract as queued/active stale work so the
            // owner re-freezes request-local obstacles against the current
            // revision instead of consuming the order as Unsupported.
            output = makeDelayed(correlation, confirmedTick,
                                 expectedRevision,
                                 NavigationTerminalReason::
                                     TopologyRevisionChanged).value;
        }
        removeFeedback(index);
        return true;
    }
    return false;
}

size_t NavigationPathService::queuedCount() const noexcept
{
    return m_requests.size();
}

size_t NavigationPathService::feedbackCount() const noexcept
{
    return m_feedbackCount;
}

bool NavigationPathService::hasActiveJob() const noexcept
{
    return m_active;
}

uint64_t NavigationPathService::stableHash() const noexcept
{
    if (!m_initialized)
        return 0;
    uint64_t hash = 14695981039346656037ULL;
    const auto feed = [&hash](uint64_t value) noexcept {
        for (size_t byte = 0; byte < sizeof(value); ++byte)
        {
            hash ^= static_cast<uint8_t>(value & 0xffU);
            hash *= 1099511628211ULL;
            value >>= 8U;
        }
    };
    const auto feedPosition = [&feed](const engine::ai::AIFixedPosition& value) noexcept {
        feed(static_cast<uint64_t>(value.xRaw));
        feed(static_cast<uint64_t>(value.yRaw));
        feed(static_cast<uint64_t>(value.zRaw));
    };
    const auto feedOrderIdentity = [&feed](const engine::ai::AIAsyncOrderIdentity& value) noexcept {
        feed(value.subject.value);
        feed(value.queueRevision);
        feed(value.externalRevision);
        feed(value.issuedTick);
        feed(value.sourceSequence);
        feed(value.sourceScriptId);
        feed(value.systemPurposeInstance);
        feed(value.source);
        feed(value.systemPurpose);
    };
    const auto feedCorrelation = [&feed, &feedOrderIdentity](
                                     const engine::ai::PathCorrelation& value) noexcept {
        feed(value.subject.value);
        feed(value.stateRequest.issuedTick);
        feed(value.stateRequest.sequence);
        feed(value.generation);
        feed(value.sourceOrderRevision);
        feedOrderIdentity(value.orderIdentity);
    };
    const auto feedRequest = [&feed, &feedPosition, &feedCorrelation](
                                 const engine::ai::PathRequest& value) noexcept {
        feedCorrelation(value.correlation);
        feedPosition(value.start);
        feedPosition(value.originalGoal);
        feed(value.adjustDestinations ? 1U : 0U);
        feed(value.ignoredObstacle.value);
        feed(value.surfaceMask);
        feed(value.clearanceProfile.radiusCells);
        feed(static_cast<uint8_t>(value.clearanceProfile.centerInCell));
        feed(static_cast<uint8_t>(value.clearanceProfile.frozen));
        feed(static_cast<uint64_t>(value.arrivalRadiusRaw));
        feed(static_cast<uint64_t>(value.minimumArrivalRadiusRaw));
        feed(static_cast<uint8_t>(value.kind));
        feed(value.currentPath.value);
        feed(value.safePathRepulsor.value);
        feedPosition(value.safePathRepulsorPosition);
        feed(static_cast<uint64_t>(value.safePathRadiusRaw));
        feed(value.safePathRepulsor2.value);
        feedPosition(value.safePathRepulsor2Position);
        feed(static_cast<uint8_t>(value.traversalMode));
        feed(value.waypointStart.value);
        feed(value.waypointGraphRevision);
        feed(value.waypointHopLimit);
        feedPosition(value.polylineOffset);
        feed(value.groupPathId);
        feed(value.groupPathMemberOrdinal);
        feed(value.groupPathMemberCount);
        feedPosition(value.groupPathOffset);
        feed(static_cast<uint64_t>(value.extraDistanceRaw));
        feed(value.pathThroughUnits ? 1U : 0U);
        feed(value.preciseFinalZ ? 1U : 0U);
        feed(value.airWings ? 1U : 0U);
        feed(value.crusherLevel);
        feed(static_cast<uint64_t>(value.dozerPassableObstacles.size()));
        for (const uint64_t object : value.dozerPassableObstacles)
            feed(object);
        feed(value.attackTarget.value);
        feed(value.attackContactWeapon ? 1U : 0U);
        feed(value.attackLineOfSightEnabled ? 1U : 0U);
        feed(value.attackSubjectContainer.value);
        feed(value.attackTargetContainer.value);
        feed(value.attackSubjectSlaver.value);
        feed(value.attackTargetSlaver.value);
        feed(static_cast<uint64_t>(
            value.attackSeeThroughObstacles.size()));
        for (const uint64_t object : value.attackSeeThroughObstacles)
            feed(object);
        feed(value.objectSnapshotTick);
        feed(static_cast<uint64_t>(value.objectCells.size()));
        for (const engine::ai::AIPathObjectCellSnapshot& objectCell :
             value.objectCells) {
            feed(objectCell.layer);
            feed(objectCell.cell);
            feed(objectCell.object.value);
            feed(static_cast<uint8_t>(objectCell.effect));
        }
        feed(value.blockingBridgeCandidate.value);
    };

    feed(14); // Feedback diagnostics and active weighted goals participate.
    feed(m_layer.value);
    feed(m_profile.value);
    feed(m_navigationRevision.value);
    feed(m_navigationRevisions.staticNavigation.value);
    feed(m_navigationRevisions.dynamicObstacles.value);
    feed(m_navigationRevisions.portalTopology.value);
    feed(m_metadataGridWidth);
    feed(m_metadataGridHeight);
    const auto queued = m_requests.entries();
    feed(static_cast<uint64_t>(queued.size()));
    for (const QueuedNavigationRequest& value : queued)
    {
        feedRequest(value.request);
        feed(value.submittedTick);
        feed(value.navigationRevision);
        feed(value.startLayer.value);
        feed(value.goalLayer.value);
    }
    feed(static_cast<uint64_t>(m_feedbackCount));
    for (size_t index = 0; index < m_feedbackCount; ++index)
    {
        const StoredFeedback& stored = m_feedback[index];
        const NavigationAdapterFeedback& value = stored.value;
        feed(static_cast<uint8_t>(value.status));
        feedCorrelation(value.feedback.correlation);
        feed(static_cast<uint8_t>(value.feedback.status));
        feed(value.feedback.confirmedTick);
        feed(value.feedback.path.value);
        feedPosition(value.feedback.adjustedGoal);
        feed(value.feedback.adjustedLayer);
        feed(value.feedback.retryPath ? 1U : 0U);
        feed(value.feedback.nextEligibleTick);
        feed(value.feedback.blockingBridge.value);
        feed(value.navigationRevision.value);
        feed(static_cast<uint8_t>(value.diagnostics.solver));
        feed(static_cast<uint8_t>(value.diagnostics.reason));
        feed(value.diagnostics.expansions);
        feed(value.diagnostics.totalCost);
        feed(value.diagnostics.closestCell.value);
        feed(value.diagnostics.traceHash);
        feed(value.diagnostics.objectSnapshotTick);
        feed(value.diagnostics.objectCellCount);
        feed(value.diagnostics.usedDestinationAdjustment ? 1U : 0U);
        feed(value.diagnostics.usedPatchSuffix ? 1U : 0U);
        feed(stored.visibleTick);
    }
    feed(m_active ? 1U : 0U);
    if (m_active)
    {
        feedRequest(m_activeRequest);
        feed(m_activeRevision.value);
        feed(m_activeStartLayer.value);
        feed(m_activeGoalLayer.value);
        feed(m_activeStartCell.value);
        feed(m_activeGoalCell.value);
        feed(m_activePatchPointCount);
        feed(m_activePatchSuffixStart);
        feed(m_activePatchGoalCount);
        feed(m_activePatchReuse ? 1U : 0U);
        if (m_activePatchReuse) {
            for (uint32_t index = 0;
                 index < m_activePatchPointCount; ++index) {
                feedPosition({
                    m_patchPathScratch[index].position.xRaw,
                    m_patchPathScratch[index].position.yRaw,
                    m_patchPathScratch[index].position.zRaw});
                feed(m_patchPathScratch[index].layer.value);
            }
            for (uint32_t index = 0;
                 index < m_activePatchGoalCount; ++index)
                feed(m_patchGoalCells[index].value);
        }
        feed(m_activeAdjustedGoalCount);
        feed(m_activeAdjustmentMaximumAnchorOffsetCost);
        for (uint32_t index = 0;
             index < m_activeAdjustedGoalCount; ++index)
        {
            feed(m_adjustedGoals[index].cell.value);
            feed(m_adjustedGoals[index].preferenceCost);
        }
        feed(m_activeUsesPortalRouter ? 1U : 0U);
        feed(m_activePortalRouteStarted ? 1U : 0U);
        if (m_activePortalRouteStarted)
            feed(m_portalScratch.stableHash());
        const NavigationSearchProgress progress =
            m_oracle.progress(m_scratch);
        feed(static_cast<uint8_t>(progress.status));
        feed(progress.totalExpansions);
        feed(progress.terminal.value);
        feed(progress.totalCost);
        feed(progress.traceHash);
    }
    feed(m_cancelPending ? 1U : 0U);
    if (m_cancelPending)
    {
        feedCorrelation(m_cancelCorrelation);
        feed(m_cancelVisibleTick);
        feed(m_cancelRevision.value);
    }
    for (const GroupPathCache& group : m_groupPaths) {
        if (group.id == 0) continue;
        feed(group.id);
        feed(group.centerPath.value);
        feedPosition(group.adjustedGoal);
        feed(group.revision.value);
        feed(group.layer.value);
        feed(group.remainingFollowers);
        feed(group.createdTick);
        feed(static_cast<uint8_t>(group.status));
    }
    return hash;
}

} // namespace engine::navigation
