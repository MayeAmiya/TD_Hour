#pragma once

#include "NavigationSearchScratch.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace engine::navigation
{

enum class NavigationOpenHeapResult : uint8_t
{
    Success = 0,
    Empty,
    CapacityExhausted,
    InvalidCell,
    AlreadyPresent,
    NotPresent,
    KeyNotImproved,
};

struct NavigationOpenHeapEntry final
{
    NavigationCellId cell = InvalidNavigationCell;
    uint32_t fCost = NavigationSearchScratch::InfiniteCost;
    uint32_t hCost = NavigationSearchScratch::InfiniteCost;
    constexpr bool operator==(const NavigationOpenHeapEntry&) const noexcept = default;
};

// Indexed binary min-heap backed entirely by NavigationSearchScratch. Active
// entries are unique by cell and ordered lexicographically by
// (fCost, hCost, cellId), independently of insertion order.
class NavigationOpenHeap final
{
public:
    explicit NavigationOpenHeap(NavigationSearchScratch& scratch) noexcept : m_scratch(scratch) {}

    [[nodiscard]] size_t size() const noexcept { return m_scratch.heapSize(); }
    [[nodiscard]] size_t capacity() const noexcept { return m_scratch.heapCapacity(); }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    [[nodiscard]] bool contains(NavigationCellId cell) const noexcept
    {
        if (!m_scratch.contains(cell))
            return false;
        const uint32_t position = m_scratch.heapPosition(cell);
        return static_cast<size_t>(position) < size() && m_scratch.heapCell(position) == cell;
    }

    [[nodiscard]] NavigationOpenHeapResult push(NavigationCellId cell,
                                                 uint32_t fCost,
                                                 uint32_t hCost) noexcept
    {
        if (!m_scratch.contains(cell))
            return NavigationOpenHeapResult::InvalidCell;
        if (contains(cell))
            return NavigationOpenHeapResult::AlreadyPresent;
        if (size() == capacity())
            return NavigationOpenHeapResult::CapacityExhausted;

        const size_t position = size();
        m_scratch.heapCell(position) = cell;
        m_scratch.heapFCost(position) = fCost;
        m_scratch.heapHCost(position) = hCost;
        m_scratch.setHeapSize(position + 1);
        m_scratch.setHeapPosition(cell, static_cast<uint32_t>(position));
        siftUp(position);
        return NavigationOpenHeapResult::Success;
    }

    [[nodiscard]] NavigationOpenHeapResult decreaseKey(NavigationCellId cell,
                                                        uint32_t fCost,
                                                        uint32_t hCost) noexcept
    {
        if (!m_scratch.contains(cell))
            return NavigationOpenHeapResult::InvalidCell;
        if (!contains(cell))
            return NavigationOpenHeapResult::NotPresent;

        const size_t position = m_scratch.heapPosition(cell);
        if (!keyLess(fCost,
                     hCost,
                     cell,
                     m_scratch.heapFCost(position),
                     m_scratch.heapHCost(position),
                     cell))
            return NavigationOpenHeapResult::KeyNotImproved;

        m_scratch.heapFCost(position) = fCost;
        m_scratch.heapHCost(position) = hCost;
        siftUp(position);
        return NavigationOpenHeapResult::Success;
    }

    [[nodiscard]] NavigationOpenHeapResult popMin(NavigationOpenHeapEntry& entry) noexcept
    {
        if (empty())
            return NavigationOpenHeapResult::Empty;

        entry = {m_scratch.heapCell(0), m_scratch.heapFCost(0), m_scratch.heapHCost(0)};
        m_scratch.setHeapPosition(entry.cell, NavigationSearchScratch::InvalidHeapPosition);

        const size_t last = size() - 1;
        if (last != 0)
        {
            m_scratch.heapCell(0) = m_scratch.heapCell(last);
            m_scratch.heapFCost(0) = m_scratch.heapFCost(last);
            m_scratch.heapHCost(0) = m_scratch.heapHCost(last);
            m_scratch.setHeapPosition(m_scratch.heapCell(0), 0);
        }

        clearSlot(last);
        m_scratch.setHeapSize(last);
        if (last != 0)
            siftDown(0);
        return NavigationOpenHeapResult::Success;
    }

    void clear() noexcept
    {
        for (size_t position = 0; position < size(); ++position)
        {
            m_scratch.setHeapPosition(m_scratch.heapCell(position),
                                      NavigationSearchScratch::InvalidHeapPosition);
            clearSlot(position);
        }
        m_scratch.setHeapSize(0);
    }

private:
    [[nodiscard]] static constexpr bool keyLess(uint32_t leftFCost,
                                                uint32_t leftHCost,
                                                NavigationCellId leftCell,
                                                uint32_t rightFCost,
                                                uint32_t rightHCost,
                                                NavigationCellId rightCell) noexcept
    {
        if (leftFCost != rightFCost)
            return leftFCost < rightFCost;
        if (leftHCost != rightHCost)
            return leftHCost < rightHCost;
        return leftCell.value < rightCell.value;
    }

    [[nodiscard]] bool less(size_t left, size_t right) const noexcept
    {
        return keyLess(m_scratch.heapFCost(left),
                       m_scratch.heapHCost(left),
                       m_scratch.heapCell(left),
                       m_scratch.heapFCost(right),
                       m_scratch.heapHCost(right),
                       m_scratch.heapCell(right));
    }

    void swapEntries(size_t left, size_t right) noexcept
    {
        using std::swap;
        swap(m_scratch.heapCell(left), m_scratch.heapCell(right));
        swap(m_scratch.heapFCost(left), m_scratch.heapFCost(right));
        swap(m_scratch.heapHCost(left), m_scratch.heapHCost(right));
        m_scratch.setHeapPosition(m_scratch.heapCell(left), static_cast<uint32_t>(left));
        m_scratch.setHeapPosition(m_scratch.heapCell(right), static_cast<uint32_t>(right));
    }

    void siftUp(size_t position) noexcept
    {
        while (position != 0)
        {
            const size_t parent = (position - 1) / 2;
            if (!less(position, parent))
                return;
            swapEntries(position, parent);
            position = parent;
        }
    }

    void siftDown(size_t position) noexcept
    {
        while (true)
        {
            const size_t left = position * 2 + 1;
            if (left >= size())
                return;
            const size_t right = left + 1;
            const size_t best = right < size() && less(right, left) ? right : left;
            if (!less(best, position))
                return;
            swapEntries(position, best);
            position = best;
        }
    }

    void clearSlot(size_t position) noexcept
    {
        m_scratch.heapCell(position) = InvalidNavigationCell;
        m_scratch.heapFCost(position) = NavigationSearchScratch::InfiniteCost;
        m_scratch.heapHCost(position) = NavigationSearchScratch::InfiniteCost;
    }

    NavigationSearchScratch& m_scratch;
};

} // namespace engine::navigation
