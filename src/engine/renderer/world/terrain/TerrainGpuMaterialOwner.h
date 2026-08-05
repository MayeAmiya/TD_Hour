#pragma once

#include "core/container/hash_containers.h"
#include "engine/renderer/world/terrain/D3D12TerrainGpuState.h"
#include "engine/renderer/world/terrain/TerrainTileMeshBuilder.h"

#include <cstddef>
#include <cstdint>

namespace engine::render {

class TerrainTextureResolver;
class WorldTextureCache;

namespace detail {

class TerrainGpuMaterialOwner final {
public:
    TerrainGpuMaterialOwner() = default;
    ~TerrainGpuMaterialOwner();

    TerrainGpuMaterialOwner(const TerrainGpuMaterialOwner&) = delete;
    TerrainGpuMaterialOwner& operator=(const TerrainGpuMaterialOwner&) = delete;

    void setSources(
        container::SharedPtr<WorldTextureCache> cache,
        container::SharedPtr<const TerrainTextureResolver> resolver) noexcept;

    [[nodiscard]] bool acquireTerrain(
        const TerrainMaterialRenderData* source,
        int32_t textureClassIndex,
        uint32_t& output,
        container::String* error);
    [[nodiscard]] bool acquireTerrainAlphaEdge(
        const TerrainMaterialRenderData* source,
        int32_t edgeTextureClassIndex,
        uint32_t& output,
        container::String* error);
    [[nodiscard]] bool acquireRoad(
        const TerrainRoadRenderSegment& source,
        uint32_t& output,
        container::String* error);
    [[nodiscard]] bool prepareRoad(
        const TerrainRoadRenderSegment& source);
    [[nodiscard]] bool acquireBridge(
        const TerrainBridgeRenderData& source,
        uint32_t& output,
        container::String* error);
    [[nodiscard]] bool acquireBib(
        container::StringView textureName,
        TerrainBibTint tint,
        uint32_t& output,
        container::String* error);

    [[nodiscard]] bool prepareLayout(
        const TerrainRenderSnapshot& terrain,
        const TerrainMaterialRenderData* materialData,
        bool simplifiedSurface,
        TerrainTileMaterialLayout* cpuLayout,
        container::String* error);
    [[nodiscard]] bool establishLayout(
        const TerrainTileMaterialLayout& layout,
        container::String* error);
    [[nodiscard]] bool prepareChunkMaterials(
        const TerrainMaterialRenderData* source,
        const TerrainTileMeshChunk& chunk);
    [[nodiscard]] bool acquireChunkMaterials(
        const TerrainMaterialRenderData* source,
        const TerrainTileMeshChunk& chunk,
        container::String* error);
    [[nodiscard]] TerrainTileMaterialLayout cpuLayout() const;
    [[nodiscard]] bool matchesLayout(
        const TerrainTileMaterialLayout& expected) const noexcept;

    [[nodiscard]] const container::Vector<TerrainGpuMaterial>& materials()
        const noexcept;
    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] const TerrainGpuMaterial* at(uint32_t index) const noexcept;
    void retire() noexcept;

private:
    container::SharedPtr<WorldTextureCache> m_cache;
    container::SharedPtr<const TerrainTextureResolver> m_resolver;
    container::Vector<TerrainGpuMaterial> m_materials;
    container::HashMap<int32_t, uint32_t> m_terrainByClass;
    container::HashMap<int32_t, uint32_t> m_alphaEdgeByClass;
    container::HashMap<container::String, uint32_t> m_roadByStyle;
    container::HashMap<container::String, uint32_t> m_bridgeByState;
    container::HashMap<container::String, uint32_t> m_bibByTexture;
    container::HashSet<container::String> m_reportedMissingRoadStyles;
    container::Vector<TerrainTileMaterialDemand> m_tileDemands;
    container::HashSet<uint32_t> m_readyTileMaterials;
};

} // namespace detail
} // namespace engine::render
