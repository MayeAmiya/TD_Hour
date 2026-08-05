#include "engine/renderer/world/terrain/TerrainGpuMaterialOwner.h"

#include "debug/debug.h"
#include "engine/renderer/world/resource/WorldTextureCache.h"
#include "engine/renderer/world/terrain/TerrainTextureResolver.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace engine::render::detail {
namespace {

void setError(container::String* error, container::String message) {
    if (error) *error = std::move(message);
}

template <typename Map>
bool sameLayout(const Map& expected, const Map& actual) noexcept {
    if (expected.size() != actual.size()) return false;
    for (const auto& [key, value] : expected) {
        const auto found = actual.find(key);
        if (found == actual.end() || found->second != value) return false;
    }
    return true;
}

} // namespace

TerrainGpuMaterialOwner::~TerrainGpuMaterialOwner() {
    retire();
}

void TerrainGpuMaterialOwner::setSources(
    container::SharedPtr<WorldTextureCache> cache,
    container::SharedPtr<const TerrainTextureResolver> resolver) noexcept {
    retire();
    m_cache = std::move(cache);
    m_resolver = std::move(resolver);
}

bool TerrainGpuMaterialOwner::acquireTerrain(
    const TerrainMaterialRenderData* source,
    int32_t textureClassIndex,
    uint32_t& output,
    container::String* error) {
    uint32_t destinationIndex = 0;
    if (const auto found = m_terrainByClass.find(textureClassIndex);
        found != m_terrainByClass.end()) {
        destinationIndex = found->second;
        output = destinationIndex;
        if (m_readyTileMaterials.contains(destinationIndex)) return true;
    } else {
        destinationIndex = static_cast<uint32_t>(m_materials.size());
        m_materials.emplace_back();
        m_terrainByClass.emplace(textureClassIndex, destinationIndex);
    }
    TerrainGpuMaterial material;
    if (source && textureClassIndex >= 0 &&
        static_cast<size_t>(textureClassIndex) < source->textureClasses.size()) {
        const TerrainTextureClassRenderData& textureClass =
            source->textureClasses[static_cast<size_t>(textureClassIndex)];
        const TerrainTextureResolution resolved = m_resolver
            ? m_resolver->resolve(textureClass.name)
            : TerrainTextureResolution{.textureName = textureClass.name};
        material.textureName = resolved.textureName;
#if TD_DEBUG_ENABLED
        TD_LOG_INFO(
            "[D3D12TerrainVisual] Terrain class {} '{}' firstTile={} tileCount={} storedWidth={} sourceGrid={} texture='{}' mapped={}",
            textureClassIndex, textureClass.name, textureClass.firstTile,
            textureClass.tileCount, textureClass.tileWidth,
            terrainSourceGridWidth(textureClass), material.textureName,
            resolved.mappedByTerrainIni);
#endif
        if (!resolved.mappedByTerrainIni && !textureClass.name.empty()) {
            TD_LOG_WARN(
                "[D3D12TerrainVisual] Terrain class '{}' has no Terrain.ini texture mapping{}; trying direct VFS name '{}'.",
                textureClass.name,
                resolved.terrainIniAvailable ? "" : " (no Terrain.ini mounted)",
                material.textureName);
        }
        if (m_cache && !material.textureName.empty()) {
            material.terrainSourceGridWidth = static_cast<uint32_t>(
                std::max(0, terrainSourceGridWidth(textureClass)));
            const std::optional<uint32_t> srv = m_cache->acquireTerrainColor(
                material.textureName, material.terrainSourceGridWidth,
                RenderAssetPriority::Visible);
            if (!srv) {
                setError(error, "D3D12 terrain texture upload failed for: " +
                                material.textureName);
                return false;
            }
            material.textureSrvIndex = *srv;
            material.ownsTextureReference = true;
            material.terrainColor = true;
        }
        material.diffuse = material.textureSrvIndex != 0
            ? math::vec4{1.0f, 1.0f, 1.0f, 1.0f}
            : terrainFallbackMaterialColour(
                  textureClass.name, textureClassIndex);
    } else {
        material.diffuse = terrainFallbackMaterialColour({}, textureClassIndex);
    }
    if (destinationIndex >= m_materials.size()) return false;
    m_materials[destinationIndex] = std::move(material);
    m_readyTileMaterials.insert(destinationIndex);
    output = destinationIndex;
    return true;
}

bool TerrainGpuMaterialOwner::acquireTerrainAlphaEdge(
    const TerrainMaterialRenderData* source,
    int32_t edgeTextureClassIndex,
    uint32_t& output,
    container::String* error) {
    uint32_t destinationIndex = 0;
    if (const auto found = m_alphaEdgeByClass.find(edgeTextureClassIndex);
        found != m_alphaEdgeByClass.end()) {
        destinationIndex = found->second;
        output = destinationIndex;
        if (m_readyTileMaterials.contains(destinationIndex)) return true;
    } else {
        destinationIndex = static_cast<uint32_t>(m_materials.size());
        m_materials.emplace_back();
        m_alphaEdgeByClass.emplace(edgeTextureClassIndex, destinationIndex);
    }
    TerrainGpuMaterial material;
    material.terrainAlphaEdge = true;
    if (source && edgeTextureClassIndex >= 0 &&
        static_cast<size_t>(edgeTextureClassIndex) <
            source->edgeTextureClasses.size()) {
        const TerrainTextureClassRenderData& edgeClass =
            source->edgeTextureClasses[
                static_cast<size_t>(edgeTextureClassIndex)];
        if (edgeClass.firstTile >= 0 && edgeClass.tileCount > 0) {
            const TerrainTextureResolution resolved = m_resolver
                ? m_resolver->resolve(edgeClass.name)
                : TerrainTextureResolution{.textureName = edgeClass.name};
            material.textureName = resolved.textureName;
            if (!resolved.mappedByTerrainIni && !edgeClass.name.empty()) {
                TD_LOG_WARN(
                    "[D3D12TerrainVisual] Terrain custom edge class '{}' has no Terrain.ini texture mapping{}; trying direct VFS name '{}'.",
                    edgeClass.name,
                    resolved.terrainIniAvailable ? "" : " (no Terrain.ini mounted)",
                    material.textureName);
            }
            if (m_cache && !material.textureName.empty()) {
                const std::optional<uint32_t> srv =
                    m_cache->acquireTerrainAlphaEdge(
                        material.textureName, RenderAssetPriority::Visible);
                if (!srv) {
                    setError(error,
                        "D3D12 terrain alpha edge upload failed for: " +
                            material.textureName);
                    return false;
                }
                material.textureSrvIndex = *srv;
                material.ownsTextureReference = true;
            }
        }
    }
    if (destinationIndex >= m_materials.size()) return false;
    m_materials[destinationIndex] = std::move(material);
    m_readyTileMaterials.insert(destinationIndex);
    output = destinationIndex;
    return true;
}

bool TerrainGpuMaterialOwner::acquireRoad(
    const TerrainRoadRenderSegment& source,
    uint32_t& output,
    container::String* error) {
    if (const auto found = m_roadByStyle.find(source.styleName);
        found != m_roadByStyle.end()) {
        output = found->second;
        return true;
    }
    TerrainGpuMaterial material;
    material.textureName = source.textureName;
    if (m_cache && !material.textureName.empty()) {
        const std::optional<uint32_t> texture = m_cache->acquire(
            material.textureName, WorldTextureCache::Variant::ColorLegacyGamma,
            RenderAssetPriority::Visible);
        if (!texture) {
            setError(error, "D3D12 road texture upload failed for: " +
                            material.textureName);
            return false;
        }
        material.textureSrvIndex = *texture;
        material.ownsTextureReference = true;
    }
    if (material.textureName.empty() &&
        m_reportedMissingRoadStyles.insert(source.styleName).second) {
        TD_LOG_WARN(
            "[D3D12TerrainVisual] Road style '{}' has no authored Roads.ini material; using a stable fallback.",
            source.styleName);
    }
    material.diffuse = material.textureSrvIndex != 0
        ? math::vec4{1.0f, 1.0f, 1.0f, 1.0f}
        : math::vec4{0.24f, 0.22f, 0.19f, 0.82f};
    output = static_cast<uint32_t>(m_materials.size());
    m_materials.push_back(std::move(material));
    m_roadByStyle.emplace(source.styleName, output);
    return true;
}

bool TerrainGpuMaterialOwner::prepareRoad(
    const TerrainRoadRenderSegment& source) {
    return !m_cache || source.textureName.empty() ||
        m_cache->prepare(
            source.textureName,
            WorldTextureCache::Variant::ColorLegacyGamma,
            RenderAssetPriority::Visible);
}

bool TerrainGpuMaterialOwner::acquireBridge(
    const TerrainBridgeRenderData& source,
    uint32_t& output,
    container::String* error) {
    const size_t state = std::min<size_t>(
        static_cast<size_t>(source.damageState), 3u);
    container::String key = source.styleName;
    key += '#';
    key += static_cast<char>('0' + state);
    if (const auto found = m_bridgeByState.find(key);
        found != m_bridgeByState.end()) {
        output = found->second;
        return true;
    }
    TerrainGpuMaterial material;
    material.textureName = source.textureNames[state].empty()
        ? source.textureNames[0]
        : source.textureNames[state];
    if (m_cache && !material.textureName.empty()) {
        const std::optional<uint32_t> texture = m_cache->acquire(
            material.textureName, WorldTextureCache::Variant::ColorLegacyGamma,
            RenderAssetPriority::Visible);
        if (!texture) {
            setError(error, "D3D12 bridge texture upload failed for: " +
                            material.textureName);
            return false;
        }
        material.textureSrvIndex = *texture;
        material.ownsTextureReference = true;
    }
    material.diffuse = material.textureSrvIndex != 0
        ? math::vec4{1.0f, 1.0f, 1.0f, 1.0f}
        : math::vec4{0.38f, 0.36f, 0.32f, 1.0f};
    output = static_cast<uint32_t>(m_materials.size());
    m_materials.push_back(std::move(material));
    m_bridgeByState.emplace(std::move(key), output);
    return true;
}

bool TerrainGpuMaterialOwner::acquireBib(
    container::StringView textureName,
    TerrainBibTint tint,
    uint32_t& output,
    container::String* error) {
    container::String key{textureName};
    key.push_back('|');
    key.push_back(static_cast<char>(tint));
    if (const auto found = m_bibByTexture.find(key);
        found != m_bibByTexture.end()) {
        output = found->second;
        return true;
    }
    TerrainGpuMaterial material;
    material.textureName = textureName.empty()
        ? "TBBib.tga" : container::String{textureName};
    if (m_cache) {
        const std::optional<uint32_t> texture = m_cache->acquire(
            material.textureName, WorldTextureCache::Variant::ColorLegacyGamma,
            RenderAssetPriority::Visible);
        if (!texture) {
            setError(error, "D3D12 bib texture upload failed for: " +
                            material.textureName);
            return false;
        }
        material.textureSrvIndex = *texture;
        material.ownsTextureReference = true;
    }
    switch (tint) {
    case TerrainBibTint::Blue:
        material.diffuse = {0.20f, 0.45f, 1.00f, 0.72f};
        break;
    case TerrainBibTint::Green:
        material.diffuse = {0.10f, 1.00f, 0.15f, 0.72f};
        break;
    case TerrainBibTint::Yellow:
        material.diffuse = {1.00f, 0.78f, 0.08f, 0.72f};
        break;
    case TerrainBibTint::Red:
        material.diffuse = {1.00f, 0.12f, 0.08f, 0.72f};
        break;
    case TerrainBibTint::Default:
        material.diffuse = material.textureSrvIndex != 0
            ? math::vec4{1.0f, 1.0f, 1.0f, 1.0f}
            : math::vec4{0.45f, 0.42f, 0.35f, 0.75f};
        break;
    }
    output = static_cast<uint32_t>(m_materials.size());
    m_materials.push_back(std::move(material));
    m_bibByTexture.emplace(key, output);
    return true;
}

bool TerrainGpuMaterialOwner::prepareLayout(
    const TerrainRenderSnapshot& terrain,
    const TerrainMaterialRenderData* materialData,
    bool simplifiedSurface,
    TerrainTileMaterialLayout* cpuLayout,
    container::String* error) {
    TerrainTileMaterialLayout layout = buildTerrainTileMaterialLayout(
        terrain, materialData, simplifiedSurface);
    if (cpuLayout) {
        *cpuLayout = std::move(layout);
        return true;
    }
    for (const TerrainTileMaterialDemand& demand : layout.demands) {
        uint32_t materialIndex = 0;
        const bool acquired = demand.kind == TerrainTileMaterialKind::Surface
            ? acquireTerrain(
                  materialData, demand.textureClass, materialIndex, error)
            : acquireTerrainAlphaEdge(
                  materialData, demand.textureClass, materialIndex, error);
        if (!acquired || materialIndex != demand.materialIndex) {
            if (acquired) {
                setError(error,
                    "Terrain GPU material order differs from CPU layout");
            }
            return false;
        }
    }
    return true;
}

bool TerrainGpuMaterialOwner::establishLayout(
    const TerrainTileMaterialLayout& layout,
    container::String* error) {
    if (!m_materials.empty() || !m_terrainByClass.empty() ||
        !m_alphaEdgeByClass.empty()) {
        if (matchesLayout(layout)) return true;
        setError(error, "Terrain GPU material layout was already established differently");
        return false;
    }
    m_materials.resize(layout.demands.size());
    m_terrainByClass = layout.materials;
    m_alphaEdgeByClass = layout.alphaEdges;
    m_tileDemands = layout.demands;
    return true;
}

bool TerrainGpuMaterialOwner::prepareChunkMaterials(
    const TerrainMaterialRenderData* source,
    const TerrainTileMeshChunk& chunk) {
    if (!m_cache) return true;
    bool ready = true;
    container::HashSet<uint32_t> indices;
    for (const TerrainTileMeshGeometry& geometry : chunk.geometries) {
        indices.insert(geometry.key.materialIndex);
        if (geometry.key.detailMaterialIndex != UINT32_MAX) {
            indices.insert(geometry.key.detailMaterialIndex);
        }
    }
    for (const uint32_t index : indices) {
        if (m_readyTileMaterials.contains(index)) continue;
        if (index >= m_tileDemands.size()) return false;
        const TerrainTileMaterialDemand& demand = m_tileDemands[index];
        if (!source || demand.textureClass < 0) continue;
        if (demand.kind == TerrainTileMaterialKind::Surface) {
            if (static_cast<size_t>(demand.textureClass) >=
                source->textureClasses.size()) continue;
            const TerrainTextureClassRenderData& textureClass =
                source->textureClasses[demand.textureClass];
            const TerrainTextureResolution resolved = m_resolver
                ? m_resolver->resolve(textureClass.name)
                : TerrainTextureResolution{.textureName = textureClass.name};
            if (!resolved.textureName.empty()) {
                ready = m_cache->prepareTerrainColor(
                    resolved.textureName,
                    static_cast<uint32_t>(std::max(
                        0, terrainSourceGridWidth(textureClass))),
                    RenderAssetPriority::Visible) && ready;
            }
        } else {
            if (static_cast<size_t>(demand.textureClass) >=
                source->edgeTextureClasses.size()) continue;
            const TerrainTextureClassRenderData& textureClass =
                source->edgeTextureClasses[demand.textureClass];
            const TerrainTextureResolution resolved = m_resolver
                ? m_resolver->resolve(textureClass.name)
                : TerrainTextureResolution{.textureName = textureClass.name};
            if (!resolved.textureName.empty()) {
                ready = m_cache->prepareTerrainAlphaEdge(
                    resolved.textureName,
                    RenderAssetPriority::Visible) && ready;
            }
        }
    }
    return ready;
}

bool TerrainGpuMaterialOwner::acquireChunkMaterials(
    const TerrainMaterialRenderData* source,
    const TerrainTileMeshChunk& chunk,
    container::String* error) {
    container::HashSet<uint32_t> indices;
    for (const TerrainTileMeshGeometry& geometry : chunk.geometries) {
        indices.insert(geometry.key.materialIndex);
        if (geometry.key.detailMaterialIndex != UINT32_MAX) {
            indices.insert(geometry.key.detailMaterialIndex);
        }
    }
    for (const uint32_t index : indices) {
        if (m_readyTileMaterials.contains(index)) continue;
        if (index >= m_tileDemands.size()) return false;
        const TerrainTileMaterialDemand& demand = m_tileDemands[index];
        uint32_t acquired = UINT32_MAX;
        const bool ok = demand.kind == TerrainTileMaterialKind::Surface
            ? acquireTerrain(source, demand.textureClass, acquired, error)
            : acquireTerrainAlphaEdge(
                  source, demand.textureClass, acquired, error);
        if (!ok || acquired != index) return false;
    }
    return true;
}

TerrainTileMaterialLayout TerrainGpuMaterialOwner::cpuLayout() const {
    return TerrainTileMaterialLayout{
        .materials = m_terrainByClass,
        .alphaEdges = m_alphaEdgeByClass,
    };
}

bool TerrainGpuMaterialOwner::matchesLayout(
    const TerrainTileMaterialLayout& expected) const noexcept {
    return sameLayout(expected.materials, m_terrainByClass) &&
        sameLayout(expected.alphaEdges, m_alphaEdgeByClass);
}

const container::Vector<TerrainGpuMaterial>&
TerrainGpuMaterialOwner::materials() const noexcept {
    return m_materials;
}

size_t TerrainGpuMaterialOwner::size() const noexcept {
    return m_materials.size();
}

const TerrainGpuMaterial* TerrainGpuMaterialOwner::at(
    uint32_t index) const noexcept {
    return index < m_materials.size() ? &m_materials[index] : nullptr;
}

void TerrainGpuMaterialOwner::retire() noexcept {
    if (m_cache) {
        for (const TerrainGpuMaterial& material : m_materials) {
            if (!material.ownsTextureReference) continue;
            if (material.terrainAlphaEdge) {
                m_cache->releaseTerrainAlphaEdge(material.textureName);
            } else if (material.terrainColor) {
                m_cache->releaseTerrainColor(
                    material.textureName,
                    material.terrainSourceGridWidth);
            } else {
                m_cache->release(material.textureName);
            }
        }
    }
    m_materials.clear();
    m_terrainByClass.clear();
    m_alphaEdgeByClass.clear();
    m_roadByStyle.clear();
    m_bridgeByState.clear();
    m_bibByTexture.clear();
    m_reportedMissingRoadStyles.clear();
    m_tileDemands.clear();
    m_readyTileMaterials.clear();
}

} // namespace engine::render::detail
