#pragma once

#include "../grid/NavigationTypes.h"

#include "core/container/container_types.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace engine::navigation
{

class NavigationSearchScratch final
{
public:
    [[nodiscard]] bool initialize(size_t cellCapacity, size_t heapCapacity)
    {
        if (cellCapacity == 0 || heapCapacity == 0 ||
            cellCapacity >= std::numeric_limits<uint32_t>::max() ||
            heapCapacity > std::numeric_limits<uint32_t>::max())
            return false;
        m_gCost.assign(cellCapacity, InfiniteCost);
        m_parent.assign(cellCapacity, InvalidNavigationCell);
        m_openEpoch.assign(cellCapacity, 0);
        m_closedEpoch.assign(cellCapacity, 0);
        m_heapPosition.assign(cellCapacity, InvalidHeapPosition);
        m_heapCell.assign(heapCapacity, InvalidNavigationCell);
        m_heapFCost.assign(heapCapacity, InfiniteCost);
        m_heapHCost.assign(heapCapacity, InfiniteCost);
        m_epoch = 0;
        m_heapSize = 0;
        return true;
    }

    // This is the only whole-column clear path. It runs once per uint32 epoch
    // wrap, never once per normal search.
    [[nodiscard]] uint32_t beginSearch() noexcept
    {
        if (m_epoch == std::numeric_limits<uint32_t>::max())
        {
            std::fill(m_openEpoch.begin(), m_openEpoch.end(), 0);
            std::fill(m_closedEpoch.begin(), m_closedEpoch.end(), 0);
            m_epoch = 1;
        }
        else
        {
            ++m_epoch;
            if (m_epoch == 0)
                m_epoch = 1;
        }
        m_heapSize = 0;
        return m_epoch;
    }

    [[nodiscard]] size_t cellCapacity() const noexcept { return m_gCost.size(); }
    [[nodiscard]] size_t heapCapacity() const noexcept { return m_heapCell.size(); }
    [[nodiscard]] size_t heapSize() const noexcept { return m_heapSize; }
    [[nodiscard]] uint32_t epoch() const noexcept { return m_epoch; }

    [[nodiscard]] bool contains(NavigationCellId cell) const noexcept
    {
        return cell && static_cast<size_t>(cell.value) < cellCapacity();
    }

    [[nodiscard]] bool isOpen(NavigationCellId cell) const noexcept
    {
        return contains(cell) && m_openEpoch[cell.value] == m_epoch;
    }

    [[nodiscard]] bool isClosed(NavigationCellId cell) const noexcept
    {
        return contains(cell) && m_closedEpoch[cell.value] == m_epoch;
    }

    [[nodiscard]] uint32_t gCost(NavigationCellId cell) const noexcept
    {
        return (isOpen(cell) || isClosed(cell)) ? m_gCost[cell.value] : InfiniteCost;
    }

    [[nodiscard]] NavigationCellId parent(NavigationCellId cell) const noexcept
    {
        return (isOpen(cell) || isClosed(cell)) ? m_parent[cell.value] : InvalidNavigationCell;
    }

    void markOpen(NavigationCellId cell, uint32_t cost, NavigationCellId parentCell) noexcept
    {
        if (!contains(cell))
            return;
        m_gCost[cell.value] = cost;
        m_parent[cell.value] = parentCell;
        m_openEpoch[cell.value] = m_epoch;
        m_closedEpoch[cell.value] = 0;
    }

    void markClosed(NavigationCellId cell) noexcept
    {
        if (!contains(cell))
            return;
        m_openEpoch[cell.value] = 0;
        m_closedEpoch[cell.value] = m_epoch;
        m_heapPosition[cell.value] = InvalidHeapPosition;
    }

    [[nodiscard]] uint32_t heapPosition(NavigationCellId cell) const noexcept
    {
        return contains(cell) ? m_heapPosition[cell.value] : InvalidHeapPosition;
    }

    void setHeapPosition(NavigationCellId cell, uint32_t position) noexcept
    {
        if (contains(cell))
            m_heapPosition[cell.value] = position;
    }

    [[nodiscard]] NavigationCellId& heapCell(size_t index) noexcept { return m_heapCell[index]; }
    [[nodiscard]] const NavigationCellId& heapCell(size_t index) const noexcept { return m_heapCell[index]; }
    [[nodiscard]] uint32_t& heapFCost(size_t index) noexcept { return m_heapFCost[index]; }
    [[nodiscard]] uint32_t heapFCost(size_t index) const noexcept { return m_heapFCost[index]; }
    [[nodiscard]] uint32_t& heapHCost(size_t index) noexcept { return m_heapHCost[index]; }
    [[nodiscard]] uint32_t heapHCost(size_t index) const noexcept { return m_heapHCost[index]; }
    void setHeapSize(size_t size) noexcept { m_heapSize = size <= heapCapacity() ? size : heapCapacity(); }

    inline static constexpr uint32_t InfiniteCost = std::numeric_limits<uint32_t>::max();
    inline static constexpr uint32_t InvalidHeapPosition = std::numeric_limits<uint32_t>::max();

private:
    container::Vector<uint32_t> m_gCost;
    container::Vector<NavigationCellId> m_parent;
    container::Vector<uint32_t> m_openEpoch;
    container::Vector<uint32_t> m_closedEpoch;
    container::Vector<uint32_t> m_heapPosition;
    container::Vector<NavigationCellId> m_heapCell;
    container::Vector<uint32_t> m_heapFCost;
    container::Vector<uint32_t> m_heapHCost;
    uint32_t m_epoch = 0;
    size_t m_heapSize = 0;
};

} // namespace engine::navigation
