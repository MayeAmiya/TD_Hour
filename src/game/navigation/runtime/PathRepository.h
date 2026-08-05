#pragma once

#include "../grid/NavigationLayers.h"
#include "../grid/NavigationDynamicOverlay.h"

#include "core/container/container_types.h"
#include "game/navigation/contracts/NavigationPathContracts.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace engine::navigation
{

enum class PathRepositoryStatus : uint8_t
{
    Success = 0,
    NotInitialized,
    AlreadyInitialized,
    InvalidCapacity,
    AllocationOverflow,
    CapacityExhausted,
    PointLimitExceeded,
    InvalidPoint,
    InvalidHandle,
    Missing,
    StaleGeneration,
    InvalidRevision,
    StaleRevision,
    PointOutOfRange,
    DestinationTooSmall,
};

enum class PathRepositoryPointValidation : uint8_t
{
    RequireNavigationCell,
    LayerOnlyWorldPolyline,
};

struct PathRepositoryPoint final
{
    NavigationWorldPosition position;
    NavigationLayerId layer;
    constexpr bool operator==(const PathRepositoryPoint&) const noexcept = default;
};

struct PathRepositoryCreateResult final
{
    PathRepositoryStatus status = PathRepositoryStatus::NotInitialized;
    engine::ai::PathHandle handle;
};

struct PathRepositoryPointQuery final
{
    PathRepositoryStatus status = PathRepositoryStatus::NotInitialized;
    PathRepositoryPoint point;
};

struct PathRepositoryCopyResult final
{
    PathRepositoryStatus status = PathRepositoryStatus::NotInitialized;
    uint32_t pointCount = 0;
};

struct PathRepositoryPointSpans final
{
    PathRepositoryStatus status = PathRepositoryStatus::NotInitialized;
    container::Span<const int64_t> xRaw;
    container::Span<const int64_t> yRaw;
    container::Span<const int64_t> zRaw;
    container::Span<const NavigationLayerId> layer;
};

struct PathRepositoryMetadataQuery final
{
    PathRepositoryStatus status = PathRepositoryStatus::NotInitialized;
    NavigationPathMetadata metadata;
};

struct PathRepositoryRevisionQuery final
{
    PathRepositoryStatus status = PathRepositoryStatus::NotInitialized;
    NavigationRevision revision = InvalidNavigationRevision;
};

// Generation-safe path storage with field-level SoA columns. initialize() is
// the only operation that allocates. Each slot owns one fixed contiguous point
// span, trading memory for O(1) offsets and fragmentation-free point access.
class PathRepository final
{
public:
    [[nodiscard]] PathRepositoryStatus initialize(uint32_t capacity, uint32_t maxPointsPerPath)
    {
        if (isInitialized())
            return PathRepositoryStatus::AlreadyInitialized;
        if (capacity == 0 || maxPointsPerPath == 0)
            return PathRepositoryStatus::InvalidCapacity;

        const uint64_t pointCapacity64 = static_cast<uint64_t>(capacity) * maxPointsPerPath;
        if (pointCapacity64 > std::numeric_limits<size_t>::max())
            return PathRepositoryStatus::AllocationOverflow;

        const size_t slotCount = capacity;
        const size_t pointCapacity = static_cast<size_t>(pointCapacity64);
        m_generation.assign(slotCount, InitialGeneration);
        m_revision.assign(slotCount, 0);
        m_pointOffset.resize(slotCount);
        m_pointCount.assign(slotCount, 0);
        m_metadata.assign(slotCount, {});
        m_xRaw.assign(pointCapacity, 0);
        m_yRaw.assign(pointCapacity, 0);
        m_zRaw.assign(pointCapacity, 0);
        m_layer.assign(pointCapacity, InvalidNavigationLayer);
        m_freeSlots.resize(slotCount);

        for (uint32_t slot = 0; slot < capacity; ++slot)
        {
            m_pointOffset[slot] = static_cast<uint64_t>(slot) * maxPointsPerPath;
            m_freeSlots[slot] = slot;
        }
        m_capacity = capacity;
        m_maxPointsPerPath = maxPointsPerPath;
        m_freeCount = capacity;
        return PathRepositoryStatus::Success;
    }

    [[nodiscard]] bool isInitialized() const noexcept { return m_capacity != 0; }
    [[nodiscard]] uint32_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] uint32_t maxPointsPerPath() const noexcept { return m_maxPointsPerPath; }
    [[nodiscard]] uint32_t availableSlots() const noexcept { return m_freeCount; }

    [[nodiscard]] PathRepositoryCreateResult create(
        NavigationRevision revision,
        container::Span<const NavigationLayerPathPoint> points) noexcept
    {
        return create(revision, points, {});
    }

    [[nodiscard]] PathRepositoryCreateResult create(
        NavigationRevision revision,
        container::Span<const NavigationLayerPathPoint> points,
        const NavigationPathMetadata& metadata,
        PathRepositoryPointValidation validation =
            PathRepositoryPointValidation::RequireNavigationCell) noexcept
    {
        if (!isInitialized())
            return {};
        if (!revision)
            return {PathRepositoryStatus::InvalidRevision, {}};
        if (points.empty())
            return {PathRepositoryStatus::InvalidPoint, {}};
        if (points.size() > m_maxPointsPerPath)
            return {PathRepositoryStatus::PointLimitExceeded, {}};
        for (const NavigationLayerPathPoint& point : points)
        {
            if (!point.location.layer ||
                (validation ==
                     PathRepositoryPointValidation::RequireNavigationCell &&
                 !point.location.cell))
                return {PathRepositoryStatus::InvalidPoint, {}};
        }
        if (m_freeCount == 0)
            return {PathRepositoryStatus::CapacityExhausted, {}};

        const uint32_t slot = popFreeSlot();
        const size_t offset = static_cast<size_t>(m_pointOffset[slot]);
        for (size_t pointIndex = 0; pointIndex < points.size(); ++pointIndex)
        {
            const size_t poolIndex = offset + pointIndex;
            m_xRaw[poolIndex] = points[pointIndex].position.xRaw;
            m_yRaw[poolIndex] = points[pointIndex].position.yRaw;
            m_zRaw[poolIndex] = points[pointIndex].position.zRaw;
            m_layer[poolIndex] = points[pointIndex].location.layer;
        }
        m_pointCount[slot] = static_cast<uint32_t>(points.size());
        m_revision[slot] = revision.value;
        m_metadata[slot] = metadata;
        return {PathRepositoryStatus::Success, makeHandle(slot, m_generation[slot])};
    }

    [[nodiscard]] PathRepositoryStatus release(engine::ai::PathHandle handle,
                                               NavigationRevision expectedRevision) noexcept
    {
        uint32_t slot = 0;
        const PathRepositoryStatus status = resolve(handle, expectedRevision, slot);
        if (status != PathRepositoryStatus::Success)
            return status;

        m_revision[slot] = 0;
        m_pointCount[slot] = 0;
        m_metadata[slot] = {};
        if (m_generation[slot] == MaximumGeneration)
            return PathRepositoryStatus::Success;

        ++m_generation[slot];
        pushFreeSlot(slot);
        return PathRepositoryStatus::Success;
    }

    [[nodiscard]] PathRepositoryPointQuery queryPoint(engine::ai::PathHandle handle,
                                                      NavigationRevision expectedRevision,
                                                      uint32_t pointIndex) const noexcept
    {
        uint32_t slot = 0;
        const PathRepositoryStatus status = resolve(handle, expectedRevision, slot);
        if (status != PathRepositoryStatus::Success)
            return {status, {}};
        if (pointIndex >= m_pointCount[slot])
            return {PathRepositoryStatus::PointOutOfRange, {}};

        const size_t poolIndex = static_cast<size_t>(m_pointOffset[slot]) + pointIndex;
        return {PathRepositoryStatus::Success,
                {{m_xRaw[poolIndex], m_yRaw[poolIndex], m_zRaw[poolIndex]}, m_layer[poolIndex]}};
    }

    [[nodiscard]] PathRepositoryCopyResult copyPoints(engine::ai::PathHandle handle,
                                                      NavigationRevision expectedRevision,
                                                      container::Span<PathRepositoryPoint> destination) const noexcept
    {
        uint32_t slot = 0;
        const PathRepositoryStatus status = resolve(handle, expectedRevision, slot);
        if (status != PathRepositoryStatus::Success)
            return {status, 0};

        const uint32_t count = m_pointCount[slot];
        if (destination.size() < count)
            return {PathRepositoryStatus::DestinationTooSmall, count};
        const size_t offset = static_cast<size_t>(m_pointOffset[slot]);
        for (uint32_t pointIndex = 0; pointIndex < count; ++pointIndex)
        {
            const size_t poolIndex = offset + pointIndex;
            destination[pointIndex] = {
                {m_xRaw[poolIndex], m_yRaw[poolIndex], m_zRaw[poolIndex]}, m_layer[poolIndex]};
        }
        return {PathRepositoryStatus::Success, count};
    }

    // Borrowed spans remain valid until this slot is released. Repository
    // storage itself never moves after initialize().
    [[nodiscard]] PathRepositoryPointSpans pointSpans(engine::ai::PathHandle handle,
                                                       NavigationRevision expectedRevision) const noexcept
    {
        uint32_t slot = 0;
        const PathRepositoryStatus status = resolve(handle, expectedRevision, slot);
        if (status != PathRepositoryStatus::Success)
            return {status, {}, {}, {}, {}};

        const size_t offset = static_cast<size_t>(m_pointOffset[slot]);
        const size_t count = m_pointCount[slot];
        return {PathRepositoryStatus::Success,
                {m_xRaw.data() + offset, count},
                {m_yRaw.data() + offset, count},
                {m_zRaw.data() + offset, count},
                {m_layer.data() + offset, count}};
    }

    [[nodiscard]] PathRepositoryMetadataQuery metadata(
        engine::ai::PathHandle handle,
        NavigationRevision expectedRevision) const noexcept
    {
        uint32_t slot = 0;
        const PathRepositoryStatus status = resolve(handle, expectedRevision, slot);
        if (status != PathRepositoryStatus::Success)
            return {status, {}};
        return {PathRepositoryStatus::Success, m_metadata[slot]};
    }

    // Generation-safe lookup used only when a Patch request must read the
    // path installed under an older topology revision. Callers still pass
    // the returned revision back through copy/query/release operations.
    [[nodiscard]] PathRepositoryRevisionQuery storedRevision(
        engine::ai::PathHandle handle) const noexcept
    {
        if (!isInitialized())
            return {};
        if (!handle)
            return {PathRepositoryStatus::InvalidHandle, {}};
        const uint32_t encodedSlot = static_cast<uint32_t>(handle.value);
        const uint32_t generation =
            static_cast<uint32_t>(handle.value >> 32U);
        if (encodedSlot == 0 || generation == 0)
            return {PathRepositoryStatus::InvalidHandle, {}};
        const uint32_t slot = encodedSlot - 1U;
        if (slot >= m_capacity)
            return {PathRepositoryStatus::InvalidHandle, {}};
        if (generation != m_generation[slot])
            return {PathRepositoryStatus::StaleGeneration, {}};
        if (m_revision[slot] == 0)
            return {PathRepositoryStatus::Missing, {}};
        return {PathRepositoryStatus::Success,
                NavigationRevision{m_revision[slot]}};
    }

    [[nodiscard]] uint64_t stableHash() const noexcept
    {
        if (!isInitialized())
            return 0;

        uint64_t hash = 14695981039346656037ULL;
        feed(hash, HashSchemaVersion);
        feed(hash, m_capacity);
        feed(hash, m_maxPointsPerPath);
        for (uint32_t slot = 0; slot < m_capacity; ++slot)
        {
            feed(hash, slot);
            feed(hash, m_generation[slot]);
            feed(hash, m_revision[slot]);
            feed(hash, m_pointOffset[slot]);
            feed(hash, m_pointCount[slot]);
            feed(hash, static_cast<uint32_t>(m_metadata[slot].affectedCells.minX));
            feed(hash, static_cast<uint32_t>(m_metadata[slot].affectedCells.minY));
            feed(hash, static_cast<uint32_t>(m_metadata[slot].affectedCells.maxX));
            feed(hash, static_cast<uint32_t>(m_metadata[slot].affectedCells.maxY));
            feed(hash, m_metadata[slot].corridorChunkCount);
            const size_t corridorChunkCount = std::min<size_t>(
                m_metadata[slot].corridorChunkCount,
                NavigationPathMetadata::MaximumCorridorChunks);
            for (size_t chunk = 0;
                 chunk < corridorChunkCount; ++chunk) {
                const NavigationCellBounds& bounds =
                    m_metadata[slot].corridorChunks[chunk];
                feed(hash, static_cast<uint32_t>(bounds.minX));
                feed(hash, static_cast<uint32_t>(bounds.minY));
                feed(hash, static_cast<uint32_t>(bounds.maxX));
                feed(hash, static_cast<uint32_t>(bounds.maxY));
            }
            feed(hash, m_metadata[slot].revisions.staticNavigation.value);
            feed(hash, m_metadata[slot].revisions.dynamicObstacles.value);
            feed(hash, m_metadata[slot].revisions.portalTopology.value);
            feed(hash, m_metadata[slot].layer.value);
            const size_t offset = static_cast<size_t>(m_pointOffset[slot]);
            for (uint32_t pointIndex = 0; pointIndex < m_pointCount[slot]; ++pointIndex)
            {
                const size_t poolIndex = offset + pointIndex;
                feed(hash, static_cast<uint64_t>(m_xRaw[poolIndex]));
                feed(hash, static_cast<uint64_t>(m_yRaw[poolIndex]));
                feed(hash, static_cast<uint64_t>(m_zRaw[poolIndex]));
                feed(hash, m_layer[poolIndex].value);
            }
        }
        return hash;
    }

private:
    inline static constexpr uint32_t InitialGeneration = 1;
    inline static constexpr uint32_t MaximumGeneration = std::numeric_limits<uint32_t>::max();
    inline static constexpr uint32_t HashSchemaVersion = 2;

    [[nodiscard]] static engine::ai::PathHandle makeHandle(uint32_t slot, uint32_t generation) noexcept
    {
        return {(static_cast<uint64_t>(generation) << 32U) | (static_cast<uint64_t>(slot) + 1U)};
    }

    [[nodiscard]] PathRepositoryStatus resolve(engine::ai::PathHandle handle,
                                               NavigationRevision expectedRevision,
                                               uint32_t& slot) const noexcept
    {
        if (!isInitialized())
            return PathRepositoryStatus::NotInitialized;
        if (!handle)
            return PathRepositoryStatus::InvalidHandle;

        const uint32_t encodedSlot = static_cast<uint32_t>(handle.value);
        const uint32_t generation = static_cast<uint32_t>(handle.value >> 32U);
        if (encodedSlot == 0 || generation == 0)
            return PathRepositoryStatus::InvalidHandle;
        slot = encodedSlot - 1U;
        if (slot >= m_capacity)
            return PathRepositoryStatus::InvalidHandle;
        if (generation != m_generation[slot])
            return PathRepositoryStatus::StaleGeneration;
        if (m_revision[slot] == 0)
            return PathRepositoryStatus::Missing;
        if (!expectedRevision)
            return PathRepositoryStatus::InvalidRevision;
        if (expectedRevision.value != m_revision[slot])
            return PathRepositoryStatus::StaleRevision;
        return PathRepositoryStatus::Success;
    }

    [[nodiscard]] uint32_t popFreeSlot() noexcept
    {
        const uint32_t result = m_freeSlots[0];
        --m_freeCount;
        if (m_freeCount == 0)
            return result;
        m_freeSlots[0] = m_freeSlots[m_freeCount];
        uint32_t position = 0;
        while (true)
        {
            const uint32_t left = position * 2U + 1U;
            if (left >= m_freeCount)
                break;
            const uint32_t right = left + 1U;
            const uint32_t smallest = right < m_freeCount && m_freeSlots[right] < m_freeSlots[left] ? right : left;
            if (m_freeSlots[position] <= m_freeSlots[smallest])
                break;
            const uint32_t swap = m_freeSlots[position];
            m_freeSlots[position] = m_freeSlots[smallest];
            m_freeSlots[smallest] = swap;
            position = smallest;
        }
        return result;
    }

    void pushFreeSlot(uint32_t slot) noexcept
    {
        uint32_t position = m_freeCount;
        m_freeSlots[position] = slot;
        ++m_freeCount;
        while (position != 0)
        {
            const uint32_t parent = (position - 1U) / 2U;
            if (m_freeSlots[parent] <= m_freeSlots[position])
                break;
            const uint32_t swap = m_freeSlots[parent];
            m_freeSlots[parent] = m_freeSlots[position];
            m_freeSlots[position] = swap;
            position = parent;
        }
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

    uint32_t m_capacity = 0;
    uint32_t m_maxPointsPerPath = 0;
    uint32_t m_freeCount = 0;
    container::Vector<uint32_t> m_generation;
    container::Vector<uint64_t> m_revision;
    container::Vector<uint64_t> m_pointOffset;
    container::Vector<uint32_t> m_pointCount;
    container::Vector<NavigationPathMetadata> m_metadata;
    container::Vector<int64_t> m_xRaw;
    container::Vector<int64_t> m_yRaw;
    container::Vector<int64_t> m_zRaw;
    container::Vector<NavigationLayerId> m_layer;
    container::Vector<uint32_t> m_freeSlots;
};

} // namespace engine::navigation
