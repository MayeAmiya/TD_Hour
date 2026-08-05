#pragma once

#include <cstddef>
#include <cstdint>

#include "game/navigation/contracts/NavigationPathContracts.h"
#include "game/object/ai/contracts/AIStateServices.h"
#include "game/object/ai/runtime/AIStateTypes.h"

namespace engine::ai
{

class AIExecutionSlotRange final
{
public:
    struct Block final
    {
        size_t begin = 0;
        size_t end = 0;
    };

    enum class Mode : uint8_t
    {
        Unspecified,
        Empty,
        Full,
        DirectSlots,
        Slots,
        Blocks,
        FilteredDense,
    };

    class Iterator final
    {
    public:
        Iterator() noexcept = default;

        explicit Iterator(const AIExecutionSlotRange& range) noexcept
            : m_mode(range.m_mode),
              m_slots(range.m_slots),
              m_blocks(range.m_blocks),
              m_filter(range.m_filter),
              m_subjects(range.m_subjects),
              m_limit(range.m_fullCount)
        {
            switch (m_mode)
            {
            case Mode::Full:
                m_done = m_limit == 0;
                break;
            case Mode::DirectSlots:
            case Mode::Slots:
                m_done = m_slots.empty();
                break;
            case Mode::Blocks:
                m_done = m_blocks.empty();
                if (!m_done)
                    m_current = m_blocks.front().begin;
                break;
            case Mode::FilteredDense:
                advanceDense();
                break;
            case Mode::Unspecified:
            case Mode::Empty:
                m_done = true;
                break;
            }
        }

        [[nodiscard]] size_t operator*() const noexcept
        {
            return (m_mode == Mode::DirectSlots || m_mode == Mode::Slots)
                ? m_slots[m_index] : m_current;
        }

        Iterator& operator++() noexcept
        {
            switch (m_mode)
            {
            case Mode::Full:
                m_done = ++m_current >= m_limit;
                break;
            case Mode::DirectSlots:
            case Mode::Slots:
                m_done = ++m_index >= m_slots.size();
                break;
            case Mode::Blocks:
                ++m_current;
                if (m_current >= m_blocks[m_blockIndex].end)
                {
                    ++m_blockIndex;
                    if (m_blockIndex >= m_blocks.size())
                        m_done = true;
                    else
                        m_current = m_blocks[m_blockIndex].begin;
                }
                break;
            case Mode::FilteredDense:
                ++m_current;
                advanceDense();
                break;
            case Mode::Unspecified:
            case Mode::Empty:
                m_done = true;
                break;
            }
            return *this;
        }

        [[nodiscard]] bool operator!=(const Iterator& other) const noexcept
        {
            return m_done != other.m_done;
        }

    private:
        void advanceDense() noexcept
        {
            while (m_current < m_limit &&
                   (m_filter[m_current] == 0 || !m_subjects[m_current]))
            {
                ++m_current;
            }
            m_done = m_current >= m_limit;
        }

        Mode m_mode = Mode::Empty;
        container::Span<const size_t> m_slots;
        container::Span<const Block> m_blocks;
        container::Span<const uint8_t> m_filter;
        container::Span<const ObjectId> m_subjects;
        size_t m_index = 0;
        size_t m_blockIndex = 0;
        size_t m_current = 0;
        size_t m_limit = 0;
        bool m_done = true;
    };

    AIExecutionSlotRange() noexcept = default;

    [[nodiscard]] static AIExecutionSlotRange none() noexcept
    {
        AIExecutionSlotRange range;
        range.m_mode = Mode::Empty;
        return range;
    }

    [[nodiscard]] static AIExecutionSlotRange full(size_t count) noexcept
    {
        AIExecutionSlotRange range;
        range.m_mode = Mode::Full;
        range.m_fullCount = count;
        range.m_count = count;
        return range;
    }

    [[nodiscard]] static AIExecutionSlotRange slots(
        container::Span<const size_t> values) noexcept
    {
        AIExecutionSlotRange range;
        range.m_mode = values.empty() ? Mode::Empty : Mode::Slots;
        range.m_slots = values;
        range.m_count = values.size();
        return range;
    }

    [[nodiscard]] static AIExecutionSlotRange directSlots(
        container::Span<const size_t> values) noexcept
    {
        AIExecutionSlotRange range = slots(values);
        if (!values.empty())
            range.m_mode = Mode::DirectSlots;
        return range;
    }

    [[nodiscard]] static AIExecutionSlotRange blocks(
        container::Span<const Block> values, size_t count) noexcept
    {
        AIExecutionSlotRange range;
        range.m_mode = count == 0 ? Mode::Empty : Mode::Blocks;
        range.m_blocks = values;
        range.m_count = count;
        return range;
    }

    [[nodiscard]] static AIExecutionSlotRange filteredDense(
        container::Span<const uint8_t> filter,
        container::Span<const ObjectId> subjects,
        size_t activeCount) noexcept
    {
        AIExecutionSlotRange range;
        range.m_mode = activeCount == 0 ? Mode::Empty : Mode::FilteredDense;
        range.m_filter = filter;
        range.m_subjects = subjects;
        range.m_fullCount = subjects.size();
        range.m_count = activeCount;
        return range;
    }

    [[nodiscard]] AIExecutionSlotRange withDefaultFullCount(size_t count) const noexcept
    {
        return m_mode == Mode::Unspecified ? full(count) : *this;
    }

    [[nodiscard]] bool specified() const noexcept { return m_mode != Mode::Unspecified; }
    [[nodiscard]] bool empty() const noexcept { return m_mode == Mode::Empty || m_count == 0; }
    [[nodiscard]] size_t size() const noexcept { return m_count; }
    [[nodiscard]] Mode mode() const noexcept { return m_mode; }
    [[nodiscard]] Iterator begin() const noexcept { return Iterator{*this}; }
    [[nodiscard]] Iterator end() const noexcept { return {}; }

private:
    Mode m_mode = Mode::Unspecified;
    container::Span<const size_t> m_slots;
    container::Span<const Block> m_blocks;
    container::Span<const uint8_t> m_filter;
    container::Span<const ObjectId> m_subjects;
    size_t m_fullCount = 0;
    size_t m_count = 0;
};

[[nodiscard]] inline AIExecutionSlotRange executionSlotRange(
    container::Span<const size_t> slots, size_t fullCount) noexcept
{
    return slots.empty() ? AIExecutionSlotRange::full(fullCount)
                         : AIExecutionSlotRange::slots(slots);
}

[[nodiscard]] inline AIExecutionSlotRange executionSlotRange(
    const AIExecutionSlotRange& slots, size_t fullCount) noexcept
{
    return slots.withDefaultFullCount(fullCount);
}

struct AIStateContext final
{
    uint64_t confirmedTick = 0;
    uint32_t idleTargetScanIntervalTicks = 3;
    uint32_t forceIdleBeforeAcquireTicks = 1;
    ObjectId subject = INVALID_OBJECT_ID;
    AIFixedPosition subjectPosition{};
    AIFixedPosition resolvedMoveTarget{};
    uint32_t ticksPerSecond = 30;
    bool effectivelyDead = false;
    bool mobile = false;
    bool moveTargetValid = false;
    bool idleAutoAcquireEnabled = false;
    bool idleTargetAvailable = false;
    bool canTurnInPlace = false;
    AIStateServicePorts services{};
    AIPathServicePorts pathServices{};
};

enum class AIStateStepKind : uint8_t
{
    Continue,
    Success,
    Failure,
    SleepUntil,
    Blocked,
    Transition,
    Unsupported,
};

struct AIStateStepResult final
{
    AIStateStepKind kind = AIStateStepKind::Continue;
    AIStateId target = AIStateId::Invalid;
    uint64_t wakeTick = 0;

    [[nodiscard]] static constexpr AIStateStepResult continueState() noexcept
    {
        return {};
    }

    [[nodiscard]] static constexpr AIStateStepResult success() noexcept
    {
        return {.kind = AIStateStepKind::Success};
    }

    [[nodiscard]] static constexpr AIStateStepResult failure() noexcept
    {
        return {.kind = AIStateStepKind::Failure};
    }

    [[nodiscard]] static constexpr AIStateStepResult sleepUntil(uint64_t tick) noexcept
    {
        return {.kind = AIStateStepKind::SleepUntil, .wakeTick = tick};
    }

    [[nodiscard]] static constexpr AIStateStepResult blocked() noexcept
    {
        return {.kind = AIStateStepKind::Blocked};
    }

    [[nodiscard]] static constexpr AIStateStepResult transitionTo(AIStateId state) noexcept
    {
        return {.kind = AIStateStepKind::Transition, .target = state};
    }

    [[nodiscard]] static constexpr AIStateStepResult unsupported() noexcept
    {
        return {.kind = AIStateStepKind::Unsupported};
    }
};

} // namespace engine::ai
