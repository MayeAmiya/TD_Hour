#include "NavigationPathService.h"
#include "../integration/NavigationPathSmoothing.h"
#include "math/fixed/q32_32.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <utility>

namespace engine::navigation
{
namespace
{
[[nodiscard]] constexpr NavigationClearanceClass toNavigationClearance(
    const engine::ai::AIPathClearanceProfile& value) noexcept
{
    return value.validFrozen()
        ? clearanceClassForGeometry(value.radiusCells, value.centerInCell)
        : NavigationClearanceClass::Centered1x1;
}

[[nodiscard]] bool navigationGoalOccupiedByObject(
    container::Span<const engine::ai::AIPathObjectCellSnapshot> objectCells,
    NavigationLayerId layer,
    NavigationCellId cell) noexcept
{
    const auto first = std::lower_bound(
        objectCells.begin(), objectCells.end(),
        std::pair<uint32_t, uint32_t>{layer.value, cell.value},
        [](const engine::ai::AIPathObjectCellSnapshot& value,
           const std::pair<uint32_t, uint32_t>& wanted) noexcept {
            return value.layer < wanted.first ||
                (value.layer == wanted.first &&
                 value.cell < wanted.second);
        });
    return first != objectCells.end() &&
        first->layer == layer.value && first->cell == cell.value;
}

struct NavigationAdjustmentGoalSet final
{
    NavigationCellId anchor = InvalidNavigationCell;
    uint32_t count = 0;
    uint32_t maximumAnchorOffsetCost = 0;
    bool exact = false;
};

// Collect every admissible endpoint in RefCode's bounded destination spiral.
// A* consumes this set as a goal predicate; this helper deliberately does not
// make a reachability decision of its own.
[[nodiscard]] NavigationAdjustmentGoalSet collectNavigationAdjustmentGoals(
    const NavigationGrid& grid,
    const NavigationWorldPosition& desired,
    NavigationLayerId layer,
    NavigationMovementMask movementMask,
    NavigationClearanceClass clearance,
    container::Span<const engine::ai::AIPathObjectCellSnapshot> objectCells,
    container::Span<NavigationSearchRequest::AdjustmentGoal> output) noexcept
{
    NavigationAdjustmentGoalSet result;
    const NavigationCellId desiredCell = grid.cellAt(desired, clearance);
    if (grid.traversable(
            desiredCell, movementMask, layer, clearance) &&
        !navigationGoalOccupiedByObject(
            objectCells, layer, desiredCell)) {
        result.anchor = desiredCell;
        result.exact = true;
        return result;
    }
    if (output.empty()) return result;

    NavigationGridCoordinate desiredCoordinate{};
    if (desiredCell) {
        desiredCoordinate = grid.coordinate(desiredCell);
    } else if (!worldAxisToCell(
                   desired.xRaw, grid.transform().originXRaw,
                   grid.transform().cellSizeRaw, desiredCoordinate.x) ||
               !worldAxisToCell(
                   desired.yRaw, grid.transform().originYRaw,
                   grid.transform().cellSizeRaw, desiredCoordinate.y)) {
        return result;
    }

    constexpr size_t kMaximumAdjustmentCellCount = 400;
    constexpr int32_t directions[4][2] = {
        {1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    int64_t x = desiredCoordinate.x;
    int64_t y = desiredCoordinate.y;
    size_t remaining = kMaximumAdjustmentCellCount;
    size_t segmentLength = 1;
    while (remaining != 0 && result.count < output.size()) {
        for (size_t direction = 0;
             direction < 4 && remaining != 0 &&
                 result.count < output.size();
             ++direction) {
            for (size_t step = 0;
                 step < segmentLength && remaining != 0;
                 ++step, --remaining) {
                x += directions[direction][0];
                y += directions[direction][1];
                if (x < 0 || y < 0 ||
                    x >= static_cast<int64_t>(grid.width()) ||
                    y >= static_cast<int64_t>(grid.height())) {
                    continue;
                }
                const NavigationCellId candidate = grid.cellId({
                    static_cast<int32_t>(x), static_cast<int32_t>(y)});
                if (!grid.traversable(
                        candidate, movementMask, layer, clearance) ||
                    navigationGoalOccupiedByObject(
                        objectCells, layer, candidate)) {
                    continue;
                }
                if (!result.anchor) result.anchor = candidate;
                uint32_t preferenceCost = 0;
                if (!AStarOracle::octileCost(
                        desiredCoordinate, grid.coordinate(candidate),
                        preferenceCost)) {
                    return {};
                }
                output[result.count++] = {
                    .cell = candidate,
                    .preferenceCost = preferenceCost,
                };
            }
            if ((direction & 1U) != 0) ++segmentLength;
        }
    }
    if (!result.anchor) return result;

    const NavigationGridCoordinate anchorCoordinate =
        grid.coordinate(result.anchor);
    for (uint32_t index = 0; index < result.count; ++index) {
        uint32_t offsetCost = 0;
        if (!AStarOracle::octileCost(
                anchorCoordinate,
                grid.coordinate(output[index].cell),
                offsetCost)) {
            return {};
        }
        result.maximumAnchorOffsetCost = std::max(
            result.maximumAnchorOffsetCost, offsetCost);
    }
    std::sort(
        output.begin(), output.begin() + result.count,
        [](const NavigationSearchRequest::AdjustmentGoal& left,
           const NavigationSearchRequest::AdjustmentGoal& right) noexcept {
            return left.cell < right.cell;
        });
    const auto uniqueEnd = std::unique(
        output.begin(), output.begin() + result.count,
        [](const NavigationSearchRequest::AdjustmentGoal& left,
           const NavigationSearchRequest::AdjustmentGoal& right) noexcept {
            return left.cell == right.cell;
        });
    result.count = static_cast<uint32_t>(uniqueEnd - output.begin());
    return result;
}

// RefCode clips the start-to-goal line to Pathfinder::m_extent before it
// chooses the start cell.  Campaign maps deliberately place reinforcements
// beyond that extent and order them toward an on-map waypoint; treating the
// authored start as an ordinary invalid destination makes those units receive
// NoPath and remain outside the battlefield.  Keep the real start in the AI
// request (locomotion still owns the ingress segment), but seed A* from the
// first grid cell crossed by that line.
[[nodiscard]] NavigationCellId clippedSegmentEntryCell(
    const NavigationGrid& grid,
    const NavigationWorldPosition& source,
    const NavigationWorldPosition& destination,
    NavigationClearanceClass clearance) noexcept
{
    if (!grid.isInitialized() || !validClearanceClass(clearance))
        return InvalidNavigationCell;

    NavigationWorldPosition start = source;
    NavigationWorldPosition goal = destination;
    const NavigationGridTransform transform = grid.transform();
    if (!clearanceCenterInCell(clearance)) {
        const int64_t phase = transform.cellSizeRaw / 2;
        if (!detail::checkedAdd(start.xRaw, phase, start.xRaw) ||
            !detail::checkedAdd(start.yRaw, phase, start.yRaw) ||
            !detail::checkedAdd(goal.xRaw, phase, goal.xRaw) ||
            !detail::checkedAdd(goal.yRaw, phase, goal.yRaw)) {
            return InvalidNavigationCell;
        }
    }

    int64_t widthRaw = 0;
    int64_t heightRaw = 0;
    int64_t maximumX = 0;
    int64_t maximumY = 0;
    if (!detail::checkedMultiply(
            static_cast<int64_t>(grid.width()), transform.cellSizeRaw,
            widthRaw) ||
        !detail::checkedMultiply(
            static_cast<int64_t>(grid.height()), transform.cellSizeRaw,
            heightRaw) ||
        !detail::checkedAdd(transform.originXRaw, widthRaw, maximumX) ||
        !detail::checkedAdd(transform.originYRaw, heightRaw, maximumY)) {
        return InvalidNavigationCell;
    }
    // The upper grid edge belongs to the first cell beyond the grid.
    if (maximumX == std::numeric_limits<int64_t>::min() ||
        maximumY == std::numeric_limits<int64_t>::min()) {
        return InvalidNavigationCell;
    }
    --maximumX;
    --maximumY;

    const math::q32_32 zero{};
    const math::q32_32 one{int32_t{1}};
    math::q32_32 enter = zero;
    math::q32_32 leave = one;
    const auto clipAxis = [&enter, &leave, zero, one](
                              int64_t begin, int64_t end,
                              int64_t minimum,
                              int64_t maximum) noexcept {
        const math::q32_32 beginFixed = math::q32_32::from_raw(begin);
        const math::q32_32 delta =
            math::q32_32::from_raw(end) - beginFixed;
        if (delta == zero)
            return begin >= minimum && begin <= maximum;

        math::q32_32 first =
            (math::q32_32::from_raw(minimum) - beginFixed) / delta;
        math::q32_32 last =
            (math::q32_32::from_raw(maximum) - beginFixed) / delta;
        if (last < first) std::swap(first, last);
        enter = math::q32_32::max(enter, first);
        leave = math::q32_32::min(leave, last);
        return enter <= leave && leave >= zero && enter <= one;
    };
    if (!clipAxis(start.xRaw, goal.xRaw, transform.originXRaw, maximumX) ||
        !clipAxis(start.yRaw, goal.yRaw, transform.originYRaw, maximumY)) {
        return InvalidNavigationCell;
    }
    enter = math::q32_32::clamp(enter, zero, one);
    const math::q32_32 clippedX = math::q32_32::from_raw(start.xRaw) +
        (math::q32_32::from_raw(goal.xRaw) -
         math::q32_32::from_raw(start.xRaw)) * enter;
    const math::q32_32 clippedY = math::q32_32::from_raw(start.yRaw) +
        (math::q32_32::from_raw(goal.yRaw) -
         math::q32_32::from_raw(start.yRaw)) * enter;
    const int64_t admittedX = std::clamp(
        clippedX.raw(), transform.originXRaw, maximumX);
    const int64_t admittedY = std::clamp(
        clippedY.raw(), transform.originYRaw, maximumY);
    int32_t cellX = 0;
    int32_t cellY = 0;
    if (!worldAxisToCell(admittedX, transform.originXRaw,
                         transform.cellSizeRaw, cellX) ||
        !worldAxisToCell(admittedY, transform.originYRaw,
                         transform.cellSizeRaw, cellY)) {
        return InvalidNavigationCell;
    }
    return grid.cellId({cellX, cellY});
}

[[nodiscard]] NavigationAirObstacleQuery firstAirObstacleOnSegment(
    const NavigationGrid& grid,
    const NavigationDynamicOverlay& overlay,
    const NavigationWorldPosition& from,
    const NavigationWorldPosition& to,
    uint64_t ignoredEntityId) noexcept
{
    const NavigationCellId fromCell = grid.cellAt(from);
    const NavigationCellId toCell = grid.cellAt(to);
    if (!fromCell || !toCell || fromCell == toCell)
        return {};
    const NavigationGridCoordinate start = grid.coordinate(fromCell);
    const NavigationGridCoordinate goal = grid.coordinate(toCell);
    const int64_t deltaX = static_cast<int64_t>(goal.x) - start.x;
    const int64_t deltaY = static_cast<int64_t>(goal.y) - start.y;
    const int32_t stepX = deltaX < 0 ? -1 : 1;
    const int32_t stepY = deltaY < 0 ? -1 : 1;
    const uint64_t stepsX =
        static_cast<uint64_t>(deltaX < 0 ? -deltaX : deltaX);
    const uint64_t stepsY =
        static_cast<uint64_t>(deltaY < 0 ? -deltaY : deltaY);
    int32_t x = start.x;
    int32_t y = start.y;
    uint64_t advancedX = 0;
    uint64_t advancedY = 0;
    const auto query = [&](NavigationCellId cell) noexcept {
        return overlay.airObstacleAt(cell, ignoredEntityId);
    };
    while (advancedX != stepsX || advancedY != stepsY) {
        const uint64_t horizontal = (advancedX * 2U + 1U) * stepsY;
        const uint64_t vertical = (advancedY * 2U + 1U) * stepsX;
        if (horizontal == vertical) {
            const NavigationCellId sideX = grid.cellId({x + stepX, y});
            const NavigationCellId sideY = grid.cellId({x, y + stepY});
            NavigationAirObstacleQuery first = query(sideX);
            const NavigationAirObstacleQuery second = query(sideY);
            if (!first.found() ||
                (second.found() && sideY < sideX))
                first = second;
            if (first.found()) return first;
            x += stepX;
            y += stepY;
            ++advancedX;
            ++advancedY;
        } else if (horizontal < vertical) {
            x += stepX;
            ++advancedX;
        } else {
            y += stepY;
            ++advancedY;
        }
        const NavigationAirObstacleQuery obstacle =
            query(grid.cellId({x, y}));
        if (obstacle.found()) return obstacle;
    }
    return {};
}
} // namespace

NavigationWorldPosition NavigationPathService::toWorld(
    const engine::ai::AIFixedPosition& value) noexcept
{
    return {value.xRaw, value.yRaw, value.zRaw};
}

engine::ai::AIFixedPosition NavigationPathService::toAI(
    const NavigationWorldPosition& value) noexcept
{
    return {value.xRaw, value.yRaw, value.zRaw};
}

NavigationAdapterProcessResult NavigationPathService::processConfirmedTick(
    uint64_t confirmedTick,
    const NavigationGrid& grid,
    PathRepository& repository,
    uint32_t expansionBudget,
    const NavigationLayerSet* layers,
    container::Span<const NavigationZoneField> zones,
    const NavigationPortalGraph* portalGraph,
    const engine::ai::AIWaypointGraphResolver* waypointGraph,
    const NavigationDynamicOverlay* dynamicOverlay,
    bool topologyPublished,
    NavigationRevisionSet navigationRevisions) noexcept
{
    m_expansionsConsumedLastProcess = 0;
    m_navigationRevisions = navigationRevisions;
    m_metadataGridWidth = grid.width();
    m_metadataGridHeight = grid.height();
    const auto gridForLayer = [&](NavigationLayerId layer) noexcept
        -> const NavigationGrid* {
        if (layers)
            return layers->find(layer);
        return layer == m_layer ? &grid : nullptr;
    };
    if (!m_initialized || confirmedTick == 0 || !m_navigationRevision)
        return NavigationAdapterProcessResult::Idle;
    // A centerline is an intra-tick optimization, not durable gameplay state.
    // Every follower normally consumes it in the same zero-cost drain after
    // ordinal zero publishes it.  A cancelled/replaced follower must not pin
    // a repository path forever, and a later re-admission must fall back to
    // its own A* rather than depending on an old group command.
    for (GroupPathCache& group : m_groupPaths) {
        if (group.id == 0 ||
            (group.createdTick == confirmedTick &&
             group.revision == m_navigationRevision)) {
            continue;
        }
        if (group.centerPath && group.revision) {
            static_cast<void>(repository.release(
                group.centerPath, group.revision));
        }
        group = {};
    }
    if (m_cancelPending)
        return publishPendingCancel(confirmedTick);
    // A topology transaction may already be authoritative in ECS while its
    // bounded overlay commit or zone rebuild is still in progress.  Neither
    // an existing A* job nor a newly queued request may consume the previous
    // published grid during that interval.  Cancellation remains available
    // above because it does not inspect grid, zone, or portal state.
    if (!topologyPublished)
    {
        return NavigationAdapterProcessResult::PublicationBlocked;
    }
    // QuickPath and exact authored waypoint paths are value-only, zero-A*
    // work. In RefCode, QuickPath is produced directly by requestPath(), so a
    // long ground search cannot make a later aircraft command wait for its
    // remaining slices. Keep queue order among immediate requests while
    // allowing this preemption of Navmesh work.
    if (const QueuedNavigationRequest* immediate =
            m_requests.firstImmediate(confirmedTick);
        immediate != nullptr && canPublish(immediate->request.correlation.subject)) {
        QueuedNavigationRequest queued;
        [[maybe_unused]] const bool popped =
            m_requests.popFirstImmediate(confirmedTick, queued);
        return finishImmediateRequest(
            confirmedTick, grid, queued, layers, waypointGraph,
            dynamicOverlay, repository);
    }
    if (m_active && !canPublish(m_activeRequest.correlation.subject))
        return NavigationAdapterProcessResult::FeedbackCapacityExceeded;
    if (m_active && m_activeRevision != m_navigationRevision)
    {
        // A path request may have started immediately before a confirmed
        // building/terrain overlay revision.  The old search cannot be
        // completed against the newly published grid, but this is not an
        // unsupported AI order.  Return the public retry result against the
        // current revision so the owner re-freezes its object snapshot and
        // submits the same correlation on the next eligible tick.
        publish(makeDelayed(m_activeRequest.correlation, confirmedTick,
                            m_navigationRevision,
                            NavigationTerminalReason::
                                TopologyRevisionChanged));
        clearActive();
        return NavigationAdapterProcessResult::FeedbackPublished;
    }

    if (!m_active)
    {
        const QueuedNavigationRequest* front = m_requests.front();
        // Admission runs before Navigation in the confirmed frame pipeline.
        // Solving a request submitted at tick T here is deterministic and
        // lets AI consume the feedback at T+1, matching ZH's next-update
        // command visibility. Requiring submittedTick < confirmedTick added
        // an unnecessary second frame of input latency.
        if (front == nullptr || front->submittedTick > confirmedTick)
            return NavigationAdapterProcessResult::Idle;
        if (!canPublish(front->request.correlation.subject))
            return NavigationAdapterProcessResult::FeedbackCapacityExceeded;
        QueuedNavigationRequest queued;
        [[maybe_unused]] const bool popped = m_requests.pop(queued);
        if (queued.navigationRevision != m_navigationRevision.value)
        {
            // Queued work can legitimately wait behind a bounded topology
            // publication (notably initial structure footprints).  A newer
            // published revision invalidates only the frozen navigation
            // snapshot, not the Move order itself.  Ask the owner to retry so
            // admission rebuilds object cells and layer data from the current
            // confirmed world.
            publish(makeDelayed(queued.request.correlation, confirmedTick,
                                m_navigationRevision,
                                NavigationTerminalReason::
                                    TopologyRevisionChanged));
            return NavigationAdapterProcessResult::FeedbackPublished;
        }
        if (queued.request.traversalMode ==
            engine::ai::AIPathTraversalMode::DirectLine)
        {
            const NavigationGrid* directGrid =
                gridForLayer(queued.startLayer);
            if (!directGrid || queued.startLayer != queued.goalLayer) {
                publish(makeTerminal(
                    queued.request.correlation, confirmedTick,
                    NavigationAdapterStatus::InvalidRequest,
                    engine::ai::PathFeedbackStatus::NoPath,
                    m_navigationRevision));
                return NavigationAdapterProcessResult::FeedbackPublished;
            }
            return finishDirectLine(
                confirmedTick, *directGrid, queued, dynamicOverlay,
                repository);
        }
        if (queued.request.traversalMode ==
            engine::ai::AIPathTraversalMode::WaypointPolyline)
        {
            return finishWaypointPolyline(
                confirmedTick, queued, layers, waypointGraph, repository);
        }
        if (queued.request.traversalMode !=
            engine::ai::AIPathTraversalMode::Navmesh)
        {
            publish(makeTerminal(queued.request.correlation,
                                 confirmedTick,
                                 NavigationAdapterStatus::UnsupportedTraversal,
                                 engine::ai::PathFeedbackStatus::Unsupported,
                                 m_navigationRevision));
            return NavigationAdapterProcessResult::FeedbackPublished;
        }
        const NavigationGrid* startGrid = gridForLayer(queued.startLayer);
        const NavigationGrid* goalGrid = gridForLayer(queued.goalLayer);
        if (!startGrid || !goalGrid) {
            publish(makeTerminal(queued.request.correlation,
                                 confirmedTick,
                                 NavigationAdapterStatus::InvalidRequest,
                                 engine::ai::PathFeedbackStatus::NoPath,
                                 m_navigationRevision));
            return NavigationAdapterProcessResult::FeedbackPublished;
        }
        if (queued.request.groupPathId != 0 &&
            queued.request.groupPathMemberOrdinal != 0) {
            const auto cache = std::find_if(
                m_groupPaths.begin(), m_groupPaths.end(),
                [&queued](const GroupPathCache& value) {
                    return value.id == queued.request.groupPathId;
                });
            if (cache != m_groupPaths.end() && cache->centerPath &&
                cache->revision.value == queued.navigationRevision &&
                cache->createdTick == confirmedTick &&
                cache->status == engine::ai::PathFeedbackStatus::Ready) {
                return finishGroupFollower(
                    confirmedTick, *goalGrid, queued, dynamicOverlay,
                    repository);
            }

            // Shared routing must never be a correctness dependency.  The
            // leader may have been delayed by a topology revision, the actor
            // may be resuming after MoveAside, or a previous command may have
            // been replaced before every follower consumed the cache.  Keep
            // the same correlation and frozen object snapshot, but solve this
            // request as an ordinary per-object path from its real start.
            queued.request.groupPathId = 0;
            queued.request.groupPathMemberOrdinal = 0;
            queued.request.groupPathMemberCount = 0;
            queued.request.groupPathOffset = {};
        }
        const NavigationClearanceClass clearance =
            toNavigationClearance(queued.request.clearanceProfile);
        const NavigationWorldPosition requestedStart =
            toWorld(queued.request.start);
        const NavigationWorldPosition requestedGoal =
            toWorld(queued.request.originalGoal);
        NavigationWorldPosition startAdjustmentSeed = requestedStart;
        NavigationCellId start =
            startGrid->cellAt(requestedStart, clearance);
        if (!start) {
            start = clippedSegmentEntryCell(
                *startGrid, requestedStart, requestedGoal, clearance);
            if (start) {
                // Destination adjustment below is a local blocked-cell escape,
                // so seed it at the clipped entry rather than hundreds of
                // cells beyond the map where its bounded spiral cannot reach.
                static_cast<void>(startGrid->cellPosition(
                    start, clearance, startAdjustmentSeed));
            }
        }
        if (!startGrid->traversable(
                start, queued.request.surfaceMask, queued.startLayer,
                clearance)) {
            // A topology commit can make an actor's current cell blocked
            // between order admission and path solving (most visibly when a
            // construction site publishes its completed footprint around the
            // builder).  Refusing the request strands the actor permanently.
            // Start the path at the deterministic nearest traversable cell;
            // locomotion then performs the short escape segment from the real
            // pose before following the stored path.
            const NavigationDestinationAdjustmentResult adjustedStart =
                layers ? adjustNavigationDestination(
                             *layers,
                             {
                                 .desired = startAdjustmentSeed,
                                 .layer = queued.startLayer,
                                 .movementMask = queued.request.surfaceMask,
                                 .clearance = clearance,
                                 .allowAdjustment = true,
                             })
                       : NavigationDestinationAdjustmentResult{};
            if (!adjustedStart.accepted()) {
                StoredFeedback failed = makeTerminal(
                    queued.request.correlation, confirmedTick,
                    NavigationAdapterStatus::NoPath,
                    engine::ai::PathFeedbackStatus::NoPath,
                    m_navigationRevision);
                annotateQueuedFeedback(
                    failed, queued, NavigationSolverKind::AStar,
                    NavigationTerminalReason::StartHasNoTraversableCell,
                    true);
                publish(failed);
                return NavigationAdapterProcessResult::FeedbackPublished;
            }
            start = adjustedStart.location.cell;
        }
        NavigationCellId goal = queued.request.kind ==
                                        engine::ai::PathRequestKind::Safe
                                    ? start
                                    : goalGrid->cellAt(requestedGoal, clearance);
        uint32_t goalRadiusCells = 0;
        if (queued.request.arrivalRadiusRaw > 0 &&
            startGrid->transform().cellSizeRaw > 0) {
            const int64_t cellSizeRaw = startGrid->transform().cellSizeRaw;
            uint64_t radiusCells = static_cast<uint64_t>(
                queued.request.arrivalRadiusRaw / cellSizeRaw);
            if (queued.request.arrivalRadiusRaw % cellSizeRaw != 0)
                ++radiusCells;
            goalRadiusCells = static_cast<uint32_t>(
                std::min<uint64_t>(radiusCells,
                                   std::numeric_limits<uint32_t>::max()));
        }
        uint32_t adjustedGoalCount = 0;
        uint32_t adjustmentMaximumAnchorOffsetCost = 0;
        if (queued.request.kind != engine::ai::PathRequestKind::Safe &&
            queued.request.adjustDestinations && layers) {
            const bool canSearchAdjustedGoalSet =
                queued.startLayer == queued.goalLayer &&
                goalRadiusCells == 0 &&
                (queued.request.kind == engine::ai::PathRequestKind::New ||
                 queued.request.kind ==
                     engine::ai::PathRequestKind::MoveAside);
            if (canSearchAdjustedGoalSet) {
                const NavigationAdjustmentGoalSet adjusted =
                    collectNavigationAdjustmentGoals(
                        *goalGrid, requestedGoal, queued.goalLayer,
                        queued.request.surfaceMask, clearance,
                        queued.request.objectCells,
                        container::Span<
                            NavigationSearchRequest::AdjustmentGoal>(
                            m_adjustedGoals.data(),
                            m_adjustedGoals.size()));
                if (!adjusted.anchor) {
                    StoredFeedback failed = makeTerminal(
                        queued.request.correlation, confirmedTick,
                        NavigationAdapterStatus::NoPath,
                        engine::ai::PathFeedbackStatus::NoPath,
                        m_navigationRevision);
                    annotateQueuedFeedback(
                        failed, queued, NavigationSolverKind::AStar,
                        NavigationTerminalReason::GoalHasNoAdmissibleCell,
                        true);
                    publish(failed);
                    return NavigationAdapterProcessResult::FeedbackPublished;
                }
                goal = adjusted.anchor;
                if (!adjusted.exact) {
                    adjustedGoalCount = adjusted.count;
                    adjustmentMaximumAnchorOffsetCost =
                        adjusted.maximumAnchorOffsetCost;
                }
            } else {
                const NavigationDestinationAdjustmentResult adjusted =
                    adjustNavigationDestination(
                        *layers,
                        {
                            .desired = requestedGoal,
                            .layer = queued.goalLayer,
                            .movementMask = queued.request.surfaceMask,
                            .clearance = clearance,
                            .allowAdjustment = true,
                        });
                if (!adjusted.accepted()) {
                    StoredFeedback failed = makeTerminal(
                        queued.request.correlation, confirmedTick,
                        NavigationAdapterStatus::NoPath,
                        engine::ai::PathFeedbackStatus::NoPath,
                        m_navigationRevision);
                    annotateQueuedFeedback(
                        failed, queued, NavigationSolverKind::AStar,
                        NavigationTerminalReason::GoalHasNoAdmissibleCell,
                        true);
                    publish(failed);
                    return NavigationAdapterProcessResult::FeedbackPublished;
                }
                goal = adjusted.location.cell;
            }
        }
        m_activePatchPointCount = 0;
        m_activePatchSuffixStart = 0;
        m_activePatchGoalCount = 0;
        m_activePatchReuse = false;
        m_activeAdjustedGoalCount = adjustedGoalCount;
        m_activeAdjustmentMaximumAnchorOffsetCost =
            adjustmentMaximumAnchorOffsetCost;
        if (queued.request.kind == engine::ai::PathRequestKind::Patch &&
            queued.startLayer == queued.goalLayer &&
            queued.request.currentPath) {
            const PathRepositoryRevisionQuery oldRevision =
                repository.storedRevision(queued.request.currentPath);
            const PathRepositoryCopyResult oldPath = oldRevision.status ==
                    PathRepositoryStatus::Success
                ? repository.copyPoints(
                      queued.request.currentPath, oldRevision.revision,
                      m_patchPathScratch)
                : PathRepositoryCopyResult{};
            if (oldPath.status == PathRepositoryStatus::Success &&
                oldPath.pointCount > 1 &&
                oldPath.pointCount <= m_patchGoalCells.size()) {
                NavigationCellId heuristicGoal = InvalidNavigationCell;
                math::q32_32 bestDistanceSquared =
                    math::q32_32::from_raw(
                        std::numeric_limits<int64_t>::max());
                uint32_t suffixStart = oldPath.pointCount;
                uint32_t goalCount = 0;
                for (uint32_t reverse = oldPath.pointCount;
                     reverse > 1; --reverse) {
                    const uint32_t index = reverse - 1;
                    const PathRepositoryPoint& point =
                        m_patchPathScratch[index];
                    if (point.layer != queued.startLayer)
                        break;
                    const NavigationCellId cell = startGrid->cellAt(
                        point.position, clearance);
                    if (!cell || !startGrid->traversable(
                            cell, queued.request.surfaceMask,
                            queued.startLayer, clearance) ||
                        detail::navigationObjectCellBlocked(
                            queued.request.objectCells,
                            queued.startLayer, cell)) {
                        break;
                    }
                    suffixStart = index;
                    m_patchGoalCells[goalCount++] = cell;
                    const math::q32_32 dx =
                        math::q32_32::from_raw(point.position.xRaw) -
                        math::q32_32::from_raw(requestedStart.xRaw);
                    const math::q32_32 dy =
                        math::q32_32::from_raw(point.position.yRaw) -
                        math::q32_32::from_raw(requestedStart.yRaw);
                    const math::q32_32 distanceSquared =
                        dx * dx + dy * dy;
                    if (!heuristicGoal ||
                        distanceSquared < bestDistanceSquared ||
                        (distanceSquared == bestDistanceSquared &&
                         cell < heuristicGoal)) {
                        heuristicGoal = cell;
                        bestDistanceSquared = distanceSquared;
                    }
                }
                std::sort(
                    m_patchGoalCells.begin(),
                    m_patchGoalCells.begin() + goalCount);
                const auto uniqueEnd = std::unique(
                    m_patchGoalCells.begin(),
                    m_patchGoalCells.begin() + goalCount);
                goalCount = static_cast<uint32_t>(
                    uniqueEnd - m_patchGoalCells.begin());
                if (goalCount != 0 && heuristicGoal) {
                    goal = heuristicGoal;
                    m_activePatchPointCount = oldPath.pointCount;
                    m_activePatchSuffixStart = suffixStart;
                    m_activePatchGoalCount = goalCount;
                    m_activePatchReuse = true;
                }
            }
        }
        m_active = true;
        m_activeRequest = queued.request;
        m_activeRevision = {queued.navigationRevision};
        m_activeStartLayer = queued.startLayer;
        m_activeGoalLayer = queued.goalLayer;
        m_activeStartCell = start;
        m_activeGoalCell = goal;
        const auto hasCompatibleZone = [&](NavigationLayerId layer) {
            return std::any_of(
                zones.begin(), zones.end(),
                [&](const NavigationZoneField& zone) {
                    return zone.layer() == layer &&
                        zone.profile() == m_profile &&
                        zone.movementMask() == queued.request.surfaceMask &&
                        zone.clearanceClass() == clearance;
                });
        };
        const bool portalZonesAvailable =
            hasCompatibleZone(queued.startLayer) &&
            hasCompatibleZone(queued.goalLayer);
        const bool blockedAnnulusGoal = goalRadiusCells != 0 &&
            queued.startLayer == queued.goalLayer && goalGrid &&
            !goalGrid->traversable(
                goal, queued.request.surfaceMask, queued.goalLayer,
                clearance);
        m_activeUsesPortalRouter = queued.request.kind !=
                engine::ai::PathRequestKind::Safe &&
            queued.request.kind !=
                engine::ai::PathRequestKind::Approach &&
            portalGraph && layers &&
            !m_activePatchReuse &&
            m_activeAdjustedGoalCount == 0 &&
            portalZonesAvailable &&
            !blockedAnnulusGoal &&
            (portalGraph->edgeCount() != 0 ||
             queued.startLayer != queued.goalLayer);
        if (queued.startLayer != queued.goalLayer &&
            !m_activeUsesPortalRouter) {
            StoredFeedback failed = makeTerminal(
                m_activeRequest.correlation, confirmedTick,
                NavigationAdapterStatus::InvalidRequest,
                engine::ai::PathFeedbackStatus::NoPath,
                m_activeRevision);
            annotateActiveFeedback(
                failed, NavigationSolverKind::Portal,
                NavigationTerminalReason::CrossLayerRouteUnavailable);
            publish(failed);
            clearActive();
            return NavigationAdapterProcessResult::FeedbackPublished;
        }
        if (m_activeUsesPortalRouter) {
            if (expansionBudget == 0)
                return NavigationAdapterProcessResult::Progressed;
            return finishPortalRoute(
                confirmedTick, layers, zones, portalGraph,
                dynamicOverlay, expansionBudget, repository);
        }
        NavigationSearchRequest searchRequest{queued.request.correlation.generation,
                                              start,
                                              goal,
                                              m_profile,
                                              queued.request.surfaceMask,
                                              queued.startLayer,
                                              clearance};
        searchRequest.goalRadiusCells = goalRadiusCells;
        searchRequest.ignoredObstacle =
            queued.request.ignoredObstacle.value;
        searchRequest.crusherLevel = queued.request.crusherLevel;
        searchRequest.dozerPassableObstacles =
            queued.request.dozerPassableObstacles;
        if (m_activeAdjustedGoalCount != 0) {
            searchRequest.adjustment.enabled = true;
            searchRequest.adjustment.goals = {
                m_adjustedGoals.data(), m_activeAdjustedGoalCount};
            searchRequest.adjustment.maximumAnchorOffsetCost =
                m_activeAdjustmentMaximumAnchorOffsetCost;
        }
        if (m_activePatchReuse) {
            searchRequest.goalRadiusCells = 0;
            searchRequest.patch.enabled = true;
            searchRequest.patch.suffixGoals = {
                m_patchGoalCells.data(), m_activePatchGoalCount};
        }
        if (queued.request.kind ==
                engine::ai::PathRequestKind::Approach) {
            searchRequest.attack.enabled = true;
            searchRequest.attack.target = requestedGoal;
            searchRequest.attack.minimumRangeRaw =
                queued.request.minimumArrivalRadiusRaw;
            searchRequest.attack.maximumRangeRaw =
                queued.request.arrivalRadiusRaw;
            searchRequest.attack.lineOfSightEnabled =
                queued.request.attackLineOfSightEnabled;
            searchRequest.attack.subject =
                queued.request.correlation.subject.value;
            searchRequest.attack.targetObject =
                queued.request.attackTarget.value;
            searchRequest.attack.subjectContainer =
                queued.request.attackSubjectContainer.value;
            searchRequest.attack.targetContainer =
                queued.request.attackTargetContainer.value;
            searchRequest.attack.subjectSlaver =
                queued.request.attackSubjectSlaver.value;
            searchRequest.attack.targetSlaver =
                queued.request.attackTargetSlaver.value;
            searchRequest.attack.seeThroughObstacles =
                queued.request.attackSeeThroughObstacles;
        }
        if (queued.request.kind == engine::ai::PathRequestKind::Safe) {
            searchRequest.safe.enabled = true;
            searchRequest.safe.repulsor1 = {
                queued.request.safePathRepulsorPosition.xRaw,
                queued.request.safePathRepulsorPosition.yRaw,
                queued.request.safePathRepulsorPosition.zRaw};
            searchRequest.safe.repulsor2 = {
                queued.request.safePathRepulsor2Position.xRaw,
                queued.request.safePathRepulsor2Position.yRaw,
                queued.request.safePathRepulsor2Position.zRaw};
            searchRequest.safe.hasRepulsor2 =
                queued.request.safePathRepulsor2 != INVALID_OBJECT_ID;
            searchRequest.safe.radiusRaw = queued.request.safePathRadiusRaw;
        }
        const NavigationSearchStatus beginStatus =
            m_oracle.begin(
                *startGrid, m_scratch, searchRequest,
                queued.request.objectCells, dynamicOverlay);
        if (beginStatus != NavigationSearchStatus::Pending)
            return finishSearch(
                confirmedTick, *startGrid, repository, beginStatus,
                dynamicOverlay);
    }

    if (m_activeRevision != m_navigationRevision)
    {
        // A topology publication may complete while this bounded search is
        // active.  The computed path snapshot is stale, but the owning Move
        // order is still valid.  Preserve the correlation and ask admission
        // to re-freeze the request against the newly published revision.
        publish(makeDelayed(m_activeRequest.correlation, confirmedTick,
                            m_navigationRevision,
                            NavigationTerminalReason::
                                TopologyRevisionChanged));
        clearActive();
        return NavigationAdapterProcessResult::FeedbackPublished;
    }
    if (expansionBudget == 0)
        return NavigationAdapterProcessResult::Progressed;
    if (m_activeUsesPortalRouter)
    {
        return finishPortalRoute(
            confirmedTick, layers, zones, portalGraph,
            dynamicOverlay, expansionBudget, repository);
    }
    const NavigationGrid* activeGrid = gridForLayer(m_activeStartLayer);
    if (!activeGrid)
        return finishSearch(confirmedTick, grid, repository,
                            NavigationSearchStatus::InvalidRequest,
                            dynamicOverlay);
    const NavigationSearchProgress progress =
        m_oracle.step(
            *activeGrid, m_scratch, expansionBudget,
            m_activeRequest.objectCells, dynamicOverlay);
    m_expansionsConsumedLastProcess = progress.expandedThisStep;
    if (progress.status == NavigationSearchStatus::Pending)
        return NavigationAdapterProcessResult::Progressed;
    return finishSearch(
        confirmedTick, *activeGrid, repository, progress.status,
        dynamicOverlay);
}

NavigationPathMetadata NavigationPathService::makePathMetadata(
    container::Span<const NavigationLayerPathPoint> points,
    NavigationLayerId layer,
    uint32_t clearanceRadiusCells) const noexcept
{
    NavigationPathMetadata metadata;
    metadata.revisions = m_navigationRevisions;
    metadata.layer = layer;
    if (m_metadataGridWidth == 0 || m_metadataGridHeight == 0)
        return metadata;

    constexpr uint32_t kCellsPerCorridorChunk = 16;
    NavigationCellBounds pendingChunk;
    uint32_t pendingCellCount = 0;
    const auto flushChunk = [&]() {
        if (!pendingChunk.valid()) return;
        if (metadata.corridorChunkCount <
            NavigationPathMetadata::MaximumCorridorChunks) {
            metadata.corridorChunks[metadata.corridorChunkCount++] =
                pendingChunk;
        } else {
            metadata.corridorChunks[
                NavigationPathMetadata::MaximumCorridorChunks - 1]
                .include(pendingChunk);
        }
        pendingChunk = {};
        pendingCellCount = 0;
    };
    const auto includeCoordinate = [&](int32_t x, int32_t y) {
        if (x < 0 || y < 0 ||
            x >= static_cast<int32_t>(m_metadataGridWidth) ||
            y >= static_cast<int32_t>(m_metadataGridHeight)) {
            return;
        }
        const NavigationCellId cell{
            static_cast<uint32_t>(y) * m_metadataGridWidth +
            static_cast<uint32_t>(x)};
        metadata.affectedCells.include(cell, m_metadataGridWidth);
        if (metadata.corridorChunkCount >=
            NavigationPathMetadata::MaximumCorridorChunks) {
            metadata.corridorChunks[
                NavigationPathMetadata::MaximumCorridorChunks - 1]
                .include(cell, m_metadataGridWidth);
            return;
        }
        pendingChunk.include(cell, m_metadataGridWidth);
        if (++pendingCellCount == kCellsPerCorridorChunk)
            flushChunk();
    };
    const auto coordinateOf = [&](NavigationCellId cell) {
        return NavigationGridCoordinate{
            static_cast<int32_t>(cell.value % m_metadataGridWidth),
            static_cast<int32_t>(cell.value / m_metadataGridWidth)};
    };

    NavigationLayerPathPoint previous;
    bool hasPrevious = false;
    for (const NavigationLayerPathPoint& point : points) {
        if (!point.location.cell) continue;
        const NavigationGridCoordinate end =
            coordinateOf(point.location.cell);
        if (!hasPrevious ||
            previous.location.layer != point.location.layer) {
            flushChunk();
            includeCoordinate(end.x, end.y);
            previous = point;
            hasPrevious = true;
            continue;
        }
        if (metadata.corridorChunkCount >=
            NavigationPathMetadata::MaximumCorridorChunks) {
            includeCoordinate(end.x, end.y);
            previous = point;
            continue;
        }

        NavigationGridCoordinate cursor =
            coordinateOf(previous.location.cell);
        const int64_t deltaX = std::abs(
            static_cast<int64_t>(end.x) - cursor.x);
        const int32_t stepX = cursor.x < end.x ? 1 : -1;
        const int64_t deltaY = -std::abs(
            static_cast<int64_t>(end.y) - cursor.y);
        const int32_t stepY = cursor.y < end.y ? 1 : -1;
        int64_t error = deltaX + deltaY;
        while (cursor.x != end.x || cursor.y != end.y) {
            const int64_t doubledError = error * 2;
            if (doubledError >= deltaY) {
                error += deltaY;
                cursor.x += stepX;
            }
            if (doubledError <= deltaX) {
                error += deltaX;
                cursor.y += stepY;
            }
            includeCoordinate(cursor.x, cursor.y);
            if (metadata.corridorChunkCount >=
                    NavigationPathMetadata::MaximumCorridorChunks &&
                (cursor.x != end.x || cursor.y != end.y)) {
                includeCoordinate(end.x, end.y);
                break;
            }
        }
        previous = point;
    }
    flushChunk();

    const int32_t radius = static_cast<int32_t>(clearanceRadiusCells);
    const auto expand = [&](NavigationCellBounds& bounds) {
        if (!bounds.valid()) return;
        bounds.minX = std::max(0, bounds.minX - radius);
        bounds.minY = std::max(0, bounds.minY - radius);
        bounds.maxX = std::min(
            static_cast<int32_t>(m_metadataGridWidth) - 1,
            bounds.maxX + radius);
        bounds.maxY = std::min(
            static_cast<int32_t>(m_metadataGridHeight) - 1,
            bounds.maxY + radius);
    };
    expand(metadata.affectedCells);
    for (size_t index = 0;
         index < metadata.corridorChunkCount; ++index) {
        expand(metadata.corridorChunks[index]);
    }
    return metadata;
}

NavigationAdapterProcessResult NavigationPathService::finishImmediateRequest(
    uint64_t confirmedTick,
    const NavigationGrid& primaryGrid,
    const QueuedNavigationRequest& queued,
    const NavigationLayerSet* layers,
    const engine::ai::AIWaypointGraphResolver* waypointGraph,
    const NavigationDynamicOverlay* dynamicOverlay,
    PathRepository& repository) noexcept
{
    if (queued.navigationRevision != m_navigationRevision.value) {
        publish(makeDelayed(queued.request.correlation, confirmedTick,
                            m_navigationRevision));
        return NavigationAdapterProcessResult::FeedbackPublished;
    }
    if (queued.request.traversalMode ==
        engine::ai::AIPathTraversalMode::DirectLine) {
        const NavigationGrid* directGrid = layers
            ? layers->find(queued.startLayer)
            : (queued.startLayer == m_layer ? &primaryGrid : nullptr);
        if (!directGrid || queued.startLayer != queued.goalLayer) {
            publish(makeTerminal(
                queued.request.correlation, confirmedTick,
                NavigationAdapterStatus::InvalidRequest,
                engine::ai::PathFeedbackStatus::NoPath,
                m_navigationRevision));
            return NavigationAdapterProcessResult::FeedbackPublished;
        }
        return finishDirectLine(
            confirmedTick, *directGrid, queued, dynamicOverlay,
            repository);
    }
    if (queued.request.traversalMode ==
        engine::ai::AIPathTraversalMode::WaypointPolyline) {
        return finishWaypointPolyline(
            confirmedTick, queued, layers, waypointGraph, repository);
    }
    publish(makeTerminal(
        queued.request.correlation, confirmedTick,
        NavigationAdapterStatus::UnsupportedTraversal,
        engine::ai::PathFeedbackStatus::Unsupported,
        m_navigationRevision));
    return NavigationAdapterProcessResult::FeedbackPublished;
}

NavigationAdapterProcessResult NavigationPathService::finishDirectLine(
    uint64_t confirmedTick,
    const NavigationGrid& grid,
    const QueuedNavigationRequest& queued,
    const NavigationDynamicOverlay* dynamicOverlay,
    PathRepository& repository) noexcept
{
    const engine::ai::PathRequest& request = queued.request;
    const NavigationRevision revision{queued.navigationRevision};
    const bool airQuickPath =
        (request.surfaceMask & NavigationMovement::Air) != 0;
    const bool validRequest =
        (request.kind == engine::ai::PathRequestKind::New ||
         request.kind == engine::ai::PathRequestKind::Patch) &&
        (queued.startLayer == queued.goalLayer || airQuickPath) &&
        m_pathPoints.size() >= 2u;
    if (!validRequest) {
        publish(makeTerminal(
            request.correlation, confirmedTick,
            NavigationAdapterStatus::InvalidRequest,
            engine::ai::PathFeedbackStatus::NoPath, revision));
        return NavigationAdapterProcessResult::FeedbackPublished;
    }

    // RefCode AIUpdateInterface::computeQuickPath creates this two-point
    // route for both airborne locomotors and the first production-exit leg.
    // The latter may start inside a producer footprint; an air route may pass
    // over arbitrary ground occupancy.  Therefore neither endpoint nor the
    // segment is admitted through the ground overlay here. Physics still owns
    // collision response and carries ignoredObstacle for the exit case.
    NavigationWorldPosition goal = toWorld(request.originalGoal);
    NavigationWorldPosition start = toWorld(request.start);
    if (request.airWings && dynamicOverlay &&
        grid.transform().cellSizeRaw > 0) {
        const NavigationCellId requestedGoalCell = grid.cellAt(goal);
        if (requestedGoalCell) {
            const NavigationGridCoordinate goalCoordinate =
                grid.coordinate(requestedGoalCell);
            const int64_t wingRadiusRaw =
                math::q32_32{int32_t{100}}.raw();
            const int64_t cellSizeRaw = grid.transform().cellSizeRaw;
            int64_t radiusCells = wingRadiusRaw / cellSizeRaw;
            if (wingRadiusRaw % cellSizeRaw != 0) ++radiusCells;
            radiusCells += 2;
            radiusCells = std::min<int64_t>(
                radiusCells, std::numeric_limits<int32_t>::max());

            NavigationAirObstacleQuery closest;
            uint64_t closestDistance = std::numeric_limits<uint64_t>::max();
            const int64_t minX = std::max<int64_t>(
                0, static_cast<int64_t>(goalCoordinate.x) - radiusCells);
            const int64_t minY = std::max<int64_t>(
                0, static_cast<int64_t>(goalCoordinate.y) - radiusCells);
            const int64_t maxX = std::min<int64_t>(
                static_cast<int64_t>(grid.width()) - 1,
                static_cast<int64_t>(goalCoordinate.x) + radiusCells);
            const int64_t maxY = std::min<int64_t>(
                static_cast<int64_t>(grid.height()) - 1,
                static_cast<int64_t>(goalCoordinate.y) + radiusCells);
            for (int64_t y = minY; y <= maxY; ++y) {
                for (int64_t x = minX; x <= maxX; ++x) {
                    const NavigationAirObstacleQuery obstacle =
                        dynamicOverlay->airObstacleAt(
                            grid.cellId({static_cast<int32_t>(x),
                                         static_cast<int32_t>(y)}),
                            request.ignoredObstacle.value);
                    if (!obstacle.found()) continue;
                    const int64_t nearestX = std::clamp<int64_t>(
                        goalCoordinate.x, obstacle.bounds.minX,
                        obstacle.bounds.maxX);
                    const int64_t nearestY = std::clamp<int64_t>(
                        goalCoordinate.y, obstacle.bounds.minY,
                        obstacle.bounds.maxY);
                    const uint64_t dx = static_cast<uint64_t>(
                        std::abs(static_cast<int64_t>(goalCoordinate.x) -
                                 nearestX));
                    const uint64_t dy = static_cast<uint64_t>(
                        std::abs(static_cast<int64_t>(goalCoordinate.y) -
                                 nearestY));
                    const uint64_t distance = dx * dx + dy * dy;
                    if (!closest.found() || distance < closestDistance ||
                        (distance == closestDistance &&
                         obstacle.entityId < closest.entityId)) {
                        closest = obstacle;
                        closestDistance = distance;
                    }
                }
            }

            if (closest.found()) {
                const int64_t expandedMinX =
                    static_cast<int64_t>(closest.bounds.minX) - radiusCells;
                const int64_t expandedMaxX =
                    static_cast<int64_t>(closest.bounds.maxX) + radiusCells;
                const int64_t expandedMinY =
                    static_cast<int64_t>(closest.bounds.minY) - radiusCells;
                const int64_t expandedMaxY =
                    static_cast<int64_t>(closest.bounds.maxY) + radiusCells;
                if (goalCoordinate.x >= expandedMinX &&
                    goalCoordinate.x <= expandedMaxX &&
                    goalCoordinate.y >= expandedMinY &&
                    goalCoordinate.y <= expandedMaxY) {
                    struct ClipCandidate final {
                        int64_t x = 0;
                        int64_t y = 0;
                        uint64_t distance =
                            std::numeric_limits<uint64_t>::max();
                    } best;
                    const int64_t candidates[4][2] = {
                        {expandedMinX - 1, goalCoordinate.y},
                        {expandedMaxX + 1, goalCoordinate.y},
                        {goalCoordinate.x, expandedMinY - 1},
                        {goalCoordinate.x, expandedMaxY + 1},
                    };
                    for (const auto& candidate : candidates) {
                        if (candidate[0] < 0 || candidate[1] < 0 ||
                            candidate[0] >=
                                static_cast<int64_t>(grid.width()) ||
                            candidate[1] >=
                                static_cast<int64_t>(grid.height())) {
                            continue;
                        }
                        const uint64_t dx = static_cast<uint64_t>(std::abs(
                            candidate[0] - goalCoordinate.x));
                        const uint64_t dy = static_cast<uint64_t>(std::abs(
                            candidate[1] - goalCoordinate.y));
                        const uint64_t distance = dx * dx + dy * dy;
                        if (distance < best.distance) {
                            best = {candidate[0], candidate[1], distance};
                        }
                    }
                    if (best.distance !=
                        std::numeric_limits<uint64_t>::max()) {
                        NavigationWorldPosition adjusted;
                        if (grid.cellPosition(
                                grid.cellId({static_cast<int32_t>(best.x),
                                             static_cast<int32_t>(best.y)}),
                                toNavigationClearance(
                                    request.clearanceProfile),
                                adjusted)) {
                            adjusted.zRaw = goal.zRaw;
                            goal = adjusted;
                        }
                    }
                }
            }
        }
    }
    // computeQuickPath flattens the synthetic first node onto the
    // destination's Z before linking the two nodes.
    start.zRaw = goal.zRaw;
    m_pathPoints[0] = {
        {queued.startLayer, grid.cellAt(start)}, start};
    m_pathPoints[1] = {
        {queued.startLayer, grid.cellAt(goal)}, goal};
    size_t pointCount = 2;
    if (airQuickPath && dynamicOverlay) {
        constexpr uint32_t kMaximumAircraftDetours = 20;
        uint32_t detours = 0;
        size_t segment = 0;
        while (segment + 1 < pointCount) {
            const NavigationAirObstacleQuery obstacle =
                firstAirObstacleOnSegment(
                    grid, *dynamicOverlay,
                    m_pathPoints[segment].position,
                    m_pathPoints[segment + 1].position,
                    request.ignoredObstacle.value);
            if (!obstacle.found()) {
                ++segment;
                continue;
            }
            if (detours == kMaximumAircraftDetours ||
                pointCount + 2 > m_pathPoints.size()) {
                publish(makeTerminal(
                    request.correlation, confirmedTick,
                    NavigationAdapterStatus::NoPath,
                    engine::ai::PathFeedbackStatus::NoPath, revision));
                return NavigationAdapterProcessResult::FeedbackPublished;
            }

            struct DetourCandidate final {
                NavigationGridCoordinate first;
                NavigationGridCoordinate second;
                uint64_t cost = std::numeric_limits<uint64_t>::max();
                bool valid = false;
            };
            const int32_t margin = static_cast<int32_t>(
                clearanceRadiusCells(toNavigationClearance(
                    request.clearanceProfile))) + 1;
            const int64_t left =
                static_cast<int64_t>(obstacle.bounds.minX) - margin;
            const int64_t right =
                static_cast<int64_t>(obstacle.bounds.maxX) + margin;
            const int64_t bottom =
                static_cast<int64_t>(obstacle.bounds.minY) - margin;
            const int64_t top =
                static_cast<int64_t>(obstacle.bounds.maxY) + margin;
            const NavigationGridCoordinate rawCandidates[8][2] = {
                {{static_cast<int32_t>(left), static_cast<int32_t>(bottom)},
                 {static_cast<int32_t>(right), static_cast<int32_t>(bottom)}},
                {{static_cast<int32_t>(right), static_cast<int32_t>(bottom)},
                 {static_cast<int32_t>(left), static_cast<int32_t>(bottom)}},
                {{static_cast<int32_t>(left), static_cast<int32_t>(top)},
                 {static_cast<int32_t>(right), static_cast<int32_t>(top)}},
                {{static_cast<int32_t>(right), static_cast<int32_t>(top)},
                 {static_cast<int32_t>(left), static_cast<int32_t>(top)}},
                {{static_cast<int32_t>(left), static_cast<int32_t>(bottom)},
                 {static_cast<int32_t>(left), static_cast<int32_t>(top)}},
                {{static_cast<int32_t>(left), static_cast<int32_t>(top)},
                 {static_cast<int32_t>(left), static_cast<int32_t>(bottom)}},
                {{static_cast<int32_t>(right), static_cast<int32_t>(bottom)},
                 {static_cast<int32_t>(right), static_cast<int32_t>(top)}},
                {{static_cast<int32_t>(right), static_cast<int32_t>(top)},
                 {static_cast<int32_t>(right), static_cast<int32_t>(bottom)}},
            };
            DetourCandidate best;
            const NavigationGridCoordinate fromCoordinate = grid.coordinate(
                grid.cellAt(m_pathPoints[segment].position));
            const NavigationGridCoordinate toCoordinate = grid.coordinate(
                grid.cellAt(m_pathPoints[segment + 1].position));
            const auto distance = [](NavigationGridCoordinate from,
                                     NavigationGridCoordinate to) noexcept {
                const uint64_t dx = from.x >= to.x
                    ? static_cast<uint64_t>(from.x) -
                          static_cast<uint64_t>(to.x)
                    : static_cast<uint64_t>(to.x) -
                          static_cast<uint64_t>(from.x);
                const uint64_t dy = from.y >= to.y
                    ? static_cast<uint64_t>(from.y) -
                          static_cast<uint64_t>(to.y)
                    : static_cast<uint64_t>(to.y) -
                          static_cast<uint64_t>(from.y);
                return dx + dy;
            };
            for (const auto& raw : rawCandidates) {
                if (raw[0].x < 0 || raw[0].y < 0 ||
                    raw[1].x < 0 || raw[1].y < 0 ||
                    raw[0].x >= static_cast<int32_t>(grid.width()) ||
                    raw[1].x >= static_cast<int32_t>(grid.width()) ||
                    raw[0].y >= static_cast<int32_t>(grid.height()) ||
                    raw[1].y >= static_cast<int32_t>(grid.height()))
                    continue;
                const NavigationCellId firstCell = grid.cellId(raw[0]);
                const NavigationCellId secondCell = grid.cellId(raw[1]);
                if (!grid.traversable(
                        firstCell, request.surfaceMask,
                        queued.startLayer,
                        toNavigationClearance(
                            request.clearanceProfile)) ||
                    !grid.traversable(
                        secondCell, request.surfaceMask,
                        queued.startLayer,
                        toNavigationClearance(
                            request.clearanceProfile)))
                    continue;
                NavigationWorldPosition firstPosition;
                NavigationWorldPosition secondPosition;
                if (!grid.cellPosition(
                        firstCell,
                        toNavigationClearance(request.clearanceProfile),
                        firstPosition) ||
                    !grid.cellPosition(
                        secondCell,
                        toNavigationClearance(request.clearanceProfile),
                        secondPosition))
                    continue;
                firstPosition.zRaw = goal.zRaw;
                secondPosition.zRaw = goal.zRaw;
                const NavigationAirObstacleQuery firstHit =
                    firstAirObstacleOnSegment(
                        grid, *dynamicOverlay,
                        m_pathPoints[segment].position, firstPosition,
                        request.ignoredObstacle.value);
                const NavigationAirObstacleQuery middleHit =
                    firstAirObstacleOnSegment(
                        grid, *dynamicOverlay, firstPosition,
                        secondPosition, request.ignoredObstacle.value);
                const NavigationAirObstacleQuery lastHit =
                    firstAirObstacleOnSegment(
                        grid, *dynamicOverlay, secondPosition,
                        m_pathPoints[segment + 1].position,
                        request.ignoredObstacle.value);
                if ((firstHit.found() &&
                     firstHit.entityId == obstacle.entityId) ||
                    (middleHit.found() &&
                     middleHit.entityId == obstacle.entityId) ||
                    (lastHit.found() &&
                     lastHit.entityId == obstacle.entityId))
                    continue;
                const uint64_t cost =
                    distance(fromCoordinate, raw[0]) +
                    distance(raw[0], raw[1]) +
                    distance(raw[1], toCoordinate);
                if (!best.valid || cost < best.cost) {
                    best = {raw[0], raw[1], cost, true};
                }
            }
            if (!best.valid) {
                publish(makeTerminal(
                    request.correlation, confirmedTick,
                    NavigationAdapterStatus::NoPath,
                    engine::ai::PathFeedbackStatus::NoPath, revision));
                return NavigationAdapterProcessResult::FeedbackPublished;
            }
            const NavigationCellId firstCell = grid.cellId(best.first);
            const NavigationCellId secondCell = grid.cellId(best.second);
            NavigationWorldPosition firstPosition;
            NavigationWorldPosition secondPosition;
            if (!grid.cellPosition(
                    firstCell,
                    toNavigationClearance(request.clearanceProfile),
                    firstPosition) ||
                !grid.cellPosition(
                    secondCell,
                    toNavigationClearance(request.clearanceProfile),
                    secondPosition)) {
                publish(makeTerminal(
                    request.correlation, confirmedTick,
                    NavigationAdapterStatus::InvalidRequest,
                    engine::ai::PathFeedbackStatus::NoPath, revision));
                return NavigationAdapterProcessResult::FeedbackPublished;
            }
            firstPosition.zRaw = goal.zRaw;
            secondPosition.zRaw = goal.zRaw;
            for (size_t move = pointCount; move > segment + 1; --move)
                m_pathPoints[move + 1] = m_pathPoints[move - 1];
            m_pathPoints[segment + 1] = {
                {queued.startLayer, firstCell}, firstPosition};
            m_pathPoints[segment + 2] = {
                {queued.startLayer, secondCell}, secondPosition};
            pointCount += 2;
            ++detours;
        }
    }
    const container::Span<const NavigationLayerPathPoint> points(
        m_pathPoints.data(), pointCount);
    const PathRepositoryCreateResult stored = repository.create(
        revision, points,
        makePathMetadata(
            points, queued.startLayer,
            request.clearanceProfile.radiusCells),
        PathRepositoryPointValidation::LayerOnlyWorldPolyline);
    if (stored.status == PathRepositoryStatus::CapacityExhausted) {
        publish(makeDelayed(request.correlation, confirmedTick, revision));
        return NavigationAdapterProcessResult::FeedbackPublished;
    }
    if (stored.status != PathRepositoryStatus::Success) {
        publish(makeTerminal(
            request.correlation, confirmedTick,
            NavigationAdapterStatus::InvalidRequest,
            engine::ai::PathFeedbackStatus::NoPath, revision));
        return NavigationAdapterProcessResult::FeedbackPublished;
    }

    StoredFeedback ready = makeTerminal(
        request.correlation, confirmedTick,
        NavigationAdapterStatus::Ready,
        engine::ai::PathFeedbackStatus::Ready, revision);
    annotateQueuedFeedback(
        ready, queued, NavigationSolverKind::DirectLine,
        NavigationTerminalReason::GoalReached,
        request.adjustDestinations);
    ready.value.feedback.path = stored.handle;
    ready.value.feedback.adjustedGoal = toAI(goal);
    ready.value.feedback.adjustedLayer = queued.goalLayer.value;
    publish(ready);
    return NavigationAdapterProcessResult::FeedbackPublished;
}

NavigationAdapterProcessResult
NavigationPathService::finishWaypointPolyline(
    uint64_t confirmedTick,
    const QueuedNavigationRequest& queued,
    const NavigationLayerSet* layers,
    const engine::ai::AIWaypointGraphResolver* waypointGraph,
    PathRepository& repository) noexcept
{
    const engine::ai::PathRequest& request = queued.request;
    const NavigationRevision revision{queued.navigationRevision};
    const NavigationGrid* grid = layers
        ? layers->find(queued.startLayer)
        : nullptr;
    const bool validRequest = waypointGraph && grid &&
        (request.kind == engine::ai::PathRequestKind::New ||
         request.kind == engine::ai::PathRequestKind::Patch) &&
        request.waypointStart && request.waypointGraphRevision != 0 &&
        request.waypointHopLimit != 0 &&
        queued.startLayer == queued.goalLayer;
    if (!validRequest || m_pathPoints.empty()) {
        publish(makeTerminal(
            request.correlation, confirmedTick,
            NavigationAdapterStatus::UnsupportedTraversal,
            engine::ai::PathFeedbackStatus::Unsupported, revision));
        return NavigationAdapterProcessResult::FeedbackPublished;
    }

    const auto saturatingAdd = [](int64_t left, int64_t right) noexcept {
        if (right > 0 &&
            left > std::numeric_limits<int64_t>::max() - right) {
            return std::numeric_limits<int64_t>::max();
        }
        if (right < 0 &&
            left < std::numeric_limits<int64_t>::min() - right) {
            return std::numeric_limits<int64_t>::min();
        }
        return left + right;
    };
    const auto appendPoint = [this, grid, &queued, &saturatingAdd](
                                 size_t& count,
                                 const engine::ai::AIFixedPosition& source,
                                 bool applyOffset) noexcept {
        if (count >= m_pathPoints.size()) return false;
        NavigationWorldPosition position = toWorld(source);
        if (applyOffset) {
            position.xRaw = saturatingAdd(
                position.xRaw, queued.request.polylineOffset.xRaw);
            position.yRaw = saturatingAdd(
                position.yRaw, queued.request.polylineOffset.yRaw);
        }
        // ZH's exact waypoint path is not a navmesh search. It preserves the
        // authored world polyline, allows invalid/off-map positions and only
        // uses the grid cell (when one exists) for dirty-region metadata.
        // Rejecting an unmappable point here turns a legal cinematic route
        // into Unsupported and leaves the whole Scenario Team motionless.
        const NavigationCellId cell = grid->cellAt(position);
        m_pathPoints[count++] = {
            {queued.startLayer, cell}, position};
        return true;
    };

    size_t pointCount = 0;
    if (!appendPoint(pointCount, request.start, false)) {
        publish(makeTerminal(
            request.correlation, confirmedTick,
            NavigationAdapterStatus::InvalidRequest,
            engine::ai::PathFeedbackStatus::NoPath, revision));
        return NavigationAdapterProcessResult::FeedbackPublished;
    }

    engine::ai::AIWaypointHandle current = request.waypointStart;
    bool reachedTerminal = false;
    for (uint32_t hop = 0; hop < request.waypointHopLimit; ++hop) {
        const engine::ai::AIWaypointQuery node = waypointGraph->node(
            current, request.waypointGraphRevision);
        if (node.status != engine::ai::AIWaypointQueryStatus::Node ||
            !appendPoint(pointCount, node.node.position, true)) {
            publish(makeTerminal(
                request.correlation, confirmedTick,
                NavigationAdapterStatus::InvalidRequest,
                engine::ai::PathFeedbackStatus::Unsupported, revision));
            return NavigationAdapterProcessResult::FeedbackPublished;
        }
        if (node.node.linkCount == 0) {
            NavigationLayerPathPoint& terminal =
                m_pathPoints[pointCount - 1];
            if (terminal.location.cell) {
                NavigationWorldPosition snapped;
                if (grid->cellPosition(
                        terminal.location.cell,
                        toNavigationClearance(
                            request.clearanceProfile),
                        snapped)) {
                    // AIUpdateInterface::setPathFromWaypoint snaps only the
                    // final waypoint to the unit's radius/phase grid slot.
                    terminal.position = snapped;
                }
            }
            reachedTerminal = true;
            break;
        }
        const engine::ai::AIWaypointLinkQuery link = waypointGraph->link(
            current, request.waypointGraphRevision, 0);
        if (link.status != engine::ai::AIWaypointQueryStatus::Node ||
            !link.target || link.target == current) {
            publish(makeTerminal(
                request.correlation, confirmedTick,
                NavigationAdapterStatus::InvalidRequest,
                engine::ai::PathFeedbackStatus::Unsupported, revision));
            return NavigationAdapterProcessResult::FeedbackPublished;
        }
        current = link.target;
    }
    // RefCode appends through WAYPOINT_PATH_LIMIT and then executes that
    // valid prefix even when a malformed/cyclic graph never terminates.
    // `reachedTerminal` only controls final-point snapping above.
    static_cast<void>(reachedTerminal);

    const PathRepositoryCreateResult stored = repository.create(
        revision,
        container::Span<const NavigationLayerPathPoint>(
            m_pathPoints.data(), pointCount),
        makePathMetadata(
            container::Span<const NavigationLayerPathPoint>(
                m_pathPoints.data(), pointCount),
            queued.startLayer,
            queued.request.clearanceProfile.radiusCells),
        PathRepositoryPointValidation::LayerOnlyWorldPolyline);
    if (stored.status == PathRepositoryStatus::CapacityExhausted) {
        publish(makeDelayed(request.correlation, confirmedTick, revision));
        return NavigationAdapterProcessResult::FeedbackPublished;
    }
    if (stored.status != PathRepositoryStatus::Success) {
        publish(makeTerminal(
            request.correlation, confirmedTick,
            NavigationAdapterStatus::InvalidRequest,
            engine::ai::PathFeedbackStatus::Unsupported, revision));
        return NavigationAdapterProcessResult::FeedbackPublished;
    }

    StoredFeedback ready = makeTerminal(
        request.correlation, confirmedTick,
        NavigationAdapterStatus::Ready,
        engine::ai::PathFeedbackStatus::Ready, revision);
    annotateQueuedFeedback(
        ready, queued, NavigationSolverKind::WaypointPolyline,
        NavigationTerminalReason::GoalReached);
    ready.value.feedback.path = stored.handle;
    ready.value.feedback.adjustedGoal =
        toAI(m_pathPoints[pointCount - 1].position);
    ready.value.feedback.adjustedLayer = queued.goalLayer.value;
    publish(ready);
    return NavigationAdapterProcessResult::FeedbackPublished;
}

NavigationAdapterProcessResult NavigationPathService::finishPortalRoute(
    uint64_t confirmedTick,
    const NavigationLayerSet* layers,
    container::Span<const NavigationZoneField> zones,
    const NavigationPortalGraph* graph,
    const NavigationDynamicOverlay* dynamicOverlay,
    uint32_t expansionBudget,
    PathRepository& repository) noexcept
{
    NavigationPortalRouteStatus status =
        NavigationPortalRouteStatus::InvalidRequest;
    NavigationPortalRouteResult route;
    if (layers && graph) {
        const NavigationGrid* startGrid =
            layers->find(m_activeStartLayer);
        const NavigationGrid* goalGrid =
            layers->find(m_activeGoalLayer);
        if (startGrid && goalGrid) {
            const NavigationPortalRouteRequest request{
                .requestId = m_activeRequest.correlation.generation,
                .start = {m_activeStartLayer, m_activeStartCell},
                .goal = {m_activeGoalLayer, m_activeGoalCell},
                .profile = m_profile,
                .movementMask = m_activeRequest.surfaceMask,
                .clearance = toNavigationClearance(
                    m_activeRequest.clearanceProfile),
                .ignoredObstacle =
                    m_activeRequest.ignoredObstacle.value,
                .crusherLevel = m_activeRequest.crusherLevel,
                .dozerPassableObstacles =
                    m_activeRequest.dozerPassableObstacles,
                .objectCells = m_activeRequest.objectCells,
            };
            if (!m_activePortalRouteStarted) {
                route = m_portalRouter.beginRoute(
                    *layers, zones, *graph, m_portalScratch, request,
                    dynamicOverlay);
                if (route.status != NavigationPortalRouteStatus::Pending) {
                    status = route.status;
                } else {
                    m_activePortalRouteStarted = true;
                }
            }
            if (m_activePortalRouteStarted)
            {
                route = m_portalRouter.stepRoute(
                    *layers, zones, *graph, m_portalScratch,
                    expansionBudget, m_pathPoints, dynamicOverlay);
                m_expansionsConsumedLastProcess =
                    m_portalScratch.workConsumedLastStep();
                status = route.status;
            }
        }
    }

    if (status == NavigationPortalRouteStatus::Pending)
        return NavigationAdapterProcessResult::Progressed;

    if (status == NavigationPortalRouteStatus::Success) {
        const size_t pointCount = route.requiredPointCount;
        size_t smoothedPointCount = pointCount;
        static_cast<void>(smoothNavigationPath(
            layers,
            nullptr,
            dynamicOverlay,
            m_activeStartLayer,
            m_activeRequest.surfaceMask,
            toNavigationClearance(m_activeRequest.clearanceProfile),
            container::Span<NavigationLayerPathPoint>(
                m_pathPoints.data(), pointCount),
            smoothedPointCount,
            m_activeRequest.objectCells));
        route.requiredPointCount = smoothedPointCount;
        const container::Span<const NavigationLayerPathPoint> finalPoints(
            m_pathPoints.data(), route.requiredPointCount);
        const NavigationPathMetadata metadata = makePathMetadata(
            finalPoints,
            m_activeStartLayer,
            m_activeRequest.clearanceProfile.radiusCells);
        const PathRepositoryCreateResult stored = repository.create(
            m_activeRevision,
            container::Span<const NavigationLayerPathPoint>(
                m_pathPoints.data(), route.requiredPointCount),
            metadata);
        if (stored.status == PathRepositoryStatus::Success) {
            StoredFeedback ready = makeTerminal(
                m_activeRequest.correlation, confirmedTick,
                NavigationAdapterStatus::Ready,
                engine::ai::PathFeedbackStatus::Ready,
                m_activeRevision);
            annotateActiveFeedback(
                ready, NavigationSolverKind::Portal,
                NavigationTerminalReason::GoalReached);
            ready.value.feedback.path = stored.handle;
            NavigationWorldPosition adjusted =
                m_pathPoints[route.requiredPointCount - 1U].position;
            if (m_activeRequest.preciseFinalZ)
                adjusted.zRaw = m_activeRequest.originalGoal.zRaw;
            ready.value.feedback.adjustedGoal = toAI(adjusted);
            ready.value.feedback.adjustedLayer =
                m_activeGoalLayer.value;
            publish(ready);
            clearActive();
            return NavigationAdapterProcessResult::FeedbackPublished;
        }
        status = NavigationPortalRouteStatus::CapacityExceeded;
    }

    if (status == NavigationPortalRouteStatus::CapacityExceeded ||
        status == NavigationPortalRouteStatus::OutputCapacityExceeded) {
        publish(makeDelayed(
            m_activeRequest.correlation, confirmedTick, m_activeRevision));
        clearActive();
        return NavigationAdapterProcessResult::FeedbackPublished;
    }
    const NavigationAdapterStatus sidecar =
        status == NavigationPortalRouteStatus::InvalidRequest
            ? NavigationAdapterStatus::InvalidRequest
            : NavigationAdapterStatus::NoPath;
    StoredFeedback failed = makeTerminal(
        m_activeRequest.correlation, confirmedTick, sidecar,
        engine::ai::PathFeedbackStatus::NoPath, m_activeRevision);
    annotateActiveFeedback(
        failed, NavigationSolverKind::Portal,
        sidecar == NavigationAdapterStatus::InvalidRequest
            ? NavigationTerminalReason::InvalidEndpoint
            : NavigationTerminalReason::SearchFrontierExhausted);
    if (sidecar == NavigationAdapterStatus::NoPath)
        failed.value.feedback.blockingBridge =
            m_activeRequest.blockingBridgeCandidate;
    publish(failed);
    clearActive();
    return NavigationAdapterProcessResult::FeedbackPublished;
}

NavigationAdapterProcessResult NavigationPathService::finishSearch(
    uint64_t confirmedTick,
    const NavigationGrid& grid,
    PathRepository& repository,
    NavigationSearchStatus status,
    const NavigationDynamicOverlay* dynamicOverlay) noexcept
{
    const NavigationSearchProgress diagnosticsProgress =
        m_oracle.progress(m_scratch);
    // A Safe ("flee") search whose bounded flood exhausts the open list, and any
    // Safe path is the only source-compatible partial search. Its bounded
    // flood finishes as PartialPath carrying the farthest reachable terminal;
    // ordinary New/Patch/Approach requests remain strict NoPath searches.
    if (status == NavigationSearchStatus::PartialPath)
        status = NavigationSearchStatus::Success;

    if (status == NavigationSearchStatus::Success)
    {
        const NavigationPathReadResult path = m_oracle.readPath(m_scratch, m_rawCells);
        if (path.status != NavigationPathReadStatus::Success)
            status = NavigationSearchStatus::CapacityExceeded;
        else
        {
            size_t pointCount = path.requiredCount;
            for (size_t index = 0; index < path.requiredCount; ++index)
            {
                NavigationWorldPosition position;
                const NavigationClearanceClass clearance =
                    toNavigationClearance(m_activeRequest.clearanceProfile);
                if (!grid.cellPosition(m_rawCells[index], clearance, position))
                {
                    status = NavigationSearchStatus::InvalidRequest;
                    break;
                }
                m_pathPoints[index] = {
                    {m_activeStartLayer, m_rawCells[index]}, position};
            }
            if (status == NavigationSearchStatus::Success &&
                m_activePatchReuse) {
                uint32_t matchedIndex = m_activePatchPointCount;
                for (uint32_t reverse = m_activePatchPointCount;
                     reverse > m_activePatchSuffixStart; --reverse) {
                    const uint32_t index = reverse - 1;
                    const NavigationCellId cell = grid.cellAt(
                        m_patchPathScratch[index].position,
                        toNavigationClearance(
                            m_activeRequest.clearanceProfile));
                    if (cell == m_oracle.terminal()) {
                        matchedIndex = index;
                        break;
                    }
                }
                const size_t suffixCount = matchedIndex <
                        m_activePatchPointCount
                    ? static_cast<size_t>(m_activePatchPointCount -
                          matchedIndex - 1)
                    : m_pathPoints.size();
                if (matchedIndex >= m_activePatchPointCount ||
                    suffixCount > m_pathPoints.size() - pointCount) {
                    status = NavigationSearchStatus::CapacityExceeded;
                } else {
                    for (uint32_t index = matchedIndex + 1;
                         index < m_activePatchPointCount; ++index) {
                        const PathRepositoryPoint& oldPoint =
                            m_patchPathScratch[index];
                        const NavigationCellId cell = grid.cellAt(
                            oldPoint.position,
                            toNavigationClearance(
                                m_activeRequest.clearanceProfile));
                        if (!cell) {
                            status =
                                NavigationSearchStatus::InvalidRequest;
                            break;
                        }
                        m_pathPoints[pointCount++] = {
                            {oldPoint.layer, cell}, oldPoint.position};
                    }
                }
            }
            if (status == NavigationSearchStatus::Success)
            {
                size_t smoothedPointCount = pointCount;
                static_cast<void>(smoothNavigationPath(
                    nullptr,
                    &grid,
                    dynamicOverlay,
                    m_activeStartLayer,
                    m_activeRequest.surfaceMask,
                    toNavigationClearance(m_activeRequest.clearanceProfile),
                    container::Span<NavigationLayerPathPoint>(
                        m_pathPoints.data(), pointCount),
                    smoothedPointCount,
                    m_activeRequest.objectCells));
                const container::Span<const NavigationLayerPathPoint>
                    finalPoints(m_pathPoints.data(), smoothedPointCount);
                const NavigationPathMetadata metadata = makePathMetadata(
                    finalPoints,
                    m_activeStartLayer,
                    m_activeRequest.clearanceProfile.radiusCells);
                const PathRepositoryCreateResult stored = repository.create(
                    m_activeRevision,
                    container::Span<const NavigationLayerPathPoint>(
                        m_pathPoints.data(), smoothedPointCount),
                        metadata);
                if (stored.status == PathRepositoryStatus::Success)
                {
                    if (m_activeRequest.groupPathId != 0 &&
                        m_activeRequest.groupPathMemberOrdinal == 0 &&
                        m_activeRequest.groupPathMemberCount > 1) {
                        const PathRepositoryCreateResult center =
                            repository.create(
                                m_activeRevision,
                                container::Span<const NavigationLayerPathPoint>(
                                    m_pathPoints.data(), smoothedPointCount),
                                metadata);
                        if (center.status != PathRepositoryStatus::Success) {
                            static_cast<void>(repository.release(
                                stored.handle, m_activeRevision));
                            publish(makeDelayed(
                                m_activeRequest.correlation, confirmedTick,
                                m_activeRevision));
                            clearActive();
                            return NavigationAdapterProcessResult::FeedbackPublished;
                        }
                        auto cache = std::find_if(
                            m_groupPaths.begin(), m_groupPaths.end(),
                            [](const GroupPathCache& value) {
                                return value.id == 0;
                            });
                        if (cache == m_groupPaths.end()) {
                            static_cast<void>(repository.release(
                                stored.handle, m_activeRevision));
                            static_cast<void>(repository.release(
                                center.handle, m_activeRevision));
                            publish(makeDelayed(
                                m_activeRequest.correlation, confirmedTick,
                                m_activeRevision));
                            clearActive();
                            return NavigationAdapterProcessResult::FeedbackPublished;
                        }
                        *cache = {
                            .id = m_activeRequest.groupPathId,
                            .centerPath = center.handle,
                            .adjustedGoal = toAI(m_pathPoints[
                                smoothedPointCount - 1u].position),
                            .revision = m_activeRevision,
                            .layer = m_activeStartLayer,
                            .remainingFollowers =
                                m_activeRequest.groupPathMemberCount - 1u,
                            .createdTick = confirmedTick,
                            .status = engine::ai::PathFeedbackStatus::Ready,
                        };
                    }
                    StoredFeedback ready = makeTerminal(m_activeRequest.correlation,
                                                        confirmedTick,
                                                        NavigationAdapterStatus::Ready,
                                                        engine::ai::PathFeedbackStatus::Ready,
                                                        m_activeRevision);
                    annotateActiveFeedback(
                        ready, NavigationSolverKind::AStar,
                        NavigationTerminalReason::GoalReached,
                        &diagnosticsProgress);
                    ready.value.feedback.path = stored.handle;
                    NavigationWorldPosition adjusted =
                        m_pathPoints[smoothedPointCount - 1].position;
                    if (m_activeRequest.preciseFinalZ)
                        adjusted.zRaw = m_activeRequest.originalGoal.zRaw;
                    ready.value.feedback.adjustedGoal = toAI(adjusted);
                    ready.value.feedback.adjustedLayer =
                        m_pathPoints[smoothedPointCount - 1]
                            .location.layer.value;
                    publish(ready);
                    clearActive();
                    return NavigationAdapterProcessResult::FeedbackPublished;
                }
                status = NavigationSearchStatus::CapacityExceeded;
            }
        }
    }

    if (status == NavigationSearchStatus::CapacityExceeded)
    {
        publish(makeDelayed(
            m_activeRequest.correlation, confirmedTick, m_activeRevision));
        clearActive();
        return NavigationAdapterProcessResult::FeedbackPublished;
    }
    const NavigationAdapterStatus sidecar =
        status == NavigationSearchStatus::InvalidRequest
            ? NavigationAdapterStatus::InvalidRequest
            : NavigationAdapterStatus::NoPath;
    StoredFeedback failed = makeTerminal(
        m_activeRequest.correlation, confirmedTick, sidecar,
        engine::ai::PathFeedbackStatus::NoPath, m_activeRevision);
    const NavigationTerminalReason reason =
        sidecar == NavigationAdapterStatus::InvalidRequest
            ? NavigationTerminalReason::InvalidEndpoint
            : m_activePatchReuse &&
                    diagnosticsProgress.totalExpansions >= 2000
                ? NavigationTerminalReason::PatchExpansionLimit
                : NavigationTerminalReason::SearchFrontierExhausted;
    annotateActiveFeedback(
        failed, NavigationSolverKind::AStar, reason,
        &diagnosticsProgress);
    if (sidecar == NavigationAdapterStatus::NoPath)
        failed.value.feedback.blockingBridge =
            m_activeRequest.blockingBridgeCandidate;
    publish(failed);
    clearActive();
    return NavigationAdapterProcessResult::FeedbackPublished;
}

NavigationAdapterProcessResult NavigationPathService::finishGroupFollower(
    uint64_t confirmedTick, const NavigationGrid& grid,
    const QueuedNavigationRequest& queued,
    const NavigationDynamicOverlay* dynamicOverlay,
    PathRepository& repository) noexcept
{
    auto cache = std::find_if(
        m_groupPaths.begin(), m_groupPaths.end(),
        [&queued](const GroupPathCache& value) {
            return value.id == queued.request.groupPathId;
        });
    const auto add = [](int64_t left, int64_t right) noexcept {
        if (right > 0 && left > std::numeric_limits<int64_t>::max() - right)
            return std::numeric_limits<int64_t>::max();
        if (right < 0 && left < std::numeric_limits<int64_t>::min() - right)
            return std::numeric_limits<int64_t>::min();
        return left + right;
    };
    const auto consumeFollower = [&]() noexcept {
        if (cache == m_groupPaths.end()) return;
        if (cache->remainingFollowers > 0) --cache->remainingFollowers;
        if (cache->remainingFollowers == 0) {
            if (cache->centerPath && cache->revision) {
                static_cast<void>(repository.release(
                    cache->centerPath, cache->revision));
            }
            *cache = {};
        }
    };
    const auto requeueIndividual = [&]() noexcept {
        engine::ai::PathRequest fallback = queued.request;
        // The shared path was rejected, but the member still owns a formation
        // slot. Convert that slot into the individual goal before clearing the
        // group metadata; otherwise every fallback request converges on the
        // same click point and the selection becomes a straight line.
        fallback.originalGoal.xRaw = add(
            fallback.originalGoal.xRaw,
            fallback.groupPathOffset.xRaw);
        fallback.originalGoal.yRaw = add(
            fallback.originalGoal.yRaw,
            fallback.groupPathOffset.yRaw);
        fallback.groupPathId = 0;
        fallback.groupPathMemberOrdinal = 0;
        fallback.groupPathMemberCount = 0;
        fallback.groupPathOffset = {};
        const NavigationRequestQueueResult result = m_requests.submit(
            fallback, queued.submittedTick, queued.navigationRevision,
            queued.startLayer, queued.goalLayer);
        consumeFollower();
        if (result == NavigationRequestQueueResult::Accepted ||
            result == NavigationRequestQueueResult::Replaced) {
            return NavigationAdapterProcessResult::RequestRequeued;
        }
        publish(makeDelayed(queued.request.correlation, confirmedTick,
                            {queued.navigationRevision}));
        return NavigationAdapterProcessResult::FeedbackPublished;
    };
    if (cache == m_groupPaths.end() ||
        cache->revision.value != queued.navigationRevision ||
        cache->createdTick != confirmedTick || !cache->centerPath ||
        cache->status != engine::ai::PathFeedbackStatus::Ready) {
        return requeueIndividual();
    }
    const PathRepositoryCopyResult copied = repository.copyPoints(
        cache->centerPath, cache->revision, m_groupPathScratch);
    if (copied.status != PathRepositoryStatus::Success ||
        copied.pointCount == 0 || copied.pointCount > m_pathPoints.size()) {
        return requeueIndividual();
    }
    const NavigationClearanceClass clearance =
        toNavigationClearance(queued.request.clearanceProfile);
    NavigationCellId previous = grid.cellAt(
        toWorld(queued.request.start), clearance);
    if (!previous || !grid.traversable(
            previous, queued.request.surfaceMask, cache->layer,
            clearance)) {
        return requeueIndividual();
    }
    for (uint32_t index = 0; index < copied.pointCount; ++index) {
        NavigationWorldPosition position = m_groupPathScratch[index].position;
        position.xRaw = add(position.xRaw,
                            queued.request.groupPathOffset.xRaw);
        position.yRaw = add(position.yRaw,
                            queued.request.groupPathOffset.yRaw);
        const NavigationCellId cell = grid.cellAt(position, clearance);
        if (!cell || !grid.traversable(
                cell, queued.request.surfaceMask, cache->layer, clearance) ||
            !detail::navigationLineOfSight(
                grid, previous, cell, queued.request.surfaceMask,
                cache->layer, clearance,
                dynamicOverlay,
                queued.request.objectCells)) {
            return requeueIndividual();
        }
        m_pathPoints[index] = {
            {cache->layer, cell}, position};
        previous = cell;
    }
    const auto points = container::Span<const NavigationLayerPathPoint>(
        m_pathPoints.data(), copied.pointCount);
    const PathRepositoryCreateResult stored = repository.create(
        cache->revision, points,
        makePathMetadata(points, cache->layer,
                         queued.request.clearanceProfile.radiusCells),
        PathRepositoryPointValidation::LayerOnlyWorldPolyline);
    if (stored.status != PathRepositoryStatus::Success) {
        publish(makeDelayed(queued.request.correlation, confirmedTick,
                            cache->revision));
        return NavigationAdapterProcessResult::FeedbackPublished;
    }
    StoredFeedback ready = makeTerminal(
        queued.request.correlation, confirmedTick,
        NavigationAdapterStatus::Ready,
        engine::ai::PathFeedbackStatus::Ready, cache->revision);
    ready.value.feedback.path = stored.handle;
    ready.value.feedback.adjustedGoal = {
        add(cache->adjustedGoal.xRaw,
            queued.request.groupPathOffset.xRaw),
        add(cache->adjustedGoal.yRaw,
            queued.request.groupPathOffset.yRaw),
        cache->adjustedGoal.zRaw};
    ready.value.feedback.adjustedLayer = cache->layer.value;
    publish(ready);
    consumeFollower();
    return NavigationAdapterProcessResult::FeedbackPublished;
}

} // namespace engine::navigation
