#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "NavigationGrid.h"
#include "core/container/container_types.h"

namespace engine::navigation
{

struct NavigationZoneId final
{
    uint32_t value = 0;
    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return value != 0;
    }
    explicit constexpr operator bool() const noexcept
    {
        return isValid();
    }
    constexpr auto operator<=>(const NavigationZoneId&) const noexcept = default;
};

inline constexpr NavigationZoneId InvalidNavigationZone{};

enum class NavigationZoneBuildResult : uint8_t
{
    Success = 0,
    InvalidCapacity,
    InvalidGrid,
    InvalidProfile,
    InvalidMovementMask,
    InvalidLayer,
    CapacityExceeded,
};

enum class NavigationZoneRelation : uint8_t
{
    Unzoned = 0,
    SameZone,
    DifferentZone,
};

// A completed field is an immutable value snapshot for one movement profile
// and one layer. It owns no grid pointer or other live-object reference.
class NavigationZoneField final
{
public:
    [[nodiscard]] NavigationZoneBuildResult initialize(size_t cellCapacity)
    {
        if (cellCapacity == 0 || cellCapacity >= std::numeric_limits<uint32_t>::max())
            return NavigationZoneBuildResult::InvalidCapacity;
        m_zoneByCell.assign(cellCapacity, InvalidNavigationZone);
        m_pendingZoneByCell.assign(cellCapacity, InvalidNavigationZone);
        clearBinding();
        return NavigationZoneBuildResult::Success;
    }

    [[nodiscard]] NavigationZoneBuildResult beginBuild(
        const NavigationGrid& grid,
        NavigationProfileId profile,
        NavigationMovementMask movementMask,
        NavigationLayerId layer,
        NavigationClearanceClass clearance =
            NavigationClearanceClass::Infantry) noexcept
    {
        if (!grid.isInitialized())
            return NavigationZoneBuildResult::InvalidGrid;
        if (!profile)
            return NavigationZoneBuildResult::InvalidProfile;
        if (movementMask == 0)
            return NavigationZoneBuildResult::InvalidMovementMask;
        if (!layer || !validClearanceClass(clearance))
            return !layer ? NavigationZoneBuildResult::InvalidLayer
                          : NavigationZoneBuildResult::InvalidProfile;
        if (grid.cellCount() > m_zoneByCell.size() ||
            grid.cellCount() > m_pendingZoneByCell.size())
            return NavigationZoneBuildResult::CapacityExceeded;

        m_profile = profile;
        m_movementMask = movementMask;
        m_layer = layer;
        m_clearance = clearance;
        m_width = grid.width();
        m_height = grid.height();
        m_cellCount = grid.cellCount();
        std::fill_n(m_pendingZoneByCell.begin(), m_cellCount,
                    InvalidNavigationZone);
        m_buildSeedIndex = 0;
        m_buildReadIndex = 0;
        m_buildWriteIndex = 0;
        m_buildZoneCount = 0;
        m_building = true;
        return NavigationZoneBuildResult::Success;
    }

    [[nodiscard]] NavigationZoneBuildResult stepBuild(
        const NavigationGrid& grid,
        container::Span<NavigationCellId> frontier,
        size_t workBudget,
        size_t& workConsumed) noexcept
    {
        workConsumed = 0;
        if (!m_building)
            return NavigationZoneBuildResult::Success;
        if (!grid.isInitialized() || grid.width() != m_width ||
            grid.height() != m_height || grid.cellCount() != m_cellCount ||
            frontier.size() < m_cellCount)
            return NavigationZoneBuildResult::InvalidGrid;

        while (m_building && workConsumed < workBudget)
        {
            if (m_buildReadIndex < m_buildWriteIndex)
            {
                const NavigationCellId current = frontier[m_buildReadIndex++];
                ++workConsumed;
                const NavigationGridCoordinate coordinateValue =
                    grid.coordinate(current);
                for (NavigationDirection8 direction : NavigationDirectionOrder)
                {
                    const NavigationDirectionDelta delta = directionDelta(direction);
                    const NavigationCellId neighbor = grid.cellId({
                        coordinateValue.x + delta.x,
                        coordinateValue.y + delta.y});
                    if (!neighbor ||
                        m_pendingZoneByCell[neighbor.value] ||
                        !grid.traversableEdge(current, neighbor,
                                              m_movementMask, m_layer,
                                              m_clearance))
                        continue;
                    m_pendingZoneByCell[neighbor.value] =
                        NavigationZoneId{m_buildZoneCount};
                    frontier[m_buildWriteIndex++] = neighbor;
                }
                continue;
            }

            if (m_buildSeedIndex >= m_cellCount)
            {
                m_zoneCount = m_buildZoneCount;
                m_pendingZoneByCell.swap(m_zoneByCell);
                m_stableHash = computeStableHash();
                m_built = true;
                m_building = false;
                continue;
            }

            const size_t seedIndex = m_buildSeedIndex++;
            ++workConsumed;
            const NavigationCellId seed{static_cast<uint32_t>(seedIndex)};
            if (m_pendingZoneByCell[seedIndex] ||
                !grid.traversable(seed, m_movementMask, m_layer,
                                  m_clearance))
                continue;
            ++m_buildZoneCount;
            m_pendingZoneByCell[seedIndex] =
                NavigationZoneId{m_buildZoneCount};
            m_buildReadIndex = 0;
            m_buildWriteIndex = 1;
            frontier[0] = seed;
        }
        return NavigationZoneBuildResult::Success;
    }

    [[nodiscard]] bool isBuildInProgress() const noexcept
    {
        return m_building;
    }

    // build() performs no allocation. initialize() may reserve more cells than
    // the current grid so the same field can be rebuilt within that capacity.
    [[nodiscard]] NavigationZoneBuildResult build(const NavigationGrid& grid,
                                                  NavigationProfileId profile,
                                                  NavigationMovementMask movementMask,
                                                  NavigationLayerId layer,
                                                  container::Span<NavigationCellId> frontier,
                                                  NavigationClearanceClass clearance =
                                                      NavigationClearanceClass::Infantry) noexcept
    {
        const NavigationZoneBuildResult started =
            beginBuild(grid, profile, movementMask, layer, clearance);
        if (started != NavigationZoneBuildResult::Success)
            return started;
        while (m_building)
        {
            size_t consumed = 0;
            const NavigationZoneBuildResult result =
                stepBuild(grid, frontier, std::numeric_limits<size_t>::max(),
                          consumed);
            if (result != NavigationZoneBuildResult::Success)
                return result;
        }
        return NavigationZoneBuildResult::Success;
    }

    [[nodiscard]] bool isBuilt() const noexcept
    {
        return m_built;
    }
    [[nodiscard]] size_t capacity() const noexcept
    {
        return m_zoneByCell.size();
    }
    [[nodiscard]] size_t cellCount() const noexcept
    {
        return m_cellCount;
    }
    [[nodiscard]] uint32_t width() const noexcept
    {
        return m_width;
    }
    [[nodiscard]] uint32_t height() const noexcept
    {
        return m_height;
    }
    [[nodiscard]] uint32_t zoneCount() const noexcept
    {
        return m_zoneCount;
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
    [[nodiscard]] NavigationClearanceClass clearanceClass() const noexcept
    {
        return m_clearance;
    }

    [[nodiscard]] NavigationZoneId zone(NavigationCellId cell) const noexcept
    {
        return contains(cell) ? m_zoneByCell[cell.value] : InvalidNavigationZone;
    }

    [[nodiscard]] NavigationZoneRelation relation(NavigationCellId first, NavigationCellId second) const noexcept
    {
        const NavigationZoneId firstZone = zone(first);
        const NavigationZoneId secondZone = zone(second);
        if (!firstZone || !secondZone)
            return NavigationZoneRelation::Unzoned;
        return firstZone == secondZone ? NavigationZoneRelation::SameZone : NavigationZoneRelation::DifferentZone;
    }

    [[nodiscard]] bool sameZone(NavigationCellId first, NavigationCellId second) const noexcept
    {
        return relation(first, second) == NavigationZoneRelation::SameZone;
    }

    [[nodiscard]] bool differentZone(NavigationCellId first, NavigationCellId second) const noexcept
    {
        return relation(first, second) == NavigationZoneRelation::DifferentZone;
    }

    [[nodiscard]] container::Span<const NavigationZoneId> zones() const noexcept
    {
        return m_built ? container::Span<const NavigationZoneId>(m_zoneByCell.data(), m_cellCount)
                       : container::Span<const NavigationZoneId>();
    }

    [[nodiscard]] uint64_t stableHash() const noexcept
    {
        return m_built ? m_stableHash : 0;
    }
    [[nodiscard]] uint64_t zoneHash() const noexcept
    {
        return stableHash();
    }

    // Hash only the not-yet-published BFS work. The active zone map remains
    // readable while this value is being built, so the normal stableHash()
    // intentionally continues to describe the published map.
    [[nodiscard]] uint64_t buildStableHash() const noexcept
    {
        if (!m_building)
            return 0;
        uint64_t hash = 14695981039346656037ULL;
        feed(hash, BuildHashSchemaVersion);
        feed(hash, static_cast<uint8_t>(m_building));
        feed(hash, static_cast<uint64_t>(m_buildSeedIndex));
        feed(hash, static_cast<uint64_t>(m_buildReadIndex));
        feed(hash, static_cast<uint64_t>(m_buildWriteIndex));
        feed(hash, m_buildZoneCount);
        for (size_t index = 0; index < m_cellCount; ++index)
            feed(hash, m_pendingZoneByCell[index].value);
        return hash;
    }

private:
    [[nodiscard]] bool contains(NavigationCellId cell) const noexcept
    {
        return m_built && cell && static_cast<size_t>(cell.value) < m_cellCount;
    }

    void clearBinding() noexcept
    {
        m_profile = InvalidNavigationProfile;
        m_movementMask = 0;
        m_layer = InvalidNavigationLayer;
        m_clearance = NavigationClearanceClass::Infantry;
        m_width = 0;
        m_height = 0;
        m_cellCount = 0;
        m_zoneCount = 0;
        m_stableHash = 0;
        m_built = false;
        m_buildSeedIndex = 0;
        m_buildReadIndex = 0;
        m_buildWriteIndex = 0;
        m_buildZoneCount = 0;
        m_building = false;
    }

    [[nodiscard]] uint64_t computeStableHash() const noexcept
    {
        uint64_t hash = 14695981039346656037ULL;
        feed(hash, HashSchemaVersion);
        feed(hash, m_profile.value);
        feed(hash, m_movementMask);
        feed(hash, m_layer.value);
        feed(hash, static_cast<uint8_t>(m_clearance));
        feed(hash, m_width);
        feed(hash, m_height);
        feed(hash, m_zoneCount);
        for (size_t index = 0; index < m_cellCount; ++index)
            feed(hash, m_zoneByCell[index].value);
        return hash;
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

    inline static constexpr uint32_t HashSchemaVersion = 3;
    inline static constexpr uint32_t BuildHashSchemaVersion = 1;
    container::Vector<NavigationZoneId> m_zoneByCell;
    container::Vector<NavigationZoneId> m_pendingZoneByCell;
    NavigationProfileId m_profile = InvalidNavigationProfile;
    NavigationMovementMask m_movementMask = 0;
    NavigationLayerId m_layer = InvalidNavigationLayer;
    NavigationClearanceClass m_clearance = NavigationClearanceClass::Infantry;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    size_t m_cellCount = 0;
    uint32_t m_zoneCount = 0;
    uint64_t m_stableHash = 0;
    bool m_built = false;
    size_t m_buildSeedIndex = 0;
    size_t m_buildReadIndex = 0;
    size_t m_buildWriteIndex = 0;
    uint32_t m_buildZoneCount = 0;
    bool m_building = false;
};

} // namespace engine::navigation
