#pragma once

#include "engine/renderer/world/terrain/D3D12TerrainGpuState.h"
#include "engine/renderer/world/terrain/TerrainGpuGeometryOwner.h"
#include "engine/renderer/world/terrain/TerrainGpuMaterialOwner.h"
#include "engine/renderer/world/terrain/TerrainRoadMeshBuilder.h"
#include "engine/renderer/world/terrain/TerrainTextureSetOwner.h"

#include <cstdint>

namespace engine::render::detail {

// Unique owner for the published terrain GPU scene. CPU preparation may
// happen elsewhere, but every resident geometry/material/texture collection
// and its accepted snapshot revisions live and retire together here.
struct TerrainGpuScene final {
    void retire() noexcept;

    d3d12::D3D12Device* device = nullptr;
    TerrainGpuGeometryOwner geometry;
    TerrainGpuMaterialOwner materials;
    TerrainTextureSetOwner textures;
    container::Vector<TerrainGpuChunk> chunks;
    container::Vector<TerrainGpuRoadChunk> roads;
    container::Vector<TerrainGpuBridgeChunk> bridges;
    container::Vector<TerrainGpuWaterChunk> waters;
    container::SharedPtr<const TerrainRoadMeshPlan> roadPlan;

    uint64_t terrainRevision = 0;
    uint64_t borderShroudRevision = 0;
    uint64_t layoutRevision = 0;
    uint64_t bridgeRevision = 0;
    uint64_t waterRevision = 0;
    int32_t borderSize = 0;
    float cellWorldSize = 0.0f;
    float heightWorldScale = 0.0f;
    bool adjustCliffTextures = true;
    bool hasMaterialData = false;
    bool borderShroudEnabled = true;
    math::vec3 playableMinimum{};
    math::vec3 playableMaximum{};
};

} // namespace engine::render::detail
