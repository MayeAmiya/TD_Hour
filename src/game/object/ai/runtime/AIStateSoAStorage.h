#pragma once

#include <cstddef>

#include "core/container/container_types.h"
#include "game/object/ai/runtime/AIStateData.h"
#include "game/object/ai/runtime/AIStateMachine.h"
#include "core/ecs/ObjectId.h"

namespace engine::ai
{

struct AIStateSoASlot final
{
    ObjectId subject = INVALID_OBJECT_ID;
    AIStateMachineRuntime& runtime;
    AIStateData& data;
};

struct AIStateSoAConstSlot final
{
    ObjectId subject = INVALID_OBJECT_ID;
    const AIStateMachineRuntime& runtime;
    const AIStateData& data;
};

// Owning staging storage with three independent dense columns. reset() is a
// setup/structural operation and may allocate; indexed simulation access does
// not allocate. Subjects must be supplied in strict ObjectId order so batch
// behavior is deterministic without a hot-path sort.
class AIStateSoAStorage final
{
public:
    [[nodiscard]] bool reset(container::Span<const ObjectId> orderedSubjects)
    {
        for (size_t index = 0; index < orderedSubjects.size(); ++index)
        {
            if (!orderedSubjects[index] || (index != 0 && orderedSubjects[index - 1] >= orderedSubjects[index]))
            {
                return false;
            }
        }

        m_subjects.assign(orderedSubjects.begin(), orderedSubjects.end());
        m_runtimes.assign(orderedSubjects.size(), AIStateMachineRuntime{});
        m_data.assign(orderedSubjects.size(), AIStateData{});
        return true;
    }

    [[nodiscard]] size_t size() const noexcept
    {
        return m_subjects.size();
    }
    [[nodiscard]] bool empty() const noexcept
    {
        return m_subjects.empty();
    }

    [[nodiscard]] AIStateSoASlot slot(size_t index) noexcept
    {
        return {m_subjects[index], m_runtimes[index], m_data[index]};
    }

    [[nodiscard]] AIStateSoAConstSlot slot(size_t index) const noexcept
    {
        return {m_subjects[index], m_runtimes[index], m_data[index]};
    }

    [[nodiscard]] container::Span<const ObjectId> subjects() const noexcept
    {
        return m_subjects;
    }
    [[nodiscard]] container::Span<AIStateMachineRuntime> runtimes() noexcept
    {
        return m_runtimes;
    }
    [[nodiscard]] container::Span<const AIStateMachineRuntime> runtimes() const noexcept
    {
        return m_runtimes;
    }
    [[nodiscard]] container::Span<AIStateData> data() noexcept
    {
        return m_data;
    }
    [[nodiscard]] container::Span<const AIStateData> data() const noexcept
    {
        return m_data;
    }

private:
    container::Vector<ObjectId> m_subjects;
    container::Vector<AIStateMachineRuntime> m_runtimes;
    container::Vector<AIStateData> m_data;
};

} // namespace engine::ai
