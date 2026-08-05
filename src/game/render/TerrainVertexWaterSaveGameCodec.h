#pragma once

#include "core/container/container_types.h"
#include "game/base/MapContentIdentity.h"
#include "game/render/TerrainVertexWaterPersistentStateCodec.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace engine {

// One transaction-scoped TerrainVisual chunk. The outer SaveGame coordinator
// restores authoritative simulation to confirmedTick before applying this
// presentation state. ReplayFileCodec deliberately has no dependency on this
// type and continues to rebuild VertexWater from tick-zero commands.
struct TerrainVertexWaterSaveGameData final {
    uint64_t confirmedTick = 0;
    game::MapContentIdentity mapIdentity;
    std::optional<TerrainVertexWaterPersistentState> vertexWater;
};

struct TerrainVertexWaterSaveGameDecodeResult final {
    bool ok = false;
    TerrainVertexWaterSaveGameData transaction;
    container::String error;
};

class TerrainVertexWaterSaveGameCodec final {
public:
    static constexpr uint32_t Magic = 0x47535754u; // "TWSG"
    static constexpr uint16_t Version = 1;
    static constexpr uint16_t HeaderBytes = 40;
    static constexpr uint32_t MaximumMapPathBytes = 4096;
    static constexpr uint32_t VertexWaterPresent = 1u << 0u;

    [[nodiscard]] static container::Vector<uint8_t> encode(
        uint64_t confirmedTick,
        const game::MapContentIdentity& mapIdentity,
        const TerrainVertexWaterState* vertexWater);
    [[nodiscard]] static TerrainVertexWaterSaveGameDecodeResult decode(
        const uint8_t* data, std::size_t size);
    [[nodiscard]] static TerrainVertexWaterSaveGameDecodeResult decode(
        const container::Vector<uint8_t>& data) {
        return decode(data.data(), data.size());
    }
};

} // namespace engine
