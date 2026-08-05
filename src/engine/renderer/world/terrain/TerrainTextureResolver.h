#pragma once

#include "core/container/hash_containers.h"
#include "presentation/render/TerrainRenderSnapshot.h"

namespace engine::render {

struct TerrainTextureResolution final {
    container::String textureName;
    bool mappedByTerrainIni = false;
    bool terrainIniAvailable = false;
};

// Renderer-owned layered Terrain.ini resolver shared by terrain GPU upload
// and minimap CPU colour extraction. Only resolved value strings cross either
// boundary; gameplay never depends on this visual catalog.
class TerrainTextureResolver final {
public:
    TerrainTextureResolver();

    [[nodiscard]] TerrainTextureResolution resolve(
        container::StringView terrainClass) const;

private:
    static container::String terrainPath(container::String texture);
    void parse(container::StringView contents);

    bool m_hasTerrainIni = false;
    container::HashMap<container::String, container::String>
        m_textureByTerrainType;
};

// Classic terrain atlases contain a square grid of source tiles. Some legacy
// maps serialize pixel width instead of grid width; this keeps the existing
// bounded inference shared by world terrain and minimap colour extraction.
[[nodiscard]] int32_t terrainSourceGridWidth(
    const TerrainTextureClassRenderData& textureClass) noexcept;

} // namespace engine::render
