#include "core/container/hash_containers.h"
#include "LockstepFrameBuffer.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace engine {

namespace {

void sortCommands(container::Vector<GameCommand>& commands)
{
    std::sort(commands.begin(), commands.end(), [](const GameCommand& a, const GameCommand& b) {
        if (a.player.value != b.player.value) return a.player.value < b.player.value;
        return a.sequence < b.sequence;
    });
}

} // namespace

void LockstepFrameBuffer::configure(PlayerId localPlayer, GameTick frameSendRate)
{
    reset();
    m_localPlayer = localPlayer;
    m_frameSendRate = frameSendRate;
}

void LockstepFrameBuffer::reset()
{
    m_nextLocalSequence = 1;
    m_nextFrameToSeal = FirstConfirmedGameTick;
    m_pendingLocalCommandCount = 0;
    m_pendingConfirmedEntryCount = 0;
    m_lastConsumedConfirmedTick.reset();
    m_localPending.clear();
    m_confirmedFrames.clear();
}

std::optional<GameCommand> LockstepFrameBuffer::submitLocal(GameCommand command, GameTick currentLogicTick)
{
    return submitLocalResolved(
        std::move(command), currentLogicTick).command;
}

LockstepLocalSubmitResult LockstepFrameBuffer::submitLocalResolved(
    GameCommand command, GameTick currentLogicTick)
{
    if (!m_localPlayer || m_localPlayer.value >= MAX_SLOTS ||
        m_frameSendRate == 0) {
        return {.rejection = LockstepLocalSubmitRejection::InvalidAuthority};
    }
    if (currentLogicTick >
        std::numeric_limits<GameTick>::max() - m_frameSendRate) {
        return {.rejection = LockstepLocalSubmitRejection::TickOverflow};
    }
    command.source = CommandSource::Local;
    command.player = m_localPlayer;
    command.tick = currentLogicTick + m_frameSendRate;
    if (command.tick < m_nextFrameToSeal) {
        return {.rejection = LockstepLocalSubmitRejection::FrameSealed};
    }
    if (m_nextLocalSequence == 0) {
        // Zero is reserved as an unassigned ingress sequence. Do not wrap a
        // live lockstep session into a duplicate command key.
        return {.rejection = LockstepLocalSubmitRejection::SequenceExhausted};
    }
    if (m_pendingLocalCommandCount >= MaximumPendingLocalCommands) {
        return {.rejection = LockstepLocalSubmitRejection::CapacityExceeded};
    }
    const auto pending = m_localPending.find(command.tick);
    if (pending != m_localPending.end() && pending->second.size() >= MaximumCommandsPerFrame) {
        return {.rejection = LockstepLocalSubmitRejection::CapacityExceeded};
    }
    command.sequence = m_nextLocalSequence++;

    m_localPending[command.tick].push_back(command);
    ++m_pendingLocalCommandCount;
    return {.command = std::move(command)};
}

container::Vector<LocalCommandFrame> LockstepFrameBuffer::sealLocalFramesThrough(GameTick tick)
{
    container::Vector<LocalCommandFrame> frames;
    while (m_nextFrameToSeal <= tick) {
        LocalCommandFrame frame;
        frame.tick = m_nextFrameToSeal;
        if (const auto pending = m_localPending.find(frame.tick); pending != m_localPending.end()) {
            frame.commands = pending->second;
        }
        frames.push_back(std::move(frame));
        ++m_nextFrameToSeal;
    }
    return frames;
}

bool LockstepFrameBuffer::receiveConfirmedFrame(ConfirmedCommandFrame frame)
{
    if (!validateConfirmedFrame(frame) ||
        (m_lastConsumedConfirmedTick && frame.tick <= *m_lastConsumedConfirmedTick) ||
        m_confirmedFrames.contains(frame.tick) ||
        m_confirmedFrames.size() >= MaximumPendingConfirmedFrames) {
        return false;
    }
    const size_t entries = frameEntryCount(frame);
    if (entries > MaximumPendingConfirmedEntries - m_pendingConfirmedEntryCount) {
        return false;
    }
    m_pendingConfirmedEntryCount += entries;
    m_confirmedFrames.emplace(frame.tick, std::move(frame));
    return true;
}

bool LockstepFrameBuffer::takeReadyFrame(
    GameTick tick, container::Vector<GameCommand>& commands,
    container::Vector<GameCommand>* rejectedLocalCommands)
{
    if (rejectedLocalCommands) rejectedLocalCommands->clear();
    if (m_lastConsumedConfirmedTick && tick <= *m_lastConsumedConfirmedTick) {
        commands.clear();
        return false;
    }
    const auto found = m_confirmedFrames.find(tick);
    if (found == m_confirmedFrames.end()) {
        commands.clear();
        return false;
    }

    const size_t entries = frameEntryCount(found->second);
    if (!assembleFrame(
            found->second, commands, rejectedLocalCommands)) {
        // The remote frame passed static ingress validation but cannot be
        // reconciled with this client's sealed local submission. Retaining it
        // would block this tick forever and would also reject a legitimate
        // retransmission of the corrected authoritative frame.
        m_pendingConfirmedEntryCount -= entries;
        m_confirmedFrames.erase(found);
        commands.clear();
        return false;
    }

    if (const auto pending = m_localPending.find(tick); pending != m_localPending.end()) {
        m_pendingLocalCommandCount -= pending->second.size();
        m_localPending.erase(pending);
    }
    m_pendingConfirmedEntryCount -= entries;
    m_confirmedFrames.erase(found);
    m_lastConsumedConfirmedTick = tick;
    return true;
}

bool LockstepFrameBuffer::assembleFrame(
    const ConfirmedCommandFrame& frame,
    container::Vector<GameCommand>& commands,
    container::Vector<GameCommand>* rejectedLocalCommands) const
{
    commands.clear();
    if (!m_localPlayer || m_localPlayer.value >= MAX_SLOTS) {
        return false;
    }

    container::Array<uint16_t, MAX_SLOTS> receivedCounts{};
    for (const auto& command : frame.commands) {
        if (command.player.value >= MAX_SLOTS || command.tick != frame.tick ||
            command.sequence == 0 ||
            receivedCounts[command.player.value] == std::numeric_limits<uint16_t>::max()) {
            return false;
        }
        ++receivedCounts[command.player.value];
    }

    if (frame.includesLocalCommands) {
        if (receivedCounts != frame.commandCounts) {
            return false;
        }
        commands = frame.commands;
        for (auto& command : commands) {
            command.source = CommandSource::Network;
        }
        sortCommands(commands);
        return true;
    }

    const auto pending = m_localPending.find(frame.tick);
    if (pending == m_localPending.end() && frame.commandCounts[m_localPlayer.value] != 0) {
        return false;
    }
    if (frame.acceptedLocalSequences.size() != frame.commandCounts[m_localPlayer.value]) {
        return false;
    }

    container::HashSet<uint32_t> accepted(frame.acceptedLocalSequences.begin(), frame.acceptedLocalSequences.end());
    if (accepted.size() != frame.acceptedLocalSequences.size() || accepted.contains(0)) {
        return false;
    }

    if (pending != m_localPending.end()) {
        for (const auto& command : pending->second) {
            if (accepted.contains(command.sequence)) {
                commands.push_back(command);
            } else if (rejectedLocalCommands) {
                rejectedLocalCommands->push_back(command);
            }
        }
    }
    if (commands.size() != frame.commandCounts[m_localPlayer.value]) {
        return false;
    }

    for (size_t player = 0; player < MAX_SLOTS; ++player) {
        if (player == m_localPlayer.value) {
            continue;
        }
        if (receivedCounts[player] != frame.commandCounts[player]) {
            return false;
        }
    }
    if (receivedCounts[m_localPlayer.value] != 0) {
        return false;
    }

    commands.insert(commands.end(), frame.commands.begin(), frame.commands.end());
    if (!validateFrameCommandKeys(commands, frame.tick)) {
        return false;
    }
    for (auto& command : commands) {
        command.source = CommandSource::Network;
    }
    sortCommands(commands);
    return true;
}

bool LockstepFrameBuffer::validateFrameCommandKeys(
    const container::Vector<GameCommand>& commands, GameTick tick)
{
    if (commands.size() > MaximumCommandsPerFrame) return false;
    container::HashSet<uint64_t> seen;
    seen.reserve(commands.size());
    for (const GameCommand& command : commands) {
        if (command.tick != tick || command.player.value >= MAX_SLOTS || command.sequence == 0) {
            return false;
        }
        const uint64_t key = (static_cast<uint64_t>(command.player.value) << 32u) |
                             static_cast<uint64_t>(command.sequence);
        if (!seen.insert(key).second) return false;
    }
    return true;
}

bool LockstepFrameBuffer::validateAcceptedSequences(
    const container::Vector<uint32_t>& sequences)
{
    if (sequences.size() > MaximumCommandsPerFrame) return false;
    container::HashSet<uint32_t> unique;
    unique.reserve(sequences.size());
    for (const uint32_t sequence : sequences) {
        if (sequence == 0 || !unique.insert(sequence).second) return false;
    }
    return true;
}

bool LockstepFrameBuffer::validateConfirmedFrame(const ConfirmedCommandFrame& frame) const
{
    if (!m_localPlayer || m_localPlayer.value >= MAX_SLOTS || m_frameSendRate == 0 ||
        !validateFrameCommandKeys(frame.commands, frame.tick) ||
        !validateAcceptedSequences(frame.acceptedLocalSequences)) {
        return false;
    }

    container::Array<uint16_t, MAX_SLOTS> receivedCounts{};
    for (const GameCommand& command : frame.commands) {
        // validateFrameCommandKeys() already bounds the command count far
        // below uint16_t saturation; retain this check in case its ceiling is
        // raised in the future without changing the wire count type.
        if (receivedCounts[command.player.value] == std::numeric_limits<uint16_t>::max()) {
            return false;
        }
        ++receivedCounts[command.player.value];
    }

    if (frame.includesLocalCommands) {
        return frame.acceptedLocalSequences.empty() && receivedCounts == frame.commandCounts;
    }

    if (receivedCounts[m_localPlayer.value] != 0 ||
        frame.acceptedLocalSequences.size() != frame.commandCounts[m_localPlayer.value]) {
        return false;
    }
    for (size_t player = 0; player < MAX_SLOTS; ++player) {
        if (player != m_localPlayer.value && receivedCounts[player] != frame.commandCounts[player]) {
            return false;
        }
    }
    return true;
}

size_t LockstepFrameBuffer::frameEntryCount(const ConfirmedCommandFrame& frame) noexcept
{
    return frame.commands.size() + frame.acceptedLocalSequences.size();
}

} // namespace engine
