#include "engine/renderer/world/terrain/TerrainGpuScene.h"

namespace engine::render::detail {

void TerrainGpuScene::retire() noexcept {
    for (TerrainGpuChunk& chunk : chunks) {
        for (TerrainGpuGeometry& value : chunk.geometries) {
            geometry.retire(value);
        }
    }
    geometry.retire(waters);
    geometry.retire(roads);
    geometry.retire(bridges);
    textures.retire();
    materials.retire();
    chunks.clear();
    roadPlan.reset();
}

} // namespace engine::render::detail
