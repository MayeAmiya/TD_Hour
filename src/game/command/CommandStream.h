#pragma once

#include "core/container/container_types.h"

#include "GameCommand.h"

#include <cstddef>
#include <cstdint>
namespace engine {

struct CommandStreamResult {
    bool ok = false;
    container::Vector<GameCommand> commands;
    container::String error;
};

class CommandStream {
public:
    static constexpr uint32_t Magic = 0x53435447; // "GTCS" little-endian
    // v2 adds explicit resource limits and strict end-of-stream validation.
    // v3 carries CommandCodec v10 activation metadata. v4 carries the
    // actorless CommandCodec v15 PurchaseScience transaction. v5 carries
    // CommandCodec v16 canonical Q32.32 positions and placement yaw.
    // Older recordings are deliberately rejected rather than being parsed
    // with a weaker integrity contract.
    static constexpr uint16_t Version = 5;
    // Replay input is untrusted file data. One million commands / 256 MiB is
    // deliberately far beyond a normal match while preventing a forged count
    // field from reserving arbitrary process memory.
    static constexpr size_t MaximumCommandCount = 1'000'000;
    static constexpr size_t MaximumEncodedBytes = 256ull * 1024ull * 1024ull;

    static container::Vector<uint8_t> encode(const container::Vector<GameCommand>& commands);
    static CommandStreamResult decode(const uint8_t* data, size_t size);
    static CommandStreamResult decode(const container::Vector<uint8_t>& data) {
        return decode(data.data(), data.size());
    }
};

} // namespace engine
