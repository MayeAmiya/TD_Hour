#pragma once

#include "core/container/container_types.h"
#include "game/command/GameCommand.h"

namespace engine {

// Commands are recorded/checksummed at their original confirmed input tick,
// but FREEZE_TIME pauses the world-side dispatcher.  This value owner retains
// their exact chronological order and creates one short-lived, re-timestamped
// execution batch when the world first unfreezes.
class DeferredFrozenCommandBuffer final {
public:
    void clear() noexcept { m_commands.clear(); }

    void defer(container::Span<const GameCommand> commands) {
        m_commands.insert(m_commands.end(), commands.begin(), commands.end());
    }

    [[nodiscard]] container::Vector<GameCommand> takeForExecution(
        GameTick executionTick) {
        if (executionTick == 0) return {};
        container::Vector<GameCommand> result;
        result.swap(m_commands);
        for (GameCommand& command : result) command.tick = executionTick;
        return result;
    }

    [[nodiscard]] bool empty() const noexcept { return m_commands.empty(); }
    [[nodiscard]] size_t size() const noexcept { return m_commands.size(); }

private:
    container::Vector<GameCommand> m_commands;
};

} // namespace engine
