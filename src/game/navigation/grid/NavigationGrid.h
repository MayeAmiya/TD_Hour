#pragma once

#include "NavigationTypes.h"

#include "core/container/container_types.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace engine::navigation
{

struct NavigationCellValue final
{
    NavigationPassability passability = NavigationPassability::Blocked;
    // Additive cost paid when entering this cell. Zero is neutral.
    uint16_t terrainCost = 0;
    NavigationMovementMask movementMask = 0;
    NavigationLayerId layer;
    uint32_t zone = 0;
    NavigationPortalId portal;
    // Authoritative Q32.32 world-space elevation for this navigation cell.
    int64_t heightRaw = 0;
    // Derived from the composed static + dynamic obstruction field.  Authored
    // ingestion does not set this value; topology publication rebuilds it.
    NavigationClearanceMask clearanceMask = NavigationClearance::All;
    constexpr bool operator==(const NavigationCellValue&) const noexcept = default;
};

enum class NavigationGridResult : uint8_t
{
    Success,
    InvalidDimensions,
    InvalidTransform,
    InvalidCell,
    InvalidLayer,
};

// Field-level SoA grid. Allocation is performed only by initialize(); reads,
// writes, hashing and search traversal never allocate per cell.
class NavigationGrid final
{
public:
    [[nodiscard]] NavigationGridResult initialize(uint32_t width,
                                                  uint32_t height,
                                                  NavigationGridTransform transform,
                                                  const NavigationCellValue& defaultValue)
    {
        if (width == 0 || height == 0 || width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
            height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
            return NavigationGridResult::InvalidDimensions;
        const uint64_t count64 = static_cast<uint64_t>(width) * height;
        if (count64 >= std::numeric_limits<uint32_t>::max() || count64 > std::numeric_limits<size_t>::max())
            return NavigationGridResult::InvalidDimensions;
        if (transform.cellSizeRaw <= 0)
            return NavigationGridResult::InvalidTransform;
        if (!defaultValue.layer)
            return NavigationGridResult::InvalidLayer;

        const size_t count = static_cast<size_t>(count64);
        m_passability.assign(count, defaultValue.passability);
        m_terrainCost.assign(count, defaultValue.terrainCost);
        m_movementMask.assign(count, defaultValue.movementMask);
        m_layer.assign(count, defaultValue.layer);
        m_zone.assign(count, defaultValue.zone);
        m_portal.assign(count, defaultValue.portal);
        m_heightRaw.assign(count, defaultValue.heightRaw);
        m_clearanceMask.assign(count, NavigationClearance::All);
        m_pinched.assign(count, uint8_t{0});
        m_weightedCellCount = defaultValue.terrainCost == 0 ? 0 : count;
        m_pinchedCellCount = 0;
        m_width = width;
        m_height = height;
        m_transform = transform;
        return NavigationGridResult::Success;
    }

    [[nodiscard]] bool isInitialized() const noexcept { return m_width != 0 && m_height != 0; }
    [[nodiscard]] uint32_t width() const noexcept { return m_width; }
    [[nodiscard]] uint32_t height() const noexcept { return m_height; }
    [[nodiscard]] size_t cellCount() const noexcept { return m_passability.size(); }
    [[nodiscard]] NavigationGridTransform transform() const noexcept { return m_transform; }
    [[nodiscard]] bool hasWeightedTerrain() const noexcept
    {
        return m_weightedCellCount != 0 || m_pinchedCellCount != 0;
    }

    [[nodiscard]] NavigationCellId cellId(NavigationGridCoordinate coordinate) const noexcept
    {
        return cellIdFromCoordinate(coordinate, m_width, m_height);
    }

    [[nodiscard]] NavigationGridCoordinate coordinate(NavigationCellId cell) const noexcept
    {
        return contains(cell) ? coordinateFromCellId(cell, m_width) : NavigationGridCoordinate{-1, -1};
    }

    [[nodiscard]] NavigationCellId cellAt(const NavigationWorldPosition& world) const noexcept
    {
        NavigationGridCoordinate coordinateValue;
        if (!worldAxisToCell(world.xRaw, m_transform.originXRaw, m_transform.cellSizeRaw, coordinateValue.x) ||
            !worldAxisToCell(world.yRaw, m_transform.originYRaw, m_transform.cellSizeRaw, coordinateValue.y))
            return InvalidNavigationCell;
        return cellId(coordinateValue);
    }

    // ZH uses floor(world/cell) for odd-diameter (cell-centered) units and
    // floor(world/cell + 0.5) for even-diameter (grid-line phase) units.
    [[nodiscard]] NavigationCellId cellAt(
        const NavigationWorldPosition& world,
        NavigationClearanceClass clearance) const noexcept
    {
        if (!validClearanceClass(clearance) ||
            clearanceCenterInCell(clearance))
            return validClearanceClass(clearance) ? cellAt(world)
                                                  : InvalidNavigationCell;
        NavigationWorldPosition shifted = world;
        if (!detail::checkedAdd(shifted.xRaw, m_transform.cellSizeRaw / 2,
                                shifted.xRaw) ||
            !detail::checkedAdd(shifted.yRaw, m_transform.cellSizeRaw / 2,
                                shifted.yRaw))
            return InvalidNavigationCell;
        return cellAt(shifted);
    }

    [[nodiscard]] bool cellCenter(NavigationCellId cell, NavigationWorldPosition& world) const noexcept
    {
        if (!contains(cell))
            return false;
        const NavigationGridCoordinate coordinateValue = coordinate(cell);
        if (!cellAxisCenter(coordinateValue.x, m_transform.originXRaw, m_transform.cellSizeRaw, world.xRaw) ||
            !cellAxisCenter(coordinateValue.y, m_transform.originYRaw, m_transform.cellSizeRaw, world.yRaw))
            return false;
        world.zRaw = m_heightRaw[cell.value];
        return true;
    }

    [[nodiscard]] bool cellPosition(
        NavigationCellId cell,
        NavigationClearanceClass clearance,
        NavigationWorldPosition& world) const noexcept
    {
        if (!contains(cell) || !validClearanceClass(clearance))
            return false;
        if (clearanceCenterInCell(clearance))
            return cellCenter(cell, world);
        const NavigationGridCoordinate coordinateValue = coordinate(cell);
        int64_t xOffset = 0;
        int64_t yOffset = 0;
        int64_t xEdge = 0;
        int64_t yEdge = 0;
        if (!detail::checkedMultiply(static_cast<int64_t>(coordinateValue.x),
                                     m_transform.cellSizeRaw, xOffset) ||
            !detail::checkedMultiply(static_cast<int64_t>(coordinateValue.y),
                                     m_transform.cellSizeRaw, yOffset) ||
            !detail::checkedAdd(m_transform.originXRaw, xOffset, xEdge) ||
            !detail::checkedAdd(m_transform.originYRaw, yOffset, yEdge) ||
            !detail::checkedAdd(xEdge, m_transform.cellSizeRaw / 20,
                                world.xRaw) ||
            !detail::checkedAdd(yEdge, m_transform.cellSizeRaw / 20,
                                world.yRaw))
            return false;
        world.zRaw = m_heightRaw[cell.value];
        return true;
    }

    [[nodiscard]] NavigationGridResult setCell(NavigationCellId cell, const NavigationCellValue& value) noexcept
    {
        if (!contains(cell))
            return NavigationGridResult::InvalidCell;
        if (!value.layer)
            return NavigationGridResult::InvalidLayer;
        const size_t index = cell.value;
        if (m_terrainCost[index] != 0)
            --m_weightedCellCount;
        if (value.terrainCost != 0)
            ++m_weightedCellCount;
        m_passability[index] = value.passability;
        m_terrainCost[index] = value.terrainCost;
        m_movementMask[index] = value.movementMask;
        m_layer[index] = value.layer;
        m_zone[index] = value.zone;
        m_portal[index] = value.portal;
        m_heightRaw[index] = value.heightRaw;
        m_clearanceMask[index] = value.clearanceMask;
        if (m_pinched[index] != 0) --m_pinchedCellCount;
        m_pinched[index] = 0;
        return NavigationGridResult::Success;
    }

    [[nodiscard]] NavigationCellValue cell(NavigationCellId id) const noexcept
    {
        if (!contains(id))
            return {};
        const size_t index = id.value;
        return {m_passability[index], m_terrainCost[index], m_movementMask[index],
                m_layer[index], m_zone[index], m_portal[index], m_heightRaw[index],
                m_clearanceMask[index]};
    }

    [[nodiscard]] bool contains(NavigationCellId cell) const noexcept
    {
        return cell && static_cast<size_t>(cell.value) < m_passability.size();
    }

    [[nodiscard]] bool pinched(NavigationCellId cell) const noexcept
    {
        return contains(cell) && m_pinched[cell.value] != 0;
    }

    [[nodiscard]] bool traversable(NavigationCellId cell,
                                   NavigationMovementMask movementMask,
                                   NavigationLayerId layer,
                                   NavigationClearanceClass clearance =
                                       NavigationClearanceClass::Infantry) const noexcept
    {
        if (!contains(cell) || !layer || !validClearanceClass(clearance))
            return false;
        const size_t index = cell.value;
        if (m_layer[index] != layer ||
            (m_movementMask[index] & movementMask) == 0) {
            return false;
        }
        // ZH maps obstacle/impassable cells to AIR rather than removing all
        // surfaces. An accepted Air locomotor therefore ignores ground
        // obstruction and ground-unit clearance while still remaining on its
        // requested navigation layer.
        if ((movementMask & NavigationMovement::Air) != 0 &&
            (m_movementMask[index] & NavigationMovement::Air) != 0) {
            return true;
        }
        return m_passability[index] == NavigationPassability::Traversable &&
               (m_clearanceMask[index] & clearanceBit(clearance)) != 0;
    }

    // RefCode rejects a diagonal only when both adjacent cardinal cells are
    // closed. One open side is sufficient for the actor-radius grid phase.
    [[nodiscard]] bool traversableEdge(NavigationCellId from,
                                       NavigationCellId to,
                                       NavigationMovementMask movementMask,
                                       NavigationLayerId layer,
                                       NavigationClearanceClass clearance =
                                           NavigationClearanceClass::Infantry) const noexcept
    {
        if (!traversable(from, movementMask, layer, clearance) ||
            !traversable(to, movementMask, layer, clearance))
            return false;
        const NavigationGridCoordinate fromCoordinate = coordinate(from);
        const NavigationGridCoordinate toCoordinate = coordinate(to);
        const int32_t dx = toCoordinate.x - fromCoordinate.x;
        const int32_t dy = toCoordinate.y - fromCoordinate.y;
        if (dx < -1 || dx > 1 || dy < -1 || dy > 1 || (dx == 0 && dy == 0))
            return false;
        if (dx == 0 || dy == 0)
            return true;
        return traversable(cellId({fromCoordinate.x + dx, fromCoordinate.y}),
                           movementMask, layer, clearance) ||
               traversable(cellId({fromCoordinate.x, fromCoordinate.y + dy}),
                           movementMask, layer, clearance);
    }

    // Rebuild only the destination cells in the supplied inclusive region.
    // Source obstruction samples extend by at most two cells, so callers expand
    // a topology dirty region by MaximumRadiusCells and avoid a whole-map pass.
    [[nodiscard]] bool rebuildClearanceCell(NavigationCellId candidate) noexcept
    {
        if (!isInitialized() || !contains(candidate))
            return false;
        const NavigationGridCoordinate coordinateValue = coordinate(candidate);
        const int32_t x = coordinateValue.x;
        const int32_t y = coordinateValue.y;
        NavigationClearanceMask mask = 0;
        if (m_passability[candidate.value] == NavigationPassability::Traversable)
        {
            bool centered1x1 = true;
            bool offset2x2 = true;
            bool centered3x3 = true;
            bool offset4x4 = true;
            bool centered5x5 = true;
            for (int32_t dy = -2; dy <= 2; ++dy)
            {
                for (int32_t dx = -2; dx <= 2; ++dx)
                {
                    const NavigationCellId sample = cellId({x + dx, y + dy});
                    const bool blocked = !sample ||
                        m_passability[sample.value] != NavigationPassability::Traversable;
                    if (!blocked)
                        continue;
                    if (dx == 0 && dy == 0)
                        centered1x1 = false;
                    if (dx >= -1 && dx <= 0 && dy >= -1 && dy <= 0)
                        offset2x2 = false;
                    if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1)
                        centered3x3 = false;
                    if (dx >= -2 && dx <= 1 && dy >= -2 && dy <= 1)
                        offset4x4 = false;
                    centered5x5 = false;
                }
            }
            if (centered1x1) mask |= NavigationClearance::Centered1x1;
            if (offset2x2) mask |= NavigationClearance::Offset2x2;
            if (centered3x3) mask |= NavigationClearance::Centered3x3;
            if (offset4x4) mask |= NavigationClearance::Offset4x4;
            if (centered5x5) mask |= NavigationClearance::Centered5x5;
        }
        m_clearanceMask[candidate.value] = mask;

        const auto clearCell = [this](NavigationCellId cell) noexcept {
            if (!contains(cell) ||
                m_passability[cell.value] !=
                    NavigationPassability::Traversable) {
                return false;
            }
            const NavigationMovementMask movement =
                m_movementMask[cell.value];
            return (movement & NavigationMovement::Ground) != 0 &&
                (movement & (NavigationMovement::Cliff |
                             NavigationMovement::Water |
                             NavigationMovement::Rubble)) == 0;
        };
        bool pinched = false;
        if (clearCell(candidate)) {
            uint32_t totalClear = 0;
            uint32_t orthogonalClear = 0;
            bool adjacentCliff = false;
            for (int32_t offsetY = -1; offsetY <= 1; ++offsetY) {
                for (int32_t offsetX = -1; offsetX <= 1; ++offsetX) {
                    if (offsetX == 0 && offsetY == 0) continue;
                    const NavigationCellId neighbor = cellId({
                        x + offsetX, y + offsetY});
                    if (!neighbor) continue;
                    if (clearCell(neighbor)) {
                        ++totalClear;
                        if (offsetX == 0 || offsetY == 0)
                            ++orthogonalClear;
                    }
                    if ((m_movementMask[neighbor.value] &
                         NavigationMovement::Cliff) != 0) {
                        adjacentCliff = true;
                    }
                }
            }
            const bool topologicallyClosed =
                orthogonalClear < 2 || totalClear < 4;
            if (topologicallyClosed) {
                // Retail Pathfinder converts this first pinched pass from
                // CELL_CLEAR to CELL_IMPASSABLE, then clears the pinched bit.
                // Preserve the authored surface but remove every clearance
                // class so all traversal queries observe the same closure.
                m_clearanceMask[candidate.value] = 0;
            } else {
                // The later cliff/obstacle-adjacency pass retains a pinched
                // marker and charges an additional traversal cost.
                pinched = adjacentCliff;
            }
        }
        const bool wasPinched = m_pinched[candidate.value] != 0;
        if (pinched != wasPinched) {
            if (pinched)
                ++m_pinchedCellCount;
            else
                --m_pinchedCellCount;
        }
        m_pinched[candidate.value] = pinched ? uint8_t{1} : uint8_t{0};
        return true;
    }

    [[nodiscard]] bool rebuildClearanceRegion(int32_t minX,
                                              int32_t minY,
                                              int32_t maxX,
                                              int32_t maxY) noexcept
    {
        if (!isInitialized() || minX < 0 || minY < 0 || maxX < minX || maxY < minY ||
            maxX >= static_cast<int32_t>(m_width) || maxY >= static_cast<int32_t>(m_height))
            return false;

        for (int32_t y = minY; y <= maxY; ++y)
        {
            for (int32_t x = minX; x <= maxX; ++x)
            {
                if (!rebuildClearanceCell(cellId({x, y})))
                    return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool rebuildClearance() noexcept
    {
        return isInitialized() && rebuildClearanceRegion(
            0, 0, static_cast<int32_t>(m_width) - 1,
            static_cast<int32_t>(m_height) - 1);
    }

    [[nodiscard]] container::Span<const NavigationPassability> passability() const noexcept { return m_passability; }
    [[nodiscard]] container::Span<const uint16_t> terrainCost() const noexcept { return m_terrainCost; }
    [[nodiscard]] container::Span<const NavigationMovementMask> movementMask() const noexcept { return m_movementMask; }
    [[nodiscard]] container::Span<const NavigationLayerId> layer() const noexcept { return m_layer; }
    [[nodiscard]] container::Span<const uint32_t> zone() const noexcept { return m_zone; }
    [[nodiscard]] container::Span<const NavigationPortalId> portal() const noexcept { return m_portal; }
    [[nodiscard]] container::Span<const int64_t> heightRaw() const noexcept { return m_heightRaw; }
    [[nodiscard]] container::Span<const NavigationClearanceMask> clearanceMask() const noexcept
    {
        return m_clearanceMask;
    }
    [[nodiscard]] container::Span<const uint8_t> pinchedCells() const noexcept
    {
        return m_pinched;
    }

    [[nodiscard]] uint64_t stableHash() const noexcept
    {
        uint64_t hash = 14695981039346656037ULL;
        feed(hash, HashSchemaVersion);
        feed(hash, m_width);
        feed(hash, m_height);
        feed(hash, static_cast<uint64_t>(m_transform.originXRaw));
        feed(hash, static_cast<uint64_t>(m_transform.originYRaw));
        feed(hash, static_cast<uint64_t>(m_transform.cellSizeRaw));
        for (size_t i = 0; i < cellCount(); ++i)
        {
            feed(hash, static_cast<uint8_t>(m_passability[i]));
            feed(hash, m_terrainCost[i]);
            feed(hash, m_movementMask[i]);
            feed(hash, m_layer[i].value);
            feed(hash, m_zone[i]);
            feed(hash, m_portal[i].value);
            feed(hash, static_cast<uint64_t>(m_heightRaw[i]));
            feed(hash, m_clearanceMask[i]);
            feed(hash, m_pinched[i]);
        }
        return hash;
    }

private:
    inline static constexpr uint32_t HashSchemaVersion = 5;

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

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    NavigationGridTransform m_transform;
    container::Vector<NavigationPassability> m_passability;
    container::Vector<uint16_t> m_terrainCost;
    container::Vector<NavigationMovementMask> m_movementMask;
    container::Vector<NavigationLayerId> m_layer;
    container::Vector<uint32_t> m_zone;
    container::Vector<NavigationPortalId> m_portal;
    container::Vector<int64_t> m_heightRaw;
    container::Vector<NavigationClearanceMask> m_clearanceMask;
    container::Vector<uint8_t> m_pinched;
    size_t m_weightedCellCount = 0;
    size_t m_pinchedCellCount = 0;
};

} // namespace engine::navigation
