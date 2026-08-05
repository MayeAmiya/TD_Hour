#pragma once

#include "core/container/container_types.h"
#include "math/fixed/q32_32.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "game/navigation/contracts/NavigationPathContracts.h"

namespace engine::ai {

// Confirmed-tick immutable projection for FollowPath. Metadata stays in SoA
// columns and all points share one flat array; kernels retain only a stable
// handle, content revision and point index.
class ObjectAIPathSequenceSnapshot final {
public:
    void clear() noexcept {
        m_handles.clear();
        m_revisions.clear();
        m_offsets.clear();
        m_counts.clear();
        m_points.clear();
    }

    void reserve(size_t sequenceCapacity, size_t pointCapacity) {
        m_handles.reserve(sequenceCapacity);
        m_revisions.reserve(sequenceCapacity);
        m_offsets.reserve(sequenceCapacity);
        m_counts.reserve(sequenceCapacity);
        m_points.reserve(pointCapacity);
    }

    [[nodiscard]] bool append(
        AIPathSequenceHandle handle,
        container::Span<const AIFixedPosition> points,
        uint64_t* revision = nullptr,
        uint64_t forcedRevision = 0) {
        if (!handle || points.empty() ||
            (!m_handles.empty() && m_handles.back().value >= handle.value) ||
            m_points.size() > std::numeric_limits<uint32_t>::max() ||
            points.size() > std::numeric_limits<uint32_t>::max() ||
            points.size() > std::numeric_limits<uint32_t>::max() -
                                m_points.size()) {
            return false;
        }

        const uint64_t contentRevision = forcedRevision != 0
            ? forcedRevision : sequenceRevision(points);
        m_handles.push_back(handle);
        m_revisions.push_back(contentRevision);
        m_offsets.push_back(static_cast<uint32_t>(m_points.size()));
        m_counts.push_back(static_cast<uint32_t>(points.size()));
        m_points.insert(m_points.end(), points.begin(), points.end());
        if (revision) *revision = contentRevision;
        return true;
    }

    [[nodiscard]] AIPathSequenceResolver resolver() const noexcept {
        return {
            .context = this,
            .queryPoint = &queryPoint,
        };
    }

    [[nodiscard]] size_t sequenceCount() const noexcept {
        return m_handles.size();
    }

    [[nodiscard]] size_t pointCount() const noexcept {
        return m_points.size();
    }

    [[nodiscard]] std::optional<uint64_t> revision(
        AIPathSequenceHandle handle) const noexcept {
        const auto found = find(handle);
        if (found == m_handles.end()) return std::nullopt;
        return m_revisions[static_cast<size_t>(found - m_handles.begin())];
    }

private:
    static constexpr uint64_t FnvOffset = 14695981039346656037ull;
    static constexpr uint64_t FnvPrime = 1099511628211ull;

    static void feedByte(uint64_t& hash, uint8_t value) noexcept {
        hash ^= value;
        hash *= FnvPrime;
    }

    static void feedU64(uint64_t& hash, uint64_t value) noexcept {
        for (uint32_t shift = 0; shift != 64; shift += 8)
            feedByte(hash, static_cast<uint8_t>(value >> shift));
    }

    [[nodiscard]] static uint64_t sequenceRevision(
        container::Span<const AIFixedPosition> points) noexcept {
        uint64_t hash = FnvOffset;
        feedU64(hash, static_cast<uint64_t>(points.size()));
        for (const AIFixedPosition& point : points) {
            feedU64(hash, static_cast<uint64_t>(point.xRaw));
            feedU64(hash, static_cast<uint64_t>(point.yRaw));
            feedU64(hash, static_cast<uint64_t>(point.zRaw));
        }
        return hash == 0 ? uint64_t{1} : hash;
    }

    [[nodiscard]] static int64_t distanceToNext(
        const AIFixedPosition& current,
        const AIFixedPosition& next) noexcept {
        const math::q32_32 dx = math::q32_32::from_raw(next.xRaw) -
            math::q32_32::from_raw(current.xRaw);
        const math::q32_32 dy = math::q32_32::from_raw(next.yRaw) -
            math::q32_32::from_raw(current.yRaw);
        return math::q32_32::sqrt(dx * dx + dy * dy).raw();
    }

    [[nodiscard]] static AIPathSequenceQuery queryPoint(
        const void* context, AIPathSequenceHandle handle,
        uint64_t revision, uint32_t index) noexcept {
        const auto* snapshot =
            static_cast<const ObjectAIPathSequenceSnapshot*>(context);
        if (!snapshot || !handle)
            return {.status = AIPathSequenceQueryStatus::Missing};

        const auto found = snapshot->find(handle);
        if (found == snapshot->m_handles.end())
            return {.status = AIPathSequenceQueryStatus::Missing};

        const size_t sequence = static_cast<size_t>(
            found - snapshot->m_handles.begin());
        if (snapshot->m_revisions[sequence] != revision)
            return {.status = AIPathSequenceQueryStatus::StaleRevision};
        const uint32_t count = snapshot->m_counts[sequence];
        if (index >= count)
            return {.status = AIPathSequenceQueryStatus::End};

        const uint32_t absolute = snapshot->m_offsets[sequence] + index;
        const bool hasNext = index + 1 < count;
        return {
            .status = AIPathSequenceQueryStatus::Point,
            .point = {
                .position = snapshot->m_points[absolute],
                .distanceToNextRaw = hasNext
                    ? distanceToNext(snapshot->m_points[absolute],
                                     snapshot->m_points[absolute + 1])
                    : 0,
                .hasNext = hasNext,
                .hasFollowing = index + 2 < count,
            },
        };
    }

    [[nodiscard]] container::Vector<AIPathSequenceHandle>::const_iterator find(
        AIPathSequenceHandle handle) const noexcept {
        const auto found = std::lower_bound(
            m_handles.begin(), m_handles.end(), handle.value,
            [](AIPathSequenceHandle candidate, uint64_t value) {
                return candidate.value < value;
            });
        return found != m_handles.end() && found->value == handle.value
            ? found : m_handles.end();
    }

    container::Vector<AIPathSequenceHandle> m_handles;
    container::Vector<uint64_t> m_revisions;
    container::Vector<uint32_t> m_offsets;
    container::Vector<uint32_t> m_counts;
    container::Vector<AIFixedPosition> m_points;
};

} // namespace engine::ai
