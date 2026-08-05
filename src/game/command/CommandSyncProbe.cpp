#include "CommandSyncProbe.h"

#include "CommandCodec.h"

namespace engine {

namespace {

constexpr uint32_t FnvPrime = 16777619u;

} // namespace

void CommandSyncProbe::reset()
{
    m_checksum = 2166136261u;
}

void CommandSyncProbe::record(const GameCommand& command)
{
    const auto encoded = CommandCodec::encode(command);
    appendUint32(static_cast<uint32_t>(encoded.size()));
    for (const auto byte : encoded) {
        appendByte(byte);
    }
}

std::optional<CommandSyncSample> CommandSyncProbe::finishTick(GameTick tick)
{
    appendUint32(tick);
    if ((tick + 1) % SampleInterval != 0) {
        return std::nullopt;
    }
    return CommandSyncSample{tick, m_checksum};
}

void CommandSyncProbe::appendByte(uint8_t value)
{
    m_checksum ^= value;
    m_checksum *= FnvPrime;
}

void CommandSyncProbe::appendUint32(uint32_t value)
{
    appendByte(static_cast<uint8_t>(value & 0xff));
    appendByte(static_cast<uint8_t>((value >> 8) & 0xff));
    appendByte(static_cast<uint8_t>((value >> 16) & 0xff));
    appendByte(static_cast<uint8_t>((value >> 24) & 0xff));
}

} // namespace engine
