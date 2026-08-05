#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"

namespace engine::ai
{

enum class AIWakeScheduleResult : uint8_t
{
    Scheduled,
    AlreadyScheduled,
    Rescheduled,
    NotScheduled,
    InvalidObjectId,
    CapacityExceeded,
};

struct AIWakeEvent
{
    ObjectId object = INVALID_OBJECT_ID;
    uint64_t wakeTick = 0;

    constexpr bool operator==(const AIWakeEvent&) const noexcept = default;
};

// A fixed-capacity deterministic queue for sleeping AI state machines.
// Entries are kept ordered by (wakeTick, ObjectId), so dequeue order does not
// depend on scheduling order or on a standard-library heap implementation.
template <size_t CapacityValue>
class AIWakeScheduler
{
    static_assert(CapacityValue > 0, "AIWakeScheduler capacity must be positive");

public:
    static constexpr size_t Capacity = CapacityValue;

    [[nodiscard]] constexpr size_t size() const noexcept
    {
        return m_size;
    }
    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return m_size == 0;
    }
    [[nodiscard]] constexpr bool full() const noexcept
    {
        return m_size == Capacity;
    }

    // Schedules an object exactly once. Use reschedule when replacing an
    // existing deadline so accidental duplicate ownership remains visible.
    [[nodiscard]] constexpr AIWakeScheduleResult schedule(ObjectId object, uint64_t wakeTick) noexcept
    {
        if (!object)
            return AIWakeScheduleResult::InvalidObjectId;
        if (find(object) != m_size)
            return AIWakeScheduleResult::AlreadyScheduled;
        if (full())
            return AIWakeScheduleResult::CapacityExceeded;

        insert({.object = object, .wakeTick = wakeTick});
        return AIWakeScheduleResult::Scheduled;
    }

    [[nodiscard]] constexpr AIWakeScheduleResult scheduleAfter(ObjectId object,
                                                               uint64_t confirmedTick,
                                                               uint64_t delayTicks) noexcept
    {
        return schedule(object, saturatingAdd(confirmedTick, delayTicks));
    }

    [[nodiscard]] constexpr AIWakeScheduleResult reschedule(ObjectId object, uint64_t wakeTick) noexcept
    {
        if (!object)
            return AIWakeScheduleResult::InvalidObjectId;
        const size_t index = find(object);
        if (index == m_size)
            return AIWakeScheduleResult::NotScheduled;

        removeAt(index);
        insert({.object = object, .wakeTick = wakeTick});
        return AIWakeScheduleResult::Rescheduled;
    }

    [[nodiscard]] constexpr AIWakeScheduleResult rescheduleAfter(ObjectId object,
                                                                 uint64_t confirmedTick,
                                                                 uint64_t delayTicks) noexcept
    {
        return reschedule(object, saturatingAdd(confirmedTick, delayTicks));
    }

    [[nodiscard]] constexpr bool cancel(ObjectId object) noexcept
    {
        if (!object)
            return false;
        const size_t index = find(object);
        if (index == m_size)
            return false;
        removeAt(index);
        return true;
    }

    [[nodiscard]] constexpr bool contains(ObjectId object) const noexcept
    {
        return object && find(object) != m_size;
    }

    [[nodiscard]] constexpr std::optional<AIWakeEvent> next() const noexcept
    {
        if (empty())
            return std::nullopt;
        return m_entries[0];
    }

    // Returns one due wake at a time. Repeated calls form a deterministic
    // batch and require no caller-owned dynamic storage.
    [[nodiscard]] constexpr std::optional<AIWakeEvent> popDue(uint64_t confirmedTick) noexcept
    {
        if (empty() || m_entries[0].wakeTick > confirmedTick)
            return std::nullopt;
        const AIWakeEvent result = m_entries[0];
        removeAt(0);
        return result;
    }

    constexpr void clear() noexcept
    {
        m_size = 0;
    }

private:
    [[nodiscard]] static constexpr uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept
    {
        constexpr uint64_t Maximum = std::numeric_limits<uint64_t>::max();
        return right > Maximum - left ? Maximum : left + right;
    }

    [[nodiscard]] constexpr size_t find(ObjectId object) const noexcept
    {
        for (size_t index = 0; index < m_size; ++index)
        {
            if (m_entries[index].object == object)
                return index;
        }
        return m_size;
    }

    [[nodiscard]] static constexpr bool precedes(const AIWakeEvent& left, const AIWakeEvent& right) noexcept
    {
        if (left.wakeTick != right.wakeTick)
            return left.wakeTick < right.wakeTick;
        return left.object < right.object;
    }

    constexpr void insert(AIWakeEvent event) noexcept
    {
        size_t index = m_size;
        while (index > 0 && precedes(event, m_entries[index - 1]))
        {
            m_entries[index] = m_entries[index - 1];
            --index;
        }
        m_entries[index] = event;
        ++m_size;
    }

    constexpr void removeAt(size_t index) noexcept
    {
        for (size_t next = index + 1; next < m_size; ++next)
            m_entries[next - 1] = m_entries[next];
        --m_size;
    }

    container::Array<AIWakeEvent, Capacity> m_entries{};
    size_t m_size = 0;
};

} // namespace engine::ai
