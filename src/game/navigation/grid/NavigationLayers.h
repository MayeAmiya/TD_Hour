#pragma once

#include "NavigationGrid.h"

#include "core/container/container_types.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace engine::navigation
{

struct NavigationLayerCell final
{
    NavigationLayerId layer = InvalidNavigationLayer;
    NavigationCellId cell = InvalidNavigationCell;
    [[nodiscard]] constexpr bool isValid() const noexcept { return layer && cell; }
    explicit constexpr operator bool() const noexcept { return isValid(); }
    constexpr auto operator<=>(const NavigationLayerCell&) const noexcept = default;
};

struct NavigationLayerPathPoint final
{
    NavigationLayerCell location;
    NavigationWorldPosition position;
    constexpr bool operator==(const NavigationLayerPathPoint&) const noexcept = default;
};

enum class NavigationLayerSetResult : uint8_t
{
    Success = 0,
    InvalidLayer,
    InvalidGrid,
    LayerMismatch,
    DuplicateLayer,
    InvalidCapacity,
    CapacityExceeded,
};

struct NavigationLayerRecord final
{
    NavigationLayerId id = InvalidNavigationLayer;
    NavigationGrid grid;
};

// Owns complete grid values in canonical layer-id order. Identical local cell
// IDs may exist on several layers, which makes bridge-over-ground overlap
// explicit without aliasing the two locations.
class NavigationLayerSet final
{
public:
    // A system owner can opt into a hard layer bound before ingesting map
    // topology. Within that bound, successful addLayer() calls do not grow
    // storage. The default-constructed compatibility mode remains unbounded
    // so existing value-oriented callers do not require initialization.
    [[nodiscard]] NavigationLayerSetResult initialize(size_t layerCapacity)
    {
        if (layerCapacity > MaxLayerCapacity)
            return NavigationLayerSetResult::InvalidCapacity;
        m_layers.clear();
        m_layers.reserve(layerCapacity);
        m_capacity = layerCapacity;
        return NavigationLayerSetResult::Success;
    }

    [[nodiscard]] NavigationLayerSetResult addLayer(NavigationLayerId layer, NavigationGrid grid)
    {
        if (!layer)
            return NavigationLayerSetResult::InvalidLayer;
        if (!grid.isInitialized())
            return NavigationLayerSetResult::InvalidGrid;
        for (NavigationLayerId cellLayer : grid.layer())
        {
            if (cellLayer != layer)
                return NavigationLayerSetResult::LayerMismatch;
        }

        size_t insertAt = 0;
        while (insertAt < m_layers.size() && m_layers[insertAt].id < layer)
            ++insertAt;
        if (insertAt < m_layers.size() && m_layers[insertAt].id == layer)
            return NavigationLayerSetResult::DuplicateLayer;
        if (m_layers.size() == m_capacity)
            return NavigationLayerSetResult::CapacityExceeded;
        m_layers.insert(m_layers.begin() + static_cast<ptrdiff_t>(insertAt), {layer, std::move(grid)});
        return NavigationLayerSetResult::Success;
    }

    [[nodiscard]] size_t size() const noexcept { return m_layers.size(); }
    [[nodiscard]] size_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] container::Span<const NavigationLayerRecord> layers() const noexcept { return m_layers; }

    [[nodiscard]] const NavigationGrid* find(NavigationLayerId layer) const noexcept
    {
        size_t first = 0;
        size_t last = m_layers.size();
        while (first < last)
        {
            const size_t middle = first + (last - first) / 2;
            if (m_layers[middle].id < layer)
                first = middle + 1;
            else
                last = middle;
        }
        return first < m_layers.size() && m_layers[first].id == layer ? &m_layers[first].grid : nullptr;
    }

    [[nodiscard]] NavigationGrid* findMutable(NavigationLayerId layer) noexcept
    {
        size_t first = 0;
        size_t last = m_layers.size();
        while (first < last)
        {
            const size_t middle = first + (last - first) / 2;
            if (m_layers[middle].id < layer)
                first = middle + 1;
            else
                last = middle;
        }
        return first < m_layers.size() && m_layers[first].id == layer ? &m_layers[first].grid : nullptr;
    }

    [[nodiscard]] NavigationLayerCell cellAt(NavigationLayerId layer,
                                             const NavigationWorldPosition& position) const noexcept
    {
        const NavigationGrid* grid = find(layer);
        if (grid == nullptr)
            return {};
        return {layer, grid->cellAt(position)};
    }

    [[nodiscard]] uint64_t stableHash() const noexcept
    {
        uint64_t hash = 14695981039346656037ULL;
        feed(hash, HashSchemaVersion, sizeof(HashSchemaVersion));
        feed(hash, static_cast<uint64_t>(m_layers.size()), sizeof(uint64_t));
        for (const NavigationLayerRecord& layer : m_layers)
        {
            feed(hash, layer.id.value, sizeof(layer.id.value));
            feed(hash, layer.grid.stableHash(), sizeof(uint64_t));
        }
        return hash;
    }

private:
    static void feed(uint64_t& hash, uint64_t value, size_t bytes) noexcept
    {
        for (size_t byte = 0; byte < bytes; ++byte)
        {
            hash ^= static_cast<uint8_t>(value & 0xffU);
            hash *= 1099511628211ULL;
            value >>= 8U;
        }
    }

    inline static constexpr uint32_t HashSchemaVersion = 1;
    inline static constexpr size_t MaxLayerCapacity =
        static_cast<size_t>(std::numeric_limits<uint32_t>::max());
    container::Vector<NavigationLayerRecord> m_layers;
    size_t m_capacity = MaxLayerCapacity;
};

} // namespace engine::navigation
