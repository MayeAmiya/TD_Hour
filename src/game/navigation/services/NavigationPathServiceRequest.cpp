#include "NavigationPathService.h"

namespace engine::navigation
{

bool NavigationPathService::initialize(size_t requestCapacity,
                                       size_t feedbackCapacity,
                                       size_t cellCapacity,
                                       NavigationLayerId layer,
                                       NavigationProfileId profile,
                                       size_t portalCapacity,
                                       size_t pathPointCapacity)
{
    if (feedbackCapacity == 0 || cellCapacity == 0 || !layer || !profile ||
        !m_requests.initialize(requestCapacity) ||
        !m_scratch.initialize(cellCapacity, cellCapacity) ||
        !m_portalScratch.initialize(portalCapacity, cellCapacity))
        return false;
    if (pathPointCapacity == 0)
        pathPointCapacity = cellCapacity;
    if (pathPointCapacity < cellCapacity)
        pathPointCapacity = cellCapacity;
    m_feedback.assign(feedbackCapacity, {});
    m_rawCells.assign(cellCapacity, InvalidNavigationCell);
    m_pathPoints.assign(pathPointCapacity, {});
    m_groupPathScratch.assign(pathPointCapacity, {});
    m_patchPathScratch.assign(pathPointCapacity, {});
    m_patchGoalCells.assign(pathPointCapacity, InvalidNavigationCell);
    m_adjustedGoals.assign(400, {});
    m_groupPaths.assign(requestCapacity, {});
    m_feedbackCount = 0;
    m_layer = layer;
    m_profile = profile;
    m_initialized = true;
    return true;
}

void NavigationPathService::rebindSnapshotStorage() noexcept
{
    if (!m_active) return;
    m_oracle.rebindBorrowedSpans(
        m_activeRequest.dozerPassableObstacles,
        m_activeRequest.attackSeeThroughObstacles,
        m_activePatchReuse
            ? container::Span<const NavigationCellId>(
                  m_patchGoalCells.data(), m_activePatchGoalCount)
            : container::Span<const NavigationCellId>{},
        m_activeAdjustedGoalCount != 0
            ? container::Span<const NavigationSearchRequest::AdjustmentGoal>(
                  m_adjustedGoals.data(), m_activeAdjustedGoalCount)
            : container::Span<
                  const NavigationSearchRequest::AdjustmentGoal>{});
    if (m_activePortalRouteStarted) {
        m_portalScratch.rebindBorrowedSpans(
            m_activeRequest.dozerPassableObstacles,
            m_activeRequest.objectCells);
    }
}

NavigationAdapterSubmitResult NavigationPathService::submit(
    const engine::ai::PathRequest& request,
    uint64_t confirmedTick,
    PathRepository* repository,
    NavigationLayerId startLayer,
    NavigationLayerId goalLayer) noexcept
{
    if (!m_initialized || !m_navigationRevision || !request.correlation.isValid() || confirmedTick == 0)
        return NavigationAdapterSubmitResult::InvalidRequest;
    if (request.kind == engine::ai::PathRequestKind::Cancel)
        return submitCancel(request.correlation, confirmedTick, repository);

    const size_t activeOrFeedback = ownedSubject(request.correlation.subject);
    bool replacedOwned = false;
    if (activeOrFeedback != NoIndex)
    {
        const engine::ai::PathCorrelation& current = correlationAt(activeOrFeedback);
        if (!replaces(current, request.correlation))
            return NavigationAdapterSubmitResult::StaleCorrelation;
        if (m_requests.size() == m_requests.capacity())
            return NavigationAdapterSubmitResult::CapacityExceeded;
        discardOwned(activeOrFeedback, repository);
        replacedOwned = true;
    }

    if (!startLayer)
        startLayer = m_layer;
    if (!goalLayer)
        goalLayer = m_layer;
    const NavigationAdapterSubmitResult result = mapQueueResult(
        m_requests.submit(request, confirmedTick,
                          m_navigationRevision.value,
                          startLayer, goalLayer));
    return replacedOwned && result == NavigationAdapterSubmitResult::Accepted
               ? NavigationAdapterSubmitResult::Replaced
               : result;
}

bool NavigationPathService::replaces(const engine::ai::PathCorrelation& current,
                                     const engine::ai::PathCorrelation& incoming) noexcept
{
    if (incoming.sourceOrderRevision != current.sourceOrderRevision)
        return incoming.sourceOrderRevision > current.sourceOrderRevision;
    return incoming.stateRequest == current.stateRequest && incoming.generation > current.generation;
}

NavigationAdapterSubmitResult NavigationPathService::mapQueueResult(
    NavigationRequestQueueResult result) noexcept
{
    switch (result)
    {
        case NavigationRequestQueueResult::Accepted: return NavigationAdapterSubmitResult::Accepted;
        case NavigationRequestQueueResult::Replaced: return NavigationAdapterSubmitResult::Replaced;
        case NavigationRequestQueueResult::Cancelled: return NavigationAdapterSubmitResult::Cancelled;
        case NavigationRequestQueueResult::InvalidRequest: return NavigationAdapterSubmitResult::InvalidRequest;
        case NavigationRequestQueueResult::StaleCorrelation:
            return NavigationAdapterSubmitResult::StaleCorrelation;
        case NavigationRequestQueueResult::NotFound: return NavigationAdapterSubmitResult::NotFound;
        case NavigationRequestQueueResult::CapacityExceeded:
            return NavigationAdapterSubmitResult::CapacityExceeded;
    }
    return NavigationAdapterSubmitResult::InvalidRequest;
}

NavigationAdapterSubmitResult NavigationPathService::submitCancel(
    const engine::ai::PathCorrelation& correlation,
    uint64_t confirmedTick,
    PathRepository* repository) noexcept
{
    if (!canPublish(correlation.subject))
        return NavigationAdapterSubmitResult::CapacityExceeded;
    if (m_active && m_activeRequest.correlation.subject == correlation.subject)
    {
        if (!(m_activeRequest.correlation == correlation))
            return NavigationAdapterSubmitResult::StaleCorrelation;
        m_cancelPending = true;
        m_cancelCorrelation = correlation;
        m_cancelVisibleTick = confirmedTick + 1U;
        m_cancelRevision = m_navigationRevision;
        clearActive(false);
        return NavigationAdapterSubmitResult::Cancelled;
    }
    for (size_t index = 0; index < m_feedbackCount; ++index)
    {
        if (m_feedback[index].value.feedback.correlation.subject != correlation.subject)
            continue;
        if (!(m_feedback[index].value.feedback.correlation == correlation))
            return NavigationAdapterSubmitResult::StaleCorrelation;
        releaseFeedbackPath(index, repository);
        m_feedback[index] = makeTerminal(correlation,
                                         confirmedTick + 1U,
                                         NavigationAdapterStatus::Cancelled,
                                         engine::ai::PathFeedbackStatus::Cancelled,
                                         m_navigationRevision);
        return NavigationAdapterSubmitResult::Cancelled;
    }
    const NavigationRequestQueueResult result = m_requests.cancel(correlation);
    if (result != NavigationRequestQueueResult::Cancelled)
        return mapQueueResult(result);
    publish(makeTerminal(correlation,
                         confirmedTick + 1U,
                         NavigationAdapterStatus::Cancelled,
                         engine::ai::PathFeedbackStatus::Cancelled,
                         m_navigationRevision));
    return NavigationAdapterSubmitResult::Cancelled;
}

} // namespace engine::navigation
