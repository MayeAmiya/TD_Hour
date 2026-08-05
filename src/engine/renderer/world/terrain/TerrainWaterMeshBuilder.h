#pragma once

#include "engine/renderer/world/model/W3dStaticModel.h"
#include "core/container/container_types.h"
#include "presentation/render/TerrainRenderSnapshot.h"

#include <cstdint>
#include <optional>

namespace engine::render {

// Complete value-owned CPU product. The D3D12 terrain visual consumes this
// result without observing triangulation, clipping, or river-strip internals.
struct TerrainWaterMeshCpu final {
    container::Vector<StaticMeshVertex> vertices;
    container::Vector<uint32_t> indices;
    math::transform worldTransform{};
};

struct TerrainWaterMeshInspectionVertex final {
    math::vec3 worldPosition{};
    math::vec2 texcoord{};
    math::vec2 skyTexcoord{};
    float alpha = 0.0f;
};

struct TerrainWaterMeshInspection final {
    bool river = false;
    container::Vector<TerrainWaterMeshInspectionVertex> vertices;
    container::Vector<uint32_t> indices;
};

[[nodiscard]] bool buildTerrainWaterMesh(
    const TerrainRenderSnapshot& terrain,
    const TerrainWaterRenderArea& area,
    const TerrainWaterMaterialRenderData& material,
    TerrainWaterMeshCpu& output);

[[nodiscard]] bool buildTerrainVertexWaterMesh(
    const TerrainVertexWaterRenderData& grid,
    const TerrainWaterMaterialRenderData& material,
    TerrainWaterMeshCpu& output);

[[nodiscard]] std::optional<TerrainWaterMeshInspection>
inspectTerrainWaterMesh(
    const TerrainRenderSnapshot& terrain,
    const TerrainWaterRenderArea& area,
    const TerrainWaterMaterialRenderData& material);

[[nodiscard]] std::optional<TerrainWaterMeshInspection>
inspectTerrainVertexWaterMesh(
    const TerrainVertexWaterRenderData& grid,
    const TerrainWaterMaterialRenderData& material);

} // namespace engine::render
