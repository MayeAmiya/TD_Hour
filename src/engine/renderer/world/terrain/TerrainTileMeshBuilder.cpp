#include "engine/renderer/world/terrain/TerrainTileMeshBuilder.h"

#include "engine/renderer/world/terrain/TerrainTextureResolver.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::render {
namespace {

constexpr uint8_t kBlendInvertedMask = 0x1;
constexpr uint8_t kBlendForcedFlipMask = 0x2;

float terrainHeightWorldUnchecked(
    const TerrainRenderSnapshot& terrain, int32_t x, int32_t y) noexcept {
    return static_cast<float>(terrain.heights[
        static_cast<size_t>(y) * static_cast<size_t>(terrain.width) +
        static_cast<size_t>(x)]) * terrain.heightWorldScale;
}

math::vec3 terrainWorldPositionUnchecked(
    const TerrainRenderSnapshot& terrain, int32_t x, int32_t y) noexcept {
    return {
        static_cast<float>(x - terrain.borderSize) * terrain.cellWorldSize,
        static_cast<float>(y - terrain.borderSize) * terrain.cellWorldSize,
        terrainHeightWorldUnchecked(terrain, x, y),
    };
}

void setError(container::String* error, container::String message) {
    if (error) *error = std::move(message);
}

math::vec3 terrainNormal(
    const TerrainRenderSnapshot& terrain, int32_t x, int32_t y) {
    const int32_t left = std::max(0, x - 1);
    const int32_t right = std::min(terrain.width - 1, x + 1);
    const int32_t down = std::max(0, y - 1);
    const int32_t up = std::min(terrain.height - 1, y + 1);
    const float dzdx =
        terrainHeightWorldUnchecked(terrain, right, y) -
        terrainHeightWorldUnchecked(terrain, left, y);
    const float dzdy =
        terrainHeightWorldUnchecked(terrain, x, up) -
        terrainHeightWorldUnchecked(terrain, x, down);
    return math::vec3{
        -dzdx, -dzdy, 2.0f * terrain.cellWorldSize}.normalized();
}

float finiteOrZero(float value) {
    return std::isfinite(value) ? value : 0.0f;
}

math::vec4 terrainVertexColor(
    const TerrainRenderSnapshot& terrain,
    const math::vec3& normal,
    const math::vec3& position,
    float alpha) {
    math::vec3 colour = terrain.globalLighting
        ? math::vec3{} : math::vec3{1.0f, 1.0f, 1.0f};
    if (terrain.globalLighting) {
        const TerrainGlobalLightingRenderData& global = *terrain.globalLighting;
        const auto& lights =
            global.terrainLights[global.terrainLightSlot()];
        colour = {
            finiteOrZero(lights.front().ambient.x()),
            finiteOrZero(lights.front().ambient.y()),
            finiteOrZero(lights.front().ambient.z()),
        };
        for (const TerrainLightingRenderData& light : lights) {
            const float lengthSquared = light.direction.length_sq();
            if (!std::isfinite(lengthSquared) ||
                lengthSquared <= 0.000001f) {
                continue;
            }
            const math::vec3 ray =
                (-light.direction) / std::sqrt(lengthSquared);
            const float diffuse = std::max(0.0f, normal.dot(ray));
            colour += math::vec3{
                finiteOrZero(light.diffuse.x()),
                finiteOrZero(light.diffuse.y()),
                finiteOrZero(light.diffuse.z()),
            } * diffuse;
        }
    }
    for (const TerrainPointLightRenderData& light : terrain.pointLights) {
        const math::vec3 toLight = light.position - position;
        const float distanceSquared = toLight.length_sq();
        if (!std::isfinite(distanceSquared) || distanceSquared <= 0.000001f ||
            distanceSquared >= light.outerRadius * light.outerRadius) {
            continue;
        }
        const float distance = std::sqrt(distanceSquared);
        const float attenuation = light.outerRadius > light.innerRadius
            ? std::clamp((light.outerRadius - distance) /
                             (light.outerRadius - light.innerRadius),
                         0.0f, 1.0f)
            : 1.0f;
        const float diffuse =
            std::max(0.0f, normal.dot(toLight / distance));
        colour += light.ambient * attenuation +
            light.diffuse * (diffuse * attenuation);
    }
    return {
        std::clamp(colour.x(), 0.0f, 1.0f),
        std::clamp(colour.y(), 0.0f, 1.0f),
        std::clamp(colour.z(), 0.0f, 1.0f),
        alpha,
    };
}

} // namespace

math::vec4 terrainFallbackMaterialColour(
    container::StringView name, int32_t classIndex) noexcept {
    uint32_t hash = 2166136261u;
    for (const unsigned char character : name) {
        hash ^= character;
        hash *= 16777619u;
    }
    hash ^= static_cast<uint32_t>(classIndex + 1) * 0x9E3779B9u;
    const auto channel = [hash](uint32_t shift, float low, float range) {
        return low + static_cast<float>((hash >> shift) & 0xffu) /
            255.0f * range;
    };
    return {
        channel(0, 0.16f, 0.34f),
        channel(8, 0.28f, 0.42f),
        channel(16, 0.08f, 0.24f),
        1.0f,
    };
}

namespace {

int32_t textureClassForTile(
    const TerrainMaterialRenderData& materials, int32_t tileIndex,
    container::Span<const int32_t> classBySourceTile = {}) {
    if (tileIndex < 0) return -1;
    const int32_t sourceTile = tileIndex >> 2;
    const container::Span<const int32_t> compiledClasses =
        !classBySourceTile.empty()
            ? classBySourceTile
            : container::Span<const int32_t>{
                  materials.textureClassBySourceTile};
    if (sourceTile >= 0 &&
        static_cast<size_t>(sourceTile) < compiledClasses.size()) {
        return compiledClasses[static_cast<size_t>(sourceTile)];
    }
    for (size_t index = 0; index < materials.textureClasses.size(); ++index) {
        const TerrainTextureClassRenderData& textureClass =
            materials.textureClasses[index];
        const int64_t end = static_cast<int64_t>(textureClass.firstTile) +
            textureClass.tileCount;
        if (textureClass.firstTile >= 0 &&
            sourceTile >= textureClass.firstTile &&
            static_cast<int64_t>(sourceTile) < end) {
            return static_cast<int32_t>(index);
        }
    }
    return -1;
}

struct ResolvedTerrainTile final {
    int32_t textureClass = -1;
    int32_t tileIndex = -1;
    bool flipTriangles = false;
};

ResolvedTerrainTile resolveBaseTile(
    const TerrainMaterialRenderData* materials, size_t sampleIndex,
    container::Span<const int32_t> classBySourceTile = {}) {
    if (!materials || sampleIndex >= materials->baseTileIndices.size()) {
        return {};
    }
    const int32_t tileIndex = materials->baseTileIndices[sampleIndex];
    const int32_t textureClass = textureClassForTile(
        *materials, tileIndex, classBySourceTile);
    return textureClass >= 0
        ? ResolvedTerrainTile{textureClass, tileIndex, false}
        : ResolvedTerrainTile{};
}

bool normalBlendTriangleFlip(
    const TerrainBlendDefinitionRenderData& definition) {
    const bool inverted =
        (definition.inverted & kBlendInvertedMask) != 0;
    return (definition.inverted & kBlendForcedFlipMask) != 0 ||
        (definition.rightDiagonal && !inverted) ||
        (definition.leftDiagonal && inverted);
}

ResolvedTerrainTile resolveBlendTile(
    const TerrainMaterialRenderData& materials, int32_t selector,
    container::Span<const int32_t> classBySourceTile = {}) {
    if (selector <= 0 ||
        static_cast<size_t>(selector) >= materials.blendDefinitions.size()) {
        return {};
    }
    const TerrainBlendDefinitionRenderData& definition =
        materials.blendDefinitions[static_cast<size_t>(selector)];
    if (definition.customEdgeTextureClass >= 0) return {};
    const int32_t textureClass =
        textureClassForTile(
            materials, definition.blendTileIndex, classBySourceTile);
    return textureClass >= 0
        ? ResolvedTerrainTile{
              textureClass, definition.blendTileIndex,
              normalBlendTriangleFlip(definition)}
        : ResolvedTerrainTile{};
}

container::Array<float, 4> blendAlpha(
    const TerrainBlendDefinitionRenderData& definition) {
    container::Array<float, 4> alpha{};
    const bool inverted =
        (definition.inverted & kBlendInvertedMask) != 0;
    if (definition.horizontal) {
        if (inverted) alpha[0] = alpha[3] = 1.0f;
        else alpha[1] = alpha[2] = 1.0f;
    }
    if (definition.vertical) {
        if (inverted) alpha[0] = alpha[1] = 1.0f;
        else alpha[2] = alpha[3] = 1.0f;
    }
    if (definition.rightDiagonal) {
        if (inverted) {
            alpha[1] = 1.0f;
            if (definition.longDiagonal) alpha[0] = alpha[2] = 1.0f;
        } else {
            alpha[2] = 1.0f;
            if (definition.longDiagonal) alpha[1] = alpha[3] = 1.0f;
        }
    }
    if (definition.leftDiagonal) {
        if (inverted) {
            alpha[0] = 1.0f;
            if (definition.longDiagonal) alpha[1] = alpha[3] = 1.0f;
        } else {
            alpha[3] = 1.0f;
            if (definition.longDiagonal) alpha[0] = alpha[2] = 1.0f;
        }
    }
    return alpha;
}

bool hasVisibleAlpha(const container::Array<float, 4>& alpha) {
    return std::any_of(alpha.begin(), alpha.end(),
        [](float value) { return value > 0.0f; });
}

bool buildSourceTileUv(
    const TerrainMaterialRenderData& materials,
    int32_t textureClassIndex,
    int32_t tileIndex,
    container::Array<math::vec2, 4>& output) {
    if (textureClassIndex < 0 ||
        static_cast<size_t>(textureClassIndex) >=
            materials.textureClasses.size() ||
        tileIndex < 0) {
        return false;
    }
    const TerrainTextureClassRenderData& textureClass =
        materials.textureClasses[static_cast<size_t>(textureClassIndex)];
    const int32_t sourceTile = tileIndex >> 2;
    const int32_t localTile = sourceTile - textureClass.firstTile;
    const int32_t gridWidth = terrainSourceGridWidth(textureClass);
    if (localTile < 0 || gridWidth <= 0 ||
        localTile >= gridWidth * gridWidth) {
        return false;
    }
    const int32_t column = localTile % gridWidth;
    const int32_t rowFromBottom = localTile / gridWidth;
    float minU = static_cast<float>(column) / gridWidth;
    float maxU = static_cast<float>(column + 1) / gridWidth;
    float minV = 1.0f - static_cast<float>(rowFromBottom + 1) / gridWidth;
    float maxV = 1.0f - static_cast<float>(rowFromBottom) / gridWidth;
    const float middleU = (minU + maxU) * 0.5f;
    const float middleV = (minV + maxV) * 0.5f;
    if ((tileIndex & 1) != 0) minU = middleU;
    else maxU = middleU;
    if ((tileIndex & 2) != 0) maxV = middleV;
    else minV = middleV;
    output = {{{minU, maxV}, {maxU, maxV}, {maxU, minV}, {minU, minV}}};
    return true;
}

void insetDirectTileUvForSampling(
    const TerrainMaterialRenderData& materials,
    int32_t textureClassIndex,
    container::Array<math::vec2, 4>& uv) noexcept {
    if (textureClassIndex < 0 ||
        static_cast<size_t>(textureClassIndex) >=
            materials.textureClasses.size()) {
        return;
    }
    const int32_t gridWidth = terrainSourceGridWidth(
        materials.textureClasses[static_cast<size_t>(textureClassIndex)]);
    if (gridWidth <= 0) return;
    constexpr float kClassicTilePixels = 64.0f;
    constexpr float kMip2HalfTexelInAuthoredPixels = 2.0f;
    const float halfTexel = kMip2HalfTexelInAuthoredPixels /
        (static_cast<float>(gridWidth) * kClassicTilePixels);
    math::vec2 center{};
    for (const math::vec2& corner : uv) center += corner;
    center = center / static_cast<float>(uv.size());
    for (math::vec2& corner : uv) {
        corner = {
            corner.x() < center.x() ? corner.x() + halfTexel
                                    : corner.x() - halfTexel,
            corner.y() < center.y() ? corner.y() + halfTexel
                                    : corner.y() - halfTexel,
        };
    }
}

void insetCustomEdgeUvForSampling(
    container::Array<math::vec2, 4>& uv) noexcept {
    constexpr float halfTexel = 0.5f / (4.0f * 64.0f);
    math::vec2 center{};
    for (const math::vec2& corner : uv) center += corner;
    center = center / static_cast<float>(uv.size());
    for (math::vec2& corner : uv) {
        corner = {
            corner.x() < center.x() ? corner.x() + halfTexel
                                    : corner.x() - halfTexel,
            corner.y() < center.y() ? corner.y() + halfTexel
                                    : corner.y() - halfTexel,
        };
    }
}

bool buildCustomEdgePatternUv(
    const TerrainBlendDefinitionRenderData& definition,
    int32_t mapX,
    int32_t mapY,
    container::Array<math::vec2, 4>& output) {
    constexpr float step = 0.25f;
    float offsetU = 0.0f;
    float offsetV = 0.0f;
    const bool inverted = definition.inverted != 0;
    if (definition.horizontal) {
        offsetU = inverted ? 0.75f : 0.0f;
        offsetV = step * static_cast<float>(1 + (mapY & 1));
    } else if (definition.vertical) {
        offsetU = step * static_cast<float>(1 + (mapX & 1));
        offsetV = inverted ? 0.0f : 0.75f;
    } else if (definition.rightDiagonal) {
        if (definition.longDiagonal) {
            offsetU = 0.5f;
            offsetV = inverted ? 0.5f : 0.25f;
        } else {
            offsetU = 0.0f;
            offsetV = inverted ? 0.0f : 0.75f;
        }
    } else if (definition.leftDiagonal) {
        if (definition.longDiagonal) {
            offsetU = 0.25f;
            offsetV = inverted ? 0.5f : 0.25f;
        } else {
            offsetU = 0.75f;
            offsetV = inverted ? 0.0f : 0.75f;
        }
    } else {
        return false;
    }
    output = {{{offsetU, offsetV + step},
               {offsetU + step, offsetV + step},
               {offsetU + step, offsetV},
               {offsetU, offsetV}}};
    return true;
}

bool buildAuthoredCliffUv(
    const TerrainMaterialRenderData& materials,
    size_t sampleIndex,
    int32_t tileIndex,
    int32_t textureClassIndex,
    container::Array<math::vec2, 4>& output,
    bool& authoredFlip,
    bool adjustCliffTextures,
    container::Span<const int32_t> classBySourceTile = {}) {
    authoredFlip = false;
    if (!adjustCliffTextures || textureClassIndex < 0 ||
        static_cast<size_t>(textureClassIndex) >=
            materials.textureClasses.size() ||
        sampleIndex >= materials.cliffInfoIndices.size()) {
        return false;
    }
    const int32_t cliffIndex = materials.cliffInfoIndices[sampleIndex];
    if (cliffIndex <= 0 ||
        static_cast<size_t>(cliffIndex) >=
            materials.cliffDefinitions.size()) {
        return false;
    }
    const TerrainCliffDefinitionRenderData& cliff =
        materials.cliffDefinitions[static_cast<size_t>(cliffIndex)];
    if (textureClassForTile(
            materials, tileIndex, classBySourceTile) != textureClassIndex ||
        textureClassForTile(
            materials, cliff.tileIndex,
            classBySourceTile) != textureClassIndex) {
        return false;
    }
    const int32_t gridWidth = terrainSourceGridWidth(
        materials.textureClasses[static_cast<size_t>(textureClassIndex)]);
    if (gridWidth <= 0) return false;
    constexpr float kClassicTerrainAtlasWidth = 2048.0f;
    constexpr float kClassicTilePixels = 64.0f;
    const float classPixels =
        static_cast<float>(gridWidth) * kClassicTilePixels;
    if (!std::isfinite(classPixels) || classPixels <= 0.0f) return false;
    const float atlasToClass = kClassicTerrainAtlasWidth / classPixels;
    for (size_t corner = 0; corner < output.size(); ++corner) {
        const float sourceU = cliff.uv[corner * 2];
        const float sourceV = cliff.uv[corner * 2 + 1];
        if (!std::isfinite(sourceU) || !std::isfinite(sourceV)) return false;
        output[corner] = {
            sourceU * atlasToClass, 1.0f + sourceV * atlasToClass};
    }
    authoredFlip = cliff.flip != 0;
    return true;
}

bool buildLegacyCliffStretchUv(
    const TerrainRenderSnapshot& terrain,
    int32_t mapX,
    int32_t mapY,
    container::Array<math::vec2, 4>& output) noexcept {
    if (!terrain.isValid() || !terrain.adjustCliffTextures || mapX < 0 ||
        mapY < 0 || mapX + 1 >= terrain.width || mapY + 1 >= terrain.height) {
        return false;
    }
    float u[4] = {
        output[0].x(), output[1].x(), output[2].x(), output[3].x()};
    float v[4] = {
        output[0].y(), output[1].y(), output[2].y(), output[3].y()};
    for (size_t index = 0; index < std::size(u); ++index) {
        if (!std::isfinite(u[index]) || !std::isfinite(v[index])) return false;
    }
    const float nU = u[0];
    const float xU = u[1];
    const float nV = v[2];
    const float xV = v[0];
    const float heightScale =
        terrain.heightWorldScale / terrain.cellWorldSize;
    if (!std::isfinite(heightScale) || heightScale <= 0.0f) return false;
    const int32_t h0 = terrain.heightSample(mapX, mapY);
    const int32_t h1 = terrain.heightSample(mapX + 1, mapY);
    const int32_t h2 = terrain.heightSample(mapX + 1, mapY + 1);
    const int32_t h3 = terrain.heightSample(mapX, mapY + 1);
    const int32_t minHeight = std::min({h0, h1, h2, h3});
    const int32_t maxHeight = std::max({h0, h1, h2, h3});
    const int32_t deltaHeight = maxHeight - minHeight;
    constexpr float kStretchLimit = 1.5f;
    constexpr float kTileLimit = 4.0f;
    constexpr float kTallStretchLimit = 2.0f;
    constexpr float kDiamondStretchLimit = 2.4f;
    const float scaledHeight = static_cast<float>(deltaHeight) * heightScale;
    if (!std::isfinite(scaledHeight) || scaledHeight < kStretchLimit) {
        return false;
    }
    const int32_t belowLimit = minHeight + (2 * deltaHeight + 1) / 3;
    const int32_t aboveLimit = minHeight + (deltaHeight + 1) / 3;
    const container::Array<int32_t, 4> heights{{h0, h1, h2, h3}};
    int32_t below = 0;
    int32_t above = 0;
    for (const int32_t height : heights) {
        if (height < belowLimit) ++below;
        if (height > aboveLimit) ++above;
    }
    float divisor = std::clamp(kTileLimit / scaledHeight, 1.0f, kTileLimit);
    constexpr float kClassMinV = 0.0f;
    constexpr float kClassMaxU = 1.0f;
    constexpr float kClassMaxV = 1.0f;
    constexpr float kDeltaV = 1.0f;
    if (above != 1 && below != 1 && (above != 2 || below != 2) &&
        scaledHeight < kDiamondStretchLimit) {
        return false;
    }
    if (below == 1 || above > below) {
        if (h0 == minHeight) v[0] = nV + kDeltaV / divisor;
        else if (h1 == minHeight) v[1] = nV + kDeltaV / divisor;
        else if (h2 == minHeight) v[2] = xV - kDeltaV / divisor;
        else if (h3 == minHeight) v[3] = xV - kDeltaV / divisor;
    } else if (above == 1 || below > above) {
        if (h0 == maxHeight) v[0] = nV + kDeltaV / divisor;
        else if (h1 == maxHeight) v[1] = nV + kDeltaV / divisor;
        else if (h2 == maxHeight) v[2] = xV - kDeltaV / divisor;
        else if (h3 == maxHeight) v[3] = xV - kDeltaV / divisor;
    } else {
        if (scaledHeight < kTallStretchLimit) return false;
        const auto stretchLength = [=](int32_t delta) {
            const float slope = static_cast<float>(delta) * heightScale;
            float value = std::sqrt(1.0f + slope * slope);
            if (value < kStretchLimit) value = 1.0f;
            return std::min(value, kTileLimit);
        };
        float dx = stretchLength(h3 - h2) * (xU - nU);
        float dy = stretchLength(h3 - h0) * (xV - nV);
        u[0] = nU;
        u[1] = nU + dx;
        u[2] = nU + dx;
        u[3] = nU;
        v[0] = nV + dy;
        v[1] = nV + dy;
        v[2] = nV;
        v[3] = nV;
        dx = stretchLength(h1 - h0) * (xU - nU);
        dy = stretchLength(h2 - h1) * (xV - nV);
        u[1] = u[0] + dx;
        v[1] = v[3] + dy;
    }
    float adjustV = 0.0f;
    for (const float value : v) {
        adjustV = std::max(adjustV, kClassMinV - value);
    }
    for (float& value : v) value += adjustV;
    float adjustU = 0.0f;
    adjustV = 0.0f;
    for (size_t index = 0; index < std::size(u); ++index) {
        adjustU = std::max(adjustU, u[index] - kClassMaxU);
        adjustV = std::max(adjustV, v[index] - kClassMaxV);
    }
    for (size_t index = 0; index < std::size(u); ++index) {
        u[index] -= adjustU;
        v[index] -= adjustV;
        if (!std::isfinite(u[index]) || !std::isfinite(v[index])) {
            return false;
        }
        output[index] = {u[index], v[index]};
    }
    return true;
}

container::Array<math::vec2, 4> fallbackUv(int32_t mapX, int32_t mapY) {
    const float minU = static_cast<float>(mapX) / 8.0f;
    const float maxU = static_cast<float>(mapX + 1) / 8.0f;
    const float minV = static_cast<float>(mapY) / 8.0f;
    const float maxV = static_cast<float>(mapY + 1) / 8.0f;
    return {{{minU, maxV}, {maxU, maxV}, {maxU, minV}, {minU, minV}}};
}

container::Vector<int32_t> buildTextureClassBySourceTile(
    const TerrainMaterialRenderData* materials) {
    container::Vector<int32_t> result;
    if (!materials) return result;
    if (!materials->textureClassBySourceTile.empty()) {
        return materials->textureClassBySourceTile;
    }
    int32_t sourceTileCount = 0;
    for (const TerrainTextureClassRenderData& textureClass :
         materials->textureClasses) {
        if (textureClass.firstTile < 0 || textureClass.tileCount <= 0) continue;
        sourceTileCount = std::max(
            sourceTileCount, textureClass.firstTile + textureClass.tileCount);
    }
    result.assign(static_cast<size_t>(sourceTileCount), -1);
    for (size_t classIndex = 0;
         classIndex < materials->textureClasses.size(); ++classIndex) {
        const TerrainTextureClassRenderData& textureClass =
            materials->textureClasses[classIndex];
        for (int32_t tile = std::max(0, textureClass.firstTile);
             tile < textureClass.firstTile + textureClass.tileCount &&
             static_cast<size_t>(tile) < result.size(); ++tile) {
            result[static_cast<size_t>(tile)] =
                static_cast<int32_t>(classIndex);
        }
    }
    return result;
}

// The compiled source-tile projection is an optional per-build accelerator:
// callers such as TerrainGpuMaterialOwner::cpuLayout() legitimately hand the
// chunk workers a layout that carries only the material index maps.  Resolving
// through textureClassForTile keeps this path identical to the non-simplified
// one, which already falls back to the snapshot's own projection and finally to
// a scan of the authored class tile ranges.
int32_t simplifiedTextureClass(
    const TerrainMaterialRenderData* materials,
    container::Span<const int32_t> bySourceTile,
    size_t sampleIndex) {
    if (!materials || sampleIndex >= materials->baseTileIndices.size()) {
        return -1;
    }
    return textureClassForTile(
        *materials, materials->baseTileIndices[sampleIndex], bySourceTile);
}

struct TerrainTileMeshGeometryKeyHash final {
    size_t operator()(const TerrainTileMeshGeometryKey& key) const noexcept {
        size_t value = key.materialIndex;
        const auto mix = [&value](uint32_t component) {
            value ^= static_cast<size_t>(component) + 0x9e3779b9u +
                (value << 6u) + (value >> 2u);
        };
        mix(key.detailMaterialIndex);
        mix(key.materialPass);
        mix(key.alphaBlend ? 1u : 0u);
        mix(key.twoSided ? 1u : 0u);
        mix(static_cast<uint32_t>(key.terrainEdgePhase));
        mix(key.samplerMode);
        mix(key.detailSamplerMode);
        return value;
    }
};

} // namespace

std::optional<TerrainTileUvResolution> resolveTerrainMaterialTileUv(
    const TerrainMaterialRenderData& materials,
    size_t sampleIndex,
    int32_t tileIndex,
    bool adjustCliffTextures) noexcept {
    const int32_t textureClass = textureClassForTile(materials, tileIndex);
    if (textureClass < 0) return std::nullopt;
    TerrainTileUvResolution result;
    result.textureClass = textureClass;
    if (!buildSourceTileUv(
            materials, textureClass, tileIndex, result.corners)) {
        return std::nullopt;
    }
    container::Array<math::vec2, 4> authoredCorners{};
    if (buildAuthoredCliffUv(
            materials, sampleIndex, tileIndex, textureClass,
            authoredCorners, result.authoredCliffFlip,
            adjustCliffTextures)) {
        result.corners = authoredCorners;
        result.usesAuthoredCliffUv = true;
        result.requiresHeightTriangleFlip = result.authoredCliffFlip;
    }
    return result;
}

std::optional<TerrainTileUvResolution> resolveTerrainTileUv(
    const TerrainRenderSnapshot& terrain,
    const TerrainMaterialRenderData& materials,
    size_t sampleIndex,
    int32_t tileIndex) noexcept {
    if (!terrain.isValid() || sampleIndex >= terrain.heights.size()) {
        return std::nullopt;
    }
    std::optional<TerrainTileUvResolution> result =
        resolveTerrainMaterialTileUv(
            materials, sampleIndex, tileIndex,
            terrain.adjustCliffTextures);
    if (!result || result->usesAuthoredCliffUv ||
        !terrain.adjustCliffTextures) {
        return result;
    }
    const int32_t mapX = static_cast<int32_t>(
        sampleIndex % static_cast<size_t>(terrain.width));
    const int32_t mapY = static_cast<int32_t>(
        sampleIndex / static_cast<size_t>(terrain.width));
    if (buildLegacyCliffStretchUv(
            terrain, mapX, mapY, result->corners)) {
        result->usesLegacyCliffStretch = true;
        result->requiresHeightTriangleFlip = true;
    }
    return result;
}

bool resolveTerrainAuthoredCliffTriangleFlip(
    const TerrainRenderSnapshot& terrain,
    int32_t mapX,
    int32_t mapY) noexcept {
    if (!terrain.isValid() || mapX < 0 || mapY < 0 ||
        mapX + 1 >= terrain.width || mapY + 1 >= terrain.height) {
        return false;
    }
    const int32_t p0 = terrain.heightSample(mapX, mapY);
    const int32_t p1 = terrain.heightSample(mapX + 1, mapY);
    const int32_t p2 = terrain.heightSample(mapX + 1, mapY + 1);
    const int32_t p3 = terrain.heightSample(mapX, mapY + 1);
    return std::abs(p0 - p2) > std::abs(p1 - p3);
}

TerrainPrimaryCellTopologyResolver
prepareTerrainPrimaryCellTopologyResolver(
    const TerrainRenderSnapshot& terrain) noexcept {
    const TerrainMaterialRenderData* materials = nullptr;
    if (terrain.isValid() && terrain.materials &&
        terrain.materials->isValidFor(terrain.heights.size())) {
        materials = &*terrain.materials;
    }
    return {
        .terrain = &terrain,
        .materials = materials,
    };
}

bool resolveTerrainPrimaryCellTriangleFlip(
    const TerrainPrimaryCellTopologyResolver& resolver,
    int32_t mapX,
    int32_t mapY) noexcept {
    if (!resolver.terrain) return false;
    const TerrainRenderSnapshot& terrain = *resolver.terrain;
    if (!terrain.isValid() || mapX < 0 || mapY < 0 ||
        mapX + 1 >= terrain.width || mapY + 1 >= terrain.height) {
        return false;
    }
    if (!resolver.materials) {
        // The emergency heightfield surface uses the height-selected
        // diagonal directly.
        return resolveTerrainAuthoredCliffTriangleFlip(
            terrain, mapX, mapY);
    }

    const TerrainMaterialRenderData& materials = *resolver.materials;
    const size_t sampleIndex = static_cast<size_t>(mapY) *
            static_cast<size_t>(terrain.width) +
        static_cast<size_t>(mapX);
    const auto resolvedFlip = [&terrain, mapX, mapY](
                                  const std::optional<
                                      TerrainTileUvResolution>& resolvedUv,
                                  bool ordinaryFlip) noexcept {
        return resolvedUv && resolvedUv->requiresHeightTriangleFlip
            ? resolveTerrainAuthoredCliffTriangleFlip(
                  terrain, mapX, mapY)
            : ordinaryFlip;
    };

    const int16_t primarySelector = materials.blendTileIndices[sampleIndex];
    if (primarySelector > 0 &&
        static_cast<size_t>(primarySelector) <
            materials.blendDefinitions.size()) {
        const TerrainBlendDefinitionRenderData& definition =
            materials.blendDefinitions[
                static_cast<size_t>(primarySelector)];
        const std::optional<TerrainTileUvResolution> primaryUv =
            resolveTerrainTileUv(
                terrain, materials, sampleIndex,
                definition.blendTileIndex);
        const bool ordinaryFlip = definition.customEdgeTextureClass >= 0
            ? false
            : normalBlendTriangleFlip(definition);
        return resolvedFlip(primaryUv, ordinaryFlip);
    }

    const ResolvedTerrainTile base =
        resolveBaseTile(&materials, sampleIndex);
    const std::optional<TerrainTileUvResolution> baseUv =
        resolveTerrainTileUv(
            terrain, materials, sampleIndex, base.tileIndex);
    return resolvedFlip(baseUv, base.flipTriangles);
}

bool resolveTerrainPrimaryCellTriangleFlip(
    const TerrainRenderSnapshot& terrain,
    int32_t mapX,
    int32_t mapY) noexcept {
    return resolveTerrainPrimaryCellTriangleFlip(
        prepareTerrainPrimaryCellTopologyResolver(terrain), mapX, mapY);
}

std::optional<TerrainCustomEdgeRenderPlan>
resolveTerrainCustomEdgeRenderPlan(
    const TerrainBlendDefinitionRenderData& definition,
    int32_t mapX,
    int32_t mapY) noexcept {
    if (definition.customEdgeTextureClass < 0) return std::nullopt;
    TerrainCustomEdgeRenderPlan result;
    if (!buildCustomEdgePatternUv(
            definition, mapX, mapY, result.patternUv)) {
        return std::nullopt;
    }
    return result;
}

math::vec4 evaluateTerrainVertexColorCpu(
    const TerrainRenderSnapshot& terrain,
    math::vec3 position,
    math::vec3 normal,
    float alpha) noexcept {
    const float lengthSquared = normal.length_sq();
    if (!std::isfinite(lengthSquared) ||
        lengthSquared <= math::EPSILON * math::EPSILON) {
        normal = {0.0f, 0.0f, 1.0f};
    } else {
        normal = normal / std::sqrt(lengthSquared);
    }
    return terrainVertexColor(
        terrain, normal, position, std::clamp(alpha, 0.0f, 1.0f));
}

TerrainTileMaterialLayout buildTerrainTileMaterialLayout(
    const TerrainRenderSnapshot& terrain,
    const TerrainMaterialRenderData* materials,
    bool simplifiedSurface) {
    TerrainTileMaterialLayout layout;
    layout.textureClassBySourceTile =
        buildTextureClassBySourceTile(materials);
    const container::Span<const int32_t> textureClasses{
        layout.textureClassBySourceTile};
    const auto materialFor = [&layout](int32_t textureClass) {
        if (layout.materials.contains(textureClass)) return;
        const uint32_t index =
            static_cast<uint32_t>(layout.demands.size());
        layout.materials.emplace(textureClass, index);
        layout.demands.push_back({
            TerrainTileMaterialKind::Surface, textureClass, index});
    };
    const auto edgeFor = [&layout](int32_t textureClass) {
        if (layout.alphaEdges.contains(textureClass)) return;
        const uint32_t index =
            static_cast<uint32_t>(layout.demands.size());
        layout.alphaEdges.emplace(textureClass, index);
        layout.demands.push_back({
            TerrainTileMaterialKind::AlphaEdge, textureClass, index});
    };
    const auto blendFor = [&](int16_t selector) {
        if (!materials || selector <= 0 ||
            static_cast<size_t>(selector) >=
                materials->blendDefinitions.size()) {
            return;
        }
        const TerrainBlendDefinitionRenderData& definition =
            materials->blendDefinitions[static_cast<size_t>(selector)];
        const ResolvedTerrainTile overlay =
            resolveBlendTile(*materials, selector, textureClasses);
        if (hasVisibleAlpha(blendAlpha(definition)) &&
            overlay.textureClass >= 0) {
            materialFor(overlay.textureClass);
        }
    };
    const auto customEdgeFor = [&](int16_t selector) {
        if (!materials || selector <= 0 ||
            static_cast<size_t>(selector) >=
                materials->blendDefinitions.size()) {
            return;
        }
        const TerrainBlendDefinitionRenderData& definition =
            materials->blendDefinitions[static_cast<size_t>(selector)];
        if (definition.customEdgeTextureClass < 0) return;
        const int32_t sourceTextureClass =
            textureClassForTile(
                *materials, definition.blendTileIndex, textureClasses);
        if (sourceTextureClass < 0) return;
        materialFor(sourceTextureClass);
        edgeFor(definition.customEdgeTextureClass);
    };
    for (int32_t y0 = 0; y0 < terrain.height - 1;
         y0 += kTerrainCellsPerChunk) {
        const int32_t cellsY = std::min(
            kTerrainCellsPerChunk, terrain.height - 1 - y0);
        for (int32_t x0 = 0; x0 < terrain.width - 1;
             x0 += kTerrainCellsPerChunk) {
            const int32_t cellsX = std::min(
                kTerrainCellsPerChunk, terrain.width - 1 - x0);
            for (int32_t y = 0; y < cellsY; ++y) {
                for (int32_t x = 0; x < cellsX; ++x) {
                    const size_t sampleIndex =
                        static_cast<size_t>(y0 + y) *
                            static_cast<size_t>(terrain.width) +
                        static_cast<size_t>(x0 + x);
                    const int32_t baseClass = simplifiedSurface
                        ? simplifiedTextureClass(
                              materials, textureClasses, sampleIndex)
                        : resolveBaseTile(
                              materials, sampleIndex,
                              textureClasses).textureClass;
                    materialFor(baseClass);
                    if (!materials || simplifiedSurface) continue;
                    const int16_t primary =
                        materials->blendTileIndices[sampleIndex];
                    blendFor(primary);
                    blendFor(materials->extraBlendTileIndices[sampleIndex]);
                    customEdgeFor(primary);
                }
            }
        }
    }
    return layout;
}

bool buildTerrainHeightfieldFallbackMeshChunk(
    const TerrainRenderSnapshot& terrain,
    uint32_t materialIndex,
    int32_t x0,
    int32_t y0,
    int32_t cellsX,
    int32_t cellsY,
    TerrainTileMeshChunk& output,
    container::String* error) {
    output = {};
    output.x0 = x0;
    output.y0 = y0;
    output.cellsX = cellsX;
    output.cellsY = cellsY;
    if (cellsX <= 0 || cellsY <= 0) {
        setError(error, "Terrain fallback chunk has no cells");
        return false;
    }
    const size_t sampleWidth = static_cast<size_t>(cellsX + 1);
    const size_t sampleHeight = static_cast<size_t>(cellsY + 1);
    TerrainTileMeshGeometry geometry;
    geometry.key.materialIndex = materialIndex;
    geometry.vertices.reserve(sampleWidth * sampleHeight);
    geometry.indices.reserve(
        static_cast<size_t>(cellsX) * static_cast<size_t>(cellsY) * 6u);
    const TerrainMaterialRenderData* materials = terrain.materials &&
            terrain.materials->isValidFor(terrain.heights.size())
        ? &*terrain.materials
        : nullptr;
    const container::Vector<int32_t> classes =
        buildTextureClassBySourceTile(materials);
    container::Vector<math::vec4> classColours;
    if (materials) {
        classColours.reserve(materials->textureClasses.size());
        for (size_t index = 0; index < materials->textureClasses.size();
             ++index) {
            classColours.push_back(terrainFallbackMaterialColour(
                materials->textureClasses[index].name,
                static_cast<int32_t>(index)));
        }
    }
    math::vec3 minimum = terrainWorldPositionUnchecked(terrain, x0, y0);
    math::vec3 maximum = minimum;
    for (int32_t sampleY = y0; sampleY <= y0 + cellsY; ++sampleY) {
        for (int32_t sampleX = x0; sampleX <= x0 + cellsX; ++sampleX) {
            const math::vec3 position =
                terrainWorldPositionUnchecked(terrain, sampleX, sampleY);
            const math::vec3 normal =
                terrainNormal(terrain, sampleX, sampleY);
            const float light = std::clamp(
                0.72f + normal.z() * 0.28f, 0.55f, 1.0f);
            math::vec4 materialColour{0.32f, 0.52f, 0.24f, 1.0f};
            if (materials) {
                const int32_t cellX = std::min(sampleX, terrain.width - 2);
                const int32_t cellY = std::min(sampleY, terrain.height - 2);
                const size_t sampleIndex = static_cast<size_t>(cellY) *
                        static_cast<size_t>(terrain.width) +
                    static_cast<size_t>(cellX);
                const int32_t textureClass = simplifiedTextureClass(
                    materials, classes, sampleIndex);
                if (textureClass >= 0 &&
                    static_cast<size_t>(textureClass) < classColours.size()) {
                    materialColour =
                        classColours[static_cast<size_t>(textureClass)];
                }
            }
            math::vec4 colour{
                materialColour.x() * light,
                materialColour.y() * light,
                materialColour.z() * light,
                1.0f,
            };
            StaticMeshVertex vertex;
            vertex.position = position;
            vertex.normal = normal;
            vertex.texcoord = {
                static_cast<float>(sampleX) * (1.0f / 32.0f),
                static_cast<float>(sampleY) * (1.0f / 32.0f),
            };
            vertex.color = colour;
            geometry.vertices.push_back(vertex);
            minimum = {
                std::min(minimum.x(), position.x()),
                std::min(minimum.y(), position.y()),
                std::min(minimum.z(), position.z()),
            };
            maximum = {
                std::max(maximum.x(), position.x()),
                std::max(maximum.y(), position.y()),
                std::max(maximum.z(), position.z()),
            };
        }
    }
    for (int32_t y = 0; y < cellsY; ++y) {
        for (int32_t x = 0; x < cellsX; ++x) {
            const uint32_t topLeft = static_cast<uint32_t>(
                static_cast<size_t>(y) * sampleWidth +
                static_cast<size_t>(x));
            const uint32_t topRight = topLeft + 1u;
            const uint32_t bottomLeft =
                topLeft + static_cast<uint32_t>(sampleWidth);
            const uint32_t bottomRight = bottomLeft + 1u;
            if (resolveTerrainAuthoredCliffTriangleFlip(
                    terrain, x0 + x, y0 + y)) {
                geometry.indices.insert(geometry.indices.end(), {
                    topRight, bottomLeft, topLeft,
                    topRight, bottomRight, bottomLeft,
                });
            } else {
                geometry.indices.insert(geometry.indices.end(), {
                    topLeft, topRight, bottomRight,
                    topLeft, bottomRight, bottomLeft,
                });
            }
        }
    }
    output.boundsCenter = (minimum + maximum) * 0.5f;
    output.boundsRadius = (maximum - output.boundsCenter).length();
    output.geometries.push_back(std::move(geometry));
    return true;
}

bool buildTerrainTileMeshChunk(
    const TerrainRenderSnapshot& terrain,
    const TerrainMaterialRenderData* materials,
    const TerrainTileMaterialLayout& materialLayout,
    int32_t x0,
    int32_t y0,
    int32_t cellsX,
    int32_t cellsY,
    bool simplifiedSurface,
    TerrainTileMeshChunk& output,
    container::String* error) {
    output = {};
    output.x0 = x0;
    output.y0 = y0;
    output.cellsX = cellsX;
    output.cellsY = cellsY;
    if (cellsX <= 0 || cellsY <= 0) {
        setError(error, "Terrain chunk has no cells");
        return false;
    }

    const size_t sampleWidth = static_cast<size_t>(cellsX + 1);
    const size_t sampleHeight = static_cast<size_t>(cellsY + 1);
    container::Vector<math::vec3> samplePositions(
        sampleWidth * sampleHeight);
    container::Vector<math::vec3> sampleNormals(
        sampleWidth * sampleHeight);
    container::Vector<math::vec4> sampleColors(
        sampleWidth * sampleHeight);
    const container::Span<const int32_t> textureClasses{
        materialLayout.textureClassBySourceTile};
    math::vec3 minimum = terrainWorldPositionUnchecked(terrain, x0, y0);
    math::vec3 maximum = minimum;
    for (int32_t sampleY = y0; sampleY <= y0 + cellsY; ++sampleY) {
        for (int32_t sampleX = x0; sampleX <= x0 + cellsX; ++sampleX) {
            const math::vec3 position =
                terrainWorldPositionUnchecked(terrain, sampleX, sampleY);
            const size_t local = static_cast<size_t>(sampleY - y0) *
                    sampleWidth +
                static_cast<size_t>(sampleX - x0);
            const math::vec3 normal =
                terrainNormal(terrain, sampleX, sampleY);
            math::vec4 color =
                terrainVertexColor(terrain, normal, position, 1.0f);
            samplePositions[local] = position;
            sampleNormals[local] = normal;
            sampleColors[local] = color;
            minimum = {
                std::min(minimum.x(), position.x()),
                std::min(minimum.y(), position.y()),
                std::min(minimum.z(), position.z()),
            };
            maximum = {
                std::max(maximum.x(), position.x()),
                std::max(maximum.y(), position.y()),
                std::max(maximum.z(), position.z()),
            };
        }
    }
    output.boundsCenter = (minimum + maximum) * 0.5f;
    output.boundsRadius = (maximum - output.boundsCenter).length();

    struct CellDraw final {
        uint32_t materialIndex = 0;
        int32_t textureClass = -1;
        int32_t tileIndex = -1;
        bool alphaBlend = false;
        uint32_t materialPass = 0;
        container::Array<float, 4> alpha{1.0f, 1.0f, 1.0f, 1.0f};
        const TerrainTileUvResolution* resolvedUv = nullptr;
        const container::Array<math::vec2, 4>* detailUv = nullptr;
        uint32_t detailMaterialIndex = UINT32_MAX;
        StaticMeshTerrainEdgePhase terrainEdgePhase =
            StaticMeshTerrainEdgePhase::Disabled;
        bool twoSided = false;
        uint8_t samplerMode = 0;
        uint8_t detailSamplerMode = 0;
        bool flipTriangles = false;
    };
    container::HashMap<
        TerrainTileMeshGeometryKey, size_t,
        TerrainTileMeshGeometryKeyHash> groupIndices;
    const size_t expectedGroups = std::max<size_t>(
        1U, materialLayout.demands.size() * 2U);
    groupIndices.reserve(expectedGroups);
    output.geometries.reserve(expectedGroups);
    const auto groupFor = [&output, &groupIndices](
                              const TerrainTileMeshGeometryKey& key)
        -> TerrainTileMeshGeometry& {
        if (const auto found = groupIndices.find(key);
            found != groupIndices.end()) {
            return output.geometries[found->second];
        }
        const size_t index = output.geometries.size();
        output.geometries.push_back({.key = key});
        groupIndices.emplace(key, index);
        return output.geometries.back();
    };
    const auto materialFor = [&materialLayout](
                                 int32_t textureClass,
                                 uint32_t& materialIndex,
                                 container::String* materialError) {
        const auto found = materialLayout.materials.find(textureClass);
        if (found == materialLayout.materials.end()) {
            setError(materialError,
                "Terrain material layout changed during partial GPU update");
            return false;
        }
        materialIndex = found->second;
        return true;
    };
    const auto alphaEdgeMaterialFor = [&materialLayout](
                                          int32_t textureClass,
                                          uint32_t& materialIndex,
                                          container::String* materialError) {
        const auto found = materialLayout.alphaEdges.find(textureClass);
        if (found == materialLayout.alphaEdges.end()) {
            setError(materialError,
                "Terrain alpha-edge material layout changed during partial GPU update");
            return false;
        }
        materialIndex = found->second;
        return true;
    };
    const auto resolveUv = [&terrain, materials, textureClasses](
                               size_t sampleIndex,
                               int32_t tileIndex)
        -> std::optional<TerrainTileUvResolution> {
        if (!materials || sampleIndex >= terrain.heights.size()) {
            return std::nullopt;
        }
        const int32_t textureClass = textureClassForTile(
            *materials, tileIndex, textureClasses);
        if (textureClass < 0) return std::nullopt;
        TerrainTileUvResolution result;
        result.textureClass = textureClass;
        if (!buildSourceTileUv(
                *materials, textureClass, tileIndex, result.corners)) {
            return std::nullopt;
        }
        container::Array<math::vec2, 4> authoredCorners{};
        if (buildAuthoredCliffUv(
                *materials, sampleIndex, tileIndex, textureClass,
                authoredCorners, result.authoredCliffFlip,
                terrain.adjustCliffTextures, textureClasses)) {
            result.corners = authoredCorners;
            result.usesAuthoredCliffUv = true;
            result.requiresHeightTriangleFlip =
                result.authoredCliffFlip;
            return result;
        }
        if (terrain.adjustCliffTextures) {
            const int32_t mapX = static_cast<int32_t>(
                sampleIndex % static_cast<size_t>(terrain.width));
            const int32_t mapY = static_cast<int32_t>(
                sampleIndex / static_cast<size_t>(terrain.width));
            if (buildLegacyCliffStretchUv(
                    terrain, mapX, mapY, result.corners)) {
                result.usesLegacyCliffStretch = true;
                result.requiresHeightTriangleFlip = true;
            }
        }
        return result;
    };
    const auto appendCell = [&](const CellDraw& draw,
                                int32_t mapX,
                                int32_t mapY) -> bool {
        container::Array<math::vec2, 4> uv{};
        if (draw.resolvedUv) {
            uv = draw.resolvedUv->corners;
        } else if (!materials || draw.textureClass < 0 ||
                   !buildSourceTileUv(
                       *materials, draw.textureClass, draw.tileIndex, uv)) {
            uv = fallbackUv(mapX, mapY);
        }
        if (materials && draw.textureClass >= 0 &&
            (!draw.resolvedUv ||
             (!draw.resolvedUv->usesAuthoredCliffUv &&
              !draw.resolvedUv->usesLegacyCliffStretch))) {
            insetDirectTileUvForSampling(
                *materials, draw.textureClass, uv);
        }
        TerrainTileMeshGeometry& geometry = groupFor({
            .materialIndex = draw.materialIndex,
            .detailMaterialIndex = draw.detailMaterialIndex,
            .materialPass = draw.materialPass,
            .alphaBlend = draw.alphaBlend,
            .twoSided = draw.twoSided,
            .terrainEdgePhase = draw.terrainEdgePhase,
            .samplerMode = draw.samplerMode,
            .detailSamplerMode = draw.detailSamplerMode,
        });
        if (geometry.vertices.size() >
            static_cast<size_t>(
                std::numeric_limits<uint32_t>::max() - 4)) {
            setError(error,
                "Terrain material geometry exceeds 32-bit index limits");
            return false;
        }
        const uint32_t firstVertex =
            static_cast<uint32_t>(geometry.vertices.size());
        const container::Array<size_t, 4> sampleIndices = {{
            static_cast<size_t>(mapY - y0) * sampleWidth +
                static_cast<size_t>(mapX - x0),
            static_cast<size_t>(mapY - y0) * sampleWidth +
                static_cast<size_t>(mapX + 1 - x0),
            static_cast<size_t>(mapY + 1 - y0) * sampleWidth +
                static_cast<size_t>(mapX + 1 - x0),
            static_cast<size_t>(mapY + 1 - y0) * sampleWidth +
                static_cast<size_t>(mapX - x0),
        }};
        for (size_t vertex = 0; vertex < sampleIndices.size(); ++vertex) {
            const size_t sample = sampleIndices[vertex];
            StaticMeshVertex vertexOutput;
            vertexOutput.position = samplePositions[sample];
            vertexOutput.normal = sampleNormals[sample];
            vertexOutput.texcoord = uv[vertex];
            if (draw.detailUv) {
                vertexOutput.detailTexcoord = (*draw.detailUv)[vertex];
            }
            const math::vec4& baseColor = sampleColors[sample];
            vertexOutput.color = {
                baseColor.x(), baseColor.y(), baseColor.z(),
                draw.alpha[vertex],
            };
            geometry.vertices.push_back(vertexOutput);
        }
        if (draw.flipTriangles) {
            geometry.indices.insert(geometry.indices.end(), {
                firstVertex + 1, firstVertex + 3, firstVertex,
                firstVertex + 1, firstVertex + 2, firstVertex + 3,
            });
        } else {
            geometry.indices.insert(geometry.indices.end(), {
                firstVertex, firstVertex + 1, firstVertex + 2,
                firstVertex, firstVertex + 2, firstVertex + 3,
            });
        }
        return true;
    };

    for (int32_t y = 0; y < cellsY; ++y) {
        for (int32_t x = 0; x < cellsX; ++x) {
            const int32_t mapX = x0 + x;
            const int32_t mapY = y0 + y;
            const size_t sampleIndex = static_cast<size_t>(mapY) *
                    static_cast<size_t>(terrain.width) +
                static_cast<size_t>(mapX);
            ResolvedTerrainTile base;
            if (simplifiedSurface && materials &&
                sampleIndex < materials->baseTileIndices.size()) {
                base.tileIndex = materials->baseTileIndices[sampleIndex];
                base.textureClass = simplifiedTextureClass(
                    materials, textureClasses, sampleIndex);
            } else {
                base = resolveBaseTile(
                    materials, sampleIndex, textureClasses);
            }
            const std::optional<TerrainTileUvResolution> baseUv =
                materials && !simplifiedSurface
                ? resolveUv(sampleIndex, base.tileIndex)
                : std::nullopt;
            const int16_t primarySelector =
                materials && !simplifiedSurface
                ? materials->blendTileIndices[sampleIndex]
                : 0;
            const TerrainBlendDefinitionRenderData* primaryDefinition =
                materials && primarySelector > 0 &&
                    static_cast<size_t>(primarySelector) <
                        materials->blendDefinitions.size()
                ? &materials->blendDefinitions[
                      static_cast<size_t>(primarySelector)]
                : nullptr;
            const std::optional<TerrainTileUvResolution> primaryUv =
                primaryDefinition
                ? resolveUv(
                      sampleIndex, primaryDefinition->blendTileIndex)
                : std::nullopt;
            const bool heightFlip =
                resolveTerrainAuthoredCliffTriangleFlip(
                    terrain, mapX, mapY);
            const bool ordinaryPrimaryFlip = primaryDefinition
                ? (primaryDefinition->customEdgeTextureClass < 0 &&
                   normalBlendTriangleFlip(*primaryDefinition))
                : base.flipTriangles;
            const TerrainTileUvResolution* topologyUv =
                primaryDefinition
                ? (primaryUv ? &*primaryUv : nullptr)
                : (baseUv ? &*baseUv : nullptr);
            const bool primaryCellFlip = simplifiedSurface
                ? heightFlip
                : topologyUv && topologyUv->requiresHeightTriangleFlip
                    ? heightFlip : ordinaryPrimaryFlip;
            uint32_t baseMaterial = 0;
            if (!materialFor(
                    base.textureClass, baseMaterial, error) ||
                !appendCell(CellDraw{
                    .materialIndex = baseMaterial,
                    .textureClass = base.textureClass,
                    .tileIndex = base.tileIndex,
                    .alphaBlend = false,
                    .materialPass = 0,
                    .alpha = {1.0f, 1.0f, 1.0f, 1.0f},
                    .resolvedUv = baseUv ? &*baseUv : nullptr,
                    .flipTriangles = primaryCellFlip,
                }, mapX, mapY)) {
                return false;
            }
            if (!materials || simplifiedSurface) continue;
            const auto appendBlendLayer =
                [&](int16_t selector,
                    uint32_t materialPass,
                    bool sharesPrimaryTopology) -> bool {
                    if (selector <= 0 ||
                        static_cast<size_t>(selector) >=
                            materials->blendDefinitions.size()) {
                        return true;
                    }
                    const TerrainBlendDefinitionRenderData& definition =
                        materials->blendDefinitions[
                            static_cast<size_t>(selector)];
                    const container::Array<float, 4> alpha =
                        blendAlpha(definition);
                    const ResolvedTerrainTile overlay =
                        resolveBlendTile(
                            *materials, selector, textureClasses);
                    if (!hasVisibleAlpha(alpha) ||
                        overlay.textureClass < 0) {
                        return true;
                    }
                    const auto overlayUv =
                        resolveUv(sampleIndex, overlay.tileIndex);
                    const bool overlayFlip = sharesPrimaryTopology
                        ? primaryCellFlip
                        : overlay.flipTriangles ||
                            (overlayUv &&
                             overlayUv->requiresHeightTriangleFlip &&
                             heightFlip);
                    uint32_t overlayMaterial = 0;
                    return materialFor(
                               overlay.textureClass,
                               overlayMaterial, error) &&
                        appendCell(CellDraw{
                            .materialIndex = overlayMaterial,
                            .textureClass = overlay.textureClass,
                            .tileIndex = overlay.tileIndex,
                            .alphaBlend = true,
                            .materialPass = materialPass,
                            .alpha = alpha,
                            .resolvedUv = overlayUv ? &*overlayUv : nullptr,
                            .flipTriangles = overlayFlip,
                        }, mapX, mapY);
                };
            const auto appendCustomEdgeLayer =
                [&](int16_t selector) -> bool {
                    if (selector <= 0 ||
                        static_cast<size_t>(selector) >=
                            materials->blendDefinitions.size()) {
                        return true;
                    }
                    const TerrainBlendDefinitionRenderData& definition =
                        materials->blendDefinitions[
                            static_cast<size_t>(selector)];
                    if (definition.customEdgeTextureClass < 0) return true;
                    const int32_t sourceTextureClass =
                        textureClassForTile(
                            *materials, definition.blendTileIndex,
                            textureClasses);
                    if (sourceTextureClass < 0) return true;
                    const auto customPlan =
                        resolveTerrainCustomEdgeRenderPlan(
                            definition, mapX, mapY);
                    if (!customPlan) return true;
                    container::Array<math::vec2, 4> edgeUv =
                        customPlan->patternUv;
                    insetCustomEdgeUvForSampling(edgeUv);
                    uint32_t sourceMaterial = 0;
                    uint32_t edgeMaterial = 0;
                    if (!materialFor(
                            sourceTextureClass, sourceMaterial, error) ||
                        !alphaEdgeMaterialFor(
                            definition.customEdgeTextureClass,
                            edgeMaterial, error)) {
                        return false;
                    }
                    const auto sourceUv = resolveUv(
                        sampleIndex, definition.blendTileIndex);
                    TerrainTileUvResolution edgeUvResolution;
                    edgeUvResolution.corners = edgeUv;
                    const container::Array<float, 4> edgeAlpha{
                        customPlan->vertexAlpha,
                        customPlan->vertexAlpha,
                        customPlan->vertexAlpha,
                        customPlan->vertexAlpha,
                    };
                    const CellDraw sourceDraw{
                        .materialIndex = sourceMaterial,
                        .textureClass = sourceTextureClass,
                        .tileIndex = definition.blendTileIndex,
                        .alphaBlend = true,
                        .materialPass =
                            customPlan->blendSourceMaterialPass,
                        .alpha = edgeAlpha,
                        .resolvedUv = sourceUv ? &*sourceUv : nullptr,
                        .detailUv = &edgeUv,
                        .detailMaterialIndex = edgeMaterial,
                        .terrainEdgePhase = customPlan->blendSourcePhase,
                        .twoSided = true,
                        .detailSamplerMode = 3,
                        .flipTriangles = primaryCellFlip,
                    };
                    const CellDraw edgeDraw{
                        .materialIndex = edgeMaterial,
                        .textureClass = -1,
                        .tileIndex = -1,
                        .alphaBlend = true,
                        .materialPass = customPlan->edgeRgbMaterialPass,
                        .alpha = edgeAlpha,
                        .resolvedUv = &edgeUvResolution,
                        .terrainEdgePhase = customPlan->edgeRgbPhase,
                        .twoSided = true,
                        .samplerMode = 3,
                        .flipTriangles = primaryCellFlip,
                    };
                    return appendCell(sourceDraw, mapX, mapY) &&
                        appendCell(edgeDraw, mapX, mapY);
                };
            if (!appendBlendLayer(primarySelector, 1, true) ||
                !appendBlendLayer(
                    materials->extraBlendTileIndices[sampleIndex],
                    2, false) ||
                !appendCustomEdgeLayer(primarySelector)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace engine::render
