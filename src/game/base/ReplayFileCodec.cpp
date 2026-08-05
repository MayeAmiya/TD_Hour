#include "core/container/container_types.h"
#include "game/base/ReplayFileCodec.h"

#include "game/command/CommandStream.h"
#include "game/player/MatchSetupCodec.h"

#include <limits>
#include <utility>

namespace game {
namespace {

void writeU16(container::Vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
}

void writeU32(container::Vector<uint8_t>& out, uint32_t value) {
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
    }
}

class Reader final {
public:
    Reader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

    bool readU16(uint16_t& value) {
        if (remaining() < 2) return false;
        value = static_cast<uint16_t>(m_data[m_offset]) |
                static_cast<uint16_t>(m_data[m_offset + 1] << 8u);
        m_offset += 2;
        return true;
    }

    bool readU32(uint32_t& value) {
        if (remaining() < 4) return false;
        value = 0;
        for (uint32_t shift = 0; shift < 32; shift += 8) {
            value |= static_cast<uint32_t>(m_data[m_offset++]) << shift;
        }
        return true;
    }

    [[nodiscard]] const uint8_t* currentData() const noexcept { return m_data + m_offset; }
    [[nodiscard]] size_t remaining() const noexcept { return m_size - m_offset; }

    bool skip(size_t count) {
        if (count > remaining()) return false;
        m_offset += count;
        return true;
    }

private:
    const uint8_t* m_data = nullptr;
    size_t m_size = 0;
    size_t m_offset = 0;
};

[[nodiscard]] engine::GameStartInfo replayStartInfoFromSetup(
    const engine::ResolvedMatchSetup& setup) {
    engine::GameStartInfo result;
    result.mode = engine::GameMode::Replay;
    result.mapName = setup.mapName;
    result.mapCRC = setup.mapCrc;
    result.mapSize = setup.mapSize;
    result.difficulty = setup.difficulty;
    result.rankPoints = setup.rankPoints;
    result.gameSpeedFPS = setup.gameSpeedFps;
    result.seed = static_cast<int>(setup.seed);
    result.superweaponRestricted = setup.superweaponRestricted;
    result.oldFactionsOnly = setup.oldFactionsOnly;
    // Replay observers are local presentation state, never serialized match
    // input.  GameSession sees an invalid local slot and creates no command
    // authority for the recording player's old seat.
    result.localPlayerSlot = -1;
    return result;
}

} // namespace

container::Vector<uint8_t> ReplayFileCodec::encode(
    const engine::ResolvedMatchSetup& resolvedMatchSetup,
    const container::Vector<engine::GameCommand>& commands) {
    container::Vector<uint8_t> setupBytes;
    container::String setupError;
    if (!engine::MatchSetupCodec::encode(resolvedMatchSetup, setupBytes, &setupError) ||
        setupBytes.size() > std::numeric_limits<uint32_t>::max()) {
        return {};
    }

    const container::Vector<uint8_t> commandBytes = engine::CommandStream::encode(commands);
    if (commandBytes.empty() || commandBytes.size() > std::numeric_limits<uint32_t>::max()) return {};

    container::Vector<uint8_t> output;
    output.reserve(12 + setupBytes.size() + commandBytes.size());
    writeU32(output, Magic);
    writeU16(output, Version);
    writeU16(output, RulesVersion);
    writeU32(output, static_cast<uint32_t>(setupBytes.size()));
    output.insert(output.end(), setupBytes.begin(), setupBytes.end());
    writeU32(output, static_cast<uint32_t>(commandBytes.size()));
    output.insert(output.end(), commandBytes.begin(), commandBytes.end());
    return output;
}

ReplayFileResult ReplayFileCodec::decode(const uint8_t* data, size_t size) {
    ReplayFileResult result;
    if (!data || size == 0) {
        result.error = "empty replay file";
        return result;
    }

    Reader reader(data, size);
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t rulesVersion = 0;
    if (!reader.readU32(magic) || !reader.readU16(version) || !reader.readU16(rulesVersion)) {
        result.error = "truncated replay header";
        return result;
    }
    if (magic != Magic) {
        result.error = "invalid replay magic";
        return result;
    }
    if (version != Version || rulesVersion != RulesVersion) {
        result.error = "unsupported replay version";
        return result;
    }

    uint32_t setupSize = 0;
    if (!reader.readU32(setupSize) || setupSize > reader.remaining()) {
        result.error = "truncated replay match setup";
        return result;
    }
    container::String setupError;
    auto setup = engine::MatchSetupCodec::decode(
        container::Span<const uint8_t>(reader.currentData(), setupSize), &setupError);
    if (!setup || !reader.skip(setupSize)) {
        result.error = setup ? "invalid replay match setup length"
                             : "invalid replay match setup: " + setupError;
        return result;
    }

    uint32_t commandSize = 0;
    if (!reader.readU32(commandSize) || commandSize > reader.remaining()) {
        result.error = "truncated replay command stream";
        return result;
    }
    const auto commands = engine::CommandStream::decode(reader.currentData(), commandSize);
    if (!commands.ok || !reader.skip(commandSize) || reader.remaining() != 0) {
        result.error = commands.ok ? "unexpected trailing replay data" : commands.error;
        return result;
    }

    result.ok = true;
    result.replay.startInfo = replayStartInfoFromSetup(*setup);
    result.replay.resolvedMatchSetup = std::move(*setup);
    result.replay.commands = commands.commands;
    return result;
}

} // namespace game
