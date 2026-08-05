#include "engine/renderer/world/terrain/TerrainTextureSetOwner.h"

#include "core/container/hash_containers.h"
#include "engine/renderer/world/resource/WorldTextureCache.h"
#include "engine/renderer/world/terrain/TerrainTextureResolver.h"
#include "presentation/render/WaterSurfaceVisualSettings.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace engine::render::detail {
namespace {

constexpr container::StringView kTerrainCloudTextureName = "TSCloudMed.tga";
constexpr container::StringView kTerrainDefaultMacroTextureName =
    "TSNoiseUrb.tga";
constexpr int32_t kNightTimeOfDay = 4;

void setError(container::String* error, container::String message) {
    if (error) *error = std::move(message);
}

} // namespace

TerrainTextureSetOwner::TerrainTextureSetOwner(
    container::SharedPtr<WorldTextureCache> cache,
    container::SharedPtr<const TerrainTextureResolver> resolver) noexcept
    : m_cache(std::move(cache)), m_resolver(std::move(resolver)) {}

TerrainTextureSetOwner::~TerrainTextureSetOwner() {
    retire();
}

void TerrainTextureSetOwner::setSources(
    container::SharedPtr<WorldTextureCache> cache,
    container::SharedPtr<const TerrainTextureResolver> resolver) noexcept {
    retire();
    m_cache = std::move(cache);
    m_resolver = std::move(resolver);
}

void TerrainTextureSetOwner::configure(
    const TerrainRenderSnapshot& terrain,
    TerrainTexturePreparationScope scope) noexcept {
    retire();
    m_scope = scope;
    m_terrainCloudTextureName = container::String{kTerrainCloudTextureName};
    m_terrainMacroTextureName =
        container::String{kTerrainDefaultMacroTextureName};
    m_cloudAllowedByTimeOfDay = !terrain.globalLighting ||
        terrain.globalLighting->timeOfDay != kNightTimeOfDay;
    m_waterMaterial = terrain.waterMaterial.value_or(
        TerrainWaterMaterialRenderData{});
    m_normalizedSkyTexelsPerUnit = m_waterMaterial.skyTexelsPerUnit;
    if (scope == TerrainTexturePreparationScope::TexturedSurface) return;
    m_waterTextureName = m_waterMaterial.textureName;
    m_standingWaterTextureName =
        m_waterMaterial.standingWaterTextureName.empty()
        ? m_waterTextureName
        : m_waterMaterial.standingWaterTextureName;
    m_skyWaterTextureName = m_waterMaterial.skyTextureName;
}

bool TerrainTextureSetOwner::prepare(
    const TerrainRenderSnapshot& terrain,
    bool includeTerrainMaterials) {
    if (!m_cache) return true;
    bool ready = true;
    container::HashSet<container::String> ordinaryTextures;
    const auto prepareOrdinary = [&](container::StringView name) {
        if (name.empty() || !ordinaryTextures.emplace(name).second) return;
        ready = m_cache->prepare(
                    name, WorldTextureCache::Variant::ColorLegacyGamma,
                    RenderAssetPriority::Visible) && ready;
    };
    prepareOrdinary(m_terrainCloudTextureName);
    prepareOrdinary(m_terrainMacroTextureName);
    prepareOrdinary(m_waterTextureName);
    prepareOrdinary(m_standingWaterTextureName);
    prepareOrdinary(m_skyWaterTextureName);
    if (includeTerrainMaterials &&
        m_scope != TerrainTexturePreparationScope::TexturedSurface) {
        for (const TerrainRoadRenderSegment& road : terrain.roads) {
            prepareOrdinary(road.textureName);
        }
        for (const TerrainBridgeRenderData& bridge : terrain.bridges) {
            for (const container::String& name : bridge.textureNames) {
                prepareOrdinary(name);
            }
        }
        for (const TerrainBibRenderData& bib : terrain.bibs) {
            prepareOrdinary(
                bib.textureName.empty() ? "TBBib.tga" : bib.textureName);
        }
    }
    if (includeTerrainMaterials &&
        m_scope != TerrainTexturePreparationScope::HeightfieldFallback &&
        terrain.materials &&
        terrain.materials->isValidFor(terrain.heights.size()) && m_resolver) {
        for (const TerrainTextureClassRenderData& textureClass :
             terrain.materials->textureClasses) {
            const TerrainTextureResolution resolved =
                m_resolver->resolve(textureClass.name);
            if (!resolved.textureName.empty()) {
                ready = m_cache->prepareTerrainColor(
                            resolved.textureName,
                            static_cast<uint32_t>(std::max(
                                0, terrainSourceGridWidth(textureClass))),
                            RenderAssetPriority::Visible) && ready;
            }
        }
        for (const TerrainTextureClassRenderData& textureClass :
             terrain.materials->edgeTextureClasses) {
            if (textureClass.firstTile < 0 || textureClass.tileCount <= 0) {
                continue;
            }
            const TerrainTextureResolution resolved =
                m_resolver->resolve(textureClass.name);
            if (!resolved.textureName.empty()) {
                ready = m_cache->prepareTerrainAlphaEdge(
                            resolved.textureName,
                            RenderAssetPriority::Visible) && ready;
            }
        }
    }
    if (!m_waterTextureName.empty()) {
        if (const auto dimensions =
                m_cache->sourceDimensions(m_waterTextureName);
            dimensions && dimensions->width != 0u) {
            m_normalizedSkyTexelsPerUnit =
                water_surface::visual_defaults::normalizedSkyTexelsPerUnit(
                    m_waterMaterial.skyTexelsPerUnit,
                    dimensions->width);
        }
    }
    return ready;
}

bool TerrainTextureSetOwner::acquire(container::String* error) {
    m_normalizedSkyTexelsPerUnit = m_waterMaterial.skyTexelsPerUnit;
    if (!m_cache) return true;
    const auto acquireOrdinary = [this, error](
        const container::String& name, uint32_t& srv, bool& owns,
        container::StringView label) {
        if (name.empty()) return true;
        const std::optional<uint32_t> texture = m_cache->acquire(
            name, WorldTextureCache::Variant::ColorLegacyGamma,
            RenderAssetPriority::Visible);
        if (!texture) {
            setError(error, container::String(label) + name);
            return false;
        }
        srv = *texture;
        owns = true;
        return true;
    };
    if (!acquireOrdinary(
            m_terrainCloudTextureName, m_terrainCloudTextureSrvIndex,
            m_ownsTerrainCloudTextureReference,
            "D3D12 terrain cloud texture upload failed for: ") ||
        !acquireOrdinary(
            m_terrainMacroTextureName, m_terrainMacroTextureSrvIndex,
            m_ownsTerrainMacroTextureReference,
            "D3D12 terrain macro/light texture upload failed for: ") ||
        !acquireOrdinary(
            m_waterTextureName, m_waterTextureSrvIndex,
            m_ownsWaterTextureReference,
            "D3D12 water texture upload failed for: ") ||
        !acquireOrdinary(
            m_standingWaterTextureName, m_standingWaterTextureSrvIndex,
            m_ownsStandingWaterTextureReference,
            "D3D12 standing/river water texture upload failed for: ") ||
        !acquireOrdinary(
            m_skyWaterTextureName, m_skyWaterTextureSrvIndex,
            m_ownsSkyWaterTextureReference,
            "D3D12 WaterSet sky/reflection texture upload failed for: ")) {
        return false;
    }
    if (m_standingWaterTextureName.empty()) {
        m_standingWaterTextureSrvIndex = m_waterTextureSrvIndex;
    }
    if (!m_waterTextureName.empty()) {
        if (const auto dimensions =
                m_cache->sourceDimensions(m_waterTextureName);
            dimensions && dimensions->width != 0u) {
            m_normalizedSkyTexelsPerUnit =
                water_surface::visual_defaults::normalizedSkyTexelsPerUnit(
                    m_waterMaterial.skyTexelsPerUnit,
                    dimensions->width);
        }
    }
    return true;
}

void TerrainTextureSetOwner::retire() noexcept {
    if (m_cache) {
        if (m_ownsWaterTextureReference) {
            m_cache->release(m_waterTextureName);
        }
        if (m_ownsStandingWaterTextureReference) {
            m_cache->release(m_standingWaterTextureName);
        }
        if (m_ownsSkyWaterTextureReference) {
            m_cache->release(m_skyWaterTextureName);
        }
        if (m_ownsTerrainCloudTextureReference) {
            m_cache->release(m_terrainCloudTextureName);
        }
        if (m_ownsTerrainMacroTextureReference) {
            m_cache->release(m_terrainMacroTextureName);
        }
    }
    m_waterTextureSrvIndex = 0;
    m_standingWaterTextureSrvIndex = 0;
    m_skyWaterTextureSrvIndex = 0;
    m_terrainCloudTextureSrvIndex = 0;
    m_terrainMacroTextureSrvIndex = 0;
    m_ownsWaterTextureReference = false;
    m_ownsStandingWaterTextureReference = false;
    m_ownsSkyWaterTextureReference = false;
    m_ownsTerrainCloudTextureReference = false;
    m_ownsTerrainMacroTextureReference = false;
    m_waterTextureName.clear();
    m_standingWaterTextureName.clear();
    m_skyWaterTextureName.clear();
    m_terrainCloudTextureName.clear();
    m_terrainMacroTextureName.clear();
}

const TerrainWaterMaterialRenderData& TerrainTextureSetOwner::waterMaterial()
    const noexcept {
    return m_waterMaterial;
}

void TerrainTextureSetOwner::setWaterMaterial(
    TerrainWaterMaterialRenderData material) noexcept {
    m_waterMaterial = std::move(material);
}

float TerrainTextureSetOwner::normalizedSkyTexelsPerUnit() const noexcept {
    return m_normalizedSkyTexelsPerUnit;
}

bool TerrainTextureSetOwner::cloudAllowedByTimeOfDay() const noexcept {
    return m_cloudAllowedByTimeOfDay;
}

container::StringView TerrainTextureSetOwner::skyWaterTextureName()
    const noexcept {
    return m_skyWaterTextureName;
}

uint32_t TerrainTextureSetOwner::waterTextureSrvIndex() const noexcept {
    return m_waterTextureSrvIndex;
}

uint32_t TerrainTextureSetOwner::standingWaterTextureSrvIndex()
    const noexcept {
    return m_standingWaterTextureSrvIndex;
}

uint32_t TerrainTextureSetOwner::skyWaterTextureSrvIndex() const noexcept {
    return m_skyWaterTextureSrvIndex;
}

uint32_t TerrainTextureSetOwner::terrainCloudTextureSrvIndex()
    const noexcept {
    return m_terrainCloudTextureSrvIndex;
}

uint32_t TerrainTextureSetOwner::terrainMacroTextureSrvIndex()
    const noexcept {
    return m_terrainMacroTextureSrvIndex;
}

} // namespace engine::render::detail
