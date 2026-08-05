#pragma once

#include "GameCommand.h"

#include <cstdint>
#include <optional>

namespace engine {

struct CommandSyncSample {
    GameTick tick = 0;
    uint32_t checksum = 0;
};

class CommandSyncProbe {
public:
    static constexpr GameTick SampleInterval = 30;

    void reset();
    void record(const GameCommand& command);
    std::optional<CommandSyncSample> finishTick(GameTick tick);

private:
    void appendByte(uint8_t value);
    void appendUint32(uint32_t value);

    uint32_t m_checksum = 2166136261u;
};

} // namespace engine
