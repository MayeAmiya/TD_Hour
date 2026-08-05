#pragma once

#include "presentation/render/RenderOverlaySnapshot.h"
#include "presentation/render/RenderViewSnapshot.h"
#include "presentation/render/TerrainRenderSnapshot.h"
#include "TacticalRadarEventPresentation.h"
#include "engine/renderer/world/terrain/TerrainTextureResolver.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace engine {
class Renderer;
class TextureManager;
struct RawTexture;
}

namespace engine::render {

inline constexpr uint32_t kTacticalRadarTextureSize = 128u;

struct TacticalRadarLayout final {
    float left = 0.0f;
    float top = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct TerrainRadarTileColor final {
    RenderVector color{};
    bool valid = false;
};

struct TerrainBridgeRadarGeometry final {
    uint64_t sourceRecordIndex = 0;
    float bridgeWidth = 0.0f;
};

// Exact CPU equivalent of TileData::getRGBDataForWidth(1) for one classic
// terrain atlas: each source tile is reduced to one RGB colour independently.
[[nodiscard]] container::Vector<TerrainRadarTileColor>
terrainRadarTileAverageColors(
    container::Span<const uint8_t> rgbaPixels,
    uint32_t textureWidth, uint32_t textureHeight,
    uint32_t sourceGridWidth, uint32_t tileCount);

[[nodiscard]] TacticalRadarLayout tacticalRadarLayout(
    const TerrainRenderSnapshot& terrain, float panelLeft, float panelTop,
    float panelWidth, float panelHeight) noexcept;
[[nodiscard]] TacticalRadarLayout tacticalRadarLayoutForViewport(
    const TerrainRenderSnapshot& terrain,
    float virtualWidth, float virtualHeight) noexcept;
[[nodiscard]] std::optional<math::vec2> tacticalRadarPixelForWorld(
    const TerrainRenderSnapshot& terrain, const TacticalRadarLayout& layout,
    RenderVector worldPosition) noexcept;
[[nodiscard]] std::optional<RenderVector> tacticalRadarWorldForPixel(
    const TerrainRenderSnapshot& terrain, const TacticalRadarLayout& layout,
    math::vec2 pixel, float worldZ = 0.0f) noexcept;
[[nodiscard]] bool tacticalRadarObjectVisible(
    const ObjectUiRenderSnapshot& object, bool spectator) noexcept;
[[nodiscard]] std::optional<RenderEntityId> tacticalRadarObjectHitTest(
    const ObjectUiRenderState& objects,
    const TerrainRenderSnapshot& terrain,
    const TacticalRadarLayout& layout,
    math::vec2 pixel, bool spectator,
    float hitRadiusPixels = 4.0f) noexcept;

// RefCode projects the four tactical-screen corner rays onto its frozen
// terrain-average-Z plane, producing a perspective quadrilateral rather than
// an axis-aligned approximation. Points may lie outside the radar layout;
// the presentation clips each edge at draw time.
[[nodiscard]] std::optional<container::Array<math::vec2, 4>>
tacticalRadarCameraViewPolygon(
    const TerrainRenderSnapshot& terrain,
    const TacticalRadarLayout& layout,
    const RenderCameraSnapshot& camera,
    float fullViewportAspectRatio,
    float terrainAverageZ) noexcept;

// Pure CPU terrain/minimap raster boundary. Bridges use their authored
// TerrainRoadType RadarColor across the full oriented bridge deck and remain
// subject to the same local visibility policy as terrain and water. A
// renderer-resolved W3D width overrides the deterministic pre-upload fallback.
// An empty result means the terrain extent is invalid.
[[nodiscard]] container::Vector<uint8_t> tacticalRadarTerrainPixels(
    const TerrainRenderSnapshot& terrain,
    const LocalVisibilityRenderSnapshot& visibility,
    bool spectator,
    container::Span<const TerrainRadarTileColor> terrainTileColors = {},
    container::Span<const TerrainBridgeRadarGeometry> bridgeGeometry = {});

// Client-only tactical minimap. Terrain/shroud is rebuilt into one procedural
// RGBA texture only when its immutable revisions change; object blips, events
// and the camera viewport remain cheap per-frame overlays.
class TacticalRadarPresentation final {
public:
    void reset() noexcept;
    [[nodiscard]] size_t render(
        const TacticalRadarRenderState& policy,
        const ObjectUiRenderState& objects,
        const container::SharedPtr<const TerrainRenderSnapshot>& terrain,
        const LocalVisibilityRenderSnapshot& visibility,
        container::Span<const TerrainBridgeRadarGeometry> bridgeGeometry,
        const RenderCameraSnapshot& camera,
        const RenderViewportMetrics& viewport,
        TacticalRadarLayout panel,
        bool panelVisible,
        uint64_t simulationFrame,
        engine::Renderer& renderer,
        engine::TextureManager& textures);

private:
    [[nodiscard]] uint64_t terrainPaletteSourceIdentity(
        const TerrainRenderSnapshot& terrain,
        engine::TextureManager& textures);
    void rebuildTerrainTileColors(
        const TerrainRenderSnapshot& terrain,
        engine::TextureManager& textures,
        uint64_t sourceIdentity);
    [[nodiscard]] const engine::RawTexture* rebuildTextureIfNeeded(
        const TerrainRenderSnapshot& terrain,
        const LocalVisibilityRenderSnapshot& visibility,
        bool spectator,
        container::Span<const TerrainBridgeRadarGeometry> bridgeGeometry,
        engine::TextureManager& textures);

    uint64_t m_epoch = 0;
    uint64_t m_terrainRevision = 0;
    uint64_t m_layoutRevision = 0;
    uint64_t m_borderShroudRevision = 0;
    uint64_t m_waterRevision = 0;
    uint64_t m_bridgeRevision = 0;
    uint64_t m_bridgeGeometryIdentity = 0;
    uint64_t m_visibilityRevision = 0;
    uint64_t m_visibilityPolicyRevision = 0;
    uint64_t m_paletteLayoutRevision = 0;
    uint64_t m_paletteSourceIdentity = 0;
    container::Vector<TerrainRadarTileColor> m_terrainTileColors;
    TerrainTextureResolver m_terrainTextureResolver;
    bool m_spectator = false;
};

} // namespace engine::render
