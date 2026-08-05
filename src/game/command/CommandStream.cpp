#include "core/container/container_types.h"
#include "CommandStream.h"
#include "CommandCodec.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace engine {

namespace {

constexpr size_t kHeaderBytes = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint32_t);
constexpr size_t kRecordLengthBytes = sizeof(uint32_t);
constexpr size_t kInitialDecodedReserve = 4096;

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

bool readU16(const uint8_t* data, size_t size, size_t& offset, uint16_t& value)
{
    if (offset + 2 > size) return false;
    value = static_cast<uint16_t>(data[offset]) |
            static_cast<uint16_t>(data[offset + 1] << 8);
    offset += 2;
    return true;
}

bool readU32(const uint8_t* data, size_t size, size_t& offset, uint32_t& value)
{
    if (offset + 4 > size) return false;
    value = static_cast<uint32_t>(data[offset]) |
            (static_cast<uint32_t>(data[offset + 1]) << 8) |
            (static_cast<uint32_t>(data[offset + 2]) << 16) |
            (static_cast<uint32_t>(data[offset + 3]) << 24);
    offset += 4;
    return true;
}

} // namespace

container::Vector<uint8_t> CommandStream::encode(const container::Vector<GameCommand>& commands)
{
    if (commands.size() > MaximumCommandCount) return {};

    container::Vector<uint8_t> out;
    out.reserve(kHeaderBytes);
    writeU32(out, Magic);
    writeU16(out, Version);
    writeU32(out, static_cast<uint32_t>(commands.size()));

    for (const auto& command : commands) {
        const auto encoded = CommandCodec::encode(command);
        // Returning an empty vector is the codec's explicit failure signal.
        // Never serialize a zero-byte placeholder: such a stream would be
        // writable but impossible to replay.
        if (encoded.empty() || encoded.size() > std::numeric_limits<uint32_t>::max() ||
            encoded.size() > MaximumEncodedBytes - kRecordLengthBytes ||
            out.size() > MaximumEncodedBytes - kRecordLengthBytes - encoded.size()) {
            return {};
        }
        writeU32(out, static_cast<uint32_t>(encoded.size()));
        out.insert(out.end(), encoded.begin(), encoded.end());
    }
    return out;
}

CommandStreamResult CommandStream::decode(const uint8_t* data, size_t size)
{
    CommandStreamResult result;
    if ((!data && size != 0) || size > MaximumEncodedBytes) {
        result.error = !data ? "null command stream data" : "command stream exceeds maximum encoded size";
        return result;
    }
    size_t offset = 0;
    uint32_t magic = 0;
    uint16_t version = 0;
    uint32_t count = 0;

    if (!readU32(data, size, offset, magic) ||
        !readU16(data, size, offset, version) ||
        !readU32(data, size, offset, count)) {
        result.error = "truncated command stream header";
        return result;
    }
    if (magic != Magic) {
        result.error = "invalid command stream magic";
        return result;
    }
    if (version != Version) {
        result.error = "unsupported command stream version";
        return result;
    }

    if (count > MaximumCommandCount) {
        result.error = "command stream declares too many commands";
        return result;
    }
    // Every declared record must at least contain its uint32 byte length.
    // Check before reserve so a forged count cannot force a huge allocation.
    if (count > (size - offset) / kRecordLengthBytes) {
        result.error = "truncated command stream record table";
        return result;
    }

    container::Vector<GameCommand> commands;
    commands.reserve(std::min<size_t>(count, kInitialDecodedReserve));
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t commandSize = 0;
        if (!readU32(data, size, offset, commandSize)) {
            result.error = "truncated command record size";
            return result;
        }
        if (commandSize > size - offset) {
            result.error = "truncated command record";
            return result;
        }

        auto decoded = CommandCodec::decode(data + offset, commandSize);
        if (!decoded.ok) {
            result.error = decoded.error;
            return result;
        }
        if (decoded.bytesRead != commandSize) {
            result.error = "command record did not consume its declared byte length";
            return result;
        }
        commands.push_back(std::move(decoded.command));
        offset += commandSize;
    }

    if (offset != size) {
        result.error = "unexpected trailing command stream data";
        return result;
    }

    result.commands = std::move(commands);
    result.ok = true;
    return result;
}

} // namespace engine
