#pragma once

#include "core/container/container_types.h"
#include "presentation/render/TerrainRenderSnapshot.h"

#include <cstdint>

namespace engine::render {

class TerrainTextureResolver;
class WorldTextureCache;

namespace detail {

enum class TerrainTexturePreparationScope : uint8_t {
    Complete,
    TexturedSurface,
    HeightfieldFallback,
};

// Owns the complete global terrain texture set and every corresponding cache
// reference. Per-class tile/road/bridge/bib materials remain in the material
// table owner and use this object's immutable cache/resolver sources.
class TerrainTextureSetOwner final {
public:
    TerrainTextureSetOwner() = default;
    TerrainTextureSetOwner(
        container::SharedPtr<WorldTextureCache> cache,
        container::SharedPtr<const TerrainTextureResolver> resolver) noexcept;
    ~TerrainTextureSetOwner();

    TerrainTextureSetOwner(const TerrainTextureSetOwner&) = delete;
    TerrainTextureSetOwner& operator=(const TerrainTextureSetOwner&) = delete;

    void setSources(
        container::SharedPtr<WorldTextureCache> cache,
        container::SharedPtr<const TerrainTextureResolver> resolver) noexcept;
    void configure(
        const TerrainRenderSnapshot& terrain,
        TerrainTexturePreparationScope scope) noexcept;
    [[nodiscard]] bool prepare(
        const TerrainRenderSnapshot& terrain,
        bool includeTerrainMaterials = true);
    [[nodiscard]] bool acquire(container::String* error);
    void retire() noexcept;

    [[nodiscard]] const TerrainWaterMaterialRenderData& waterMaterial()
        const noexcept;
    void setWaterMaterial(TerrainWaterMaterialRenderData material) noexcept;
    [[nodiscard]] float normalizedSkyTexelsPerUnit() const noexcept;
    [[nodiscard]] bool cloudAllowedByTimeOfDay() const noexcept;
    [[nodiscard]] container::StringView skyWaterTextureName() const noexcept;
    [[nodiscard]] uint32_t waterTextureSrvIndex() const noexcept;
    [[nodiscard]] uint32_t standingWaterTextureSrvIndex() const noexcept;
    [[nodiscard]] uint32_t skyWaterTextureSrvIndex() const noexcept;
    [[nodiscard]] uint32_t terrainCloudTextureSrvIndex() const noexcept;
    [[nodiscard]] uint32_t terrainMacroTextureSrvIndex() const noexcept;

private:
    container::SharedPtr<WorldTextureCache> m_cache;
    container::SharedPtr<const TerrainTextureResolver> m_resolver;
    TerrainWaterMaterialRenderData m_waterMaterial;
    float m_normalizedSkyTexelsPerUnit = 0.0f;
    container::String m_waterTextureName;
    uint32_t m_waterTextureSrvIndex = 0;
    bool m_ownsWaterTextureReference = false;
    container::String m_standingWaterTextureName;
    uint32_t m_standingWaterTextureSrvIndex = 0;
    bool m_ownsStandingWaterTextureReference = false;
    container::String m_skyWaterTextureName;
    uint32_t m_skyWaterTextureSrvIndex = 0;
    bool m_ownsSkyWaterTextureReference = false;
    container::String m_terrainCloudTextureName;
    uint32_t m_terrainCloudTextureSrvIndex = 0;
    bool m_ownsTerrainCloudTextureReference = false;
    container::String m_terrainMacroTextureName;
    uint32_t m_terrainMacroTextureSrvIndex = 0;
    bool m_ownsTerrainMacroTextureReference = false;
    bool m_cloudAllowedByTimeOfDay = true;
    TerrainTexturePreparationScope m_scope =
        TerrainTexturePreparationScope::Complete;
};

} // namespace detail
} // namespace engine::render
