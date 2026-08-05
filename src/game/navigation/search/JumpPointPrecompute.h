#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "AStarOracle.h"
#include "core/container/container_types.h"

namespace engine::navigation
{

struct JpsSearchContext final
{
    NavigationRevision navigationRevision = InvalidNavigationRevision;
    uint64_t gridHash = 0;
    bool singleLayer = false;
    bool portalTraversal = false;
    bool topologyClean = false;
};

enum class JumpPointPrecomputeResult : uint8_t
{
    Success = 0,
    InvalidCapacity,
    InvalidGrid,
    InvalidBinding,
    UnsupportedWeightedTerrain,
    UnsupportedMultilayer,
    UnsupportedPortalTraversal,
    DirtyTopology,
    CapacityExceeded,
};

namespace jps_detail
{
[[nodiscard]] constexpr uint32_t movementCost(int32_t dx, int32_t dy) noexcept
{
    return dx != 0 && dy != 0 ? 14U : 10U;
}

[[nodiscard]] inline bool legalStep(const NavigationGrid& grid,
                                    NavigationGridCoordinate from,
                                    NavigationGridCoordinate to,
                                    NavigationMovementMask movementMask,
                                    NavigationLayerId layer) noexcept
{
    const NavigationCellId fromCell = grid.cellId(from);
    const NavigationCellId toCell = grid.cellId(to);
    return fromCell && toCell && grid.traversableEdge(fromCell, toCell, movementMask, layer);
}

// A strict-corner forced neighbor is a legal successor whose route through
// current cannot be replaced, at equal or lower octile cost, by a one- or
// two-edge local route from the predecessor which avoids current. Expressing
// the rule as local dominance avoids weak-corner formulae that incorrectly
// nominate an illegal diagonal when one of its cardinal supports is blocked.
[[nodiscard]] inline bool isForcedNeighbor(const NavigationGrid& grid,
                                           NavigationCellId current,
                                           NavigationDirection8 travelDirection,
                                           NavigationDirection8 candidateDirection,
                                           NavigationMovementMask movementMask,
                                           NavigationLayerId layer) noexcept
{
    if (!current || candidateDirection == travelDirection)
        return false;
    const NavigationDirectionDelta travel = directionDelta(travelDirection);
    const NavigationDirectionDelta candidate = directionDelta(candidateDirection);
    const NavigationGridCoordinate coordinate = grid.coordinate(current);
    const NavigationGridCoordinate predecessor{coordinate.x - travel.x, coordinate.y - travel.y};
    const NavigationGridCoordinate successor{coordinate.x + candidate.x, coordinate.y + candidate.y};
    if (!legalStep(grid, predecessor, coordinate, movementMask, layer) ||
        !legalStep(grid, coordinate, successor, movementMask, layer))
        return false;

    const uint32_t throughCost = movementCost(travel.x, travel.y) + movementCost(candidate.x, candidate.y);
    const int32_t directDx = successor.x - predecessor.x;
    const int32_t directDy = successor.y - predecessor.y;
    if (directDx >= -1 && directDx <= 1 && directDy >= -1 && directDy <= 1 && (directDx != 0 || directDy != 0) &&
        legalStep(grid, predecessor, successor, movementMask, layer) && movementCost(directDx, directDy) <= throughCost)
        return false;

    for (NavigationDirection8 firstDirection : NavigationDirectionOrder)
    {
        const NavigationDirectionDelta first = directionDelta(firstDirection);
        const NavigationGridCoordinate middle{predecessor.x + first.x, predecessor.y + first.y};
        if (middle == coordinate || !legalStep(grid, predecessor, middle, movementMask, layer))
            continue;
        const int32_t secondDx = successor.x - middle.x;
        const int32_t secondDy = successor.y - middle.y;
        if (secondDx < -1 || secondDx > 1 || secondDy < -1 || secondDy > 1 || (secondDx == 0 && secondDy == 0) ||
            !legalStep(grid, middle, successor, movementMask, layer))
            continue;
        if (movementCost(first.x, first.y) + movementCost(secondDx, secondDy) <= throughCost)
            return false;
    }
    return true;
}

[[nodiscard]] inline bool hasForcedNeighbor(const NavigationGrid& grid,
                                            NavigationCellId current,
                                            NavigationDirection8 travelDirection,
                                            NavigationMovementMask movementMask,
                                            NavigationLayerId layer) noexcept
{
    for (NavigationDirection8 candidate : NavigationDirectionOrder)
    {
        if (isForcedNeighbor(grid, current, travelDirection, candidate, movementMask, layer))
            return true;
    }
    return false;
}
} // namespace jps_detail

// Eight independent, contiguous SoA columns. Positive distance reaches the
// first forced jump point; negative distance reaches the last legal cell before
// an obstruction; zero means no legal edge in that direction. The goal remains
// request-specific and is intercepted before the stored distance is consumed.
class JumpPointPrecompute final
{
public:
    [[nodiscard]] JumpPointPrecomputeResult initialize(size_t cellCapacity)
    {
        if (cellCapacity == 0 || cellCapacity >= std::numeric_limits<uint32_t>::max())
            return JumpPointPrecomputeResult::InvalidCapacity;
        for (container::Vector<int32_t>& column : m_distance)
            column.assign(cellCapacity, 0);
        clearBinding();
        return JumpPointPrecomputeResult::Success;
    }

    [[nodiscard]] JumpPointPrecomputeResult build(const NavigationGrid& grid,
                                                  NavigationProfileId profile,
                                                  NavigationMovementMask movementMask,
                                                  NavigationLayerId layer,
                                                  const JpsSearchContext& context) noexcept
    {
        clearBinding();
        if (!grid.isInitialized())
            return JumpPointPrecomputeResult::InvalidGrid;
        if (!profile || movementMask == 0 || !layer || !context.navigationRevision)
            return JumpPointPrecomputeResult::InvalidBinding;
        if (grid.hasWeightedTerrain())
            return JumpPointPrecomputeResult::UnsupportedWeightedTerrain;
        if (!context.singleLayer)
            return JumpPointPrecomputeResult::UnsupportedMultilayer;
        if (context.portalTraversal)
            return JumpPointPrecomputeResult::UnsupportedPortalTraversal;
        if (!context.topologyClean)
            return JumpPointPrecomputeResult::DirtyTopology;
        if (grid.cellCount() > capacity())
            return JumpPointPrecomputeResult::CapacityExceeded;

        for (size_t directionIndex = 0; directionIndex < NavigationDirectionOrder.size(); ++directionIndex)
        {
            const NavigationDirection8 direction = NavigationDirectionOrder[directionIndex];
            const NavigationDirectionDelta delta = directionDelta(direction);
            container::Vector<int32_t>& column = m_distance[directionIndex];
            for (size_t originIndex = 0; originIndex < grid.cellCount(); ++originIndex)
            {
                const NavigationCellId origin{static_cast<uint32_t>(originIndex)};
                column[originIndex] = 0;
                if (!grid.traversable(origin, movementMask, layer))
                    continue;
                NavigationCellId cursor = origin;
                NavigationGridCoordinate coordinate = grid.coordinate(origin);
                int32_t distance = 0;
                while (distance < std::numeric_limits<int32_t>::max())
                {
                    const NavigationCellId next = grid.cellId({coordinate.x + delta.x, coordinate.y + delta.y});
                    if (!next || !grid.traversableEdge(cursor, next, movementMask, layer))
                        break;
                    cursor = next;
                    coordinate.x += delta.x;
                    coordinate.y += delta.y;
                    ++distance;
                    if (jps_detail::hasForcedNeighbor(grid, cursor, direction, movementMask, layer))
                    {
                        column[originIndex] = distance;
                        break;
                    }
                }
                if (column[originIndex] == 0 && distance != 0)
                    column[originIndex] = -distance;
            }
        }

        m_profile = profile;
        m_movementMask = movementMask;
        m_layer = layer;
        m_navigationRevision = context.navigationRevision;
        const uint64_t builtGridHash = grid.stableHash();
        if (context.gridHash == 0 || context.gridHash != builtGridHash)
        {
            clearBinding();
            return JumpPointPrecomputeResult::InvalidBinding;
        }
        m_gridHash = context.gridHash;
        m_width = grid.width();
        m_height = grid.height();
        m_cellCount = grid.cellCount();
        m_stableHash = computeStableHash();
        m_built = true;
        return JumpPointPrecomputeResult::Success;
    }

    [[nodiscard]] bool isBuilt() const noexcept
    {
        return m_built;
    }
    [[nodiscard]] size_t capacity() const noexcept
    {
        return m_distance[0].size();
    }
    [[nodiscard]] size_t cellCount() const noexcept
    {
        return m_cellCount;
    }
    [[nodiscard]] NavigationProfileId profile() const noexcept
    {
        return m_profile;
    }
    [[nodiscard]] NavigationMovementMask movementMask() const noexcept
    {
        return m_movementMask;
    }
    [[nodiscard]] NavigationLayerId layer() const noexcept
    {
        return m_layer;
    }
    [[nodiscard]] NavigationRevision navigationRevision() const noexcept
    {
        return m_navigationRevision;
    }
    [[nodiscard]] uint64_t gridHash() const noexcept
    {
        return m_gridHash;
    }
    [[nodiscard]] uint64_t stableHash() const noexcept
    {
        return m_built ? m_stableHash : 0;
    }

    [[nodiscard]] container::Span<const int32_t> distances(NavigationDirection8 direction) const noexcept
    {
        if (!m_built)
            return {};
        return {m_distance[static_cast<uint8_t>(direction)].data(), m_cellCount};
    }

    [[nodiscard]] int32_t distance(NavigationCellId cell, NavigationDirection8 direction) const noexcept
    {
        return m_built && cell && static_cast<size_t>(cell.value) < m_cellCount
                   ? m_distance[static_cast<uint8_t>(direction)][cell.value]
                   : 0;
    }

    [[nodiscard]] bool matches(const NavigationGrid& grid,
                               const NavigationSearchRequest& request,
                               NavigationRevision revision) const noexcept
    {
        return m_built && revision && revision == m_navigationRevision && request.profile == m_profile &&
               request.movementMask == m_movementMask && request.layer == m_layer && grid.width() == m_width &&
               grid.height() == m_height && grid.cellCount() == m_cellCount && m_gridHash != 0;
    }

private:
    void clearBinding() noexcept
    {
        m_profile = InvalidNavigationProfile;
        m_movementMask = 0;
        m_layer = InvalidNavigationLayer;
        m_navigationRevision = InvalidNavigationRevision;
        m_gridHash = 0;
        m_width = 0;
        m_height = 0;
        m_cellCount = 0;
        m_stableHash = 0;
        m_built = false;
    }

    template <typename Unsigned>
    static void feed(uint64_t& hash, Unsigned value) noexcept
    {
        static_assert(std::is_unsigned_v<Unsigned>);
        uint64_t remaining = static_cast<uint64_t>(value);
        for (size_t byte = 0; byte < sizeof(Unsigned); ++byte)
        {
            hash ^= static_cast<uint8_t>(remaining & 0xffU);
            hash *= 1099511628211ULL;
            remaining >>= 8U;
        }
    }

    [[nodiscard]] uint64_t computeStableHash() const noexcept
    {
        uint64_t hash = 14695981039346656037ULL;
        feed(hash, HashSchemaVersion);
        feed(hash, m_profile.value);
        feed(hash, m_movementMask);
        feed(hash, m_layer.value);
        feed(hash, m_navigationRevision.value);
        feed(hash, m_gridHash);
        feed(hash, m_width);
        feed(hash, m_height);
        for (size_t direction = 0; direction < m_distance.size(); ++direction)
        {
            feed(hash, static_cast<uint8_t>(direction));
            for (size_t cell = 0; cell < m_cellCount; ++cell)
                feed(hash, static_cast<uint32_t>(m_distance[direction][cell]));
        }
        return hash;
    }

    inline static constexpr uint32_t HashSchemaVersion = 1;
    container::Array<container::Vector<int32_t>, 8> m_distance;
    NavigationProfileId m_profile = InvalidNavigationProfile;
    NavigationMovementMask m_movementMask = 0;
    NavigationLayerId m_layer = InvalidNavigationLayer;
    NavigationRevision m_navigationRevision = InvalidNavigationRevision;
    uint64_t m_gridHash = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    size_t m_cellCount = 0;
    uint64_t m_stableHash = 0;
    bool m_built = false;
};

} // namespace engine::navigation
