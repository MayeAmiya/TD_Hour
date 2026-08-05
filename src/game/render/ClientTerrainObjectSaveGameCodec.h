#pragma once

#include "core/container/container_types.h"
#include "game/base/MapContentIdentity.h"
#include "game/render/ClientTerrainObjectPersistentStateCodec.h"

#include <cstddef>
#include <cstdint>

namespace engine {

// Transaction framing for the client-owned TerrainVisual tree/prop overlay.
// The nested persistent-state codec owns mutation values; this outer chunk
// binds those values to the authoritative SaveGame tick and map bytes.
struct ClientTerrainObjectSaveGameData final {
    uint64_t confirmedTick = 0;
    game::MapContentIdentity mapIdentity;
    ClientTerrainObjectPersistentState state;
};

struct ClientTerrainObjectSaveGameDecodeResult final {
    bool ok = false;
    ClientTerrainObjectSaveGameData transaction;
    container::String error;
};

class ClientTerrainObjectSaveGameCodec final {
public:
    static constexpr uint32_t Magic = 0x47535443u; // "CTSG"
    static constexpr uint16_t Version = 1;
    static constexpr uint16_t HeaderBytes = 40;
    static constexpr uint32_t MaximumMapPathBytes = 4096;
    static constexpr uint32_t MaximumPayloadBytes =
        24u + ClientTerrainObjectPersistentStateCodec::kMaximumMutations * 64u;

    [[nodiscard]] static container::Vector<uint8_t> encode(
        uint64_t confirmedTick,
        const game::MapContentIdentity& mapIdentity,
        const ClientTerrainObjectPersistentState& state);
    [[nodiscard]] static ClientTerrainObjectSaveGameDecodeResult decode(
        const uint8_t* data, std::size_t size);
    [[nodiscard]] static ClientTerrainObjectSaveGameDecodeResult decode(
        const container::Vector<uint8_t>& data) {
        return decode(data.data(), data.size());
    }
};

} // namespace engine
