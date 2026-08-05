#pragma once

#include "NavigationTypes.h"

#include "core/container/container_types.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace engine::navigation
{

// Preallocated variable-length storage for retained navigation footprints.
// Allocation and release are deterministic first-fit operations and never
// allocate after initialize(). This avoids imposing one worst-case footprint
// size on every entity/event slot while preserving a hard session memory
// budget.
class NavigationCellArena final
{
public:
    struct Range final
    {
        uint32_t offset = 0;
        uint32_t count = 0;
        constexpr bool operator==(const Range&) const noexcept = default;
    };

    [[nodiscard]] bool initialize(uint64_t cellCapacity,
                                  uint32_t allocationCapacity)
    {
        if (cellCapacity == 0 ||
            cellCapacity > std::numeric_limits<uint32_t>::max() ||
            allocationCapacity == 0)
            return false;
        m_cells.assign(static_cast<size_t>(cellCapacity),
                       InvalidNavigationCell);
        m_freeRanges.clear();
        // release() may briefly insert between two adjacent free ranges before
        // coalescing all three, so retain one extra transient slot as well.
        m_freeRanges.reserve(static_cast<size_t>(allocationCapacity) + 2U);
        m_freeRanges.push_back({0, static_cast<uint32_t>(cellCapacity)});
        m_allocationCapacity = allocationCapacity;
        return true;
    }

    [[nodiscard]] bool isInitialized() const noexcept
    {
        return !m_cells.empty() && m_allocationCapacity != 0;
    }

    [[nodiscard]] uint32_t capacity() const noexcept
    {
        return static_cast<uint32_t>(m_cells.size());
    }

    [[nodiscard]] bool allocate(uint32_t count, Range& output) noexcept
    {
        output = {};
        if (count == 0)
            return true;
        for (size_t index = 0; index < m_freeRanges.size(); ++index)
        {
            Range& available = m_freeRanges[index];
            if (available.count < count)
                continue;
            output = {available.offset, count};
            available.offset += count;
            available.count -= count;
            if (available.count == 0)
                m_freeRanges.erase(m_freeRanges.begin() +
                                   static_cast<std::ptrdiff_t>(index));
            return true;
        }
        return false;
    }

    void release(Range released) noexcept
    {
        if (released.count == 0 || !contains(released))
            return;
        const auto position = std::lower_bound(
            m_freeRanges.begin(), m_freeRanges.end(), released.offset,
            [](const Range& range, uint32_t offset)
            { return range.offset < offset; });
        const size_t index = static_cast<size_t>(
            position - m_freeRanges.begin());
        m_freeRanges.insert(position, released);

        size_t merged = index;
        if (merged != 0)
        {
            Range& previous = m_freeRanges[merged - 1U];
            Range& current = m_freeRanges[merged];
            if (previous.offset + previous.count == current.offset)
            {
                previous.count += current.count;
                m_freeRanges.erase(m_freeRanges.begin() +
                                   static_cast<std::ptrdiff_t>(merged));
                --merged;
            }
        }
        if (merged + 1U < m_freeRanges.size())
        {
            Range& current = m_freeRanges[merged];
            const Range& next = m_freeRanges[merged + 1U];
            if (current.offset + current.count == next.offset)
            {
                current.count += next.count;
                m_freeRanges.erase(m_freeRanges.begin() +
                                   static_cast<std::ptrdiff_t>(merged + 1U));
            }
        }
    }

    [[nodiscard]] bool contains(Range range) const noexcept
    {
        return range.count == 0 ||
            (range.offset < m_cells.size() &&
             static_cast<uint64_t>(range.offset) + range.count <=
                 m_cells.size());
    }

    [[nodiscard]] NavigationCellId* data(Range range) noexcept
    {
        return contains(range) ? m_cells.data() + range.offset : nullptr;
    }

    [[nodiscard]] const NavigationCellId* data(Range range) const noexcept
    {
        return contains(range) ? m_cells.data() + range.offset : nullptr;
    }

    [[nodiscard]] const container::Vector<NavigationCellId>& cells() const noexcept
    {
        return m_cells;
    }

    [[nodiscard]] const container::Vector<Range>& freeRanges() const noexcept
    {
        return m_freeRanges;
    }

    [[nodiscard]] bool structurallyValid() const noexcept
    {
        if (!isInitialized() ||
            m_freeRanges.size() >
                static_cast<size_t>(m_allocationCapacity) + 1U)
            return false;
        uint64_t previousEnd = 0;
        bool first = true;
        for (Range range : m_freeRanges)
        {
            if (range.count == 0 || !contains(range) ||
                (!first && range.offset <= previousEnd))
                return false;
            previousEnd = static_cast<uint64_t>(range.offset) + range.count;
            first = false;
        }
        return true;
    }

private:
    container::Vector<NavigationCellId> m_cells;
    container::Vector<Range> m_freeRanges;
    uint32_t m_allocationCapacity = 0;
};

} // namespace engine::navigation
