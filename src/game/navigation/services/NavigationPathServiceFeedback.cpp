#include "NavigationPathService.h"

#include <algorithm>
#include <limits>

namespace engine::navigation
{

size_t NavigationPathService::ownedSubject(engine::ObjectId subject) const noexcept
{
    if (m_active && m_activeRequest.correlation.subject == subject)
        return m_feedback.size();
    for (size_t index = 0; index < m_feedbackCount; ++index)
    {
        if (m_feedback[index].value.feedback.correlation.subject == subject)
            return index;
    }
    return NoIndex;
}

const engine::ai::PathCorrelation& NavigationPathService::correlationAt(
    size_t index) const noexcept
{
    return index == m_feedback.size() ? m_activeRequest.correlation
                                      : m_feedback[index].value.feedback.correlation;
}

void NavigationPathService::discardOwned(size_t index, PathRepository* repository) noexcept
{
    if (index == m_feedback.size())
        clearActive();
    else
    {
        releaseFeedbackPath(index, repository);
        removeFeedback(index);
    }
}

void NavigationPathService::releaseFeedbackPath(size_t index,
                                                PathRepository* repository) noexcept
{
    if (repository == nullptr || index >= m_feedbackCount)
        return;
    const NavigationAdapterFeedback& feedback = m_feedback[index].value;
    if (feedback.feedback.path && feedback.navigationRevision)
    {
        static_cast<void>(repository->release(
            feedback.feedback.path, feedback.navigationRevision));
    }
}

bool NavigationPathService::canPublish(engine::ObjectId subject) const noexcept
{
    if (m_feedbackCount < m_feedback.size())
        return true;
    for (size_t index = 0; index < m_feedbackCount; ++index)
    {
        if (m_feedback[index].value.feedback.correlation.subject == subject)
            return true;
    }
    return false;
}

void NavigationPathService::publish(const StoredFeedback& feedback) noexcept
{
    for (size_t index = 0; index < m_feedbackCount; ++index)
    {
        if (m_feedback[index].value.feedback.correlation.subject ==
            feedback.value.feedback.correlation.subject)
        {
            m_feedback[index] = feedback;
            return;
        }
    }
    m_feedback[m_feedbackCount++] = feedback;
}

NavigationPathService::StoredFeedback NavigationPathService::makeTerminal(
    const engine::ai::PathCorrelation& correlation,
    uint64_t confirmedTick,
    NavigationAdapterStatus sidecar,
    engine::ai::PathFeedbackStatus publicStatus,
    NavigationRevision revision) noexcept
{
    NavigationAdapterFeedback value;
    value.status = sidecar;
    value.navigationRevision = revision;
    value.feedback.correlation = correlation;
    value.feedback.status = publicStatus;
    value.feedback.confirmedTick = confirmedTick;
    if (publicStatus == engine::ai::PathFeedbackStatus::Cancelled)
        value.diagnostics.reason = NavigationTerminalReason::Cancelled;
    else if (sidecar == NavigationAdapterStatus::UnsupportedTraversal ||
             sidecar == NavigationAdapterStatus::UnsupportedSafe)
        value.diagnostics.reason =
            NavigationTerminalReason::UnsupportedTraversal;
    return {value, confirmedTick};
}

NavigationPathService::StoredFeedback NavigationPathService::makeDelayed(
    const engine::ai::PathCorrelation& correlation,
    uint64_t confirmedTick,
    NavigationRevision revision,
    NavigationTerminalReason reason) noexcept
{
    StoredFeedback delayed = makeTerminal(
        correlation,
        confirmedTick,
        NavigationAdapterStatus::CapacityExceeded,
        engine::ai::PathFeedbackStatus::Delayed,
        revision);
    constexpr uint64_t MaxTick = std::numeric_limits<uint64_t>::max();
    const uint64_t nextEligibleTick =
        confirmedTick > MaxTick - CapacityRetryDelayTicks
            ? MaxTick
            : confirmedTick + CapacityRetryDelayTicks;
    delayed.value.feedback.nextEligibleTick = nextEligibleTick;
    delayed.value.diagnostics.reason = reason;
    return delayed;
}

void NavigationPathService::annotateActiveFeedback(
    StoredFeedback& feedback,
    NavigationSolverKind solver,
    NavigationTerminalReason reason,
    const NavigationSearchProgress* progress) const noexcept
{
    NavigationQueryDiagnostics& diagnostics = feedback.value.diagnostics;
    diagnostics.solver = solver;
    diagnostics.reason = reason;
    diagnostics.objectSnapshotTick = m_activeRequest.objectSnapshotTick;
    diagnostics.objectCellCount = static_cast<uint32_t>(
        std::min<size_t>(m_activeRequest.objectCells.size(),
                         std::numeric_limits<uint32_t>::max()));
    diagnostics.usedDestinationAdjustment =
        m_activeAdjustedGoalCount != 0;
    diagnostics.usedPatchSuffix = m_activePatchReuse;
    if (progress) {
        diagnostics.expansions = progress->totalExpansions;
        diagnostics.totalCost = progress->totalCost;
        diagnostics.closestCell = progress->terminal
            ? progress->terminal
            : m_oracle.closestCell();
        diagnostics.traceHash = progress->traceHash;
    }
}

void NavigationPathService::annotateQueuedFeedback(
    StoredFeedback& feedback,
    const QueuedNavigationRequest& queued,
    NavigationSolverKind solver,
    NavigationTerminalReason reason,
    bool usedDestinationAdjustment) noexcept
{
    NavigationQueryDiagnostics& diagnostics = feedback.value.diagnostics;
    diagnostics.solver = solver;
    diagnostics.reason = reason;
    diagnostics.objectSnapshotTick = queued.request.objectSnapshotTick;
    diagnostics.objectCellCount = static_cast<uint32_t>(
        std::min<size_t>(queued.request.objectCells.size(),
                         std::numeric_limits<uint32_t>::max()));
    diagnostics.usedDestinationAdjustment = usedDestinationAdjustment;
}

NavigationAdapterProcessResult NavigationPathService::publishPendingCancel(
    uint64_t confirmedTick) noexcept
{
    if (confirmedTick < m_cancelVisibleTick)
        return NavigationAdapterProcessResult::Idle;
    publish(makeTerminal(m_cancelCorrelation,
                         confirmedTick,
                         NavigationAdapterStatus::Cancelled,
                         engine::ai::PathFeedbackStatus::Cancelled,
                         m_cancelRevision));
    m_cancelPending = false;
    m_cancelCorrelation = {};
    m_cancelRevision = InvalidNavigationRevision;
    return NavigationAdapterProcessResult::FeedbackPublished;
}

void NavigationPathService::clearActive(bool clearCancel) noexcept
{
    m_active = false;
    m_activeRequest = {};
    m_activeRevision = InvalidNavigationRevision;
    m_activeStartLayer = InvalidNavigationLayer;
    m_activeGoalLayer = InvalidNavigationLayer;
    m_activeStartCell = InvalidNavigationCell;
    m_activeGoalCell = InvalidNavigationCell;
    m_activePatchPointCount = 0;
    m_activePatchSuffixStart = 0;
    m_activePatchGoalCount = 0;
    m_activePatchReuse = false;
    m_activeAdjustedGoalCount = 0;
    m_activeAdjustmentMaximumAnchorOffsetCost = 0;
    m_activeUsesPortalRouter = false;
    m_activePortalRouteStarted = false;
    if (clearCancel)
    {
        m_cancelPending = false;
        m_cancelCorrelation = {};
        m_cancelVisibleTick = 0;
        m_cancelRevision = InvalidNavigationRevision;
    }
}

void NavigationPathService::removeFeedback(size_t index) noexcept
{
    for (size_t move = index + 1; move < m_feedbackCount; ++move)
        m_feedback[move - 1] = m_feedback[move];
    --m_feedbackCount;
    m_feedback[m_feedbackCount] = {};
}

} // namespace engine::navigation
