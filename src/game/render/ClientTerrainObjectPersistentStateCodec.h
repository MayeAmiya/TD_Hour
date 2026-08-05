#pragma once

#include <cstddef>
#include <cstdint>

#include "ClientTerrainObjectStore.h"
#include "core/container/container_types.h"

namespace engine
{

struct ClientTerrainObjectPersistentStateDecodeResult final
{
    bool ok = false;
    ClientTerrainObjectPersistentState state;
    container::String error;
};

// Stable, value-only wire format for the client terrain mutable overlay.
// SaveGame containers own framing and checkpoint timing; this codec owns only
// the renderer-independent overlay payload. Ordinary replay deliberately has
// no client-terrain checkpoint and rebuilds the baseline from tick zero.
class ClientTerrainObjectPersistentStateCodec final
{
public:
    static constexpr uint32_t kMagic = 0x50544f43u; // "COTP"
    static constexpr uint16_t kCodecVersion = 1;
    static constexpr uint32_t kMaximumMutations = 1u << 20u;

    [[nodiscard]] static container::Vector<uint8_t> encode(const ClientTerrainObjectPersistentState& state);
    [[nodiscard]] static ClientTerrainObjectPersistentStateDecodeResult decode(const uint8_t* data, size_t size);
    [[nodiscard]] static ClientTerrainObjectPersistentStateDecodeResult decode(const container::Vector<uint8_t>& data)
    {
        return decode(data.data(), data.size());
    }
};

} // namespace engine
