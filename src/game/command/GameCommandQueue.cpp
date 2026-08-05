#include "core/container/container_types.h"
#include "GameCommandQueue.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace engine {

void GameCommandQueue::clear()
{
    m_pending.clear();
    m_pendingKeys.clear();
    m_nextSequence = 1;
    m_expiredSinceLastDrain = 0;
    m_expiredCommands.clear();
    m_rejectedSinceLastDrain = 0;
}

bool GameCommandQueue::submit(GameCommand command)
{
    return static_cast<bool>(submitResolved(std::move(command)));
}

GameCommandQueueSubmitResult GameCommandQueue::submitResolved(
    GameCommand command)
{
    if (m_pending.size() >= MaximumPendingCommands) {
        ++m_rejectedSinceLastDrain;
        return {.rejection = GameCommandQueueRejection::CapacityExceeded};
    }
    if (command.sequence == 0) {
        command.sequence = allocateSequence(command.tick, command.player);
        if (command.sequence == 0) {
            ++m_rejectedSinceLastDrain;
            return {.rejection = GameCommandQueueRejection::SequenceExhausted};
        }
    } else if (hasPendingSequence(command.tick, command.player, command.sequence)) {
        ++m_rejectedSinceLastDrain;
        return {.rejection = GameCommandQueueRejection::DuplicateSequence};
    } else if (command.sequence >= m_nextSequence) {
        m_nextSequence = command.sequence == std::numeric_limits<uint32_t>::max()
            ? 1u
            : command.sequence + 1u;
    }
    m_pending.push_back(std::move(command));
    m_pendingKeys.insert(keyFor(m_pending.back()));
    return {.command = m_pending.back()};
}

container::Vector<GameCommand> GameCommandQueue::drainForTick(GameTick tick)
{
    container::Vector<GameCommand> ready;
    auto it = m_pending.begin();
    while (it != m_pending.end()) {
        if (it->tick == tick) {
            m_pendingKeys.erase(keyFor(*it));
            ready.push_back(std::move(*it));
            it = m_pending.erase(it);
        } else if (it->tick < tick) {
            m_pendingKeys.erase(keyFor(*it));
            ++m_expiredSinceLastDrain;
            m_expiredCommands.push_back(std::move(*it));
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }

    std::sort(ready.begin(), ready.end(), [](const GameCommand& a, const GameCommand& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.player.value != b.player.value) return a.player.value < b.player.value;
        return a.sequence < b.sequence;
    });
    return ready;
}

container::Vector<GameCommand> GameCommandQueue::takeExpiredCommands() {
    container::Vector<GameCommand> result;
    result.swap(m_expiredCommands);
    return result;
}

size_t GameCommandQueue::takeExpiredCount() noexcept {
    const size_t result = m_expiredSinceLastDrain;
    m_expiredSinceLastDrain = 0;
    return result;
}

size_t GameCommandQueue::takeRejectedCount() noexcept {
    const size_t result = m_rejectedSinceLastDrain;
    m_rejectedSinceLastDrain = 0;
    return result;
}

bool GameCommandQueue::hasPendingSequence(GameTick tick, PlayerId player,
                                           uint32_t sequence) const noexcept {
    return m_pendingKeys.contains({.tick = tick, .player = player.value, .sequence = sequence});
}

uint32_t GameCommandQueue::allocateSequence(GameTick tick, PlayerId player) noexcept {
    // A sequence is uint32_t and zero is reserved. The side index makes this
    // practically O(1), while remaining correct if callers supplied explicit
    // values that occupy the next locally generated key.
    for (uint64_t attempts = 0; attempts < std::numeric_limits<uint32_t>::max(); ++attempts) {
        if (m_nextSequence == 0) m_nextSequence = 1;
        const uint32_t candidate = m_nextSequence;
        m_nextSequence = candidate == std::numeric_limits<uint32_t>::max()
            ? 1u
            : candidate + 1u;
        if (!hasPendingSequence(tick, player, candidate)) return candidate;
    }
    return 0;
}

GameCommandQueue::PendingCommandKey GameCommandQueue::keyFor(const GameCommand& command) noexcept {
    return {.tick = command.tick, .player = command.player.value, .sequence = command.sequence};
}

size_t GameCommandQueue::PendingCommandKeyHash::operator()(const PendingCommandKey& key) const noexcept {
    // Keep the complete 32-bit tick and sequence plus the map-player byte;
    // equality still guards collisions. This is an internal membership index,
    // never an ordering source for confirmed simulation.
    uint64_t value = (static_cast<uint64_t>(key.tick) << 32u) |
                     static_cast<uint64_t>(key.sequence);
    value ^= static_cast<uint64_t>(key.player) * 0x9E3779B97F4A7C15ull;
    value ^= value >> 30u;
    value *= 0xBF58476D1CE4E5B9ull;
    value ^= value >> 27u;
    return static_cast<size_t>(value ^ (value >> 31u));
}

} // namespace engine
