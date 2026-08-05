#include "core/container/container_types.h"
#include "CommandPlayback.h"
#include "GameCommandQueue.h"

#include <algorithm>
#include <utility>

namespace engine {

void CommandPlayback::clear()
{
    m_commands.clear();
    m_nextCommand = 0;
    m_expiredSinceLastSubmit = 0;
    m_active = false;
}

void CommandPlayback::load(container::Vector<GameCommand> commands, CommandSource source)
{
    m_commands = std::move(commands);
    m_nextCommand = 0;
    m_expiredSinceLastSubmit = 0;
    m_active = true;

    std::stable_sort(m_commands.begin(), m_commands.end(), [](const GameCommand& a, const GameCommand& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.player.value != b.player.value) return a.player.value < b.player.value;
        return a.sequence < b.sequence;
    });

    for (auto& command : m_commands) {
        command.source = source;
    }
}

void CommandPlayback::submitDueCommands(GameTick tick, GameCommandQueue& queue)
{
    if (!m_active) {
        return;
    }

    while (m_nextCommand < m_commands.size() && m_commands[m_nextCommand].tick < tick) {
        ++m_expiredSinceLastSubmit;
        ++m_nextCommand;
    }
    while (m_nextCommand < m_commands.size() && m_commands[m_nextCommand].tick == tick) {
        queue.submit(m_commands[m_nextCommand]);
        ++m_nextCommand;
    }
}

size_t CommandPlayback::takeExpiredCount() noexcept {
    const size_t result = m_expiredSinceLastSubmit;
    m_expiredSinceLastSubmit = 0;
    return result;
}

} // namespace engine
