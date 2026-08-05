#include "LocalFrameAuthority.h"

#include <utility>

namespace engine {

void LocalFrameAuthority::reset()
{
    m_commandQueue.clear();
}

bool LocalFrameAuthority::submit(GameCommand command)
{
    return m_commandQueue.submit(std::move(command));
}

GameCommandQueueSubmitResult LocalFrameAuthority::submitResolved(
    GameCommand command)
{
    return m_commandQueue.submitResolved(std::move(command));
}

ConfirmedCommandFrame LocalFrameAuthority::confirmFrame(GameTick tick)
{
    ConfirmedCommandFrame frame;
    frame.tick = tick;
    frame.includesLocalCommands = true;
    frame.commands = m_commandQueue.drainForTick(tick);
    frame.expiredCommandCount = m_commandQueue.takeExpiredCount();
    frame.expiredLocalCommands = m_commandQueue.takeExpiredCommands();
    frame.rejectedCommandCount = m_commandQueue.takeRejectedCount();
    for (const auto& command : frame.commands) {
        if (command.player.value < MAX_SLOTS) {
            ++frame.commandCounts[command.player.value];
        }
    }
    return frame;
}

} // namespace engine
