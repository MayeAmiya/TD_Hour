#pragma once

#include <cstddef>

namespace engine {
class TerrainConstructionCatalog;
}

namespace game::terrain {

struct TerrainBlendTileData;

// Resolves WorldHeightMap's packed base tile through the authored terrain
// texture classes, then queries the session-frozen construction catalog.
// The map-format adapter belongs to game_world; the catalog stays a base
// value type with no dependency on terrain runtime storage.
[[nodiscard]] bool terrainRestrictsConstructionAtBaseTile(
    const engine::TerrainConstructionCatalog& catalog,
    const TerrainBlendTileData& blendTiles,
    size_t sampleIndex) noexcept;

} // namespace game::terrain
