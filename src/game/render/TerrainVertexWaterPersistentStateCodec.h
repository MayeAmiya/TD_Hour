#pragma once

#include "game/terrain/TerrainVertexWaterState.h"
#include "presentation/render/WaterSurfacePerformanceSettings.h"
#include "core/container/container_types.h"

#include <cstddef>
#include <cstdint>

namespace engine {

struct TerrainVertexWaterPersistentState final {
    TerrainVertexWaterGridConfig config;
    container::Vector<TerrainVertexWaterPoint> points;
};

struct TerrainVertexWaterPersistentStateDecodeResult final {
    bool ok = false;
    TerrainVertexWaterPersistentState state;
    container::String error;
};

// Value-only payload for the TerrainVisual part of a future full SaveGame.
// Replay deliberately does not use this codec: ordinary replay rebuilds the
// grid by replaying WaveGuide simulation from tick zero.
class TerrainVertexWaterPersistentStateCodec final {
public:
    static constexpr uint32_t Magic = 0x31575654u; // "TVW1"
    static constexpr uint16_t Version = 1;
    static constexpr uint32_t MaximumPoints =
        water_surface::performance_limits::kMaximumVertexWaterPoints;

    [[nodiscard]] static container::Vector<uint8_t> encode(
        const TerrainVertexWaterState& state);
    [[nodiscard]] static TerrainVertexWaterPersistentStateDecodeResult decode(
        const uint8_t* data, size_t size);
    [[nodiscard]] static TerrainVertexWaterPersistentStateDecodeResult decode(
        const container::Vector<uint8_t>& data) {
        return decode(data.data(), data.size());
    }
};

} // namespace engine
