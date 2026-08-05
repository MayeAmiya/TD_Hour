#pragma once

#include "core/container/container_types.h"
#include "engine/renderer/world/terrain/D3D12TerrainGpuState.h"
#include "presentation/render/RenderViewSnapshot.h"

#include <cstddef>
#include <cstdint>

namespace engine::render::detail {

struct TerrainDrawPacketSource {
    d3d12::D3D12Device* device = nullptr;
    const container::Vector<TerrainGpuMaterial>* materials = nullptr;
    const container::Vector<TerrainGpuChunk>* chunks = nullptr;
    const container::Vector<TerrainGpuRoadChunk>* roads = nullptr;
    const container::Vector<TerrainGpuWaterChunk>* waters = nullptr;
    const container::Vector<TerrainGpuBridgeChunk>* bridges = nullptr;
    const container::Vector<TerrainGpuBibChunk>* bibs = nullptr;
    const TerrainWaterMaterialRenderData* waterMaterial = nullptr;
    container::StringView skyWaterTextureName;
    uint32_t waterTextureSrvIndex = 0;
    uint32_t standingWaterTextureSrvIndex = 0;
    uint32_t skyWaterTextureSrvIndex = 0;
    uint32_t terrainCloudTextureSrvIndex = 0;
    uint32_t terrainMacroTextureSrvIndex = 0;
    bool cloudAllowedByTimeOfDay = true;
};

struct TerrainDrawPacketStats {
    size_t visibleChunks = 0;
    size_t culledChunks = 0;
};

[[nodiscard]] bool terrainChunkSphereVisible(
    const RenderCameraSnapshot& camera,
    float viewportAspectRatio,
    math::vec3 center,
    float radius) noexcept;

[[nodiscard]] TerrainDrawPacketStats appendTerrainDrawPackets(
    const TerrainDrawPacketSource& source,
    container::Vector<StaticMeshDrawPacket>& output,
    const RenderCameraSnapshot* camera,
    float viewportAspectRatio,
    float visualTimeSeconds,
    bool useCloudMap,
    bool useLightMap);

} // namespace engine::render::detail
