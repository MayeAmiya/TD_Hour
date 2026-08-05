#pragma once

#include "engine/renderer/world/model/W3dStaticModel.h"
#include "engine/renderer/world/pipeline/WorldRenderer.h"
#include "core/container/hash_containers.h"
#include "core/container/container_types.h"
#include "presentation/render/TerrainRenderSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace engine::render {

inline constexpr int32_t kTerrainCellsPerChunk = 64;

// Direct-source UVs for one authored terrain selector.  The result is fully
// detached from maps, VFS state and GPU resources.
struct TerrainTileUvResolution final {
    int32_t textureClass = -1;
    container::Array<math::vec2, 4> corners{};
    bool usesAuthoredCliffUv = false;
    bool authoredCliffFlip = false;
    bool usesLegacyCliffStretch = false;
    bool requiresHeightTriangleFlip = false;
};

// Value-only reconstruction of the dormant W3DCustomEdging pass pair.
struct TerrainCustomEdgeRenderPlan final {
    container::Array<math::vec2, 4> patternUv{};
    float vertexAlpha = 0x80 / 255.0f;
    bool flipTriangles = false;
    StaticMeshTerrainEdgePhase blendSourcePhase =
        StaticMeshTerrainEdgePhase::BlendSource;
    StaticMeshTerrainEdgePhase edgeRgbPhase =
        StaticMeshTerrainEdgePhase::EdgeRgb;
    uint32_t blendSourceMaterialPass = 3;
    uint32_t edgeRgbMaterialPass = 4;
};

// Prepared, read-only topology lookup for terrain-conforming overlays.  The
// complete detached material payload is validated once when this value is
// created; resolving thousands of covered cells must not rescan every map
// selector for every cell.
struct TerrainPrimaryCellTopologyResolver final {
    const TerrainRenderSnapshot* terrain = nullptr;
    const TerrainMaterialRenderData* materials = nullptr;
};

enum class TerrainTileMaterialKind : uint8_t {
    Surface,
    AlphaEdge,
};

struct TerrainTileMaterialDemand final {
    TerrainTileMaterialKind kind = TerrainTileMaterialKind::Surface;
    int32_t textureClass = -1;
    uint32_t materialIndex = 0;
};

// Stable first-use material order plus O(1) class lookups used by parallel
// chunk builders.  The renderer consumes demands in order, so the logical
// indices embedded in CPU geometry are also the final GPU material indices.
struct TerrainTileMaterialLayout final {
    container::HashMap<int32_t, uint32_t> materials;
    container::HashMap<int32_t, uint32_t> alphaEdges;
    container::Vector<TerrainTileMaterialDemand> demands;
    // Source-tile to texture-class projection compiled once for all chunks.
    // The old path linearly scanned textureClasses several times per cell.
    container::Vector<int32_t> textureClassBySourceTile;
};

struct TerrainTileMeshGeometryKey final {
    uint32_t materialIndex = 0;
    uint32_t detailMaterialIndex = UINT32_MAX;
    uint32_t materialPass = 0;
    bool alphaBlend = false;
    bool twoSided = false;
    StaticMeshTerrainEdgePhase terrainEdgePhase =
        StaticMeshTerrainEdgePhase::Disabled;
    uint8_t samplerMode = 0;
    uint8_t detailSamplerMode = 0;

    [[nodiscard]] bool operator==(
        const TerrainTileMeshGeometryKey&) const noexcept = default;
};

struct TerrainTileMeshGeometry final {
    TerrainTileMeshGeometryKey key;
    container::Vector<StaticMeshVertex> vertices;
    container::Vector<uint32_t> indices;
};

// Complete detached CPU product for one fixed-size main-terrain chunk.
struct TerrainTileMeshChunk final {
    int32_t x0 = 0;
    int32_t y0 = 0;
    int32_t cellsX = 0;
    int32_t cellsY = 0;
    math::vec3 boundsCenter{};
    float boundsRadius = 0.0f;
    container::Vector<TerrainTileMeshGeometry> geometries;
};

struct TerrainTileMeshBuildResult final {
    TerrainTileMeshChunk chunk;
    container::String error;
    bool success = false;
};

[[nodiscard]] TerrainTileMaterialLayout buildTerrainTileMaterialLayout(
    const TerrainRenderSnapshot& terrain,
    const TerrainMaterialRenderData* materials,
    bool simplifiedSurface);

[[nodiscard]] bool buildTerrainTileMeshChunk(
    const TerrainRenderSnapshot& terrain,
    const TerrainMaterialRenderData* materials,
    const TerrainTileMaterialLayout& materialLayout,
    int32_t x0,
    int32_t y0,
    int32_t cellsX,
    int32_t cellsY,
    bool simplifiedSurface,
    TerrainTileMeshChunk& output,
    container::String* error = nullptr);

[[nodiscard]] bool buildTerrainHeightfieldFallbackMeshChunk(
    const TerrainRenderSnapshot& terrain,
    uint32_t materialIndex,
    int32_t x0,
    int32_t y0,
    int32_t cellsX,
    int32_t cellsY,
    TerrainTileMeshChunk& output,
    container::String* error = nullptr);

[[nodiscard]] std::optional<TerrainTileUvResolution>
resolveTerrainMaterialTileUv(
    const TerrainMaterialRenderData& materials,
    size_t sampleIndex,
    int32_t tileIndex,
    bool adjustCliffTextures = true) noexcept;

[[nodiscard]] std::optional<TerrainTileUvResolution> resolveTerrainTileUv(
    const TerrainRenderSnapshot& terrain,
    const TerrainMaterialRenderData& materials,
    size_t sampleIndex,
    int32_t tileIndex) noexcept;

[[nodiscard]] bool resolveTerrainAuthoredCliffTriangleFlip(
    const TerrainRenderSnapshot& terrain,
    int32_t mapX,
    int32_t mapY) noexcept;

// Resolves the diagonal used by the completed terrain surface for one cell.
// Unlike the height-only cliff helper, this includes primary blend topology
// and authored cliff-UV requirements. Terrain-conforming overlays must use
// this result so their triangles are coplanar with the rendered base surface.
[[nodiscard]] bool resolveTerrainPrimaryCellTriangleFlip(
    const TerrainRenderSnapshot& terrain,
    int32_t mapX,
    int32_t mapY) noexcept;

[[nodiscard]] TerrainPrimaryCellTopologyResolver
prepareTerrainPrimaryCellTopologyResolver(
    const TerrainRenderSnapshot& terrain) noexcept;

[[nodiscard]] bool resolveTerrainPrimaryCellTriangleFlip(
    const TerrainPrimaryCellTopologyResolver& resolver,
    int32_t mapX,
    int32_t mapY) noexcept;

[[nodiscard]] std::optional<TerrainCustomEdgeRenderPlan>
resolveTerrainCustomEdgeRenderPlan(
    const TerrainBlendDefinitionRenderData& definition,
    int32_t mapX,
    int32_t mapY) noexcept;

[[nodiscard]] math::vec4 evaluateTerrainVertexColorCpu(
    const TerrainRenderSnapshot& terrain,
    math::vec3 position,
    math::vec3 normal = {0.0f, 0.0f, 1.0f},
    float alpha = 1.0f) noexcept;

[[nodiscard]] math::vec4 terrainFallbackMaterialColour(
    container::StringView name, int32_t classIndex) noexcept;

} // namespace engine::render
