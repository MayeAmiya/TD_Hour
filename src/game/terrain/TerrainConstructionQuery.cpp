#include "TerrainConstructionQuery.h"

#include "game/data/base/TerrainConstructionCatalog.h"
#include "game/terrain/MapHeightfieldLoader.h"

namespace game::terrain {

bool terrainRestrictsConstructionAtBaseTile(
    const engine::TerrainConstructionCatalog& catalog,
    const TerrainBlendTileData& blendTiles,
    size_t sampleIndex) noexcept {
    if (sampleIndex >= blendTiles.baseTileIndices.size()) return false;
    const int32_t packedTile = blendTiles.baseTileIndices[sampleIndex];
    if (packedTile < 0) return false;

    const int32_t bitmapTile = packedTile >> 2;
    for (const TerrainTextureClass& textureClass :
         blendTiles.textureClasses) {
        if (textureClass.firstTile < 0 || textureClass.tileCount <= 0)
            continue;
        const int64_t first = textureClass.firstTile;
        const int64_t end = first + textureClass.tileCount;
        if (bitmapTile < first || bitmapTile >= end) continue;
        const engine::TerrainConstructionDefinition* definition =
            catalog.find(textureClass.name);
        return definition && definition->restrictConstruction;
    }
    return false;
}

} // namespace game::terrain
