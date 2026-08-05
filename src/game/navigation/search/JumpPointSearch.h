#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "JumpPointPrecompute.h"
#include "NavigationOpenHeap.h"

namespace engine::navigation
{

enum class JpsFallbackReason : uint8_t
{
    None = 0,
    WeightedTerrain,
    Multilayer,
    PortalTraversal,
    DirtyTopology,
    InvalidNavigationRevision,
    MissingGridHash,
    MissingPrecompute,
    StalePrecomputeRevision,
    PrecomputeProfileMismatch,
    PrecomputeGridMismatch,
};

enum class JumpPointSearchMode : uint8_t
{
    Online = 0,
    Precomputed,
};

// Deterministic strict-corner baseline. Unit successors retain A*'s complete
// graph while exact-cost jump successors contract straight runs. This makes
// the baseline parity-safe even where strict-corner canonical JPS pruning is
// more conservative than weak-corner formulae. The only pruned unit edge is
// the immediate reversal to the predecessor, which cannot improve a path with
// positive edge costs. No node storage is allocated by begin(), step(), or
// readPath().
class JumpPointSearch final
{
public:
    [[nodiscard]] NavigationSearchStatus begin(const NavigationGrid& grid,
                                               NavigationSearchScratch& scratch,
                                               const NavigationSearchRequest& request,
                                               const JpsSearchContext& context) noexcept
    {
        return beginInternal(grid, scratch, request, context, nullptr, JumpPointSearchMode::Online);
    }

    [[nodiscard]] NavigationSearchStatus begin(const NavigationGrid& grid,
                                               NavigationSearchScratch& scratch,
                                               const NavigationSearchRequest& request,
                                               const JpsSearchContext& context,
                                               const JumpPointPrecompute& precompute) noexcept
    {
        return beginInternal(grid, scratch, request, context, &precompute, JumpPointSearchMode::Precomputed);
    }

    [[nodiscard]] NavigationSearchProgress step(const NavigationGrid& grid,
                                                NavigationSearchScratch& scratch,
                                                uint32_t expansionBudget) noexcept
    {
        return stepInternal(grid, scratch, expansionBudget, nullptr);
    }

    [[nodiscard]] NavigationSearchProgress step(const NavigationGrid& grid,
                                                NavigationSearchScratch& scratch,
                                                uint32_t expansionBudget,
                                                const JumpPointPrecompute& precompute) noexcept
    {
        return stepInternal(grid, scratch, expansionBudget, &precompute);
    }

    [[nodiscard]] NavigationPathReadResult readPath(const NavigationSearchScratch& scratch,
                                                    container::Span<NavigationCellId> output) const noexcept
    {
        if (m_usedFallback)
            return m_fallback.readPath(scratch, output);
        NavigationPathReadResult result;
        if ((m_status != NavigationSearchStatus::Success && m_status != NavigationSearchStatus::PartialPath) ||
            !m_terminal)
            return result;

        NavigationCellId cursor = m_terminal;
        size_t count = 1;
        size_t parentCount = 0;
        while (cursor != m_request.start)
        {
            if (!scratch.contains(cursor) || parentCount++ == scratch.cellCapacity())
            {
                result.status = NavigationPathReadStatus::CorruptParentChain;
                return result;
            }
            const NavigationCellId parent = scratch.parent(cursor);
            size_t segmentLength = 0;
            if (!segmentCellCount(cursor, parent, segmentLength) ||
                segmentLength > std::numeric_limits<size_t>::max() - count)
            {
                result.status = NavigationPathReadStatus::CorruptParentChain;
                return result;
            }
            count += segmentLength;
            cursor = parent;
        }

        result.requiredCount = count;
        result.totalCost = scratch.gCost(m_terminal);
        result.layer = m_request.layer;
        if (output.size() < count)
        {
            result.status = NavigationPathReadStatus::OutputCapacityExceeded;
            return result;
        }

        size_t write = count;
        cursor = m_terminal;
        while (cursor != m_request.start)
        {
            const NavigationCellId parent = scratch.parent(cursor);
            const NavigationGridCoordinate cursorCoordinate = coordinate(cursor);
            const NavigationGridCoordinate parentCoordinate = coordinate(parent);
            const int32_t dx = sign(parentCoordinate.x - cursorCoordinate.x);
            const int32_t dy = sign(parentCoordinate.y - cursorCoordinate.y);
            NavigationGridCoordinate emitted = cursorCoordinate;
            while (emitted != parentCoordinate)
            {
                output[--write] = cellId(emitted);
                emitted.x += dx;
                emitted.y += dy;
            }
            cursor = parent;
        }
        output[--write] = m_request.start;
        if (write != 0)
        {
            result.status = NavigationPathReadStatus::CorruptParentChain;
            return result;
        }
        result.status = NavigationPathReadStatus::Success;
        return result;
    }

    [[nodiscard]] bool cancel(uint64_t requestId) noexcept
    {
        if (m_usedFallback)
            return m_fallback.cancel(requestId);
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
        if (m_usedFallback)
            return m_fallback.delay(requestId);
        if (m_status != NavigationSearchStatus::Pending || requestId == 0 || requestId != m_request.requestId)
            return false;
        m_status = NavigationSearchStatus::Delayed;
        traceEvent(TraceEvent::Delay, InvalidNavigationCell, 0, 0);
        return true;
    }

    [[nodiscard]] bool resume(uint64_t requestId) noexcept
    {
        if (m_usedFallback)
            return m_fallback.resume(requestId);
        if (m_status != NavigationSearchStatus::Delayed || requestId == 0 || requestId != m_request.requestId)
            return false;
        m_status = NavigationSearchStatus::Pending;
        traceEvent(TraceEvent::Resume, InvalidNavigationCell, 0, 0);
        return true;
    }

    [[nodiscard]] bool usedFallback() const noexcept
    {
        return m_usedFallback;
    }
    [[nodiscard]] JpsFallbackReason fallbackReason() const noexcept
    {
        return m_fallbackReason;
    }
    [[nodiscard]] JumpPointSearchMode mode() const noexcept
    {
        return m_mode;
    }
    [[nodiscard]] NavigationSearchStatus status() const noexcept
    {
        return m_usedFallback ? m_fallback.status() : m_status;
    }
    [[nodiscard]] uint64_t requestId() const noexcept
    {
        return m_usedFallback ? m_fallback.requestId() : m_request.requestId;
    }
    [[nodiscard]] uint64_t totalExpansions() const noexcept
    {
        return m_usedFallback ? m_fallback.totalExpansions() : m_totalExpansions;
    }
    [[nodiscard]] uint64_t traceHash() const noexcept
    {
        return m_usedFallback ? m_fallback.traceHash() : m_traceHash;
    }
    [[nodiscard]] NavigationCellId terminal() const noexcept
    {
        return m_usedFallback ? m_fallback.terminal() : m_terminal;
    }

    [[nodiscard]] static bool isForcedNeighbor(const NavigationGrid& grid,
                                               NavigationCellId current,
                                               NavigationDirection8 travelDirection,
                                               NavigationDirection8 candidateDirection,
                                               NavigationMovementMask movementMask,
                                               NavigationLayerId layer) noexcept
    {
        return jps_detail::isForcedNeighbor(grid, current, travelDirection, candidateDirection, movementMask, layer);
    }

private:
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

    [[nodiscard]] NavigationSearchProgress stepInternal(const NavigationGrid& grid,
                                                        NavigationSearchScratch& scratch,
                                                        uint32_t expansionBudget,
                                                        const JumpPointPrecompute* precompute) noexcept
    {
        if (m_usedFallback)
            return m_fallback.step(grid, scratch, expansionBudget);
        NavigationSearchProgress progress = currentProgress(scratch);
        if (m_status != NavigationSearchStatus::Pending || expansionBudget == 0)
            return progress;
        if (m_mode == JumpPointSearchMode::Precomputed &&
            (precompute == nullptr || !precompute->isBuilt() ||
             precompute->navigationRevision() != m_precomputeRevision ||
             precompute->gridHash() != m_precomputeGridHash || precompute->profile() != m_request.profile ||
             precompute->movementMask() != m_request.movementMask || precompute->layer() != m_request.layer ||
             precompute->cellCount() != grid.cellCount()))
        {
            finish(NavigationSearchStatus::InvalidRequest, InvalidNavigationCell);
            return currentProgress(scratch);
        }
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
            scratch.markClosed(entry.cell);
            ++progress.expandedThisStep;
            ++m_totalExpansions;
            const uint32_t currentGCost = scratch.gCost(entry.cell);
            traceEvent(TraceEvent::Expand, entry.cell, currentGCost, entry.hCost);
            updateBest(entry.cell, currentGCost, entry.hCost);
            if (entry.cell == m_request.goal)
            {
                finish(NavigationSearchStatus::Success, entry.cell);
                return currentProgress(scratch, progress.expandedThisStep);
            }
            if (!expandSuccessors(grid, scratch, open, entry.cell, currentGCost, precompute))
                return currentProgress(scratch, progress.expandedThisStep);
        }
        return currentProgress(scratch, progress.expandedThisStep);
    }

    [[nodiscard]] NavigationSearchStatus beginInternal(const NavigationGrid& grid,
                                                       NavigationSearchScratch& scratch,
                                                       const NavigationSearchRequest& request,
                                                       const JpsSearchContext& context,
                                                       const JumpPointPrecompute* precompute,
                                                       JumpPointSearchMode mode) noexcept
    {
        reset(request, grid.width(), grid.height(), mode);
        const JpsFallbackReason reason = ineligibleReason(grid, request, context, precompute, mode);
        if (reason != JpsFallbackReason::None)
        {
            m_usedFallback = true;
            m_fallbackReason = reason;
            return m_fallback.begin(grid, scratch, request);
        }
        if (!validRequest(grid, request))
            return finish(NavigationSearchStatus::InvalidRequest, InvalidNavigationCell);
        if (scratch.cellCapacity() < grid.cellCount() || scratch.heapCapacity() < grid.cellCount())
            return finish(NavigationSearchStatus::CapacityExceeded, InvalidNavigationCell);

        if (mode == JumpPointSearchMode::Precomputed)
        {
            m_precomputeRevision = context.navigationRevision;
            m_precomputeGridHash = context.gridHash;
        }
        [[maybe_unused]] const uint32_t epoch = scratch.beginSearch();
        uint32_t hCost = 0;
        if (!AStarOracle::octileCost(grid.coordinate(request.start), grid.coordinate(request.goal), hCost))
            return finish(NavigationSearchStatus::CapacityExceeded, InvalidNavigationCell);
        scratch.markOpen(request.start, 0, InvalidNavigationCell);
        NavigationOpenHeap open(scratch);
        if (open.push(request.start, hCost, hCost) != NavigationOpenHeapResult::Success)
            return finish(NavigationSearchStatus::CapacityExceeded, InvalidNavigationCell);
        m_status = NavigationSearchStatus::Pending;
        traceEvent(TraceEvent::Begin, request.start, 0, hCost);
        return m_status;
    }

    [[nodiscard]] static JpsFallbackReason ineligibleReason(const NavigationGrid& grid,
                                                            const NavigationSearchRequest& request,
                                                            const JpsSearchContext& context,
                                                            const JumpPointPrecompute* precompute,
                                                            JumpPointSearchMode mode) noexcept
    {
        if (grid.hasWeightedTerrain())
            return JpsFallbackReason::WeightedTerrain;
        if (!context.singleLayer)
            return JpsFallbackReason::Multilayer;
        if (context.portalTraversal)
            return JpsFallbackReason::PortalTraversal;
        if (!context.topologyClean)
            return JpsFallbackReason::DirtyTopology;
        if (!context.navigationRevision)
            return JpsFallbackReason::InvalidNavigationRevision;
        if (context.gridHash == 0)
            return JpsFallbackReason::MissingGridHash;
        if (mode == JumpPointSearchMode::Online)
            return JpsFallbackReason::None;
        if (precompute == nullptr || !precompute->isBuilt())
            return JpsFallbackReason::MissingPrecompute;
        if (precompute->navigationRevision() != context.navigationRevision)
            return JpsFallbackReason::StalePrecomputeRevision;
        if (precompute->profile() != request.profile || precompute->movementMask() != request.movementMask ||
            precompute->layer() != request.layer)
            return JpsFallbackReason::PrecomputeProfileMismatch;
        if (precompute->gridHash() != context.gridHash || precompute->cellCount() != grid.cellCount())
            return JpsFallbackReason::PrecomputeGridMismatch;
        return JpsFallbackReason::None;
    }

    [[nodiscard]] static bool validRequest(const NavigationGrid& grid, const NavigationSearchRequest& request) noexcept
    {
        return request.requestId != 0 && request.profile && request.movementMask != 0 && request.layer &&
               grid.contains(request.start) && grid.contains(request.goal) &&
               grid.traversable(request.start, request.movementMask, request.layer);
    }

    [[nodiscard]] bool expandSuccessors(const NavigationGrid& grid,
                                        NavigationSearchScratch& scratch,
                                        NavigationOpenHeap& open,
                                        NavigationCellId current,
                                        uint32_t currentGCost,
                                        const JumpPointPrecompute* precompute) noexcept
    {
        const NavigationGridCoordinate currentCoordinate = grid.coordinate(current);
        NavigationDirectionDelta reverse{};
        bool hasReverse = false;
        const NavigationCellId parent = scratch.parent(current);
        if (parent)
        {
            const NavigationGridCoordinate parentCoordinate = grid.coordinate(parent);
            reverse.x = static_cast<int8_t>(sign(parentCoordinate.x - currentCoordinate.x));
            reverse.y = static_cast<int8_t>(sign(parentCoordinate.y - currentCoordinate.y));
            hasReverse = true;
        }

        for (NavigationDirection8 direction : NavigationDirectionOrder)
        {
            const NavigationDirectionDelta delta = directionDelta(direction);
            if (hasReverse && delta.x == reverse.x && delta.y == reverse.y)
                continue;
            const NavigationCellId neighbor =
                grid.cellId({currentCoordinate.x + delta.x, currentCoordinate.y + delta.y});
            if (!neighbor || !grid.traversableEdge(current, neighbor, m_request.movementMask, m_request.layer))
                continue;
            const uint32_t unitCost = isDiagonal(direction) ? AStarOracle::DiagonalCost : AStarOracle::OrthogonalCost;
            if (!relax(grid, scratch, open, current, neighbor, currentGCost, unitCost))
                return false;

            int32_t jumpDistance = 0;
            if (m_mode == JumpPointSearchMode::Precomputed)
                jumpDistance = precomputedJumpDistance(grid, current, direction, *precompute);
            else
                jumpDistance = onlineJumpDistance(grid, current, direction);
            if (jumpDistance <= 1)
                continue;
            const NavigationCellId jumpCell = grid.cellId(
                {currentCoordinate.x + delta.x * jumpDistance, currentCoordinate.y + delta.y * jumpDistance});
            const uint64_t jumpCost64 = static_cast<uint64_t>(unitCost) * static_cast<uint32_t>(jumpDistance);
            if (!jumpCell || jumpCost64 >= NavigationSearchScratch::InfiniteCost ||
                !relax(grid, scratch, open, current, jumpCell, currentGCost, static_cast<uint32_t>(jumpCost64)))
                return false;
        }
        return true;
    }

    [[nodiscard]] int32_t onlineJumpDistance(const NavigationGrid& grid,
                                             NavigationCellId origin,
                                             NavigationDirection8 direction) const noexcept
    {
        const NavigationDirectionDelta delta = directionDelta(direction);
        NavigationCellId cursor = origin;
        NavigationGridCoordinate coordinateValue = grid.coordinate(origin);
        int32_t distance = 0;
        while (distance < std::numeric_limits<int32_t>::max())
        {
            const NavigationCellId next = grid.cellId({coordinateValue.x + delta.x, coordinateValue.y + delta.y});
            if (!next || !grid.traversableEdge(cursor, next, m_request.movementMask, m_request.layer))
                break;
            cursor = next;
            coordinateValue.x += delta.x;
            coordinateValue.y += delta.y;
            ++distance;
            if (cursor == m_request.goal ||
                jps_detail::hasForcedNeighbor(grid, cursor, direction, m_request.movementMask, m_request.layer))
                break;
        }
        return distance;
    }

    [[nodiscard]] int32_t precomputedJumpDistance(const NavigationGrid& grid,
                                                  NavigationCellId origin,
                                                  NavigationDirection8 direction,
                                                  const JumpPointPrecompute& precompute) const noexcept
    {
        const int32_t stored = precompute.distance(origin, direction);
        const int32_t extent = stored < 0 ? -stored : stored;
        if (extent == 0)
            return 0;
        const NavigationGridCoordinate from = grid.coordinate(origin);
        const NavigationGridCoordinate goal = grid.coordinate(m_request.goal);
        const NavigationDirectionDelta delta = directionDelta(direction);
        const int32_t goalDx = goal.x - from.x;
        const int32_t goalDy = goal.y - from.y;
        int32_t goalDistance = 0;
        if (delta.x == 0 && goalDx == 0 && sign(goalDy) == delta.y)
            goalDistance = goalDy < 0 ? -goalDy : goalDy;
        else if (delta.y == 0 && goalDy == 0 && sign(goalDx) == delta.x)
            goalDistance = goalDx < 0 ? -goalDx : goalDx;
        else if (delta.x != 0 && delta.y != 0 && sign(goalDx) == delta.x && sign(goalDy) == delta.y &&
                 (goalDx < 0 ? -goalDx : goalDx) == (goalDy < 0 ? -goalDy : goalDy))
            goalDistance = goalDx < 0 ? -goalDx : goalDx;
        return goalDistance > 0 && goalDistance <= extent ? goalDistance : extent;
    }

    [[nodiscard]] bool relax(const NavigationGrid& grid,
                             NavigationSearchScratch& scratch,
                             NavigationOpenHeap& open,
                             NavigationCellId parent,
                             NavigationCellId successor,
                             uint32_t parentGCost,
                             uint32_t edgeCost) noexcept
    {
        uint32_t tentativeGCost = 0;
        if (!checkedAdd(parentGCost, edgeCost, tentativeGCost))
            return failCapacity();
        const bool wasOpen = scratch.isOpen(successor);
        const bool wasClosed = scratch.isClosed(successor);
        if ((wasOpen || wasClosed) && tentativeGCost >= scratch.gCost(successor))
            return true;
        uint32_t hCost = 0;
        uint32_t fCost = 0;
        if (!AStarOracle::octileCost(grid.coordinate(successor), grid.coordinate(m_request.goal), hCost) ||
            !checkedAdd(tentativeGCost, hCost, fCost))
            return failCapacity();
        scratch.markOpen(successor, tentativeGCost, parent);
        const NavigationOpenHeapResult heapResult =
            wasOpen ? open.decreaseKey(successor, fCost, hCost) : open.push(successor, fCost, hCost);
        if (heapResult != NavigationOpenHeapResult::Success)
            return failCapacity();
        traceEvent(TraceEvent::Relax, successor, tentativeGCost, hCost);
        return true;
    }

    [[nodiscard]] static constexpr bool checkedAdd(uint32_t left, uint32_t right, uint32_t& result) noexcept
    {
        if (right > std::numeric_limits<uint32_t>::max() - left)
            return false;
        result = left + right;
        return result != NavigationSearchScratch::InfiniteCost;
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

    void finishExhausted() noexcept
    {
        finish(NavigationSearchStatus::NoPath, InvalidNavigationCell);
    }

    NavigationSearchStatus finish(NavigationSearchStatus statusValue, NavigationCellId terminalValue) noexcept
    {
        m_status = statusValue;
        m_terminal = terminalValue;
        traceEvent(TraceEvent::Finish, terminalValue, static_cast<uint32_t>(statusValue), 0);
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

    void reset(const NavigationSearchRequest& request,
               uint32_t width,
               uint32_t height,
               JumpPointSearchMode modeValue) noexcept
    {
        m_request = request;
        m_width = width;
        m_height = height;
        m_mode = modeValue;
        m_precomputeRevision = InvalidNavigationRevision;
        m_precomputeGridHash = 0;
        m_status = NavigationSearchStatus::Idle;
        m_terminal = InvalidNavigationCell;
        m_best = InvalidNavigationCell;
        m_bestHCost = NavigationSearchScratch::InfiniteCost;
        m_bestGCost = NavigationSearchScratch::InfiniteCost;
        m_totalExpansions = 0;
        m_usedFallback = false;
        m_fallbackReason = JpsFallbackReason::None;
        m_traceHash = 14695981039346656037ULL;
        traceValue(TraceSchemaVersion);
        traceValue(request.requestId);
        traceValue(static_cast<uint8_t>(modeValue));
    }

    [[nodiscard]] NavigationGridCoordinate coordinate(NavigationCellId cell) const noexcept
    {
        return coordinateFromCellId(cell, m_width);
    }

    [[nodiscard]] NavigationCellId cellId(NavigationGridCoordinate coordinateValue) const noexcept
    {
        return cellIdFromCoordinate(coordinateValue, m_width, m_height);
    }

    [[nodiscard]] bool segmentCellCount(NavigationCellId from, NavigationCellId to, size_t& count) const noexcept
    {
        if (!from || !to)
            return false;
        const NavigationGridCoordinate first = coordinate(from);
        const NavigationGridCoordinate second = coordinate(to);
        const int64_t dx = static_cast<int64_t>(first.x) - second.x;
        const int64_t dy = static_cast<int64_t>(first.y) - second.y;
        const uint64_t absDx = dx < 0 ? static_cast<uint64_t>(-dx) : static_cast<uint64_t>(dx);
        const uint64_t absDy = dy < 0 ? static_cast<uint64_t>(-dy) : static_cast<uint64_t>(dy);
        if ((absDx == 0 && absDy == 0) || (absDx != 0 && absDy != 0 && absDx != absDy))
            return false;
        const uint64_t value = absDx > absDy ? absDx : absDy;
        if (value > std::numeric_limits<size_t>::max())
            return false;
        count = static_cast<size_t>(value);
        return true;
    }

    [[nodiscard]] static constexpr int32_t sign(int32_t value) noexcept
    {
        return value < 0 ? -1 : (value > 0 ? 1 : 0);
    }

    void traceEvent(TraceEvent event, NavigationCellId cell, uint32_t first, uint32_t second) noexcept
    {
        traceValue(static_cast<uint8_t>(event));
        traceValue(cell.value);
        traceValue(first);
        traceValue(second);
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

    inline static constexpr uint32_t TraceSchemaVersion = 1;
    AStarOracle m_fallback;
    NavigationSearchRequest m_request;
    NavigationRevision m_precomputeRevision = InvalidNavigationRevision;
    uint64_t m_precomputeGridHash = 0;
    NavigationSearchStatus m_status = NavigationSearchStatus::Idle;
    NavigationCellId m_terminal = InvalidNavigationCell;
    NavigationCellId m_best = InvalidNavigationCell;
    uint32_t m_bestHCost = NavigationSearchScratch::InfiniteCost;
    uint32_t m_bestGCost = NavigationSearchScratch::InfiniteCost;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint64_t m_totalExpansions = 0;
    uint64_t m_traceHash = 0;
    JumpPointSearchMode m_mode = JumpPointSearchMode::Online;
    bool m_usedFallback = false;
    JpsFallbackReason m_fallbackReason = JpsFallbackReason::None;
};

} // namespace engine::navigation
