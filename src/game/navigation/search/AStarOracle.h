#pragma once

#include "../grid/NavigationGrid.h"
#include "../grid/NavigationDynamicOverlay.h"
#include "NavigationOpenHeap.h"
#include "game/navigation/contracts/NavigationPathContracts.h"
#include "math/fixed/q32_32.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace engine::navigation
{

enum class NavigationSearchStatus : uint8_t
{
    Idle = 0,
    Pending,
    Delayed,
    Success,
    PartialPath,
    NoPath,
    Cancelled,
    InvalidRequest,
    CapacityExceeded,
};

struct NavigationSearchRequest final
{
    struct AdjustmentGoal final
    {
        NavigationCellId cell = InvalidNavigationCell;
        uint32_t preferenceCost = 0;
    };

    uint64_t requestId = 0;
    NavigationCellId start = InvalidNavigationCell;
    NavigationCellId goal = InvalidNavigationCell;
    NavigationProfileId profile = InvalidNavigationProfile;
    NavigationMovementMask movementMask = 0;
    NavigationLayerId layer = InvalidNavigationLayer;
    NavigationClearanceClass clearance = NavigationClearanceClass::Infantry;
    uint64_t ignoredObstacle = 0;
    uint8_t crusherLevel = 0;
    container::Span<const uint64_t> dozerPassableObstacles{};
    // Inclusive Euclidean goal annulus in navigation-cell units. Zero keeps
    // the historical exact-cell goal. This lets Enter/attack-range movement
    // stop at a traversable perimeter cell even when the target's own cell is
    // occupied by its authoritative footprint.
    uint32_t goalRadiusCells = 0;

    // Destination adjustment is a bounded set of admissible endpoint cells,
    // not a preselected replacement goal.  Keeping the predicate inside A*
    // means a nearby cell separated by a wall cannot hide a slightly farther
    // reachable endpoint.  `maximumAnchorOffsetCost` is the greatest octile
    // distance from `goal` to any candidate and lets the ordinary goal
    // heuristic remain an admissible lower bound for the whole set.
    struct AdjustmentProfile final
    {
        bool enabled = false;
        container::Span<const AdjustmentGoal> goals{};
        uint32_t maximumAnchorOffsetCost = 0;
    } adjustment;

    struct AttackProfile final
    {
        bool enabled = false;
        NavigationWorldPosition target;
        int64_t minimumRangeRaw = 0;
        int64_t maximumRangeRaw = 0;
        bool lineOfSightEnabled = false;
        uint64_t subject = 0;
        uint64_t targetObject = 0;
        uint64_t subjectContainer = 0;
        uint64_t targetContainer = 0;
        uint64_t subjectSlaver = 0;
        uint64_t targetSlaver = 0;
        container::Span<const uint64_t> seeThroughObstacles{};
    } attack;

    struct PatchProfile final
    {
        bool enabled = false;
        // Canonical ascending, unique cells from the still-valid old suffix.
        container::Span<const NavigationCellId> suffixGoals{};
        uint32_t expansionLimit = 2000;
    } patch;

    // ZH's Safe path is a bounded flood ordered by accumulated movement cost,
    // rather than a path to an invented point in the repulsion direction.
    // The profile is value-only so a worker never reads mutable ECS state.
    struct SafeProfile final
    {
        bool enabled = false;
        NavigationWorldPosition repulsor1;
        NavigationWorldPosition repulsor2;
        bool hasRepulsor2 = false;
        int64_t radiusRaw = 0;
        uint32_t expansionLimit = 2000;
    } safe;
};

struct NavigationSearchProgress final
{
    NavigationSearchStatus status = NavigationSearchStatus::Idle;
    uint32_t expandedThisStep = 0;
    uint64_t totalExpansions = 0;
    NavigationCellId terminal = InvalidNavigationCell;
    uint32_t totalCost = NavigationSearchScratch::InfiniteCost;
    uint64_t traceHash = 0;
};

enum class NavigationPathReadStatus : uint8_t
{
    Success = 0,
    NotReady,
    OutputCapacityExceeded,
    CorruptParentChain,
};

struct NavigationPathReadResult final
{
    NavigationPathReadStatus status = NavigationPathReadStatus::NotReady;
    size_t requiredCount = 0;
    uint32_t totalCost = NavigationSearchScratch::InfiniteCost;
    NavigationLayerId layer = InvalidNavigationLayer;
};

// Resumable, allocation-free A* correctness oracle. The job stores only value
// state; its per-job scratch is supplied explicitly and must remain paired with
// the job until a terminal status is reached.
class AStarOracle final
{
public:
    [[nodiscard]] static bool allowsTraversalCell(
        const NavigationGrid& grid,
        const NavigationSearchRequest& request,
        NavigationCellId cell,
        const NavigationDynamicOverlay* dynamicOverlay,
        bool allowAnyDynamicObstacle = false) noexcept
    {
        return cellTraversal(
                   grid, request, cell, dynamicOverlay,
                   allowAnyDynamicObstacle)
            .allowed;
    }

    [[nodiscard]] NavigationSearchStatus begin(const NavigationGrid& grid,
                                               NavigationSearchScratch& scratch,
                                               const NavigationSearchRequest& request,
                                               container::Span<const engine::ai::AIPathObjectCellSnapshot>
                                                   objectCells = {},
                                               const NavigationDynamicOverlay*
                                                   dynamicOverlay = nullptr) noexcept
    {
        reset(request);
        traceObjectCells(objectCells);
        if (!validRequest(grid, request, objectCells, dynamicOverlay))
            return finish(NavigationSearchStatus::InvalidRequest, InvalidNavigationCell);
        m_tunneling =
            !allowsTraversalCell(
                grid, request, request.start, dynamicOverlay, false) &&
            allowsTraversalCell(
                grid, request, request.start, dynamicOverlay, true);
        if (scratch.cellCapacity() < grid.cellCount() || scratch.heapCapacity() < grid.cellCount())
            return finish(NavigationSearchStatus::CapacityExceeded, InvalidNavigationCell);

        [[maybe_unused]] const uint32_t epoch = scratch.beginSearch();
        uint32_t startHCost = 0;
        if (!request.safe.enabled &&
            !searchHeuristic(grid.coordinate(request.start),
                             grid.coordinate(request.goal),
                             request, startHCost))
            return finish(NavigationSearchStatus::CapacityExceeded, InvalidNavigationCell);

        uint32_t startFCost = 0;
        if (!checkedAdd(0, startHCost, startFCost))
            return finish(NavigationSearchStatus::CapacityExceeded, InvalidNavigationCell);

        scratch.markOpen(request.start, 0, InvalidNavigationCell);
        NavigationOpenHeap open(scratch);
        if (open.push(request.start, startFCost, startHCost) != NavigationOpenHeapResult::Success)
            return finish(NavigationSearchStatus::CapacityExceeded, InvalidNavigationCell);

        m_status = NavigationSearchStatus::Pending;
        traceEvent(TraceEvent::Begin, request.start, 0, startHCost);
        return m_status;
    }

    [[nodiscard]] NavigationSearchProgress step(const NavigationGrid& grid,
                                                NavigationSearchScratch& scratch,
                                                uint32_t expansionBudget,
                                                container::Span<const engine::ai::AIPathObjectCellSnapshot>
                                                    objectCells = {},
                                                const NavigationDynamicOverlay*
                                                    dynamicOverlay = nullptr) noexcept
    {
        NavigationSearchProgress progress = currentProgress(scratch);
        if (m_status != NavigationSearchStatus::Pending || expansionBudget == 0)
            return progress;
        if (!scratch.contains(m_request.start) || scratch.cellCapacity() < grid.cellCount())
        {
            finish(NavigationSearchStatus::CapacityExceeded, InvalidNavigationCell);
            return currentProgress(scratch);
        }

        NavigationOpenHeap open(scratch);
        while (progress.expandedThisStep < expansionBudget)
        {
            if (open.empty())
            {
                finishExhausted();
                return currentProgress(scratch, progress.expandedThisStep);
            }

            NavigationOpenHeapEntry entry;
            if (open.popMin(entry) != NavigationOpenHeapResult::Success)
            {
                finish(NavigationSearchStatus::CapacityExceeded, InvalidNavigationCell);
                return currentProgress(scratch, progress.expandedThisStep);
            }

            // Adjustment candidates are virtual edges to one common sink.
            // Their terminal preference cost is non-negative, so once the
            // smallest remaining A* key is strictly worse than the best
            // complete candidate, that candidate is globally final. Equal
            // keys remain visible for the deterministic preference tie-break.
            if (m_adjustmentBest &&
                entry.fCost > m_adjustmentBestTotalCost) {
                finish(NavigationSearchStatus::Success,
                       m_adjustmentBest);
                return currentProgress(
                    scratch, progress.expandedThisStep);
            }

            scratch.markClosed(entry.cell);
            ++progress.expandedThisStep;
            ++m_totalExpansions;
            const uint32_t currentGCost = scratch.gCost(entry.cell);
            traceEvent(TraceEvent::Expand, entry.cell, currentGCost, entry.hCost);

            if (m_request.safe.enabled) {
                updateSafeBest(grid, entry.cell, currentGCost);
                if (safeCandidate(grid, entry.cell) ||
                    (m_totalExpansions >= m_request.safe.expansionLimit && m_safeBest)) {
                    finish(NavigationSearchStatus::Success, m_safeBest);
                    return currentProgress(scratch, progress.expandedThisStep);
                }
            } else {
                updateBest(entry.cell, currentGCost, entry.hCost);
            }

            const NavigationSearchRequest::AdjustmentGoal*
                adjustmentGoal = m_request.adjustment.enabled
                ? findAdjustmentGoal(entry.cell)
                : nullptr;
            if (adjustmentGoal) {
                uint32_t totalCost = 0;
                if (!checkedAdd(
                        currentGCost, adjustmentGoal->preferenceCost,
                        totalCost)) {
                    finish(NavigationSearchStatus::CapacityExceeded,
                           InvalidNavigationCell);
                    return currentProgress(
                        scratch, progress.expandedThisStep);
                }
                if (!m_adjustmentBest ||
                    totalCost < m_adjustmentBestTotalCost ||
                    (totalCost == m_adjustmentBestTotalCost &&
                     (adjustmentGoal->preferenceCost <
                          m_adjustmentBestPreferenceCost ||
                      (adjustmentGoal->preferenceCost ==
                           m_adjustmentBestPreferenceCost &&
                       (currentGCost < m_adjustmentBestGCost ||
                        (currentGCost == m_adjustmentBestGCost &&
                         entry.cell < m_adjustmentBest)))))) {
                    m_adjustmentBest = entry.cell;
                    m_adjustmentBestTotalCost = totalCost;
                    m_adjustmentBestPreferenceCost =
                        adjustmentGoal->preferenceCost;
                    m_adjustmentBestGCost = currentGCost;
                }
            }

            const bool reached = m_request.attack.enabled
                ? attackGoalReached(grid, entry.cell, dynamicOverlay)
                : m_request.patch.enabled
                    ? std::binary_search(
                          m_request.patch.suffixGoals.begin(),
                          m_request.patch.suffixGoals.end(), entry.cell)
                    : m_request.adjustment.enabled
                        ? false
                    : goalReached(
                      grid.coordinate(entry.cell),
                      grid.coordinate(m_request.goal),
                      m_request.goalRadiusCells);
            if (!m_request.safe.enabled && reached)
            {
                finish(NavigationSearchStatus::Success, entry.cell);
                return currentProgress(scratch, progress.expandedThisStep);
            }
            if (m_request.patch.enabled &&
                m_totalExpansions >= m_request.patch.expansionLimit) {
                finish(NavigationSearchStatus::NoPath,
                       InvalidNavigationCell);
                return currentProgress(scratch,
                                       progress.expandedThisStep);
            }

            if (!expandNeighbors(
                    grid, scratch, open, entry.cell, currentGCost,
                    objectCells, dynamicOverlay))
                return currentProgress(scratch, progress.expandedThisStep);
        }
        return currentProgress(scratch, progress.expandedThisStep);
    }

    [[nodiscard]] bool cancel(uint64_t requestId) noexcept
    {
        if ((m_status != NavigationSearchStatus::Pending && m_status != NavigationSearchStatus::Delayed) ||
            requestId == 0 || requestId != m_request.requestId)
            return false;
        traceEvent(TraceEvent::Cancel, InvalidNavigationCell, 0, 0);
        m_status = NavigationSearchStatus::Cancelled;
        m_terminal = InvalidNavigationCell;
        return true;
    }

    [[nodiscard]] bool delay(uint64_t requestId) noexcept
    {
        if (m_status != NavigationSearchStatus::Pending || requestId == 0 || requestId != m_request.requestId)
            return false;
        m_status = NavigationSearchStatus::Delayed;
        traceEvent(TraceEvent::Delay, InvalidNavigationCell, 0, 0);
        return true;
    }

    [[nodiscard]] bool resume(uint64_t requestId) noexcept
    {
        if (m_status != NavigationSearchStatus::Delayed || requestId == 0 || requestId != m_request.requestId)
            return false;
        m_status = NavigationSearchStatus::Pending;
        traceEvent(TraceEvent::Resume, InvalidNavigationCell, 0, 0);
        return true;
    }

    [[nodiscard]] NavigationPathReadResult readPath(const NavigationSearchScratch& scratch,
                                                    container::Span<NavigationCellId> output) const noexcept
    {
        NavigationPathReadResult result;
        if ((m_status != NavigationSearchStatus::Success && m_status != NavigationSearchStatus::PartialPath) ||
            !m_terminal)
            return result;

        NavigationCellId cursor = m_terminal;
        size_t count = 0;
        while (true)
        {
            if (!scratch.contains(cursor) || count == scratch.cellCapacity())
            {
                result.status = NavigationPathReadStatus::CorruptParentChain;
                return result;
            }
            ++count;
            if (cursor == m_request.start)
                break;
            cursor = scratch.parent(cursor);
            if (!cursor)
            {
                result.status = NavigationPathReadStatus::CorruptParentChain;
                return result;
            }
        }

        result.requiredCount = count;
        result.totalCost = scratch.gCost(m_terminal);
        result.layer = m_request.layer;
        if (output.size() < count)
        {
            result.status = NavigationPathReadStatus::OutputCapacityExceeded;
            return result;
        }

        cursor = m_terminal;
        for (size_t index = count; index > 0; --index)
        {
            output[index - 1] = cursor;
            if (cursor != m_request.start)
                cursor = scratch.parent(cursor);
        }
        result.status = NavigationPathReadStatus::Success;
        return result;
    }

    [[nodiscard]] NavigationSearchStatus status() const noexcept { return m_status; }
    [[nodiscard]] uint64_t requestId() const noexcept { return m_request.requestId; }
    [[nodiscard]] uint64_t totalExpansions() const noexcept { return m_totalExpansions; }
    [[nodiscard]] uint64_t traceHash() const noexcept { return m_traceHash; }
    [[nodiscard]] NavigationCellId terminal() const noexcept { return m_terminal; }
    [[nodiscard]] NavigationCellId closestCell() const noexcept {
        return m_best;
    }

    // Snapshot copies duplicate owning vectors but std::span keeps the source
    // address. Rebind only borrowed request storage without resetting the
    // resumable heap, parent chain, trace, or terminal state.
    void rebindBorrowedSpans(
        container::Span<const uint64_t> dozerPassableObstacles,
        container::Span<const uint64_t> attackSeeThroughObstacles,
        container::Span<const NavigationCellId> patchGoals,
        container::Span<const NavigationSearchRequest::AdjustmentGoal>
            adjustmentGoals) noexcept
    {
        m_request.dozerPassableObstacles = dozerPassableObstacles;
        m_request.attack.seeThroughObstacles =
            attackSeeThroughObstacles;
        m_request.patch.suffixGoals = patchGoals;
        m_request.adjustment.goals = adjustmentGoals;
    }

    [[nodiscard]] static constexpr bool octileCost(NavigationGridCoordinate from,
                                                   NavigationGridCoordinate to,
                                                   uint32_t& cost) noexcept
    {
        const uint64_t dx = from.x >= to.x ? static_cast<uint64_t>(from.x) - static_cast<uint64_t>(to.x)
                                           : static_cast<uint64_t>(to.x) - static_cast<uint64_t>(from.x);
        const uint64_t dy = from.y >= to.y ? static_cast<uint64_t>(from.y) - static_cast<uint64_t>(to.y)
                                           : static_cast<uint64_t>(to.y) - static_cast<uint64_t>(from.y);
        const uint64_t diagonal = dx < dy ? dx : dy;
        const uint64_t straight = (dx > dy ? dx : dy) - diagonal;
        const uint64_t value = diagonal * DiagonalCost + straight * OrthogonalCost;
        if (value >= NavigationSearchScratch::InfiniteCost)
            return false;
        cost = static_cast<uint32_t>(value);
        return true;
    }

    [[nodiscard]] static constexpr bool goalReached(
        NavigationGridCoordinate candidate,
        NavigationGridCoordinate goal,
        uint32_t radiusCells) noexcept
    {
        if (radiusCells == 0)
            return candidate == goal;
        const uint64_t dx = candidate.x >= goal.x
            ? static_cast<uint64_t>(candidate.x) - static_cast<uint64_t>(goal.x)
            : static_cast<uint64_t>(goal.x) - static_cast<uint64_t>(candidate.x);
        const uint64_t dy = candidate.y >= goal.y
            ? static_cast<uint64_t>(candidate.y) - static_cast<uint64_t>(goal.y)
            : static_cast<uint64_t>(goal.y) - static_cast<uint64_t>(candidate.y);
        const uint64_t radius = radiusCells;
        return dx * dx + dy * dy <= radius * radius;
    }

    [[nodiscard]] static constexpr bool goalHeuristic(
        NavigationGridCoordinate from,
        NavigationGridCoordinate goal,
        uint32_t radiusCells,
        uint32_t& cost) noexcept
    {
        uint32_t centerCost = 0;
        if (!octileCost(from, goal, centerCost))
            return false;
        const uint64_t removable =
            static_cast<uint64_t>(radiusCells) * OrthogonalCost;
        cost = removable >= centerCost
            ? 0u
            : centerCost - static_cast<uint32_t>(removable);
        return true;
    }

    [[nodiscard]] static constexpr bool searchHeuristic(
        NavigationGridCoordinate from,
        NavigationGridCoordinate goal,
        const NavigationSearchRequest& request,
        uint32_t& cost) noexcept
    {
        if (!goalHeuristic(from, goal, request.goalRadiusCells, cost))
            return false;
        if (!request.adjustment.enabled)
            return true;
        cost = request.adjustment.maximumAnchorOffsetCost >= cost
            ? 0u
            : cost - request.adjustment.maximumAnchorOffsetCost;
        return true;
    }

    [[nodiscard]] const NavigationSearchRequest::AdjustmentGoal*
    findAdjustmentGoal(NavigationCellId cell) const noexcept
    {
        const auto found = std::lower_bound(
            m_request.adjustment.goals.begin(),
            m_request.adjustment.goals.end(), cell,
            [](const NavigationSearchRequest::AdjustmentGoal& value,
               NavigationCellId wanted) noexcept {
                return value.cell < wanted;
            });
        return found != m_request.adjustment.goals.end() &&
                found->cell == cell
            ? &*found
            : nullptr;
    }

    [[nodiscard]] bool attackGoalReached(
        const NavigationGrid& grid, NavigationCellId candidate,
        const NavigationDynamicOverlay* dynamicOverlay) const noexcept
    {
        const NavigationGridCoordinate candidateCoordinate =
            grid.coordinate(candidate);
        const NavigationGridCoordinate goalCoordinate =
            grid.coordinate(m_request.goal);
        if (m_request.attack.maximumRangeRaw <= 0) {
            if (candidateCoordinate != goalCoordinate)
                return false;
        } else {
            NavigationWorldPosition candidatePosition;
            if (!grid.cellPosition(
                    candidate, m_request.clearance, candidatePosition))
                return false;
            const math::q32_32 dx = math::q32_32::from_raw(
                candidatePosition.xRaw) - math::q32_32::from_raw(
                    m_request.attack.target.xRaw);
            const math::q32_32 dy = math::q32_32::from_raw(
                candidatePosition.yRaw) - math::q32_32::from_raw(
                    m_request.attack.target.yRaw);
            const math::q32_32 distanceSquared = dx * dx + dy * dy;
            const math::q32_32 minimum = math::q32_32::from_raw(
                std::max<int64_t>(0, m_request.attack.minimumRangeRaw));
            const math::q32_32 maximum = math::q32_32::from_raw(
                std::max<int64_t>(0, m_request.attack.maximumRangeRaw));
            if (distanceSquared < minimum * minimum ||
                distanceSquared > maximum * maximum)
                return false;
        }
        if (!m_request.attack.lineOfSightEnabled)
            return true;
        if (!dynamicOverlay)
            return false;
        NavigationWorldPosition candidatePosition;
        if (!grid.cellPosition(
                candidate, m_request.clearance, candidatePosition))
            return false;
        return !attackViewBlockedByObstacle(
            grid, *dynamicOverlay,
            {
                .attacker = candidatePosition,
                .victim = m_request.attack.target,
                .attackerEntityId = m_request.attack.subject,
                .victimEntityId = m_request.attack.targetObject,
                .attackerContainerEntityId =
                    m_request.attack.subjectContainer,
                .victimContainerEntityId =
                    m_request.attack.targetContainer,
                .attackerSlaverEntityId =
                    m_request.attack.subjectSlaver,
                .victimSlaverEntityId =
                    m_request.attack.targetSlaver,
                .seeThroughEntityIds =
                    m_request.attack.seeThroughObstacles,
            });
    }

    inline static constexpr uint32_t OrthogonalCost = 10;
    inline static constexpr uint32_t DiagonalCost = 14;

    [[nodiscard]] NavigationSearchProgress progress(
        const NavigationSearchScratch& scratch) const noexcept
    {
        return currentProgress(scratch);
    }

private:
    struct CellTraversal final
    {
        bool allowed = false;
        bool dozerHack = false;
        bool tunnelingRelaxation = false;
    };

    enum class TraceEvent : uint8_t
    {
        Begin = 1,
        Expand = 2,
        Relax = 3,
        Finish = 4,
        Cancel = 5,
        Delay = 6,
        Resume = 7,
    };

    [[nodiscard]] static constexpr bool checkedAdd(uint32_t left, uint32_t right, uint32_t& result) noexcept
    {
        if (right > std::numeric_limits<uint32_t>::max() - left)
            return false;
        result = left + right;
        return result != NavigationSearchScratch::InfiniteCost;
    }

    [[nodiscard]] static constexpr uint64_t absoluteDifference(
        int64_t left, int64_t right) noexcept
    {
        if ((left < 0) == (right < 0)) {
            return left >= right
                ? static_cast<uint64_t>(left - right)
                : static_cast<uint64_t>(right - left);
        }
        const auto magnitude = [](int64_t value) constexpr noexcept {
            return value >= 0
                ? static_cast<uint64_t>(value)
                : static_cast<uint64_t>(-(value + 1)) + 1U;
        };
        const uint64_t leftMagnitude = magnitude(left);
        const uint64_t rightMagnitude = magnitude(right);
        return rightMagnitude >
                std::numeric_limits<uint64_t>::max() - leftMagnitude
            ? std::numeric_limits<uint64_t>::max()
            : leftMagnitude + rightMagnitude;
    }

    [[nodiscard]] static CellTraversal cellTraversal(
        const NavigationGrid& grid,
        const NavigationSearchRequest& request,
        NavigationCellId cell,
        const NavigationDynamicOverlay* dynamicOverlay,
        bool allowAnyDynamicObstacle) noexcept
    {
        CellTraversal result;
        if (grid.traversable(
                cell, request.movementMask, request.layer,
                request.clearance))
            return {.allowed = true};
        if (!dynamicOverlay || !grid.contains(cell) ||
            (request.movementMask & NavigationMovement::Air) != 0)
            return {};

        int32_t minOffset = 0;
        int32_t maxOffset = 0;
        switch (request.clearance) {
        case NavigationClearanceClass::Centered1x1:
            break;
        case NavigationClearanceClass::Offset2x2:
            minOffset = -1;
            break;
        case NavigationClearanceClass::Centered3x3:
            minOffset = -1;
            maxOffset = 1;
            break;
        case NavigationClearanceClass::Offset4x4:
            minOffset = -2;
            maxOffset = 1;
            break;
        case NavigationClearanceClass::Centered5x5:
            minOffset = -2;
            maxOffset = 2;
            break;
        }

        const NavigationGridCoordinate center = grid.coordinate(cell);
        for (int32_t y = minOffset; y <= maxOffset; ++y) {
            for (int32_t x = minOffset; x <= maxOffset; ++x) {
                const NavigationCellId sample =
                    grid.cellId({center.x + x, center.y + y});
                if (!sample) return {};
                const NavigationCellValue value = grid.cell(sample);
                if (value.layer != request.layer ||
                    (value.movementMask & request.movementMask) == 0)
                    return {};
                if (value.passability ==
                    NavigationPassability::Traversable)
                    continue;

                const uint32_t originalOwners =
                    dynamicOverlay->ownerCount(sample);
                if (originalOwners == 0)
                    return {};
                uint32_t owners = originalOwners;
                uint32_t fences = dynamicOverlay->fenceOwnerCount(sample);
                if (request.ignoredObstacle != 0 &&
                    dynamicOverlay->ownsCell(
                        request.ignoredObstacle, sample)) {
                    --owners;
                    if (dynamicOverlay->entityFenceSurface(
                            request.ignoredObstacle))
                        --fences;
                }
                if (owners == 0)
                    continue;
                if (request.crusherLevel != 0 && fences == owners)
                    continue;
                uint32_t dozerOwners = 0;
                for (const uint64_t owner :
                     request.dozerPassableObstacles) {
                    if (owner == request.ignoredObstacle) continue;
                    if (dynamicOverlay->ownsCell(owner, sample) &&
                        ++dozerOwners >= owners) {
                        break;
                    }
                }
                if (dozerOwners >= owners) {
                    result.dozerHack = true;
                    continue;
                }
                if (allowAnyDynamicObstacle) {
                    result.tunnelingRelaxation = true;
                    continue;
                }
                return {};
            }
        }
        if (result.tunnelingRelaxation)
            result.dozerHack = false;
        result.allowed = true;
        return result;
    }

    [[nodiscard]] static bool cellPinched(
        const NavigationGrid& grid,
        const NavigationDynamicOverlay* dynamicOverlay,
        NavigationCellId cell) noexcept
    {
        if (grid.pinched(cell)) return true;
        if (!dynamicOverlay || !grid.contains(cell)) return false;
        const NavigationGridCoordinate coordinate = grid.coordinate(cell);
        constexpr NavigationDirectionDelta orthogonal[4] = {
            {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        for (const NavigationDirectionDelta delta : orthogonal) {
            const NavigationCellId neighbor = grid.cellId({
                coordinate.x + delta.x, coordinate.y + delta.y});
            if (neighbor && dynamicOverlay->ownerCount(neighbor) != 0)
                return true;
        }
        return false;
    }

    [[nodiscard]] bool validRequest(
        const NavigationGrid& grid,
        const NavigationSearchRequest& request,
        container::Span<const engine::ai::AIPathObjectCellSnapshot>
            objectCells,
        const NavigationDynamicOverlay* dynamicOverlay) const noexcept
    {
        if (!(request.requestId != 0 && request.profile && request.movementMask != 0 && request.layer &&
               grid.contains(request.start) && grid.contains(request.goal) &&
               validClearanceClass(request.clearance) &&
               allowsTraversalCell(
                   grid, request, request.start, dynamicOverlay, true)))
            return false;
        if (request.patch.enabled) {
            if (request.patch.expansionLimit == 0 ||
                request.patch.suffixGoals.empty())
                return false;
            for (size_t index = 0;
                 index < request.patch.suffixGoals.size(); ++index) {
                const NavigationCellId cell =
                    request.patch.suffixGoals[index];
                if (!grid.contains(cell) ||
                    (index != 0 && cell <=
                        request.patch.suffixGoals[index - 1]))
                    return false;
            }
        }
        for (size_t index = 0;
             index < request.dozerPassableObstacles.size(); ++index) {
            const uint64_t object = request.dozerPassableObstacles[index];
            if (object == 0 ||
                (index != 0 && object <=
                    request.dozerPassableObstacles[index - 1])) {
                return false;
            }
        }
        if (request.adjustment.enabled) {
            if (request.adjustment.goals.empty() ||
                request.patch.enabled || request.attack.enabled ||
                request.safe.enabled || request.goalRadiusCells != 0)
                return false;
            for (size_t index = 0;
                 index < request.adjustment.goals.size(); ++index) {
                const NavigationCellId cell =
                    request.adjustment.goals[index].cell;
                if (!grid.contains(cell) ||
                    (index != 0 && cell <=
                        request.adjustment.goals[index - 1].cell))
                    return false;
            }
        }
        for (size_t index = 0; index < objectCells.size(); ++index)
        {
            const engine::ai::AIPathObjectCellSnapshot& value =
                objectCells[index];
            if (!value.object || value.layer == UINT32_MAX ||
                (value.layer == request.layer.value &&
                 value.cell >= grid.cellCount()) ||
                (value.effect != engine::ai::AIPathObjectCellEffect::FriendlyCost &&
                 value.effect != engine::ai::AIPathObjectCellEffect::EnemyBlock &&
                 value.effect != engine::ai::AIPathObjectCellEffect::NeutralBlock))
                return false;
            if (index == 0)
                continue;
            const engine::ai::AIPathObjectCellSnapshot& previous =
                objectCells[index - 1];
            if (value.layer < previous.layer ||
                (value.layer == previous.layer &&
                 (value.cell < previous.cell ||
                  (value.cell == previous.cell &&
                   value.object <= previous.object))))
                return false;
        }
        return true;
    }

    struct ObjectCellPolicy final
    {
        bool blocked = false;
        bool friendlyCost = false;
    };

    [[nodiscard]] static ObjectCellPolicy objectCellPolicy(
        container::Span<const engine::ai::AIPathObjectCellSnapshot>
            objectCells,
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
        ObjectCellPolicy result;
        for (auto current = first;
             current != objectCells.end() &&
                 current->layer == layer.value &&
                 current->cell == cell.value;
             ++current)
        {
            if (current->effect ==
                    engine::ai::AIPathObjectCellEffect::EnemyBlock ||
                current->effect ==
                    engine::ai::AIPathObjectCellEffect::NeutralBlock)
            {
                result.blocked = true;
                return result;
            }
            result.friendlyCost = true;
        }
        return result;
    }

    void reset(const NavigationSearchRequest& request) noexcept
    {
        m_request = request;
        m_status = NavigationSearchStatus::Idle;
        m_terminal = InvalidNavigationCell;
        m_best = InvalidNavigationCell;
        m_bestHCost = NavigationSearchScratch::InfiniteCost;
        m_bestGCost = NavigationSearchScratch::InfiniteCost;
        m_safeBest = InvalidNavigationCell;
        m_safeBestDistance = math::q32_32{};
        m_adjustmentBest = InvalidNavigationCell;
        m_adjustmentBestTotalCost =
            NavigationSearchScratch::InfiniteCost;
        m_adjustmentBestPreferenceCost =
            NavigationSearchScratch::InfiniteCost;
        m_adjustmentBestGCost = NavigationSearchScratch::InfiniteCost;
        m_totalExpansions = 0;
        m_traceHash = 14695981039346656037ULL;
        traceValue(TraceSchemaVersion);
        traceValue(request.requestId);
        traceValue(request.start.value);
        traceValue(request.goal.value);
        traceValue(request.profile.value);
        traceValue(request.movementMask);
        traceValue(request.layer.value);
        traceValue(static_cast<uint8_t>(request.clearance));
        traceValue(request.ignoredObstacle);
        traceValue(request.crusherLevel);
        traceValue(static_cast<uint64_t>(
            request.dozerPassableObstacles.size()));
        for (const uint64_t object : request.dozerPassableObstacles)
            traceValue(object);
        traceValue(request.goalRadiusCells);
        traceValue(static_cast<uint8_t>(request.adjustment.enabled));
        if (request.adjustment.enabled) {
            traceValue(request.adjustment.maximumAnchorOffsetCost);
            traceValue(static_cast<uint64_t>(
                request.adjustment.goals.size()));
            for (const NavigationSearchRequest::AdjustmentGoal& goal :
                 request.adjustment.goals) {
                traceValue(goal.cell.value);
                traceValue(goal.preferenceCost);
            }
        }
        traceValue(static_cast<uint8_t>(request.attack.enabled));
        if (request.attack.enabled) {
            traceValue(static_cast<uint64_t>(request.attack.target.xRaw));
            traceValue(static_cast<uint64_t>(request.attack.target.yRaw));
            traceValue(static_cast<uint64_t>(request.attack.target.zRaw));
            traceValue(static_cast<uint64_t>(
                request.attack.minimumRangeRaw));
            traceValue(static_cast<uint64_t>(
                request.attack.maximumRangeRaw));
            traceValue(static_cast<uint8_t>(
                request.attack.lineOfSightEnabled));
            traceValue(request.attack.subject);
            traceValue(request.attack.targetObject);
            traceValue(request.attack.subjectContainer);
            traceValue(request.attack.targetContainer);
            traceValue(request.attack.subjectSlaver);
            traceValue(request.attack.targetSlaver);
            traceValue(static_cast<uint64_t>(
                request.attack.seeThroughObstacles.size()));
            for (const uint64_t object :
                 request.attack.seeThroughObstacles)
                traceValue(object);
        }
        traceValue(static_cast<uint8_t>(request.patch.enabled));
        if (request.patch.enabled) {
            traceValue(request.patch.expansionLimit);
            traceValue(static_cast<uint64_t>(
                request.patch.suffixGoals.size()));
            for (const NavigationCellId cell :
                 request.patch.suffixGoals)
                traceValue(cell.value);
        }
        traceValue(static_cast<uint8_t>(request.safe.enabled));
        if (request.safe.enabled) {
            traceValue(static_cast<uint64_t>(request.safe.repulsor1.xRaw));
            traceValue(static_cast<uint64_t>(request.safe.repulsor1.yRaw));
            traceValue(static_cast<uint64_t>(request.safe.repulsor2.xRaw));
            traceValue(static_cast<uint64_t>(request.safe.repulsor2.yRaw));
            traceValue(static_cast<uint8_t>(request.safe.hasRepulsor2));
            traceValue(static_cast<uint64_t>(request.safe.radiusRaw));
            traceValue(request.safe.expansionLimit);
        }
    }

    [[nodiscard]] bool expandNeighbors(const NavigationGrid& grid,
                                       NavigationSearchScratch& scratch,
                                       NavigationOpenHeap& open,
                                       NavigationCellId current,
                                       uint32_t currentGCost,
                                       container::Span<const engine::ai::AIPathObjectCellSnapshot>
                                           objectCells,
                                       const NavigationDynamicOverlay*
                                           dynamicOverlay) noexcept
    {
        const NavigationGridCoordinate coordinate = grid.coordinate(current);
        for (NavigationDirection8 direction : NavigationDirectionOrder)
        {
            const NavigationDirectionDelta delta = directionDelta(direction);
            const NavigationCellId neighbor = grid.cellId({coordinate.x + delta.x, coordinate.y + delta.y});
            if (!neighbor)
                continue;
            const CellTraversal traversal = cellTraversal(
                grid, m_request, neighbor, dynamicOverlay, m_tunneling);
            if (!traversal.allowed) continue;

            const ObjectCellPolicy unitPolicy =
                objectCellPolicy(objectCells, m_request.layer, neighbor);
            if (unitPolicy.blocked)
                continue;
            if (isDiagonal(direction))
            {
                const NavigationCellId sideX = grid.cellId(
                    {coordinate.x + delta.x, coordinate.y});
                const NavigationCellId sideY = grid.cellId(
                    {coordinate.x, coordinate.y + delta.y});
                if (!sideX || !sideY ||
                    ((!allowsTraversalCell(
                          grid, m_request, sideX, dynamicOverlay,
                          m_tunneling) ||
                      objectCellPolicy(
                          objectCells, m_request.layer, sideX).blocked) &&
                     (!allowsTraversalCell(
                          grid, m_request, sideY, dynamicOverlay,
                          m_tunneling) ||
                      objectCellPolicy(
                          objectCells, m_request.layer, sideY).blocked)))
                    continue;
            }

            uint32_t stepCost = isDiagonal(direction) ? DiagonalCost : OrthogonalCost;
            const bool ordinaryTraversal = grid.traversable(
                neighbor, m_request.movementMask, m_request.layer,
                m_request.clearance);
            if (m_tunneling &&
                (ordinaryTraversal || traversal.dozerHack))
                m_tunneling = false;
            else if (m_tunneling && !checkedAdd(
                         stepCost, 10U * OrthogonalCost, stepCost))
                return failCapacity();
            if (traversal.dozerHack && !checkedAdd(
                    stepCost, 100U * OrthogonalCost, stepCost))
                return failCapacity();
            if (!checkedAdd(stepCost, grid.terrainCost()[neighbor.value], stepCost))
                return failCapacity();
            const NavigationCellValue neighborValue = grid.cell(neighbor);
            if ((neighborValue.movementMask & NavigationMovement::Cliff) != 0) {
                const int64_t fromHeight = grid.heightRaw()[current.value];
                const int64_t toHeight = grid.heightRaw()[neighbor.value];
                const uint64_t heightDelta = absoluteDifference(
                    fromHeight, toHeight);
                if (heightDelta < static_cast<uint64_t>(
                        grid.transform().cellSizeRaw) &&
                    !checkedAdd(
                        stepCost, 7U * DiagonalCost, stepCost)) {
                    return failCapacity();
                }
            }
            if (cellPinched(grid, dynamicOverlay, neighbor) && !checkedAdd(
                    stepCost, OrthogonalCost, stepCost)) {
                return failCapacity();
            }
            // RefCode charges one fixed ally penalty of
            // 3 * COST_DIAGONAL = 42 for an occupied candidate cell. It never
            // turns that cell into global topology.
            if (unitPolicy.friendlyCost && !checkedAdd(
                    stepCost, 3U * DiagonalCost, stepCost))
                return failCapacity();
            uint32_t tentativeGCost = 0;
            if (!checkedAdd(currentGCost, stepCost, tentativeGCost))
                return failCapacity();

            const bool wasOpen = scratch.isOpen(neighbor);
            const bool wasClosed = scratch.isClosed(neighbor);
            if ((wasOpen || wasClosed) && tentativeGCost >= scratch.gCost(neighbor))
                continue;

            uint32_t hCost = 0;
            uint32_t fCost = 0;
            if (!m_request.safe.enabled &&
                !searchHeuristic(grid.coordinate(neighbor),
                                 grid.coordinate(m_request.goal),
                                 m_request, hCost))
                return failCapacity();
            if (!m_request.safe.enabled) {
                if (!checkedAdd(tentativeGCost, hCost, fCost))
                    return failCapacity();
            } else {
                fCost = tentativeGCost;
            }

            scratch.markOpen(neighbor, tentativeGCost, current);
            NavigationOpenHeapResult heapResult = NavigationOpenHeapResult::Success;
            if (wasOpen)
                heapResult = open.decreaseKey(neighbor, fCost, hCost);
            else
                heapResult = open.push(neighbor, fCost, hCost);
            if (heapResult != NavigationOpenHeapResult::Success)
                return failCapacity();
            traceEvent(TraceEvent::Relax, neighbor, tentativeGCost, hCost);
        }
        return true;
    }

    [[nodiscard]] bool failCapacity() noexcept
    {
        finish(NavigationSearchStatus::CapacityExceeded, InvalidNavigationCell);
        return false;
    }

    void updateBest(NavigationCellId cell, uint32_t gCost, uint32_t hCost) noexcept
    {
        if (!m_best || hCost < m_bestHCost ||
            (hCost == m_bestHCost && (gCost < m_bestGCost || (gCost == m_bestGCost && cell < m_best))))
        {
            m_best = cell;
            m_bestHCost = hCost;
            m_bestGCost = gCost;
        }
    }

    [[nodiscard]] math::q32_32 safeDistanceSquared(
        const NavigationGrid& grid, NavigationCellId cell) const noexcept
    {
        NavigationWorldPosition position;
        if (!grid.cellPosition(cell, m_request.clearance, position))
            return math::q32_32{};
        const math::q32_32 x = math::q32_32::from_raw(position.xRaw);
        const math::q32_32 y = math::q32_32::from_raw(position.yRaw);
        const auto distanceSquared = [&](const NavigationWorldPosition& repulsor) {
            const math::q32_32 dx = x - math::q32_32::from_raw(repulsor.xRaw);
            const math::q32_32 dy = y - math::q32_32::from_raw(repulsor.yRaw);
            return dx * dx + dy * dy;
        };
        const math::q32_32 first = distanceSquared(m_request.safe.repulsor1);
        if (!m_request.safe.hasRepulsor2)
            return first;
        const math::q32_32 second = distanceSquared(m_request.safe.repulsor2);
        return second < first ? second : first;
    }

    void updateSafeBest(const NavigationGrid& grid,
                        NavigationCellId cell,
                        uint32_t gCost) noexcept
    {
        const math::q32_32 distance = safeDistanceSquared(grid, cell);
        if (!m_safeBest || distance > m_safeBestDistance ||
            (distance == m_safeBestDistance &&
             (gCost < m_safeBestGCost ||
              (gCost == m_safeBestGCost && cell < m_safeBest)))) {
            m_safeBest = cell;
            m_safeBestDistance = distance;
            m_safeBestGCost = gCost;
        }
    }

    [[nodiscard]] bool safeCandidate(const NavigationGrid& grid,
                                     NavigationCellId cell) const noexcept
    {
        const math::q32_32 radius = math::q32_32::from_raw(
            std::max<int64_t>(0, m_request.safe.radiusRaw));
        return safeDistanceSquared(grid, cell) > radius * radius;
    }

    void finishExhausted() noexcept
    {
        if (m_adjustmentBest) {
            finish(NavigationSearchStatus::Success, m_adjustmentBest);
            return;
        }
        if (m_request.safe.enabled) {
            if (m_safeBest)
                finish(NavigationSearchStatus::PartialPath, m_safeBest);
            else
                finish(NavigationSearchStatus::NoPath, InvalidNavigationCell);
            return;
        }
        finish(NavigationSearchStatus::NoPath, InvalidNavigationCell);
    }

    NavigationSearchStatus finish(NavigationSearchStatus status, NavigationCellId terminal) noexcept
    {
        m_status = status;
        m_terminal = terminal;
        traceEvent(TraceEvent::Finish, terminal, static_cast<uint32_t>(status), 0);
        return m_status;
    }

    [[nodiscard]] NavigationSearchProgress currentProgress(const NavigationSearchScratch& scratch,
                                                           uint32_t expandedThisStep = 0) const noexcept
    {
        return {m_status,
                expandedThisStep,
                m_totalExpansions,
                m_terminal,
                m_terminal ? scratch.gCost(m_terminal) : NavigationSearchScratch::InfiniteCost,
                m_traceHash};
    }

    void traceEvent(TraceEvent event, NavigationCellId cell, uint32_t first, uint32_t second) noexcept
    {
        traceValue(static_cast<uint8_t>(event));
        traceValue(cell.value);
        traceValue(first);
        traceValue(second);
    }

    void traceObjectCells(
        container::Span<const engine::ai::AIPathObjectCellSnapshot>
            objectCells) noexcept
    {
        traceValue(static_cast<uint64_t>(objectCells.size()));
        for (const engine::ai::AIPathObjectCellSnapshot& value : objectCells)
        {
            traceValue(value.layer);
            traceValue(value.cell);
            traceValue(value.object.value);
            traceValue(static_cast<uint8_t>(value.effect));
        }
    }

    template <typename Unsigned>
    void traceValue(Unsigned value) noexcept
    {
        static_assert(std::is_unsigned_v<Unsigned>);
        uint64_t remaining = static_cast<uint64_t>(value);
        for (size_t byte = 0; byte < sizeof(Unsigned); ++byte)
        {
            m_traceHash ^= static_cast<uint8_t>(remaining & 0xffU);
            m_traceHash *= 1099511628211ULL;
            remaining >>= 8U;
        }
    }

    inline static constexpr uint32_t TraceSchemaVersion = 11;
    NavigationSearchRequest m_request;
    bool m_tunneling = false;
    NavigationSearchStatus m_status = NavigationSearchStatus::Idle;
    NavigationCellId m_terminal = InvalidNavigationCell;
    NavigationCellId m_best = InvalidNavigationCell;
    uint32_t m_bestHCost = NavigationSearchScratch::InfiniteCost;
    uint32_t m_bestGCost = NavigationSearchScratch::InfiniteCost;
    NavigationCellId m_safeBest = InvalidNavigationCell;
    math::q32_32 m_safeBestDistance;
    uint32_t m_safeBestGCost = NavigationSearchScratch::InfiniteCost;
    NavigationCellId m_adjustmentBest = InvalidNavigationCell;
    uint32_t m_adjustmentBestTotalCost =
        NavigationSearchScratch::InfiniteCost;
    uint32_t m_adjustmentBestPreferenceCost =
        NavigationSearchScratch::InfiniteCost;
    uint32_t m_adjustmentBestGCost = NavigationSearchScratch::InfiniteCost;
    uint64_t m_totalExpansions = 0;
    uint64_t m_traceHash = 0;
};

} // namespace engine::navigation
