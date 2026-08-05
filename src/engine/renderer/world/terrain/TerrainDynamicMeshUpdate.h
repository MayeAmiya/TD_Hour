#pragma once

#include "engine/renderer/world/terrain/TerrainGpuScene.h"

namespace engine::render::detail {

[[nodiscard]] bool replaceTerrainRoads(
    TerrainGpuScene& target,
    const TerrainRenderSnapshot& terrain,
    container::String* error);
[[nodiscard]] bool replaceTerrainRoadsInDirtyRegion(
    TerrainGpuScene& target,
    const TerrainRenderSnapshot& terrain,
    const TerrainRenderDirtyRegion& dirty,
    container::String* error);
[[nodiscard]] bool replaceTerrainBridges(
    TerrainGpuScene& target,
    const TerrainRenderSnapshot& terrain,
    container::String* error);
[[nodiscard]] bool replaceTerrainWater(
    TerrainGpuScene& target,
    const TerrainRenderSnapshot& terrain,
    container::String* error);
[[nodiscard]] bool updateTerrainDirtyGeometry(
    TerrainGpuScene& target,
    const TerrainRenderSnapshot& terrain,
    container::String* error);

} // namespace engine::render::detail
