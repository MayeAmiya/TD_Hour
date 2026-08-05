#include "TacticalRadarPresentation.h"

#include "engine/renderer/runtime/Renderer.h"
#include "engine/texture/TextureManager.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace engine::render {
namespace {

constexpr container::StringView kRuntimeTextureKey = "tactical_radar";

[[nodiscard]] RenderVector heightShadedTerrainColor(
    RenderVector color, float height, float averageZ,
    float maximumZ, float minimumZ) noexcept {
    // RefCode calls interpolateColorForHeight(color, z, average, max, min).
    // Preserve that parameter order, including its historical naming quirk.
    float high = averageZ;
    float middle = maximumZ;
    float low = minimumZ;
    if (high == middle) high = middle + 0.1f;
    if (middle == low) low = middle - 0.1f;
    if (high == low) high = low + 0.2f;
    float amount = 0.0f;
    RenderVector target;
    if (height >= middle) {
        amount = (height - middle) / (high - middle);
        target = color + (RenderVector{1.0f, 1.0f, 1.0f} - color) * 0.95f;
    } else {
        amount = (middle - height) / (middle - low);
        target = color * 0.40f;
    }
    const RenderVector shaded = color + (target - color) * amount;
    return {
        std::clamp(shaded.x(), 0.0f, 1.0f),
        std::clamp(shaded.y(), 0.0f, 1.0f),
        std::clamp(shaded.z(), 0.0f, 1.0f),
    };
}

[[nodiscard]] bool finiteExtent(const TerrainRenderSnapshot& terrain,
                                float& minX, float& minY,
                                float& maxX, float& maxY) noexcept {
    minX = terrain.playableMinimum.x();
    minY = terrain.playableMinimum.y();
    maxX = terrain.playableMaximum.x();
    maxY = terrain.playableMaximum.y();
    if (!std::isfinite(minX) || !std::isfinite(minY) ||
        !std::isfinite(maxX) || !std::isfinite(maxY) ||
        maxX <= minX || maxY <= minY) {
        minX = 0.0f;
        minY = 0.0f;
        maxX = static_cast<float>(std::max(
            0, terrain.width - terrain.borderSize * 2)) *
            terrain.cellWorldSize;
        maxY = static_cast<float>(std::max(
            0, terrain.height - terrain.borderSize * 2)) *
            terrain.cellWorldSize;
    }
    return std::isfinite(minX) && std::isfinite(minY) &&
        std::isfinite(maxX) && std::isfinite(maxY) &&
        maxX > minX && maxY > minY;
}

[[nodiscard]] bool pointInPolygon(float x, float y,
    const container::Vector<RenderVector>& polygon) noexcept {
    if (polygon.size() < 3) return false;
    bool inside = false;
    size_t previous = polygon.size() - 1u;
    for (size_t current = 0; current < polygon.size(); ++current) {
        const float ax = polygon[current].x();
        const float ay = polygon[current].y();
        const float bx = polygon[previous].x();
        const float by = polygon[previous].y();
        if (((ay > y) != (by > y)) &&
            x < (bx - ax) * (y - ay) /
                    ((by - ay) == 0.0f ? math::EPSILON : (by - ay)) + ax) {
            inside = !inside;
        }
        previous = current;
    }
    return inside;
}

[[nodiscard]] float terrainSurfaceHeightAt(
    const TerrainRenderSnapshot& terrain, float worldX,
    float worldY) noexcept {
    if (terrain.width < 2 || terrain.height < 2 ||
        !std::isfinite(worldX) || !std::isfinite(worldY)) {
        return 0.0f;
    }
    const float cellSize = std::max(terrain.cellWorldSize, math::EPSILON);
    float gridX = worldX / cellSize + static_cast<float>(terrain.borderSize);
    float gridY = worldY / cellSize + static_cast<float>(terrain.borderSize);
    gridX = std::clamp(gridX, 0.0f,
        static_cast<float>(terrain.width - 1));
    gridY = std::clamp(gridY, 0.0f,
        static_cast<float>(terrain.height - 1));
    const int32_t cellX = std::min(
        static_cast<int32_t>(std::floor(gridX)), terrain.width - 2);
    const int32_t cellY = std::min(
        static_cast<int32_t>(std::floor(gridY)), terrain.height - 2);
    const float fractionX = gridX - static_cast<float>(cellX);
    const float fractionY = gridY - static_cast<float>(cellY);
    const float p0 = terrain.heightWorld(cellX, cellY);
    const float p1 = terrain.heightWorld(cellX + 1, cellY);
    const float p2 = terrain.heightWorld(cellX + 1, cellY + 1);
    const float p3 = terrain.heightWorld(cellX, cellY + 1);
    if (fractionY > fractionX) {
        return p3 + (1.0f - fractionY) * (p0 - p3) +
            fractionX * (p2 - p3);
    }
    return p1 + fractionY * (p2 - p1) +
        (1.0f - fractionX) * (p0 - p1);
}

[[nodiscard]] float radarWorldX(
    uint32_t pixelX, float minimumX, float maximumX) noexcept {
    return minimumX + static_cast<float>(pixelX) /
        static_cast<float>(kTacticalRadarTextureSize) *
        (maximumX - minimumX);
}

[[nodiscard]] float radarWorldY(
    uint32_t pixelY, float minimumY, float maximumY) noexcept {
    return minimumY + static_cast<float>(
        kTacticalRadarTextureSize - 1u - pixelY) /
        static_cast<float>(kTacticalRadarTextureSize) *
        (maximumY - minimumY);
}

[[nodiscard]] const TerrainWaterRenderArea* underwaterAreaAt(
    const TerrainRenderSnapshot& terrain, float worldX,
    float worldY, float groundHeight) noexcept {
    const float legacyX = std::floor(worldX + 0.5f);
    const float legacyY = std::floor(worldY + 0.5f);
    const TerrainWaterRenderArea* highest = nullptr;
    for (const TerrainWaterRenderArea& water : terrain.waterAreas) {
        if (std::isfinite(water.surfaceHeight) &&
            groundHeight < water.surfaceHeight &&
            pointInPolygon(legacyX, legacyY, water.polygon) &&
            (!highest || water.surfaceHeight >= highest->surfaceHeight)) {
            highest = &water;
        }
    }
    return highest;
}

[[nodiscard]] uint64_t bridgeRadarGeometryIdentity(
    container::Span<const TerrainBridgeRadarGeometry> geometry) noexcept {
    uint64_t hash = 1469598103934665603ull;
    const auto combine = [&hash](uint64_t value) {
        for (uint32_t byte = 0; byte < 8u; ++byte) {
            hash ^= (value >> (byte * 8u)) & 0xffu;
            hash *= 1099511628211ull;
        }
    };
    combine(geometry.size());
    for (const TerrainBridgeRadarGeometry& bridge : geometry) {
        combine(bridge.sourceRecordIndex);
        combine(std::bit_cast<uint32_t>(bridge.bridgeWidth));
    }
    return hash != 0u ? hash : 1u;
}

[[nodiscard]] float radarTerrainAverageZ(
    const TerrainRenderSnapshot& terrain, float minimumX, float minimumY,
    float maximumX, float maximumY) noexcept {
    double heightSum = 0.0;
    uint32_t samples = 0;
    for (uint32_t y = 0; y < kTacticalRadarTextureSize; y += 2u) {
        const float worldY = minimumY + static_cast<float>(y) /
            static_cast<float>(kTacticalRadarTextureSize) *
            (maximumY - minimumY);
        for (uint32_t x = 0; x < kTacticalRadarTextureSize; x += 2u) {
            const float worldX = minimumX + static_cast<float>(x) /
                static_cast<float>(kTacticalRadarTextureSize) *
                (maximumX - minimumX);
            const float height = terrainSurfaceHeightAt(
                terrain, worldX, worldY);
            if (underwaterAreaAt(terrain, worldX, worldY, height)) continue;
            heightSum += height;
            ++samples;
        }
    }
    return samples != 0u
        ? static_cast<float>(heightSum / static_cast<double>(samples))
        : 0.0f;
}

void writePixel(container::Vector<uint8_t>& pixels, int32_t x, int32_t y,
                uint8_t red, uint8_t green, uint8_t blue,
                float visibilityScale = 1.0f) noexcept {
    if (x < 0 || y < 0 ||
        x >= static_cast<int32_t>(kTacticalRadarTextureSize) ||
        y >= static_cast<int32_t>(kTacticalRadarTextureSize)) return;
    const size_t offset = (static_cast<size_t>(y) * kTacticalRadarTextureSize +
                           static_cast<size_t>(x)) * 4u;
    pixels[offset + 0u] = static_cast<uint8_t>(
        std::clamp(std::lround(red * visibilityScale), 0l, 255l));
    pixels[offset + 1u] = static_cast<uint8_t>(
        std::clamp(std::lround(green * visibilityScale), 0l, 255l));
    pixels[offset + 2u] = static_cast<uint8_t>(
        std::clamp(std::lround(blue * visibilityScale), 0l, 255l));
    pixels[offset + 3u] = 255u;
}

[[nodiscard]] float visibilityScaleAt(
    const LocalVisibilityRenderSnapshot& visibility,
    RenderVector world, bool spectator) noexcept {
    if (spectator || !visibility.enabled) return 1.0f;
    switch (visibility.worldState(world)) {
    case LocalVisibilityRenderCellState::Visible: return 1.0f;
    case LocalVisibilityRenderCellState::Explored: return 0.5f;
    case LocalVisibilityRenderCellState::Shrouded: return 0.0f;
    }
    return 0.0f;
}

[[nodiscard]] std::optional<std::pair<math::vec2, math::vec2>>
clipLineToRadar(math::vec2 start, math::vec2 end,
                const TacticalRadarLayout& layout) noexcept {
    const float left = layout.left;
    const float right = layout.left + layout.width;
    const float top = layout.top;
    const float bottom = layout.top + layout.height;
    const float deltaX = end.x() - start.x();
    const float deltaY = end.y() - start.y();
    float enter = 0.0f;
    float leave = 1.0f;
    const auto clip = [&enter, &leave](float denominator, float numerator) {
        if (std::abs(denominator) <= math::EPSILON) return numerator >= 0.0f;
        const float ratio = numerator / denominator;
        if (denominator < 0.0f) {
            if (ratio > leave) return false;
            enter = std::max(enter, ratio);
        } else {
            if (ratio < enter) return false;
            leave = std::min(leave, ratio);
        }
        return true;
    };
    if (!clip(-deltaX, start.x() - left) ||
        !clip(deltaX, right - start.x()) ||
        !clip(-deltaY, start.y() - top) ||
        !clip(deltaY, bottom - start.y()) ||
        enter > leave) {
        return std::nullopt;
    }
    return std::pair<math::vec2, math::vec2>{
        math::vec2{start.x() + deltaX * enter,
                   start.y() + deltaY * enter},
        math::vec2{start.x() + deltaX * leave,
                   start.y() + deltaY * leave}};
}

} // namespace

container::Vector<TerrainRadarTileColor> terrainRadarTileAverageColors(
    container::Span<const uint8_t> rgbaPixels,
    uint32_t textureWidth, uint32_t textureHeight,
    uint32_t sourceGridWidth, uint32_t tileCount) {
    container::Vector<TerrainRadarTileColor> output;
    const uint64_t sourceTileCapacity =
        static_cast<uint64_t>(sourceGridWidth) * sourceGridWidth;
    const uint64_t expectedBytes = static_cast<uint64_t>(textureWidth) *
        textureHeight * 4u;
    if (textureWidth == 0u || textureHeight == 0u ||
        sourceGridWidth == 0u || tileCount == 0u ||
        textureWidth % sourceGridWidth != 0u ||
        textureHeight % sourceGridWidth != 0u ||
        expectedBytes > std::numeric_limits<size_t>::max() ||
        rgbaPixels.size() != static_cast<size_t>(expectedBytes)) {
        return output;
    }
    const uint32_t tileWidth = textureWidth / sourceGridWidth;
    const uint32_t tileHeight = textureHeight / sourceGridWidth;
    if (tileWidth == 0u || tileWidth != tileHeight ||
        (tileWidth & (tileWidth - 1u)) != 0u) {
        return output;
    }
    const uint32_t usableTileCount = static_cast<uint32_t>(
        std::min<uint64_t>(tileCount, sourceTileCapacity));
    output.resize(usableTileCount);
    for (uint32_t tile = 0; tile < usableTileCount; ++tile) {
        const uint32_t tileX = tile % sourceGridWidth;
        const uint32_t tileRowFromBottom = tile / sourceGridWidth;
        const uint32_t tileY =
            sourceGridWidth - tileRowFromBottom - 1u;
        container::Vector<uint8_t> mip(
            static_cast<size_t>(tileWidth) * tileWidth * 3u);
        for (uint32_t y = 0; y < tileWidth; ++y) {
            const uint32_t sourceY = tileY * tileWidth + y;
            for (uint32_t x = 0; x < tileWidth; ++x) {
                const uint32_t sourceX = tileX * tileWidth + x;
                const size_t sourceOffset =
                    (static_cast<size_t>(sourceY) * textureWidth + sourceX) *
                    4u;
                const size_t destinationOffset =
                    (static_cast<size_t>(y) * tileWidth + x) * 3u;
                mip[destinationOffset] = rgbaPixels[sourceOffset];
                mip[destinationOffset + 1u] = rgbaPixels[sourceOffset + 1u];
                mip[destinationOffset + 2u] = rgbaPixels[sourceOffset + 2u];
            }
        }
        uint32_t mipWidth = tileWidth;
        while (mipWidth > 1u) {
            const uint32_t nextWidth = mipWidth / 2u;
            container::Vector<uint8_t> next(
                static_cast<size_t>(nextWidth) * nextWidth * 3u);
            for (uint32_t y = 0; y < mipWidth; y += 2u) {
                for (uint32_t x = 0; x < mipWidth; x += 2u) {
                    for (uint32_t channel = 0; channel < 3u; ++channel) {
                        const auto sample = [&](uint32_t sampleX,
                                                uint32_t sampleY) {
                            return static_cast<uint32_t>(mip[
                                (static_cast<size_t>(sampleY) * mipWidth +
                                 sampleX) * 3u + channel]);
                        };
                        next[(static_cast<size_t>(y / 2u) * nextWidth +
                              x / 2u) * 3u + channel] =
                            static_cast<uint8_t>((sample(x, y) +
                                sample(x + 1u, y) + sample(x, y + 1u) +
                                sample(x + 1u, y + 1u) + 2u) / 4u);
                    }
                }
            }
            mip = std::move(next);
            mipWidth = nextWidth;
        }
        output[tile] = {
            .color = {
                static_cast<float>(mip[0]) / 255.0f,
                static_cast<float>(mip[1]) / 255.0f,
                static_cast<float>(mip[2]) / 255.0f,
            },
            .valid = true,
        };
    }
    return output;
}

TacticalRadarLayout tacticalRadarLayout(
    const TerrainRenderSnapshot& terrain, float panelLeft, float panelTop,
    float panelWidth, float panelHeight) noexcept {
    float minX = 0, minY = 0, maxX = 0, maxY = 0;
    if (!finiteExtent(terrain, minX, minY, maxX, maxY) ||
        panelWidth <= 0.0f || panelHeight <= 0.0f) return {};
    const float worldWidth = maxX - minX;
    const float worldHeight = maxY - minY;
    const float scale = std::min(panelWidth / worldWidth,
                                 panelHeight / worldHeight);
    const float width = worldWidth * scale;
    const float height = worldHeight * scale;
    return {
        .left = panelLeft + (panelWidth - width) * 0.5f,
        .top = panelTop + (panelHeight - height) * 0.5f,
        .width = width,
        .height = height,
    };
}

TacticalRadarLayout tacticalRadarLayoutForViewport(
    const TerrainRenderSnapshot& terrain,
    float virtualWidth, float virtualHeight) noexcept {
    if (!std::isfinite(virtualWidth) || !std::isfinite(virtualHeight) ||
        virtualWidth <= 0.0f || virtualHeight <= 0.0f) return {};
    const float panelSize = std::clamp(
        std::min(virtualWidth * 0.22f, virtualHeight * 0.25f),
        96.0f, 176.0f);
    const float panelLeft = 12.0f;
    const float panelTop = std::max(
        8.0f, virtualHeight - panelSize - 12.0f);
    return tacticalRadarLayout(
        terrain, panelLeft, panelTop, panelSize, panelSize);
}

std::optional<math::vec2> tacticalRadarPixelForWorld(
    const TerrainRenderSnapshot& terrain, const TacticalRadarLayout& layout,
    RenderVector worldPosition) noexcept {
    float minX = 0, minY = 0, maxX = 0, maxY = 0;
    if (!finiteExtent(terrain, minX, minY, maxX, maxY) ||
        layout.width <= 0.0f || layout.height <= 0.0f ||
        !std::isfinite(worldPosition.x()) ||
        !std::isfinite(worldPosition.y()) ||
        worldPosition.x() < minX || worldPosition.x() > maxX ||
        worldPosition.y() < minY || worldPosition.y() > maxY) {
        return std::nullopt;
    }
    // Never clamp an off-map object onto the radar rim. RefCode's partition
    // gives such an object no cells, so it is fully shrouded even to an
    // observer; the radar follows that same active-boundary contract.
    const float x = (worldPosition.x() - minX) / (maxX - minX);
    const float y = (worldPosition.y() - minY) / (maxY - minY);
    return math::vec2{layout.left + x * layout.width,
                      layout.top + (1.0f - y) * layout.height};
}

std::optional<RenderVector> tacticalRadarWorldForPixel(
    const TerrainRenderSnapshot& terrain, const TacticalRadarLayout& layout,
    math::vec2 pixel, float worldZ) noexcept {
    float minX = 0, minY = 0, maxX = 0, maxY = 0;
    if (!finiteExtent(terrain, minX, minY, maxX, maxY) ||
        layout.width <= 0.0f || layout.height <= 0.0f ||
        !std::isfinite(pixel.x()) || !std::isfinite(pixel.y()) ||
        pixel.x() < layout.left || pixel.x() > layout.left + layout.width ||
        pixel.y() < layout.top || pixel.y() > layout.top + layout.height) {
        return std::nullopt;
    }
    const float x = (pixel.x() - layout.left) / layout.width;
    const float y = 1.0f - (pixel.y() - layout.top) / layout.height;
    return RenderVector{
        minX + x * (maxX - minX), minY + y * (maxY - minY), worldZ};
}

bool tacticalRadarObjectVisible(
    const ObjectUiRenderSnapshot& object, bool spectator) noexcept {
    if ((!object.radarStructure && !object.radarUnit) ||
        object.effectivelyDead ||
        object.ignoredInGui ||
        (!spectator && object.visibility !=
            LocalVisibilityRenderCellState::Visible)) return false;
    if (object.radarLocalOnly && !spectator &&
        object.relationship != ObjectUiRelationship::Owned) return false;
    if (!spectator && object.relationship == ObjectUiRelationship::Enemy &&
        object.stealthed && !object.detected) return false;
    return true;
}

std::optional<RenderEntityId> tacticalRadarObjectHitTest(
    const ObjectUiRenderState& objects,
    const TerrainRenderSnapshot& terrain,
    const TacticalRadarLayout& layout,
    math::vec2 pixel, bool spectator,
    float hitRadiusPixels) noexcept {
    if (!std::isfinite(hitRadiusPixels) || hitRadiusPixels <= 0.0f ||
        !tacticalRadarWorldForPixel(terrain, layout, pixel)) {
        return std::nullopt;
    }
    std::optional<RenderEntityId> hit;
    float closest = std::numeric_limits<float>::max();
    for (const ObjectUiRenderSnapshot& object : objects.objects) {
        if (!tacticalRadarObjectVisible(object, spectator)) continue;
        const std::optional<math::vec2> objectPixel = tacticalRadarPixelForWorld(
            terrain, layout, object.worldPosition);
        if (!objectPixel) continue;
        const float dx = pixel.x() - objectPixel->x();
        const float dy = pixel.y() - objectPixel->y();
        const float distanceSquared = dx * dx + dy * dy;
        const float objectRadius = hitRadiusPixels +
            (object.radarStructure ? 1.5f : 1.0f);
        if (distanceSquared > objectRadius * objectRadius) continue;
        if (!hit || distanceSquared < closest ||
            (distanceSquared == closest && object.objectId < *hit)) {
            hit = object.objectId;
            closest = distanceSquared;
        }
    }
    return hit;
}

std::optional<container::Array<math::vec2, 4>>
tacticalRadarCameraViewPolygon(
    const TerrainRenderSnapshot& terrain,
    const TacticalRadarLayout& layout,
    const RenderCameraSnapshot& camera,
    float fullViewportAspectRatio,
    float terrainAverageZ) noexcept {
    float minimumX = 0.0f, minimumY = 0.0f;
    float maximumX = 0.0f, maximumY = 0.0f;
    if (!finiteExtent(
            terrain, minimumX, minimumY, maximumX, maximumY) ||
        layout.width <= 0.0f || layout.height <= 0.0f ||
        !std::isfinite(terrainAverageZ)) {
        return std::nullopt;
    }
    RenderVector forward = camera.target - camera.position;
    const float forwardLength = forward.length();
    RenderVector cameraUp = camera.up;
    const float upLength = cameraUp.length();
    if (!std::isfinite(forwardLength) || !std::isfinite(upLength) ||
        forwardLength <= math::EPSILON || upLength <= math::EPSILON) {
        return std::nullopt;
    }
    forward = forward / forwardLength;
    cameraUp = cameraUp / upLength;
    RenderVector right = forward.cross(cameraUp);
    const float rightLength = right.length();
    if (!std::isfinite(rightLength) || rightLength <= math::EPSILON) {
        return std::nullopt;
    }
    right = right / rightLength;
    cameraUp = right.cross(forward).normalized();
    const float verticalTangent = std::tan(
        renderCameraVerticalFovRadians(camera, fullViewportAspectRatio) *
        0.5f);
    const float horizontalTangent = verticalTangent *
        renderCameraEffectiveAspectRatio(camera, fullViewportAspectRatio);
    if (!std::isfinite(verticalTangent) ||
        !std::isfinite(horizontalTangent)) {
        return std::nullopt;
    }
    const container::Array<math::vec2, 4> corners{{
        {-1.0f, 1.0f}, {1.0f, 1.0f},
        {1.0f, -1.0f}, {-1.0f, -1.0f},
    }};
    container::Array<math::vec2, 4> output{};
    for (size_t index = 0; index < corners.size(); ++index) {
        RenderVector direction = forward +
            right * (corners[index].x() * horizontalTangent) +
            cameraUp * (corners[index].y() * verticalTangent);
        if (!std::isfinite(direction.z()) ||
            std::abs(direction.z()) <= math::EPSILON) {
            return std::nullopt;
        }
        const float distance =
            (terrainAverageZ - camera.position.z()) / direction.z();
        if (!std::isfinite(distance) || distance < 0.0f) {
            return std::nullopt;
        }
        const RenderVector world = camera.position + direction * distance;
        const float normalizedX =
            (world.x() - minimumX) / (maximumX - minimumX);
        const float normalizedY =
            (world.y() - minimumY) / (maximumY - minimumY);
        output[index] = {
            layout.left + normalizedX * layout.width,
            layout.top + (1.0f - normalizedY) * layout.height,
        };
        if (!std::isfinite(output[index].x()) ||
            !std::isfinite(output[index].y())) {
            return std::nullopt;
        }
    }
    return output;
}

void TacticalRadarPresentation::reset() noexcept {
    m_epoch = 0;
    m_terrainRevision = 0;
    m_layoutRevision = 0;
    m_borderShroudRevision = 0;
    m_waterRevision = 0;
    m_bridgeRevision = 0;
    m_bridgeGeometryIdentity = 0;
    m_visibilityRevision = 0;
    m_visibilityPolicyRevision = 0;
    m_paletteLayoutRevision = 0;
    m_paletteSourceIdentity = 0;
    m_terrainTileColors.clear();
    m_spectator = false;
}

uint64_t TacticalRadarPresentation::terrainPaletteSourceIdentity(
    const TerrainRenderSnapshot& terrain,
    engine::TextureManager& textures) {
    constexpr uint64_t offsetBasis = 1469598103934665603ull;
    constexpr uint64_t prime = 1099511628211ull;
    uint64_t identity = offsetBasis;
    const auto combine = [&](uint64_t value) {
        for (uint32_t byte = 0; byte < 8u; ++byte) {
            identity ^= (value >> (byte * 8u)) & 0xffu;
            identity *= prime;
        }
    };
    if (terrain.materials &&
        terrain.materials->isValidFor(terrain.heights.size())) {
        for (const TerrainTextureClassRenderData& textureClass :
             terrain.materials->textureClasses) {
            if (textureClass.firstTile < 0 || textureClass.tileCount <= 0) {
                continue;
            }
            const TerrainTextureResolution resolved =
                m_terrainTextureResolver.resolve(textureClass.name);
            const engine::RawTexture* texture = textures.loadTexture(
                resolved.textureName);
            combine(texture && texture != textures.getPlaceholder()
                ? texture->rendererIdentity : 0u);
        }
    }
    return identity != 0u ? identity : 1u;
}

void TacticalRadarPresentation::rebuildTerrainTileColors(
    const TerrainRenderSnapshot& terrain,
    engine::TextureManager& textures,
    uint64_t sourceIdentity) {
    m_terrainTileColors.clear();
    m_paletteLayoutRevision = terrain.layoutRevision;
    m_paletteSourceIdentity = sourceIdentity;
    if (!terrain.materials ||
        !terrain.materials->isValidFor(terrain.heights.size())) return;
    const TerrainMaterialRenderData& materials = *terrain.materials;
    size_t paletteSize = static_cast<size_t>(std::max(
        0, materials.bitmapTileCount));
    for (const TerrainTextureClassRenderData& textureClass :
         materials.textureClasses) {
        if (textureClass.firstTile < 0 || textureClass.tileCount <= 0) {
            continue;
        }
        paletteSize = std::max(paletteSize,
            static_cast<size_t>(textureClass.firstTile) +
                static_cast<size_t>(textureClass.tileCount));
    }
    // Classic terrain is bounded to 10x10 tiles per class; this larger guard
    // rejects malformed map counts without converting authored colour into an
    // operational runtime budget.
    constexpr size_t hardMaximumSourceTiles = 65536u;
    if (paletteSize == 0u || paletteSize > hardMaximumSourceTiles) return;
    m_terrainTileColors.resize(paletteSize);

    for (const TerrainTextureClassRenderData& textureClass :
         materials.textureClasses) {
        if (textureClass.firstTile < 0 || textureClass.tileCount <= 0) {
            continue;
        }
        const int32_t gridWidth = terrainSourceGridWidth(textureClass);
        if (gridWidth <= 0) continue;
        const TerrainTextureResolution resolved =
            m_terrainTextureResolver.resolve(textureClass.name);
        const engine::RawTexture* texture = textures.loadTexture(
            resolved.textureName);
        if (!texture || texture == textures.getPlaceholder() ||
            !texture->hasData()) {
            continue;
        }
        const container::Vector<TerrainRadarTileColor> colors =
            terrainRadarTileAverageColors(
                texture->pixels, texture->width, texture->height,
                static_cast<uint32_t>(gridWidth),
                static_cast<uint32_t>(textureClass.tileCount));
        for (size_t local = 0; local < colors.size(); ++local) {
            const size_t destination =
                static_cast<size_t>(textureClass.firstTile) + local;
            if (destination >= m_terrainTileColors.size()) break;
            m_terrainTileColors[destination] = colors[local];
        }
    }
}

container::Vector<uint8_t> tacticalRadarTerrainPixels(
    const TerrainRenderSnapshot& terrain,
    const LocalVisibilityRenderSnapshot& visibility,
    bool spectator,
    container::Span<const TerrainRadarTileColor> terrainTileColors,
    container::Span<const TerrainBridgeRadarGeometry> bridgeGeometry) {
    float minX = 0, minY = 0, maxX = 0, maxY = 0;
    if (!terrain.isValid() ||
        !finiteExtent(terrain, minX, minY, maxX, maxY)) {
        return {};
    }
    container::Vector<uint8_t> pixels(
        static_cast<size_t>(kTacticalRadarTextureSize) *
            kTacticalRadarTextureSize * 4u,
        255u);
    float minimumZ = std::numeric_limits<float>::max();
    float maximumZ = std::numeric_limits<float>::lowest();
    for (int32_t y = 0; y < terrain.height; ++y) {
        for (int32_t x = 0; x < terrain.width; ++x) {
            const float height = terrain.heightWorld(x, y);
            minimumZ = std::min(minimumZ, height);
            maximumZ = std::max(maximumZ, height);
        }
    }
    const float averageZ = radarTerrainAverageZ(
        terrain, minX, minY, maxX, maxY);
    const TerrainMaterialRenderData* materials =
        terrain.materials &&
            terrain.materials->isValidFor(terrain.heights.size())
        ? &*terrain.materials : nullptr;
    const auto terrainColorAt = [&](float worldX, float worldY) {
        const int32_t sampleX = std::clamp(
            static_cast<int32_t>(std::floor(
                worldX / std::max(terrain.cellWorldSize, math::EPSILON))) +
                terrain.borderSize,
            0, std::max(0, terrain.width - 1));
        const int32_t sampleY = std::clamp(
            static_cast<int32_t>(std::floor(
                worldY / std::max(terrain.cellWorldSize, math::EPSILON))) +
                terrain.borderSize,
            0, std::max(0, terrain.height - 1));
        const float height = terrainSurfaceHeightAt(
            terrain, worldX, worldY);
        RenderVector color{};
        bool authored = false;
        if (materials) {
            const size_t sampleIndex =
                static_cast<size_t>(sampleY) * terrain.width + sampleX;
            if (sampleIndex < materials->baseTileIndices.size()) {
                const int32_t sourceTile =
                    materials->baseTileIndices[sampleIndex] >> 2;
                if (sourceTile >= 0 &&
                    static_cast<size_t>(sourceTile) <
                        terrainTileColors.size() &&
                    terrainTileColors[static_cast<size_t>(sourceTile)].valid) {
                    color = terrainTileColors[
                        static_cast<size_t>(sourceTile)].color;
                    authored = true;
                }
            }
        }
        return authored
            ? heightShadedTerrainColor(
                  color, height, averageZ, maximumZ, minimumZ)
            : color;
    };
    for (uint32_t y = 0; y < kTacticalRadarTextureSize; ++y) {
        const float worldY = radarWorldY(y, minY, maxY);
        for (uint32_t x = 0; x < kTacticalRadarTextureSize; ++x) {
            const float worldX = radarWorldX(x, minX, maxX);
            RenderVector averagedColor{};
            uint32_t samples = 0;
            const float centerGround = terrainSurfaceHeightAt(
                terrain, worldX, worldY);
            const TerrainWaterRenderArea* centerWater = underwaterAreaAt(
                terrain, worldX, worldY, centerGround);
            for (int32_t sampleY = static_cast<int32_t>(y) - 1;
                 sampleY <= static_cast<int32_t>(y) + 1; ++sampleY) {
                if (sampleY < 0 || sampleY >=
                        static_cast<int32_t>(kTacticalRadarTextureSize)) {
                    continue;
                }
                const float sampleWorldY = radarWorldY(
                    static_cast<uint32_t>(sampleY), minY, maxY);
                for (int32_t sampleX = static_cast<int32_t>(x) - 1;
                     sampleX <= static_cast<int32_t>(x) + 1; ++sampleX) {
                    if (sampleX < 0 || sampleX >=
                            static_cast<int32_t>(kTacticalRadarTextureSize)) {
                        continue;
                    }
                    const float sampleWorldX = radarWorldX(
                        static_cast<uint32_t>(sampleX), minX, maxX);
                    if (centerWater) {
                        const float sampleGround = terrainSurfaceHeightAt(
                            terrain, sampleWorldX, sampleWorldY);
                        if (!underwaterAreaAt(terrain, sampleWorldX,
                                             sampleWorldY, sampleGround)) {
                            continue;
                        }
                        const math::vec4 radarWater = terrain.waterMaterial
                            ? terrain.waterMaterial->radarWaterColor
                            : math::vec4{0.55f, 0.55f, 1.0f, 1.0f};
                        averagedColor += heightShadedTerrainColor(
                            {radarWater.x(), radarWater.y(), radarWater.z()},
                            sampleGround, centerWater->surfaceHeight,
                            centerWater->surfaceHeight, minimumZ);
                    } else {
                        averagedColor += terrainColorAt(
                            sampleWorldX, sampleWorldY);
                    }
                    ++samples;
                }
            }
            if (samples != 0u) {
                averagedColor = averagedColor *
                    (1.0f / static_cast<float>(samples));
            }
            uint8_t red = static_cast<uint8_t>(std::clamp(
                averagedColor.x() * 255.0f, 0.0f, 255.0f));
            uint8_t green = static_cast<uint8_t>(std::clamp(
                averagedColor.y() * 255.0f, 0.0f, 255.0f));
            uint8_t blue = static_cast<uint8_t>(std::clamp(
                averagedColor.z() * 255.0f, 0.0f, 255.0f));
            const RenderVector world{worldX, worldY, 0.0f};
            writePixel(pixels, static_cast<int32_t>(x),
                static_cast<int32_t>(y), red, green, blue,
                visibilityScaleAt(visibility, world, spectator));
        }
    }

    // RefCode asks TerrainLogic::findBridgeAt for every radar sample, which
    // covers the complete deck quadrilateral and suppresses water beneath it.
    // Our detached bridge is represented by its centre endpoints. Once the
    // renderer has parsed BRIDGE_LEFT, its real W3D lateral extent overrides
    // the pre-upload 34*BridgeScale fallback. Only BODY_RUBBLE removes it.
    for (const TerrainBridgeRenderData& bridge : terrain.bridges) {
        float bridgeWidth = bridge.bridgeWidth;
        for (const TerrainBridgeRadarGeometry& resolved : bridgeGeometry) {
            if (resolved.sourceRecordIndex == bridge.sourceRecordIndex &&
                std::isfinite(resolved.bridgeWidth) &&
                resolved.bridgeWidth > 0.0f) {
                bridgeWidth = resolved.bridgeWidth;
                break;
            }
        }
        if (bridge.damageState == TerrainBridgeDamageState::Rubble ||
            !std::isfinite(bridgeWidth) || bridgeWidth <= 0.0f) {
            continue;
        }
        const float deltaX = bridge.end.x() - bridge.start.x();
        const float deltaY = bridge.end.y() - bridge.start.y();
        const float lengthSquared = deltaX * deltaX + deltaY * deltaY;
        if (!std::isfinite(lengthSquared) || lengthSquared <= math::EPSILON) {
            continue;
        }
        const float halfWidthSquared =
            bridgeWidth * bridgeWidth * 0.25f;
        const float bridgeZ = (bridge.start.z() + bridge.end.z()) * 0.5f;
        const RenderVector bridgeBaseColor = bridge.hasRadarColor
            ? RenderVector{bridge.radarColor[0], bridge.radarColor[1],
                           bridge.radarColor[2]}
            : RenderVector{1.0f, 1.0f, 1.0f};
        const RenderVector bridgeColor = heightShadedTerrainColor(
            bridgeBaseColor, bridgeZ, averageZ, maximumZ, minimumZ);
        const uint8_t red = static_cast<uint8_t>(std::clamp(
            bridgeColor.x() * 255.0f, 0.0f, 255.0f));
        const uint8_t green = static_cast<uint8_t>(std::clamp(
            bridgeColor.y() * 255.0f, 0.0f, 255.0f));
        const uint8_t blue = static_cast<uint8_t>(std::clamp(
            bridgeColor.z() * 255.0f, 0.0f, 255.0f));
        for (uint32_t y = 0; y < kTacticalRadarTextureSize; ++y) {
            const float worldY = radarWorldY(y, minY, maxY);
            for (uint32_t x = 0; x < kTacticalRadarTextureSize; ++x) {
                const float worldX = radarWorldX(x, minX, maxX);
                const float relativeX = worldX - bridge.start.x();
                const float relativeY = worldY - bridge.start.y();
                const float along =
                    (relativeX * deltaX + relativeY * deltaY) / lengthSquared;
                if (along < 0.0f || along > 1.0f) continue;
                const float nearestX = bridge.start.x() + deltaX * along;
                const float nearestY = bridge.start.y() + deltaY * along;
                const float awayX = worldX - nearestX;
                const float awayY = worldY - nearestY;
                if (awayX * awayX + awayY * awayY > halfWidthSquared) continue;
                const RenderVector world{worldX, worldY, bridgeZ};
                writePixel(pixels, static_cast<int32_t>(x),
                    static_cast<int32_t>(y), red, green, blue,
                    visibilityScaleAt(visibility, world, spectator));
            }
        }
    }
    return pixels;
}

const engine::RawTexture* TacticalRadarPresentation::rebuildTextureIfNeeded(
    const TerrainRenderSnapshot& terrain,
    const LocalVisibilityRenderSnapshot& visibility, bool spectator,
    container::Span<const TerrainBridgeRadarGeometry> bridgeGeometry,
    engine::TextureManager& textures) {
    const uint64_t paletteSourceIdentity =
        terrainPaletteSourceIdentity(terrain, textures);
    const bool paletteCurrent =
        m_paletteLayoutRevision == terrain.layoutRevision &&
        m_paletteSourceIdentity == paletteSourceIdentity;
    const uint64_t geometryIdentity =
        bridgeRadarGeometryIdentity(bridgeGeometry);
    const engine::RawTexture* cached =
        textures.getCached("runtime:" + container::String(kRuntimeTextureKey));
    const bool current = cached &&
        m_terrainRevision == terrain.revision &&
        m_layoutRevision == terrain.layoutRevision &&
        m_borderShroudRevision == terrain.borderShroudRevision &&
        m_waterRevision == terrain.waterRevision &&
        m_bridgeRevision == terrain.bridgeRevision &&
        m_bridgeGeometryIdentity == geometryIdentity &&
        m_visibilityRevision == visibility.revision &&
        m_visibilityPolicyRevision == visibility.policyRevision &&
        m_spectator == spectator && paletteCurrent;
    if (current) return cached;

    if (!paletteCurrent) {
        rebuildTerrainTileColors(
            terrain, textures, paletteSourceIdentity);
    }
    container::Vector<uint8_t> pixels = tacticalRadarTerrainPixels(
        terrain, visibility, spectator, m_terrainTileColors,
        bridgeGeometry);
    if (pixels.empty()) return nullptr;

    cached = textures.updateRuntimeRgbaTexture(
        container::String(kRuntimeTextureKey), kTacticalRadarTextureSize,
        kTacticalRadarTextureSize, std::move(pixels));
    m_terrainRevision = terrain.revision;
    m_layoutRevision = terrain.layoutRevision;
    m_borderShroudRevision = terrain.borderShroudRevision;
    m_waterRevision = terrain.waterRevision;
    m_bridgeRevision = terrain.bridgeRevision;
    m_bridgeGeometryIdentity = geometryIdentity;
    m_visibilityRevision = visibility.revision;
    m_visibilityPolicyRevision = visibility.policyRevision;
    m_spectator = spectator;
    return cached;
}

size_t TacticalRadarPresentation::render(
    const TacticalRadarRenderState& policy,
    const ObjectUiRenderState& objects,
    const container::SharedPtr<const TerrainRenderSnapshot>& terrain,
    const LocalVisibilityRenderSnapshot& visibility,
    container::Span<const TerrainBridgeRadarGeometry> bridgeGeometry,
    const RenderCameraSnapshot& camera,
    const RenderViewportMetrics& viewport, TacticalRadarLayout panel,
    bool panelVisible, uint64_t simulationFrame,
    engine::Renderer& renderer, engine::TextureManager& textures) {
    if (!policy.visible || !panelVisible || !terrain || !terrain->isValid())
        return 0;
    if (m_epoch != policy.presentationEpoch) {
        reset();
        // Terrain.ini is VFS content. Rebuild its renderer-owned value map at
        // a presentation epoch boundary so a new session/content mount cannot
        // retain texture names parsed under the previous archive set.
        m_terrainTextureResolver = TerrainTextureResolver{};
        m_epoch = policy.presentationEpoch;
    }
    const engine::RawTexture* map = rebuildTextureIfNeeded(
        *terrain, visibility, policy.spectator, bridgeGeometry, textures);
    if (!map) return 0;

    if (!viewport.valid()) return 0;
    if (!std::isfinite(panel.left) || !std::isfinite(panel.top) ||
        !std::isfinite(panel.width) || !std::isfinite(panel.height) ||
        panel.width <= 0.0f || panel.height <= 0.0f) return 0;
    const TacticalRadarLayout layout = tacticalRadarLayout(
        *terrain, panel.left, panel.top, panel.width, panel.height);
    if (layout.width <= 0.0f || layout.height <= 0.0f) return 0;
    renderer.drawQuad(
        panel.left, panel.top, panel.width, panel.height, 0xff000000u);
    renderer.drawTexture(map, layout.left, layout.top,
                         layout.width, layout.height, 0xffffffffu);
    size_t draws = 2;

    for (const ObjectUiRenderSnapshot& object : objects.objects) {
        if (!tacticalRadarObjectVisible(object, policy.spectator)) continue;
        const auto pixel = tacticalRadarPixelForWorld(*terrain, layout,
                                                       object.worldPosition);
        if (!pixel) continue;
        uint32_t color = object.indicatorColor;
        if (object.relationship == ObjectUiRelationship::Neutral)
            color = 0xffb0b0b0u;
        uint8_t alpha = 255u;
        if (object.stealthed &&
            (object.relationship == ObjectUiRelationship::Owned ||
             object.relationship == ObjectUiRelationship::Allied)) {
            const uint32_t frames = 30u;
            const float phase = static_cast<float>(simulationFrame % frames) /
                (static_cast<float>(frames) * 0.5f);
            alpha = static_cast<uint8_t>(std::clamp(
                std::lround((std::abs(phase - 1.0f) * 223.0f) + 32.0f),
                32l, 255l));
            color = (color & 0x00ffffffu) |
                (static_cast<uint32_t>(alpha) << 24u);
        }
        const float size = object.radarStructure ? 3.0f : 2.0f;
        renderer.drawQuad(pixel->x() - size * 0.5f,
                          pixel->y() - size * 0.5f,
                          size, size, color);
        ++draws;
    }

    for (const TacticalRadarEventRenderSnapshot& event : policy.events) {
        const auto pixel = tacticalRadarPixelForWorld(*terrain, layout,
                                                       event.worldPosition);
        if (!pixel) continue;
        const auto triangle = tacticalRadarEventTriangle(
            event, *pixel, layout.width, simulationFrame,
            objects.logicFramesPerSecond);
        if (!triangle) continue;
        for (size_t edge = 0; edge < triangle->vertices.size(); ++edge) {
            const size_t next = (edge + 1u) % triangle->vertices.size();
            const auto clipped = clipLineToRadar(
                triangle->vertices[edge], triangle->vertices[next], layout);
            if (!clipped) continue;
            renderer.drawLine(
                clipped->first.x(), clipped->first.y(),
                clipped->second.x(), clipped->second.y(), 1.0f,
                triangle->startColor, triangle->endColor);
        }
        ++draws;
    }

    float minimumX = 0.0f, minimumY = 0.0f;
    float maximumX = 0.0f, maximumY = 0.0f;
    if (finiteExtent(
            *terrain, minimumX, minimumY, maximumX, maximumY)) {
        const float averageZ = radarTerrainAverageZ(
            *terrain, minimumX, minimumY, maximumX, maximumY);
        const auto cameraPolygon = tacticalRadarCameraViewPolygon(
            *terrain, layout, camera,
            viewport.fullAspectRatio(),
            averageZ);
        if (cameraPolygon) {
            constexpr uint32_t topColor = 0xffe1e100u;
            constexpr uint32_t bottomColor = 0xff9e9e00u;
            constexpr container::Array<uint32_t, 4> startColors{
                topColor, topColor, bottomColor, bottomColor};
            constexpr container::Array<uint32_t, 4> endColors{
                topColor, bottomColor, bottomColor, topColor};
            for (size_t edge = 0; edge < cameraPolygon->size(); ++edge) {
                const size_t next = (edge + 1u) % cameraPolygon->size();
                const auto clipped = clipLineToRadar(
                    (*cameraPolygon)[edge], (*cameraPolygon)[next], layout);
                if (!clipped) continue;
                renderer.drawLine(
                    clipped->first.x(), clipped->first.y(),
                    clipped->second.x(), clipped->second.y(), 1.0f,
                    startColors[edge], endColors[edge]);
            }
            ++draws;
        }
    }
    return draws;
}

} // namespace engine::render
