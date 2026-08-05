#pragma once

#include "engine/renderer/world/model/W3dStaticModel.h"
#include "core/container/container_types.h"
#include "presentation/render/TerrainRenderSnapshot.h"

#include <cstdint>
#include <optional>

namespace engine::render {

// Complete value-owned CPU products. The terrain visual only schedules these
// builders and uploads their results; bridge/bib geometry policy lives here.
struct TerrainBridgeMeshCpu final {
    container::Vector<StaticMeshVertex> vertices;
    container::Vector<uint32_t> indices;
};

struct TerrainBibMeshCpu final {
    container::Vector<StaticMeshVertex> vertices;
    container::Vector<uint32_t> indices;
};

struct TerrainBridgeMeshInspection final {
    container::Vector<math::vec3> vertices;
    container::Vector<uint32_t> indices;
};

struct TerrainBibMeshInspection final {
    container::Vector<math::vec3> vertices;
    container::Vector<uint32_t> indices;
};

[[nodiscard]] bool buildTerrainBridgeMesh(
    const TerrainBridgeRenderData& source,
    TerrainBridgeMeshCpu& output);

[[nodiscard]] bool buildTerrainBibMesh(
    const TerrainBibRenderData& source,
    TerrainBibMeshCpu& output);

[[nodiscard]] std::optional<TerrainBridgeMeshInspection>
inspectTerrainBridgeMesh(const TerrainBridgeRenderData& bridge);

[[nodiscard]] std::optional<TerrainBibMeshInspection>
inspectTerrainBibMesh(const TerrainBibRenderData& bib);

} // namespace engine::render
