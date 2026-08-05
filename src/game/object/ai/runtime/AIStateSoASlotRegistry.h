#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>

#include "core/container/container_types.h"
#include "game/object/ai/runtime/AIStateSoASnapshot.h"

namespace engine::ai
{

struct AIActorHandle final
{
    static constexpr uint32_t InvalidIndex = std::numeric_limits<uint32_t>::max();

    uint32_t batch = InvalidIndex;
    uint32_t slot = InvalidIndex;
    uint32_t generation = 0;

    [[nodiscard]] explicit constexpr operator bool() const noexcept
    {
        return batch != InvalidIndex && slot != InvalidIndex && generation != 0;
    }

    constexpr bool operator==(const AIActorHandle&) const noexcept = default;
};

enum class AIStateSoASlotStatus : uint8_t
{
    Success,
    NotInitialized,
    InvalidCapacity,
    InvalidSubject,
    DuplicateSubject,
    CapacityExceeded,
    SubjectNotFound,
    StorageRejected,
};

struct AIStateSoASubjectSlot final
{
    ObjectId subject = INVALID_OBJECT_ID;
    AIActorHandle handle;
};

struct AIStateSoASlotRegistrySnapshot final
{
    static constexpr uint32_t SchemaVersion = 1;

    uint32_t schemaVersion = SchemaVersion;
    uint32_t batchIndex = AIActorHandle::InvalidIndex;
    container::Vector<uint32_t> generations;
    container::Vector<AIStateSoASlotSnapshot> slots;
};

enum class AIStateSoASlotSnapshotStatus : uint8_t
{
    Success,
    NotInitialized,
    InvalidSchema,
    SizeMismatch,
    InvalidGeneration,
    InvalidSubject,
    DuplicateSubject,
    StorageRejected,
};

// Fixed-capacity production membership for one aligned SoA storage. Structural
// changes are rare compared with state updates, so membership lookup and the
// execution index stay ObjectId-sorted while hot state columns retain stable
// physical slots and never resize during a confirmed tick.
class AIStateSoASlotRegistry final
{
public:
    [[nodiscard]] AIStateSoASlotStatus initialize(size_t capacity,
                                                   uint32_t batchIndex = 0)
    {
        if (capacity > static_cast<size_t>(AIActorHandle::InvalidIndex) ||
            batchIndex == AIActorHandle::InvalidIndex)
            return AIStateSoASlotStatus::InvalidCapacity;

        if (!m_storage.initializeCapacity(capacity))
            return AIStateSoASlotStatus::StorageRejected;
        m_generation.assign(capacity, 1);
        m_activeMask.assign(capacity, 0);
        m_freeSlots.clear();
        m_freeSlots.reserve(capacity);
        for (size_t slot = capacity; slot > 0; --slot)
            m_freeSlots.push_back(static_cast<uint32_t>(slot - 1));

        m_subjectSlots.clear();
        m_subjectSlots.reserve(capacity);
        m_orderedExecutionSlots.clear();
        m_orderedExecutionSlots.reserve(capacity);
        m_batchIndex = batchIndex;
        m_initialized = true;
        return AIStateSoASlotStatus::Success;
    }

    [[nodiscard]] AIStateSoASlotStatus addSubject(ObjectId subject,
                                                   AIActorHandle& output)
    {
        output = {};
        if (!m_initialized)
            return AIStateSoASlotStatus::NotInitialized;
        if (!subject)
            return AIStateSoASlotStatus::InvalidSubject;

        const auto position = lowerBound(subject);
        if (position != m_subjectSlots.end() && position->subject == subject)
            return AIStateSoASlotStatus::DuplicateSubject;
        if (m_freeSlots.empty())
            return AIStateSoASlotStatus::CapacityExceeded;

        const uint32_t slot = m_freeSlots.back();
        const AIActorHandle handle{m_batchIndex, slot, m_generation[slot]};
        if (!m_storage.bindSubject(slot, subject))
            return AIStateSoASlotStatus::StorageRejected;

        m_freeSlots.pop_back();
        m_activeMask[slot] = 1;
        const size_t orderedIndex = static_cast<size_t>(position - m_subjectSlots.begin());
        m_subjectSlots.insert(position, {subject, handle});
        m_orderedExecutionSlots.insert(
            m_orderedExecutionSlots.begin() + static_cast<std::ptrdiff_t>(orderedIndex),
            slot);
        output = handle;
        return AIStateSoASlotStatus::Success;
    }

    [[nodiscard]] AIStateSoASlotStatus removeSubject(ObjectId subject) noexcept
    {
        if (!m_initialized)
            return AIStateSoASlotStatus::NotInitialized;
        if (!subject)
            return AIStateSoASlotStatus::InvalidSubject;

        const auto position = lowerBound(subject);
        if (position == m_subjectSlots.end() || position->subject != subject)
            return AIStateSoASlotStatus::SubjectNotFound;

        const size_t orderedIndex = static_cast<size_t>(position - m_subjectSlots.begin());
        const uint32_t slot = position->handle.slot;
        if (!m_storage.releaseSubject(slot))
            return AIStateSoASlotStatus::StorageRejected;
        m_activeMask[slot] = 0;

        uint32_t& generation = m_generation[slot];
        ++generation;
        if (generation == 0)
            ++generation;

        m_subjectSlots.erase(position);
        m_orderedExecutionSlots.erase(
            m_orderedExecutionSlots.begin() + static_cast<std::ptrdiff_t>(orderedIndex));
        const auto freePosition = std::lower_bound(
            m_freeSlots.begin(), m_freeSlots.end(), slot, std::greater<uint32_t>{});
        m_freeSlots.insert(freePosition, slot);
        return AIStateSoASlotStatus::Success;
    }

    [[nodiscard]] std::optional<AIActorHandle> find(ObjectId subject) const noexcept
    {
        if (!m_initialized || !subject)
            return std::nullopt;
        const auto position = lowerBound(subject);
        return position != m_subjectSlots.end() && position->subject == subject
            ? std::optional<AIActorHandle>{position->handle}
            : std::nullopt;
    }

    [[nodiscard]] ObjectId resolve(AIActorHandle handle) const noexcept
    {
        if (!m_initialized || !handle || handle.batch != m_batchIndex ||
            handle.slot >= m_generation.size() ||
            m_generation[handle.slot] != handle.generation ||
            !m_storage.occupied(handle.slot))
        {
            return INVALID_OBJECT_ID;
        }
        return m_storage.subjects()[handle.slot];
    }

    [[nodiscard]] bool initialized() const noexcept { return m_initialized; }
    [[nodiscard]] size_t capacity() const noexcept { return m_generation.size(); }
    [[nodiscard]] size_t activeCount() const noexcept { return m_subjectSlots.size(); }
    [[nodiscard]] size_t freeCount() const noexcept { return m_freeSlots.size(); }

    [[nodiscard]] container::Span<const AIStateSoASubjectSlot> orderedSubjects() const noexcept
    {
        return m_subjectSlots;
    }

    [[nodiscard]] container::Span<const uint32_t> orderedExecutionSlots() const noexcept
    {
        return m_orderedExecutionSlots;
    }

    [[nodiscard]] container::Span<const uint8_t> activeMask() const noexcept
    {
        return m_activeMask;
    }

    [[nodiscard]] AIStateFamilySoAStorage& storage() noexcept { return m_storage; }
    [[nodiscard]] const AIStateFamilySoAStorage& storage() const noexcept { return m_storage; }

    [[nodiscard]] AIStateSoASlotSnapshotStatus captureSnapshot(
        AIStateSoASlotRegistrySnapshot& output) const
    {
        if (!m_initialized)
            return AIStateSoASlotSnapshotStatus::NotInitialized;

        AIStateSoASlotRegistrySnapshot candidate;
        candidate.batchIndex = m_batchIndex;
        candidate.generations = m_generation;
        candidate.slots.resize(capacity());
        if (!captureAIStateSoASnapshot(m_storage, candidate.slots).succeeded())
            return AIStateSoASlotSnapshotStatus::StorageRejected;
        output = std::move(candidate);
        return AIStateSoASlotSnapshotStatus::Success;
    }

    [[nodiscard]] AIStateSoASlotSnapshotStatus restoreSnapshot(
        const AIStateSoASlotRegistrySnapshot& snapshot)
    {
        if (snapshot.schemaVersion != AIStateSoASlotRegistrySnapshot::SchemaVersion ||
            snapshot.batchIndex == AIActorHandle::InvalidIndex)
        {
            return AIStateSoASlotSnapshotStatus::InvalidSchema;
        }
        if (snapshot.generations.size() != snapshot.slots.size() ||
            snapshot.slots.size() > static_cast<size_t>(AIActorHandle::InvalidIndex))
        {
            return AIStateSoASlotSnapshotStatus::SizeMismatch;
        }

        AIStateSoASlotRegistry candidate;
        if (candidate.initialize(snapshot.slots.size(), snapshot.batchIndex) !=
            AIStateSoASlotStatus::Success)
        {
            return AIStateSoASlotSnapshotStatus::StorageRejected;
        }
        candidate.m_generation = snapshot.generations;
        for (const uint32_t generation : candidate.m_generation)
        {
            if (generation == 0)
                return AIStateSoASlotSnapshotStatus::InvalidGeneration;
        }

        candidate.m_freeSlots.clear();
        candidate.m_subjectSlots.clear();
        candidate.m_orderedExecutionSlots.clear();
        for (size_t slot = 0; slot < snapshot.slots.size(); ++slot)
        {
            const ObjectId subject = snapshot.slots[slot].subject;
            if (!subject)
            {
                candidate.m_freeSlots.push_back(static_cast<uint32_t>(slot));
                continue;
            }
            if (!candidate.m_storage.bindSubject(slot, subject))
                return AIStateSoASlotSnapshotStatus::DuplicateSubject;
            candidate.m_activeMask[slot] = 1;
            candidate.m_subjectSlots.push_back({
                subject,
                {snapshot.batchIndex, static_cast<uint32_t>(slot),
                 candidate.m_generation[slot]}});
        }
        std::sort(candidate.m_freeSlots.begin(), candidate.m_freeSlots.end(),
                  std::greater<uint32_t>{});
        std::sort(
            candidate.m_subjectSlots.begin(), candidate.m_subjectSlots.end(),
            [](const AIStateSoASubjectSlot& left,
               const AIStateSoASubjectSlot& right) {
                return left.subject < right.subject;
            });
        for (size_t index = 1; index < candidate.m_subjectSlots.size(); ++index)
        {
            if (candidate.m_subjectSlots[index - 1].subject ==
                candidate.m_subjectSlots[index].subject)
            {
                return AIStateSoASlotSnapshotStatus::DuplicateSubject;
            }
        }
        for (const AIStateSoASubjectSlot& record : candidate.m_subjectSlots)
            candidate.m_orderedExecutionSlots.push_back(record.handle.slot);

        if (!restoreAIStateSoASnapshot(candidate.m_storage, snapshot.slots).succeeded())
            return AIStateSoASlotSnapshotStatus::StorageRejected;
        *this = std::move(candidate);
        return AIStateSoASlotSnapshotStatus::Success;
    }

private:
    using SubjectIterator = container::Vector<AIStateSoASubjectSlot>::iterator;
    using ConstSubjectIterator = container::Vector<AIStateSoASubjectSlot>::const_iterator;

    [[nodiscard]] SubjectIterator lowerBound(ObjectId subject) noexcept
    {
        return std::lower_bound(
            m_subjectSlots.begin(), m_subjectSlots.end(), subject,
            [](const AIStateSoASubjectSlot& record, ObjectId value) {
                return record.subject < value;
            });
    }

    [[nodiscard]] ConstSubjectIterator lowerBound(ObjectId subject) const noexcept
    {
        return std::lower_bound(
            m_subjectSlots.begin(), m_subjectSlots.end(), subject,
            [](const AIStateSoASubjectSlot& record, ObjectId value) {
                return record.subject < value;
            });
    }

    AIStateFamilySoAStorage m_storage;
    container::Vector<uint32_t> m_generation;
    container::Vector<uint8_t> m_activeMask;
    // Descending order makes pop_back() select the lowest free slot.
    container::Vector<uint32_t> m_freeSlots;
    container::Vector<AIStateSoASubjectSlot> m_subjectSlots;
    container::Vector<uint32_t> m_orderedExecutionSlots;
    uint32_t m_batchIndex = AIActorHandle::InvalidIndex;
    bool m_initialized = false;
};

} // namespace engine::ai
