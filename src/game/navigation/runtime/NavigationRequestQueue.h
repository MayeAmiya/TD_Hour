#pragma once

#include "../grid/NavigationTypes.h"
#include "game/navigation/contracts/NavigationPathContracts.h"

#include "core/container/container_types.h"

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <limits>

namespace engine::navigation
{

enum class NavigationRequestQueueResult : uint8_t
{
    Accepted = 0,
    Replaced,
    Cancelled,
    InvalidRequest,
    StaleCorrelation,
    NotFound,
    CapacityExceeded,
};

struct QueuedNavigationRequest final
{
    engine::ai::PathRequest request;
    uint64_t submittedTick = 0;
    uint64_t navigationRevision = 0;
    NavigationLayerId startLayer = InvalidNavigationLayer;
    NavigationLayerId goalLayer = InvalidNavigationLayer;
};

// One subject owns at most one queued path request. Values are copied into
// bounded storage and kept in canonical confirmed-tick/correlation order.
class NavigationRequestQueue final
{
public:
    [[nodiscard]] bool initialize(size_t capacity)
    {
        if (capacity == 0)
            return false;
        if (capacity > std::numeric_limits<size_t>::max() / 2u)
            return false;
        size_t subjectTableCapacity = 1;
        const size_t minimumSubjectTableCapacity = capacity * 2u;
        while (subjectTableCapacity < minimumSubjectTableCapacity)
        {
            if (subjectTableCapacity >
                std::numeric_limits<size_t>::max() / 2u)
                return false;
            subjectTableCapacity *= 2u;
        }
        m_entries.assign(capacity, {});
        m_subjectSlots.assign(subjectTableCapacity, {});
        m_count = 0;
        return true;
    }

    [[nodiscard]] size_t size() const noexcept { return m_count; }
    [[nodiscard]] size_t capacity() const noexcept { return m_entries.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_count == 0; }
    [[nodiscard]] container::Span<const QueuedNavigationRequest> entries() const noexcept
    {
        return {m_entries.data(), m_count};
    }

    [[nodiscard]] NavigationRequestQueueResult submit(const engine::ai::PathRequest& request,
                                                       uint64_t confirmedTick,
                                                       uint64_t navigationRevision,
                                                       NavigationLayerId startLayer,
                                                       NavigationLayerId goalLayer) noexcept
    {
        if (!validRequest(request, confirmedTick) || navigationRevision == 0 ||
            !startLayer || !goalLayer)
            return NavigationRequestQueueResult::InvalidRequest;
        if (request.kind == engine::ai::PathRequestKind::Cancel)
            return cancel(request.correlation);

        const size_t owned = findSubject(request.correlation.subject);
        if (owned != m_count)
        {
            if (!replaces(m_entries[owned].request.correlation, request.correlation))
                return NavigationRequestQueueResult::StaleCorrelation;
            removeAt(owned);
            insert({request, confirmedTick, navigationRevision,
                    startLayer, goalLayer});
            return NavigationRequestQueueResult::Replaced;
        }
        if (m_count == capacity())
            return NavigationRequestQueueResult::CapacityExceeded;
        insert({request, confirmedTick, navigationRevision,
                startLayer, goalLayer});
        return NavigationRequestQueueResult::Accepted;
    }

    // Single-layer compatibility entry used by isolated contracts. Production
    // NavigationSystem always supplies explicit start/goal layers.
    [[nodiscard]] NavigationRequestQueueResult submit(
        const engine::ai::PathRequest& request,
        uint64_t confirmedTick,
        uint64_t navigationRevision) noexcept
    {
        constexpr NavigationLayerId GroundLayer{1};
        return submit(request, confirmedTick, navigationRevision,
                      GroundLayer, GroundLayer);
    }

    [[nodiscard]] NavigationRequestQueueResult cancel(const engine::ai::PathCorrelation& correlation) noexcept
    {
        if (!correlation.isValid())
            return NavigationRequestQueueResult::InvalidRequest;
        const size_t owned = findSubject(correlation.subject);
        if (owned == m_count)
            return NavigationRequestQueueResult::NotFound;
        if (!(m_entries[owned].request.correlation == correlation))
            return NavigationRequestQueueResult::StaleCorrelation;
        removeAt(owned);
        return NavigationRequestQueueResult::Cancelled;
    }

    [[nodiscard]] const QueuedNavigationRequest* front() const noexcept
    {
        return m_count == 0 ? nullptr : &m_entries[0];
    }

    // RefCode computes airborne QuickPath immediately instead of queueing it
    // behind terrain A*; exact authored waypoint polylines likewise have no
    // terrain-search cost. Keep those zero-expansion requests in their normal
    // canonical store (so replacement/cancellation ownership remains one
    // path), but let NavigationPathService take the first due one before it
    // advances a resumable Navmesh job.
    [[nodiscard]] const QueuedNavigationRequest* firstImmediate(
        uint64_t confirmedTick) const noexcept
    {
        for (size_t index = 0; index < m_count; ++index)
        {
            const QueuedNavigationRequest& entry = m_entries[index];
            if (entry.submittedTick <= confirmedTick &&
                entry.request.traversalMode !=
                    engine::ai::AIPathTraversalMode::Navmesh) {
                return &entry;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool popFirstImmediate(
        uint64_t confirmedTick, QueuedNavigationRequest& output) noexcept
    {
        for (size_t index = 0; index < m_count; ++index)
        {
            const QueuedNavigationRequest& entry = m_entries[index];
            if (entry.submittedTick > confirmedTick ||
                entry.request.traversalMode ==
                    engine::ai::AIPathTraversalMode::Navmesh) {
                continue;
            }
            output = entry;
            removeAt(index);
            return true;
        }
        return false;
    }

    [[nodiscard]] bool pop(QueuedNavigationRequest& output) noexcept
    {
        if (m_count == 0)
            return false;
        output = m_entries[0];
        removeAt(0);
        return true;
    }

private:
    [[nodiscard]] static bool validRequest(const engine::ai::PathRequest& request,
                                           uint64_t confirmedTick) noexcept
    {
        if (!request.correlation.isValid() || confirmedTick == 0)
            return false;
        if (request.kind != engine::ai::PathRequestKind::Cancel &&
            !request.clearanceProfile.validFrozen())
            return false;
        if (request.kind == engine::ai::PathRequestKind::Patch && !request.currentPath)
            return false;
        if (request.kind == engine::ai::PathRequestKind::Safe && !request.safePathRepulsor)
            return false;
        if (request.kind == engine::ai::PathRequestKind::Cancel)
            return request.objectCells.empty() &&
                request.attackSeeThroughObstacles.empty() &&
                request.dozerPassableObstacles.empty();
        if (request.kind == engine::ai::PathRequestKind::Approach) {
            if (request.arrivalRadiusRaw < 0 ||
                request.minimumArrivalRadiusRaw < 0 ||
                request.minimumArrivalRadiusRaw >
                    request.arrivalRadiusRaw)
                return false;
        } else if (request.minimumArrivalRadiusRaw != 0 ||
                   request.attackTarget || request.attackContactWeapon ||
                   request.attackLineOfSightEnabled ||
                   request.attackSubjectContainer ||
                   request.attackTargetContainer ||
                   request.attackSubjectSlaver ||
                   request.attackTargetSlaver ||
                   !request.attackSeeThroughObstacles.empty()) {
            return false;
        }
        if ((request.groupPathId == 0) !=
                (request.groupPathMemberCount == 0) ||
            (request.groupPathId != 0 &&
             (request.kind != engine::ai::PathRequestKind::New ||
              request.groupPathMemberCount < 2 ||
              request.groupPathMemberOrdinal >=
                  request.groupPathMemberCount)))
            return false;
        if (request.objectSnapshotTick != confirmedTick ||
            (request.pathThroughUnits && !request.objectCells.empty()))
            return false;
        for (size_t index = 0; index < request.objectCells.size(); ++index)
        {
            const engine::ai::AIPathObjectCellSnapshot& value =
                request.objectCells[index];
            if (!value.object || value.layer == UINT32_MAX ||
                value.cell == UINT32_MAX ||
                (value.effect != engine::ai::AIPathObjectCellEffect::FriendlyCost &&
                 value.effect != engine::ai::AIPathObjectCellEffect::EnemyBlock &&
                 value.effect != engine::ai::AIPathObjectCellEffect::NeutralBlock))
                return false;
            if (index == 0)
                continue;
            const engine::ai::AIPathObjectCellSnapshot& previous =
                request.objectCells[index - 1];
            if (value.layer < previous.layer ||
                (value.layer == previous.layer &&
                 (value.cell < previous.cell ||
                  (value.cell == previous.cell &&
                   value.object <= previous.object))))
                return false;
        }
        for (size_t index = 0;
             index < request.attackSeeThroughObstacles.size(); ++index) {
            const uint64_t object =
                request.attackSeeThroughObstacles[index];
            if (object == 0 ||
                (index != 0 && object <=
                    request.attackSeeThroughObstacles[index - 1]))
                return false;
        }
        for (size_t index = 0;
             index < request.dozerPassableObstacles.size(); ++index) {
            const uint64_t object = request.dozerPassableObstacles[index];
            if (object == 0 ||
                (index != 0 && object <=
                    request.dozerPassableObstacles[index - 1]))
                return false;
        }
        return true;
    }

    [[nodiscard]] static bool replaces(const engine::ai::PathCorrelation& current,
                                       const engine::ai::PathCorrelation& incoming) noexcept
    {
        if (incoming.sourceOrderRevision != current.sourceOrderRevision)
            return incoming.sourceOrderRevision > current.sourceOrderRevision;
        if (!(incoming.stateRequest == current.stateRequest))
            return false;
        return incoming.generation > current.generation;
    }

    [[nodiscard]] static bool less(const QueuedNavigationRequest& left,
                                   const QueuedNavigationRequest& right) noexcept
    {
        const auto priority = [](const QueuedNavigationRequest& value) noexcept {
            const engine::ai::PathRequest& request = value.request;
            if (request.kind == engine::ai::PathRequestKind::Approach ||
                request.kind == engine::ai::PathRequestKind::MoveAside)
                return uint8_t{0};
            // AIAsyncOrderIdentity mirrors ObjectAIOrderSource numerically:
            // Player=0, Script=1, System=2. Keep the navigation contract free
            // of the ECS admission header while preserving that stable wire.
            if (request.correlation.orderIdentity.source == 0)
                return uint8_t{1};
            if (request.correlation.orderIdentity.source == 1)
                return uint8_t{2};
            if (request.kind == engine::ai::PathRequestKind::Safe)
                return uint8_t{4};
            if (request.groupPathId != 0)
                return uint8_t{5};
            return uint8_t{3};
        };
        const uint8_t leftPriority = priority(left);
        const uint8_t rightPriority = priority(right);
        if (leftPriority != rightPriority)
            return leftPriority < rightPriority;
        if (left.submittedTick != right.submittedTick)
            return left.submittedTick < right.submittedTick;
        // A group centerline is a tiny transaction: ordinal zero produces the
        // immutable route and later ordinals consume it.  ObjectId ordering
        // alone is insufficient because the RefCode-compatible leader is the
        // real member nearest the group centre, not necessarily the smallest
        // ObjectId.
        if (left.request.groupPathId != 0 &&
            left.request.groupPathId == right.request.groupPathId &&
            left.request.groupPathMemberOrdinal !=
                right.request.groupPathMemberOrdinal) {
            return left.request.groupPathMemberOrdinal <
                right.request.groupPathMemberOrdinal;
        }
        const engine::ai::PathCorrelation& a = left.request.correlation;
        const engine::ai::PathCorrelation& b = right.request.correlation;
        if (a.subject != b.subject)
            return a.subject < b.subject;
        if (a.stateRequest != b.stateRequest)
            return a.stateRequest < b.stateRequest;
        if (a.generation != b.generation)
            return a.generation < b.generation;
        return a.sourceOrderRevision < b.sourceOrderRevision;
    }

    [[nodiscard]] size_t findSubject(engine::ObjectId subject) const noexcept
    {
        if (!subject || m_subjectSlots.empty()) return m_count;
        const size_t mask = m_subjectSlots.size() - 1u;
        size_t slot = (static_cast<size_t>(subject.value) *
                       static_cast<size_t>(2654435761u)) & mask;
        for (size_t probe = 0; probe < m_subjectSlots.size(); ++probe)
        {
            const SubjectSlot& entry = m_subjectSlots[slot];
            if (entry.subject == 0u) return m_count;
            if (entry.subject == subject.value) return entry.queueSlot;
            slot = (slot + 1u) & mask;
        }
        return m_count;
    }

    void rebuildSubjectIndex() noexcept
    {
        std::fill(m_subjectSlots.begin(), m_subjectSlots.end(), SubjectSlot{});
        if (m_subjectSlots.empty()) return;
        const size_t mask = m_subjectSlots.size() - 1u;
        for (size_t queueSlot = 0; queueSlot < m_count; ++queueSlot)
        {
            const uint32_t subject =
                m_entries[queueSlot].request.correlation.subject.value;
            size_t slot = (static_cast<size_t>(subject) *
                           static_cast<size_t>(2654435761u)) & mask;
            while (m_subjectSlots[slot].subject != 0u)
                slot = (slot + 1u) & mask;
            m_subjectSlots[slot] = {subject, queueSlot};
        }
    }

    void insert(const QueuedNavigationRequest& entry) noexcept
    {
        size_t insertAt = m_count;
        while (insertAt > 0 && less(entry, m_entries[insertAt - 1]))
        {
            m_entries[insertAt] = m_entries[insertAt - 1];
            --insertAt;
        }
        m_entries[insertAt] = entry;
        ++m_count;
        rebuildSubjectIndex();
    }

    void removeAt(size_t index) noexcept
    {
        for (size_t move = index + 1; move < m_count; ++move)
        {
            m_entries[move - 1] = m_entries[move];
        }
        --m_count;
        m_entries[m_count] = {};
        rebuildSubjectIndex();
    }

    struct SubjectSlot final
    {
        uint32_t subject = 0;
        size_t queueSlot = 0;
    };

    container::Vector<QueuedNavigationRequest> m_entries;
    // Derived lookup only. Canonical ordering and snapshot/hash semantics are
    // still defined exclusively by m_entries[0, m_count).
    container::Vector<SubjectSlot> m_subjectSlots;
    size_t m_count = 0;
};

} // namespace engine::navigation
