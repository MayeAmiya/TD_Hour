#include "core/container/hash_containers.h"
#include "LockstepPacketCodec.h"

#include "game/command/CommandCodec.h"

#include <limits>
#include <utility>

namespace engine {

namespace {

constexpr size_t MaxPacketStringLength = 1024 * 1024;
// Keep the byte decoder and the in-memory lockstep ingress on one explicit
// command ceiling. A future transport must not be able to bypass its queue
// budget merely by constructing ConfirmedCommandFrame directly.
constexpr size_t MaxPacketCommandCount = LockstepFrameBuffer::MaximumCommandsPerFrame;
// A command batch can carry several sealed ticks. Limit its aggregate too:
// applying the per-frame ceiling independently would otherwise permit a
// single hostile packet to allocate 4,096 * 4,096 commands.
constexpr size_t MaxPacketBatchCommandCount = LockstepFrameBuffer::MaximumPendingLocalCommands;
constexpr size_t MaxPacketEncodedBytes = 16ull * 1024ull * 1024ull;

void writeU8(container::Vector<uint8_t>& out, uint8_t value)
{
    out.push_back(value);
}

void writeU16(container::Vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void writeU32(container::Vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

void writeU64(container::Vector<uint8_t>& out, uint64_t value)
{
    for (uint32_t shift = 0; shift < 64; shift += 8) {
        writeU8(out, static_cast<uint8_t>((value >> shift) & 0xffu));
    }
}

void writeString(container::Vector<uint8_t>& out, const container::String& value)
{
    writeU32(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

void writeHeader(container::Vector<uint8_t>& out, LockstepPacketType type)
{
    writeU8(out, static_cast<uint8_t>(type));
    writeU16(out, LockstepPacketCodec::Version);
}

class Reader {
public:
    Reader(const uint8_t* data, size_t size)
        : m_data(data), m_size(size)
    {
    }

    bool readU8(uint8_t& value)
    {
        if (m_offset + 1 > m_size) return false;
        value = m_data[m_offset++];
        return true;
    }

    bool readU16(uint16_t& value)
    {
        if (m_offset + 2 > m_size) return false;
        value = static_cast<uint16_t>(m_data[m_offset]) |
                static_cast<uint16_t>(m_data[m_offset + 1] << 8);
        m_offset += 2;
        return true;
    }

    bool readU32(uint32_t& value)
    {
        if (m_offset + 4 > m_size) return false;
        value = static_cast<uint32_t>(m_data[m_offset]) |
                (static_cast<uint32_t>(m_data[m_offset + 1]) << 8) |
                (static_cast<uint32_t>(m_data[m_offset + 2]) << 16) |
                (static_cast<uint32_t>(m_data[m_offset + 3]) << 24);
        m_offset += 4;
        return true;
    }

    bool readU64(uint64_t& value)
    {
        if (m_offset + 8 > m_size) return false;
        value = 0;
        for (uint32_t shift = 0; shift < 64; shift += 8) {
            value |= static_cast<uint64_t>(m_data[m_offset++]) << shift;
        }
        return true;
    }

    bool readString(container::String& value)
    {
        uint32_t length = 0;
        if (!readU32(length) || length > MaxPacketStringLength || m_offset + length > m_size) {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(m_data + m_offset), length);
        m_offset += length;
        return true;
    }

    const uint8_t* currentData() const { return m_data + m_offset; }
    size_t remaining() const { return m_size - m_offset; }
    bool skip(size_t count)
    {
        if (count > remaining()) return false;
        m_offset += count;
        return true;
    }

private:
    const uint8_t* m_data = nullptr;
    size_t m_size = 0;
    size_t m_offset = 0;
};

bool readHeader(Reader& reader, LockstepPacketType expected, container::String& error)
{
    uint8_t type = 0;
    uint16_t version = 0;
    if (!reader.readU8(type) || !reader.readU16(version)) {
        error = "truncated lockstep packet header";
        return false;
    }
    if (type != static_cast<uint8_t>(expected)) {
        error = "unexpected lockstep packet type";
        return false;
    }
    if (version != LockstepPacketCodec::Version) {
        error = "unsupported lockstep packet version";
        return false;
    }
    return true;
}

[[nodiscard]] bool appendCommand(container::Vector<uint8_t>& out, const GameCommand& command)
{
    const auto encoded = CommandCodec::encode(command);
    if (encoded.empty() || encoded.size() > std::numeric_limits<uint32_t>::max() ||
        encoded.size() > MaxPacketEncodedBytes - sizeof(uint32_t) ||
        out.size() > MaxPacketEncodedBytes - sizeof(uint32_t) - encoded.size()) {
        return false;
    }
    writeU32(out, static_cast<uint32_t>(encoded.size()));
    out.insert(out.end(), encoded.begin(), encoded.end());
    return true;
}

bool readCommand(Reader& reader, GameCommand& command)
{
    uint32_t size = 0;
    if (!reader.readU32(size) || size > reader.remaining()) {
        return false;
    }
    const auto decoded = CommandCodec::decode(reader.currentData(), size);
    if (!decoded.ok || decoded.bytesRead != size || !reader.skip(size)) {
        return false;
    }
    command = decoded.command;
    return true;
}

[[nodiscard]] bool validPacketInput(const uint8_t* data, size_t size) noexcept
{
    return data != nullptr && size <= MaxPacketEncodedBytes;
}

void writeSyncSample(container::Vector<uint8_t>& out,
                     const LockstepSyncSample& sample)
{
    writeU32(out, sample.tick);
    writeU32(out, sample.commandChecksum);
    writeU32(out, sample.combinedChecksum);
    writeU64(out, sample.aiRuntime);
    writeU64(out, sample.navigation);
    writeU64(out, sample.movement);
    writeU64(out, sample.economy);
    writeU64(out, sample.players);
    writeU64(out, sample.worldCombined);
}

[[nodiscard]] bool readSyncSample(Reader& reader,
                                  LockstepSyncSample& sample)
{
    return reader.readU32(sample.tick) &&
           reader.readU32(sample.commandChecksum) &&
           reader.readU32(sample.combinedChecksum) &&
           reader.readU64(sample.aiRuntime) &&
           reader.readU64(sample.navigation) &&
           reader.readU64(sample.movement) &&
           reader.readU64(sample.economy) &&
           reader.readU64(sample.players) &&
           reader.readU64(sample.worldCombined);
}

[[nodiscard]] bool hasUniqueFrameCommandKeys(const container::Vector<GameCommand>& commands,
                                              GameTick tick)
{
    container::HashSet<uint64_t> keys;
    keys.reserve(commands.size());
    for (const GameCommand& command : commands) {
        if (command.tick != tick || command.player.value >= MAX_SLOTS || command.sequence == 0) {
            return false;
        }
        const uint64_t key = (static_cast<uint64_t>(command.player.value) << 32u) |
                             static_cast<uint64_t>(command.sequence);
        if (!keys.insert(key).second) return false;
    }
    return true;
}

[[nodiscard]] bool hasUniqueNonzeroSequences(const container::Vector<uint32_t>& sequences)
{
    container::HashSet<uint32_t> values;
    values.reserve(sequences.size());
    for (const uint32_t sequence : sequences) {
        if (sequence == 0 || !values.insert(sequence).second) return false;
    }
    return true;
}

} // namespace

container::Vector<uint8_t> LockstepPacketCodec::encodeClientHello(const GameStartInfo& info,
                                                            LockstepMatchIdentity matchIdentity)
{
    if (!matchIdentity.isValid() || info.localPlayerSlot < 0 ||
        info.localPlayerSlot >= MAX_SLOTS ||
        info.network.sessionId.size() > MaxPacketStringLength ||
        info.network.joinToken.size() > MaxPacketStringLength ||
        info.mapName.size() > MaxPacketStringLength) {
        return {};
    }
    container::Vector<uint8_t> out;
    writeHeader(out, LockstepPacketType::ClientHello);
    writeString(out, info.network.sessionId);
    writeString(out, info.network.joinToken);
    writeString(out, info.mapName);
    writeU32(out, info.mapCRC);
    writeU32(out, info.mapSize);
    writeU32(out, info.rulesCRC);
    writeU64(out, matchIdentity.simulationContentFingerprint);
    writeU64(out, matchIdentity.resolvedSetupSimulationDigest);
    writeU32(out, static_cast<uint32_t>(info.seed));
    writeU16(out, info.network.protocolVersion);
    writeU32(out, info.network.frameSendRate);
    writeU8(out, static_cast<uint8_t>(info.localPlayerSlot));
    return out.size() <= MaxPacketEncodedBytes ? out : container::Vector<uint8_t>{};
}

container::Vector<uint8_t> LockstepPacketCodec::encodeServerHello(const LockstepServerHello& hello)
{
    if (hello.error.size() > MaxPacketStringLength) return {};
    container::Vector<uint8_t> out;
    writeHeader(out, LockstepPacketType::ServerHello);
    writeU8(out, hello.accepted ? 1 : 0);
    writeU16(out, hello.protocolVersion);
    writeU32(out, hello.frameSendRate);
    writeString(out, hello.error);
    return out.size() <= MaxPacketEncodedBytes ? out : container::Vector<uint8_t>{};
}

container::Vector<uint8_t> LockstepPacketCodec::encodeCommandBatch(const container::Vector<LocalCommandFrame>& frames)
{
    if (frames.size() > MaxPacketCommandCount) return {};

    size_t totalCommands = 0;
    container::HashSet<GameTick> frameTicks;
    frameTicks.reserve(frames.size());
    for (const LocalCommandFrame& frame : frames) {
        if (frame.commands.size() > MaxPacketCommandCount ||
            frame.commands.size() > MaxPacketBatchCommandCount - totalCommands ||
            !frameTicks.insert(frame.tick).second ||
            !hasUniqueFrameCommandKeys(frame.commands, frame.tick)) {
            return {};
        }
        totalCommands += frame.commands.size();
    }

    container::Vector<uint8_t> out;
    writeHeader(out, LockstepPacketType::CommandBatch);
    writeU32(out, static_cast<uint32_t>(frames.size()));
    for (const auto& frame : frames) {
        writeU32(out, frame.tick);
        writeU32(out, static_cast<uint32_t>(frame.commands.size()));
        for (const auto& command : frame.commands) {
            if (!appendCommand(out, command)) return {};
        }
    }
    return out.size() <= MaxPacketEncodedBytes ? out : container::Vector<uint8_t>{};
}

container::Vector<uint8_t> LockstepPacketCodec::encodeConfirmedFrame(const ConfirmedCommandFrame& frame)
{
    if (frame.acceptedLocalSequences.size() > MaxPacketCommandCount ||
        frame.commands.size() > MaxPacketCommandCount ||
        frame.acceptedLocalSequences.size() >
            LockstepFrameBuffer::MaximumPendingConfirmedEntries - frame.commands.size() ||
        !hasUniqueNonzeroSequences(frame.acceptedLocalSequences) ||
        !hasUniqueFrameCommandKeys(frame.commands, frame.tick)) {
        return {};
    }

    if (frame.includesLocalCommands) {
        container::Array<uint16_t, MAX_SLOTS> receivedCounts{};
        for (const GameCommand& command : frame.commands) {
            if (receivedCounts[command.player.value] == std::numeric_limits<uint16_t>::max()) {
                return {};
            }
            ++receivedCounts[command.player.value];
        }
        if (!frame.acceptedLocalSequences.empty() || receivedCounts != frame.commandCounts) return {};
    }

    container::Vector<uint8_t> out;
    writeHeader(out, LockstepPacketType::ConfirmedFrame);
    writeU32(out, frame.tick);
    writeU8(out, frame.includesLocalCommands ? 1 : 0);
    for (const auto count : frame.commandCounts) {
        writeU16(out, count);
    }
    writeU32(out, static_cast<uint32_t>(frame.acceptedLocalSequences.size()));
    for (const auto sequence : frame.acceptedLocalSequences) {
        writeU32(out, sequence);
    }
    writeU32(out, static_cast<uint32_t>(frame.commands.size()));
    for (const auto& command : frame.commands) {
        if (!appendCommand(out, command)) return {};
    }
    return out.size() <= MaxPacketEncodedBytes ? out : container::Vector<uint8_t>{};
}

container::Vector<uint8_t> LockstepPacketCodec::encodeSyncSample(
    const LockstepSyncSample& sample)
{
    container::Vector<uint8_t> out;
    writeHeader(out, LockstepPacketType::SyncSample);
    writeSyncSample(out, sample);
    return out;
}

container::Vector<uint8_t> LockstepPacketCodec::encodeSyncMismatch(
    const LockstepSyncMismatch& mismatch)
{
    if (mismatch.tick != mismatch.reference.tick ||
        mismatch.tick != mismatch.divergent.tick ||
        mismatch.referenceSlot >= MAX_SLOTS ||
        mismatch.divergentSlot >= MAX_SLOTS ||
        mismatch.mismatchMask == 0)
        return {};
    container::Vector<uint8_t> out;
    writeHeader(out, LockstepPacketType::SyncMismatch);
    writeU32(out, mismatch.tick);
    writeU8(out, mismatch.referenceSlot);
    writeU8(out, mismatch.divergentSlot);
    writeU32(out, mismatch.mismatchMask);
    writeSyncSample(out, mismatch.reference);
    writeSyncSample(out, mismatch.divergent);
    return out;
}

LockstepPacketResult LockstepPacketCodec::decodeServerHello(const uint8_t* data, size_t size,
                                                             LockstepServerHello& hello)
{
    LockstepPacketResult result;
    if (!validPacketInput(data, size)) {
        result.error = "invalid server hello packet size";
        return result;
    }
    Reader reader(data, size);
    if (!readHeader(reader, LockstepPacketType::ServerHello, result.error)) {
        return result;
    }

    uint8_t accepted = 0;
    if (!reader.readU8(accepted) || accepted > 1 ||
        !reader.readU16(hello.protocolVersion) ||
        !reader.readU32(hello.frameSendRate) ||
        !reader.readString(hello.error) ||
        reader.remaining() != 0) {
        result.error = "truncated server hello";
        return result;
    }

    hello.accepted = accepted != 0;
    result.ok = true;
    return result;
}

LockstepPacketResult LockstepPacketCodec::decodeClientHello(const uint8_t* data, size_t size,
                                                             LockstepClientHello& hello)
{
    LockstepPacketResult result;
    if (!validPacketInput(data, size)) {
        result.error = "invalid client hello packet size";
        return result;
    }
    Reader reader(data, size);
    if (!readHeader(reader, LockstepPacketType::ClientHello, result.error)) {
        return result;
    }

    uint32_t seed = 0;
    if (!reader.readString(hello.sessionId) ||
        !reader.readString(hello.joinToken) ||
        !reader.readString(hello.mapName) ||
        !reader.readU32(hello.mapCRC) ||
        !reader.readU32(hello.mapSize) ||
        !reader.readU32(hello.rulesCRC) ||
        !reader.readU64(hello.matchIdentity.simulationContentFingerprint) ||
        !reader.readU64(hello.matchIdentity.resolvedSetupSimulationDigest) ||
        !reader.readU32(seed) ||
        !reader.readU16(hello.protocolVersion) ||
        !reader.readU32(hello.frameSendRate) ||
        !reader.readU8(hello.localPlayerSlot) || hello.localPlayerSlot >= MAX_SLOTS ||
        reader.remaining() != 0) {
        result.error = "invalid client hello";
        return result;
    }

    hello.seed = static_cast<int>(seed);
    if (!hello.matchIdentity.isValid()) {
        result.error = "client hello is missing canonical simulation identity";
        return result;
    }
    result.ok = true;
    return result;
}

LockstepPacketResult LockstepPacketCodec::decodeCommandBatch(const uint8_t* data, size_t size,
                                                              container::Vector<LocalCommandFrame>& frames)
{
    LockstepPacketResult result;
    if (!validPacketInput(data, size)) {
        result.error = "invalid command batch packet size";
        return result;
    }
    Reader reader(data, size);
    if (!readHeader(reader, LockstepPacketType::CommandBatch, result.error)) {
        return result;
    }

    uint32_t frameCount = 0;
    if (!reader.readU32(frameCount) || frameCount > MaxPacketCommandCount) {
        result.error = "invalid command batch frame count";
        return result;
    }

    frames.clear();
    frames.reserve(frameCount);
    container::HashSet<GameTick> frameTicks;
    frameTicks.reserve(frameCount);
    size_t totalCommands = 0;
    for (uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        LocalCommandFrame frame;
        uint32_t commandCount = 0;
        if (!reader.readU32(frame.tick) ||
            !reader.readU32(commandCount) ||
            commandCount > MaxPacketCommandCount ||
            commandCount > MaxPacketBatchCommandCount - totalCommands) {
            result.error = "invalid command frame header";
            return result;
        }
        totalCommands += commandCount;
        if (!frameTicks.insert(frame.tick).second) {
            result.error = "command batch contains the same tick more than once";
            return result;
        }
        frame.commands.resize(commandCount);
        for (auto& command : frame.commands) {
            if (!readCommand(reader, command) || command.tick != frame.tick) {
                result.error = "invalid command frame command";
                return result;
            }
        }
        if (!hasUniqueFrameCommandKeys(frame.commands, frame.tick)) {
            result.error = "command frame has duplicate or unassigned player sequence";
            return result;
        }
        frames.push_back(std::move(frame));
    }

    if (reader.remaining() != 0) {
        result.error = "unexpected trailing command batch data";
        return result;
    }
    result.ok = true;
    return result;
}

LockstepPacketResult LockstepPacketCodec::decodeConfirmedFrame(const uint8_t* data, size_t size,
                                                                ConfirmedCommandFrame& frame)
{
    LockstepPacketResult result;
    if (!validPacketInput(data, size)) {
        result.error = "invalid confirmed frame packet size";
        return result;
    }
    Reader reader(data, size);
    if (!readHeader(reader, LockstepPacketType::ConfirmedFrame, result.error)) {
        return result;
    }

    uint8_t includesLocal = 0;
    if (!reader.readU32(frame.tick) || !reader.readU8(includesLocal) || includesLocal > 1) {
        result.error = "truncated confirmed frame header";
        return result;
    }
    frame.includesLocalCommands = includesLocal != 0;

    for (auto& count : frame.commandCounts) {
        if (!reader.readU16(count)) {
            result.error = "truncated confirmed frame command counts";
            return result;
        }
    }

    uint32_t acceptedCount = 0;
    if (!reader.readU32(acceptedCount) || acceptedCount > MaxPacketCommandCount) {
        result.error = "invalid confirmed frame local command count";
        return result;
    }
    frame.acceptedLocalSequences.resize(acceptedCount);
    for (auto& sequence : frame.acceptedLocalSequences) {
        if (!reader.readU32(sequence)) {
            result.error = "truncated confirmed frame local sequences";
            return result;
        }
    }

    uint32_t commandCount = 0;
    if (!reader.readU32(commandCount) || commandCount > MaxPacketCommandCount ||
        acceptedCount > LockstepFrameBuffer::MaximumPendingConfirmedEntries - commandCount) {
        result.error = "invalid confirmed frame command count";
        return result;
    }
    frame.commands.resize(commandCount);
    for (auto& command : frame.commands) {
        if (!readCommand(reader, command) || command.tick != frame.tick) {
            result.error = "invalid confirmed frame command";
            return result;
        }
    }

    if (!hasUniqueNonzeroSequences(frame.acceptedLocalSequences) ||
        !hasUniqueFrameCommandKeys(frame.commands, frame.tick)) {
        result.error = "confirmed frame has duplicate or unassigned command sequence";
        return result;
    }

    if (reader.remaining() != 0) {
        result.error = "unexpected trailing confirmed frame data";
        return result;
    }

    result.ok = true;
    return result;
}

LockstepPacketResult LockstepPacketCodec::decodeSyncSample(
    const uint8_t* data, size_t size, LockstepSyncSample& sample)
{
    LockstepPacketResult result;
    if (!validPacketInput(data, size)) {
        result.error = "invalid sync sample packet size";
        return result;
    }
    Reader reader(data, size);
    if (!readHeader(reader, LockstepPacketType::SyncSample, result.error))
        return result;
    if (!readSyncSample(reader, sample) || reader.remaining() != 0) {
        result.error = "invalid sync sample";
        return result;
    }
    result.ok = true;
    return result;
}

LockstepPacketResult LockstepPacketCodec::decodeSyncMismatch(
    const uint8_t* data, size_t size, LockstepSyncMismatch& mismatch)
{
    LockstepPacketResult result;
    if (!validPacketInput(data, size)) {
        result.error = "invalid sync mismatch packet size";
        return result;
    }
    Reader reader(data, size);
    if (!readHeader(reader, LockstepPacketType::SyncMismatch, result.error))
        return result;
    if (!reader.readU32(mismatch.tick) ||
        !reader.readU8(mismatch.referenceSlot) ||
        !reader.readU8(mismatch.divergentSlot) ||
        !reader.readU32(mismatch.mismatchMask) ||
        !readSyncSample(reader, mismatch.reference) ||
        !readSyncSample(reader, mismatch.divergent) ||
        reader.remaining() != 0 ||
        mismatch.referenceSlot >= MAX_SLOTS ||
        mismatch.divergentSlot >= MAX_SLOTS ||
        mismatch.reference.tick != mismatch.tick ||
        mismatch.divergent.tick != mismatch.tick ||
        mismatch.mismatchMask == 0 ||
        mismatch.mismatchMask != lockstepSyncMismatchMask(
            mismatch.reference, mismatch.divergent)) {
        result.error = "invalid sync mismatch";
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace engine
