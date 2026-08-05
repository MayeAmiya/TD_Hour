#pragma once

#include "core/container/container_types.h"
#include "engine/renderer/d3d12/runtime/D3D12Device.h"
#include "engine/renderer/world/pipeline/WorldRenderer.h"
#include "presentation/render/TerrainRenderSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace engine::render::detail {

struct TerrainGpuGeometry {
    d3d12::StaticBufferAllocation vertexBuffer;
    d3d12::StaticBufferAllocation indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vertexView{};
    D3D12_INDEX_BUFFER_VIEW indexView{};
    uint32_t indexCount = 0;
    uint32_t materialIndex = 0;
    uint32_t detailMaterialIndex = UINT32_MAX;
    uint32_t materialPass = 0;
    bool alphaBlend = false;
    bool twoSided = false;
    StaticMeshTerrainEdgePhase terrainEdgePhase =
        StaticMeshTerrainEdgePhase::Disabled;
    uint8_t samplerMode = 0;
    uint8_t detailSamplerMode = 0;
};

struct TerrainGpuChunk {
    int32_t x0 = 0;
    int32_t y0 = 0;
    int32_t cellsX = 0;
    int32_t cellsY = 0;
    math::vec3 boundsCenter{};
    float boundsRadius = 0.0f;
    container::Vector<TerrainGpuGeometry> geometries;
};

struct TerrainGpuWaterChunk {
    TerrainGpuGeometry geometry;
    math::transform worldTransform{};
    bool river = false;
    std::optional<TerrainVertexWaterRenderData> vertexWater;
};

struct TerrainGpuRoadChunk {
    TerrainGpuGeometry geometry;
    math::vec3 boundsCenter{};
    float boundsRadius = 0.0f;
    uint32_t materialPass = 0;
    size_t sourceRoadIndex = 0;
};

struct TerrainGpuBridgeChunk {
    TerrainGpuGeometry geometry;
    math::vec3 boundsCenter{};
    float boundsRadius = 0.0f;
};

struct TerrainGpuBibChunk {
    TerrainGpuGeometry geometry;
    math::vec3 boundsCenter{};
    float boundsRadius = 0.0f;
    bool receivesVisibility = true;
};

struct TerrainGpuMaterial {
    container::String textureName;
    uint32_t textureSrvIndex = 0;
    uint32_t terrainSourceGridWidth = 0;
    math::vec4 diffuse{1.0f, 1.0f, 1.0f, 1.0f};
    bool ownsTextureReference = false;
    bool terrainColor = false;
    bool terrainAlphaEdge = false;
};

} // namespace engine::render::detail
