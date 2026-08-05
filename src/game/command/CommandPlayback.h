#pragma once

#include "core/container/container_types.h"

#include "GameCommand.h"

#include <cstddef>
namespace engine {

class GameCommandQueue;

class CommandPlayback {
public:
    void clear();
    void load(container::Vector<GameCommand> commands, CommandSource source);
    void submitDueCommands(GameTick tick, GameCommandQueue& queue);
    [[nodiscard]] size_t takeExpiredCount() noexcept;

    bool active() const { return m_active; }
    bool finished() const { return m_active && m_nextCommand >= m_commands.size(); }
    size_t commandCount() const { return m_commands.size(); }

private:
    container::Vector<GameCommand> m_commands;
    size_t m_nextCommand = 0;
    size_t m_expiredSinceLastSubmit = 0;
    bool m_active = false;
};

} // namespace engine
