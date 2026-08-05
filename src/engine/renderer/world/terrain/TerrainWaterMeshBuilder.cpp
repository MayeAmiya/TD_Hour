#include "engine/renderer/world/terrain/TerrainWaterMeshBuilder.h"

#include "engine/renderer/world/terrain/TerrainTileMeshBuilder.h"
#include "presentation/render/WaterSurfacePerformanceSettings.h"
#include "presentation/render/WaterSurfaceVisualSettings.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace engine::render {
namespace {

constexpr uint8_t kBlendInvertedMask = 0x1;
constexpr uint8_t kBlendForcedFlipMask = 0x2;

[[nodiscard]] bool normalWaterBlendTriangleFlip(
    const TerrainBlendDefinitionRenderData& definition) noexcept {
    const bool inverted =
        (definition.inverted & kBlendInvertedMask) != 0;
    return (definition.inverted & kBlendForcedFlipMask) != 0 ||
        (definition.rightDiagonal && !inverted) ||
        (definition.leftDiagonal && inverted);
}

float signedArea2D(const container::Vector<RenderVector>& polygon) {
    float area = 0.0f;
    for (size_t index = 0; index < polygon.size(); ++index) {
        const RenderVector& a = polygon[index];
        const RenderVector& b = polygon[(index + 1) % polygon.size()];
        area += a.x() * b.y() - b.x() * a.y();
    }
    return area * 0.5f;
}

float cross2D(const RenderVector& a, const RenderVector& b, const RenderVector& c) {
    return (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
}

bool pointInTriangle2D(const RenderVector& point, const RenderVector& a,
                       const RenderVector& b, const RenderVector& c) {
    constexpr float epsilon = 0.0001f;
    const float ab = cross2D(a, b, point);
    const float bc = cross2D(b, c, point);
    const float ca = cross2D(c, a, point);
    return ab >= -epsilon && bc >= -epsilon && ca >= -epsilon;
}

struct WaterClipVertex final {
    math::vec2 position{};
    float terrainHeight = 0.0f;
    float waterHeight = 0.0f;
};

struct WaterEar final {
    container::Array<RenderVector, 3> vertices{};
    math::vec2 minimum{};
    math::vec2 maximum{};
};

[[nodiscard]] float waterEarHeightAt(
    const WaterEar& ear, math::vec2 position) noexcept {
    const RenderVector& a = ear.vertices[0];
    const RenderVector& b = ear.vertices[1];
    const RenderVector& c = ear.vertices[2];
    const float denominator =
        (b.y() - c.y()) * (a.x() - c.x()) +
        (c.x() - b.x()) * (a.y() - c.y());
    if (!std::isfinite(denominator) ||
        std::abs(denominator) <= math::EPSILON) {
        return a.z();
    }
    const float wa = ((b.y() - c.y()) * (position.x() - c.x()) +
                      (c.x() - b.x()) * (position.y() - c.y())) /
        denominator;
    const float wb = ((c.y() - a.y()) * (position.x() - c.x()) +
                      (a.x() - c.x()) * (position.y() - c.y())) /
        denominator;
    return wa * a.z() + wb * b.z() + (1.0f - wa - wb) * c.z();
}

[[nodiscard]] WaterClipVertex interpolateWaterClipVertex(
    const WaterClipVertex& start,
    const WaterClipVertex& end,
    float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    return {
        .position = start.position + (end.position - start.position) * t,
        .terrainHeight = start.terrainHeight +
            (end.terrainHeight - start.terrainHeight) * t,
        .waterHeight = start.waterHeight +
            (end.waterHeight - start.waterHeight) * t,
    };
}

void compactWaterClipPolygon(container::Vector<WaterClipVertex>& polygon,
                             float positionEpsilon) {
    if (polygon.empty()) return;
    const float epsilonSquared = positionEpsilon * positionEpsilon;
    container::Vector<WaterClipVertex> compacted;
    compacted.reserve(polygon.size());
    for (const WaterClipVertex& vertex : polygon) {
        if (!std::isfinite(vertex.position.x()) ||
            !std::isfinite(vertex.position.y()) ||
            !std::isfinite(vertex.terrainHeight) ||
            !std::isfinite(vertex.waterHeight)) {
            polygon.clear();
            return;
        }
        if (!compacted.empty() &&
            (vertex.position - compacted.back().position).length_sq() <=
                epsilonSquared) {
            continue;
        }
        compacted.push_back(vertex);
    }
    if (compacted.size() > 1 &&
        (compacted.front().position - compacted.back().position).length_sq() <=
            epsilonSquared) {
        compacted.pop_back();
    }
    polygon = std::move(compacted);
}

void clipWaterPolygonToDepth(
    const container::Vector<WaterClipVertex>& input,
    container::Vector<WaterClipVertex>& output,
    float minimumVisibleDepth) {
    output.clear();
    if (input.empty()) return;
    if (output.capacity() < input.size() + 1u) {
        output.reserve(input.size() + 1u);
    }
    WaterClipVertex previous = input.back();
    float previousDistance = previous.waterHeight - previous.terrainHeight -
        minimumVisibleDepth;
    bool previousInside = previousDistance >= 0.0f;
    for (const WaterClipVertex& current : input) {
        const float currentDistance = current.waterHeight -
            current.terrainHeight - minimumVisibleDepth;
        const bool currentInside = currentDistance >= 0.0f;
        if (previousInside != currentInside) {
            const float denominator = previousDistance - currentDistance;
            if (std::isfinite(denominator) &&
                std::abs(denominator) > math::EPSILON) {
                output.push_back(interpolateWaterClipVertex(
                    previous, current, previousDistance / denominator));
            }
        }
        if (currentInside) output.push_back(current);
        previous = current;
        previousDistance = currentDistance;
        previousInside = currentInside;
    }
}

[[nodiscard]] float normalizedWaterEarDistance(
    const RenderVector& edgeStart,
    const RenderVector& edgeEnd,
    const WaterClipVertex& point) noexcept {
    const float edgeX = edgeEnd.x() - edgeStart.x();
    const float edgeY = edgeEnd.y() - edgeStart.y();
    const float edgeLength = std::sqrt(edgeX * edgeX + edgeY * edgeY);
    if (!std::isfinite(edgeLength) || edgeLength <= math::EPSILON) {
        return -std::numeric_limits<float>::max();
    }
    return (edgeX * (point.position.y() - edgeStart.y()) -
            edgeY * (point.position.x() - edgeStart.x())) /
        edgeLength;
}

void clipWaterPolygonToEarEdge(
    const container::Vector<WaterClipVertex>& input,
    container::Vector<WaterClipVertex>& output,
    const RenderVector& edgeStart,
    const RenderVector& edgeEnd,
    float positionEpsilon) {
    output.clear();
    if (input.empty()) return;
    if (output.capacity() < input.size() + 1u) {
        output.reserve(input.size() + 1u);
    }
    WaterClipVertex previous = input.back();
    float previousDistance = normalizedWaterEarDistance(
        edgeStart, edgeEnd, previous);
    bool previousInside = previousDistance >= -positionEpsilon;
    for (const WaterClipVertex& current : input) {
        const float currentDistance = normalizedWaterEarDistance(
            edgeStart, edgeEnd, current);
        const bool currentInside = currentDistance >= -positionEpsilon;
        if (previousInside != currentInside) {
            const float denominator = previousDistance - currentDistance;
            if (std::isfinite(denominator) &&
                std::abs(denominator) > math::EPSILON) {
                output.push_back(interpolateWaterClipVertex(
                    previous, current, previousDistance / denominator));
            }
        }
        if (currentInside) output.push_back(current);
        previous = current;
        previousDistance = currentDistance;
        previousInside = currentInside;
    }
}

[[nodiscard]] float waterClipPolygonArea(
    const container::Vector<WaterClipVertex>& polygon) noexcept {
    float area = 0.0f;
    for (size_t index = 0; index < polygon.size(); ++index) {
        const math::vec2& a = polygon[index].position;
        const math::vec2& b = polygon[(index + 1u) % polygon.size()].position;
        area += a.x() * b.y() - b.x() * a.y();
    }
    return area * 0.5f;
}

[[nodiscard]] bool waterPolygonContainsPoint(
    const container::Vector<RenderVector>& polygon,
    math::vec2 point,
    float positionEpsilon) noexcept {
    bool inside = false;
    for (size_t current = 0, previous = polygon.size() - 1u;
         current < polygon.size(); previous = current++) {
        const RenderVector& a = polygon[previous];
        const RenderVector& b = polygon[current];
        const float edgeX = b.x() - a.x();
        const float edgeY = b.y() - a.y();
        const float pointX = point.x() - a.x();
        const float pointY = point.y() - a.y();
        const float edgeLengthSquared = edgeX * edgeX + edgeY * edgeY;
        if (edgeLengthSquared > math::EPSILON) {
            const float projection =
                (pointX * edgeX + pointY * edgeY) / edgeLengthSquared;
            const float distance = std::abs(edgeX * pointY - edgeY * pointX) /
                std::sqrt(edgeLengthSquared);
            if (projection >= 0.0f && projection <= 1.0f &&
                distance <= positionEpsilon) {
                return true;
            }
        }
        const bool crosses = (a.y() > point.y()) != (b.y() > point.y());
        if (!crosses) continue;
        const float intersectionX = (b.x() - a.x()) *
                (point.y() - a.y()) / (b.y() - a.y()) +
            a.x();
        if (point.x() < intersectionX) inside = !inside;
    }
    return inside;
}

[[nodiscard]] bool renderedTerrainCellFlip(
    const TerrainRenderSnapshot& terrain,
    int32_t mapX,
    int32_t mapY) noexcept {
    const TerrainMaterialRenderData* materialData =
        terrain.materials &&
            terrain.materials->isValidFor(terrain.heights.size())
        ? &*terrain.materials
        : nullptr;
    if (!materialData) return false;
    const size_t sampleIndex = static_cast<size_t>(mapY) *
            static_cast<size_t>(terrain.width) +
        static_cast<size_t>(mapX);
    const int32_t baseTileIndex = materialData->baseTileIndices[sampleIndex];
    const std::optional<TerrainTileUvResolution> baseUv =
        resolveTerrainTileUv(
            terrain, *materialData, sampleIndex, baseTileIndex);
    const int16_t primarySelector = materialData->blendTileIndices[sampleIndex];
    const TerrainBlendDefinitionRenderData* primaryDefinition =
        primarySelector > 0 &&
            static_cast<size_t>(primarySelector) <
                materialData->blendDefinitions.size()
        ? &materialData->blendDefinitions[static_cast<size_t>(primarySelector)]
        : nullptr;
    const std::optional<TerrainTileUvResolution> primaryUv = primaryDefinition
        ? resolveTerrainTileUv(
              terrain, *materialData, sampleIndex,
              primaryDefinition->blendTileIndex)
        : std::nullopt;
    const std::optional<TerrainTileUvResolution>& resolvedUv =
        primaryDefinition ? primaryUv : baseUv;
    const bool ordinaryFlip = primaryDefinition
        ? (primaryDefinition->customEdgeTextureClass >= 0
               ? false
               : normalWaterBlendTriangleFlip(*primaryDefinition))
        : false;
    return resolvedUv && resolvedUv->requiresHeightTriangleFlip
        ? resolveTerrainAuthoredCliffTriangleFlip(
              terrain, mapX, mapY)
        : ordinaryFlip;
}

[[nodiscard]] float waterSurfaceVertexOpacity(
    const TerrainWaterMaterialRenderData& material,
    float authoredAlpha,
    float waterDepth,
    float minimumVisibleDepth) noexcept {
    if (!std::isfinite(authoredAlpha) || !std::isfinite(waterDepth) ||
        waterDepth < minimumVisibleDepth) {
        return 0.0f;
    }
    const float alpha = std::clamp(authoredAlpha, 0.0f, 1.0f);
    if (!material.showSoftEdge ||
        !std::isfinite(material.transparentWaterDepth) ||
        material.transparentWaterDepth <= math::EPSILON) {
        return alpha;
    }
    const float depthFactor = std::clamp(
        (waterDepth - minimumVisibleDepth) /
            material.transparentWaterDepth,
        0.0f, 1.0f);
    return alpha * std::lerp(
        std::clamp(material.minimumOpacity, 0.0f, 1.0f),
        1.0f, depthFactor);
}

[[nodiscard]] bool buildRiverWaterMesh(
    const TerrainWaterRenderArea& source,
    const TerrainWaterMaterialRenderData& material,
    TerrainWaterMeshCpu& output) {
    const size_t pointCount = source.polygon.size();
    if (pointCount < 4u || (pointCount & 1u) != 0u ||
        pointCount > water_surface::performance_limits::kMaximumPolygonVertices) {
        return false;
    }
    for (const RenderVector& point : source.polygon) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y()) ||
            !std::isfinite(point.z())) return false;
    }

    const size_t crossSectionCount = pointCount / 2u;
    const auto wrap = [pointCount](int64_t index) noexcept {
        index %= static_cast<int64_t>(pointCount);
        if (index < 0) index += static_cast<int64_t>(pointCount);
        return static_cast<size_t>(index);
    };
    const size_t authoredInner = wrap(source.riverStart);
    const size_t authoredOuter = wrap(static_cast<int64_t>(authoredInner) + 1);
    const RenderVector& firstInner = source.polygon[authoredOuter];
    const RenderVector& firstOuter = source.polygon[authoredInner];
    const float endWidth = std::hypot(
        firstInner.x() - firstOuter.x(), firstInner.y() - firstOuter.y());
    if (!std::isfinite(endWidth) || endWidth <= math::EPSILON) return false;

    float perimeter = 0.0f;
    for (size_t index = 0; index < pointCount; ++index) {
        const RenderVector& a = source.polygon[index];
        const RenderVector& b = source.polygon[(index + 1u) % pointCount];
        perimeter += std::hypot(a.x() - b.x(), a.y() - b.y());
    }
    const float authoredLength = std::max(0.0f, perimeter * 0.5f - endWidth);
    const float vStep = crossSectionCount > 1u
        ? (authoredLength / endWidth) /
              static_cast<float>(crossSectionCount - 1u)
        : 0.0f;

    output.worldTransform = {};
    output.vertices.reserve(crossSectionCount * 2u);
    output.indices.reserve((crossSectionCount - 1u) * 6u);
    int64_t innerIndex = static_cast<int64_t>(authoredInner);
    int64_t outerIndex = static_cast<int64_t>(authoredOuter);
    const float edgeAlpha = material.showSoftEdge
        ? std::clamp(material.minimumOpacity, 0.0f, 1.0f) : 1.0f;
    for (size_t section = 0; section < crossSectionCount; ++section) {
        const RenderVector& inner = source.polygon[wrap(outerIndex++)];
        const RenderVector& outer = source.polygon[wrap(innerIndex--)];
        const float v = static_cast<float>(section) * vStep;
        StaticMeshVertex innerVertex;
        innerVertex.position = {inner.x(), inner.y(), inner.z()};
        innerVertex.normal = {0.0f, 0.0f, 1.0f};
        innerVertex.texcoord = {0.5f, v};
        innerVertex.detailTexcoord = {
            inner.x() * material.skyTexelsPerUnit,
            inner.y() * material.skyTexelsPerUnit,
        };
        innerVertex.color = {1.0f, 1.0f, 1.0f, 1.0f};
        StaticMeshVertex outerVertex = innerVertex;
        outerVertex.position = {outer.x(), outer.y(), outer.z()};
        outerVertex.texcoord = {0.0f, v};
        outerVertex.detailTexcoord = {
            outer.x() * material.skyTexelsPerUnit,
            outer.y() * material.skyTexelsPerUnit,
        };
        outerVertex.color = {1.0f, 1.0f, 1.0f, edgeAlpha};
        output.vertices.push_back(innerVertex);
        output.vertices.push_back(outerVertex);
        if (section + 1u == crossSectionCount) continue;
        const uint32_t base = static_cast<uint32_t>(section * 2u);
        output.indices.insert(output.indices.end(), {
            base, base + 1u, base + 3u,
            base, base + 3u, base + 2u,
        });
    }
    return !output.indices.empty();
}

} // namespace

bool buildTerrainWaterMesh(const TerrainRenderSnapshot& terrain,
                    const TerrainWaterRenderArea& source,
                    const TerrainWaterMaterialRenderData& material,
                    TerrainWaterMeshCpu& output) {
    if (source.river) return buildRiverWaterMesh(source, material, output);
    container::Vector<RenderVector> polygon;
    polygon.reserve(source.polygon.size());
    constexpr float duplicateEpsilon = 0.0001f;
    for (const RenderVector& point : source.polygon) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y()) ||
            !std::isfinite(point.z()) || !std::isfinite(source.surfaceHeight)) {
            return false;
        }
        if (!polygon.empty()) {
            const RenderVector& previous = polygon.back();
            if (std::abs(previous.x() - point.x()) <= duplicateEpsilon &&
                std::abs(previous.y() - point.y()) <= duplicateEpsilon) {
                continue;
            }
        }
        polygon.push_back(point);
    }
    if (polygon.size() > 2) {
        const RenderVector& first = polygon.front();
        const RenderVector& last = polygon.back();
        if (std::abs(first.x() - last.x()) <= duplicateEpsilon &&
            std::abs(first.y() - last.y()) <= duplicateEpsilon) {
            polygon.pop_back();
        }
    }
    if (polygon.size() < 3 ||
        polygon.size() > water_surface::performance_limits::kMaximumPolygonVertices ||
        std::abs(signedArea2D(polygon)) <= duplicateEpsilon) {
        return false;
    }

    // Ear clipping handles concave trigger polygons, unlike a triangle fan.
    // Normalize to counter-clockwise winding so every emitted triangle faces
    // +Z under the shared world convention.
    container::Vector<uint32_t> remaining(polygon.size());
    for (uint32_t index = 0; index < remaining.size(); ++index) remaining[index] = index;
    if (signedArea2D(polygon) < 0.0f) std::reverse(remaining.begin(), remaining.end());

    container::Vector<uint32_t> polygonIndices;
    polygonIndices.reserve((polygon.size() - 2) * 3);
    size_t guard = 0;
    while (remaining.size() > 3 && guard++ < polygon.size() * polygon.size()) {
        bool clipped = false;
        for (size_t index = 0; index < remaining.size(); ++index) {
            const uint32_t previous = remaining[(index + remaining.size() - 1) % remaining.size()];
            const uint32_t current = remaining[index];
            const uint32_t next = remaining[(index + 1) % remaining.size()];
            if (cross2D(polygon[previous], polygon[current], polygon[next]) <= duplicateEpsilon) continue;

            bool containsPoint = false;
            for (const uint32_t candidate : remaining) {
                if (candidate == previous || candidate == current || candidate == next) continue;
                if (pointInTriangle2D(polygon[candidate], polygon[previous], polygon[current], polygon[next])) {
                    containsPoint = true;
                    break;
                }
            }
            if (containsPoint) continue;
            polygonIndices.insert(polygonIndices.end(), {previous, current, next});
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(index));
            clipped = true;
            break;
        }
        if (!clipped) return false;
    }
    if (remaining.size() != 3) return false;
    polygonIndices.insert(
        polygonIndices.end(), {remaining[0], remaining[1], remaining[2]});

    math::vec3 center{};
    math::vec2 minimum{std::numeric_limits<float>::max(),
                       std::numeric_limits<float>::max()};
    math::vec2 maximum{-std::numeric_limits<float>::max(),
                       -std::numeric_limits<float>::max()};
    for (const RenderVector& point : polygon) center += {point.x(), point.y(), 0.0f};
    for (const RenderVector& point : polygon) {
        minimum = {std::min(minimum.x(), point.x()), std::min(minimum.y(), point.y())};
        maximum = {std::max(maximum.x(), point.x()), std::max(maximum.y(), point.y())};
    }
    center = center / static_cast<float>(polygon.size());
    output.worldTransform = math::transform::translation(center);
    const float extentX = std::max(maximum.x() - minimum.x(), 0.001f);
    const float extentY = std::max(maximum.y() - minimum.y(), 0.001f);
    const auto buildAuthoredPolygonFallback = [&]() {
        // W3DWater::renderWater always submits the authored standing-water
        // polygon and relies on terrain depth to hide submerged pixels.  The
        // terrain-conforming mesh below is an optional shoreline refinement:
        // no refinement budget, terrain overlap or numeric-classification
        // failure may remove an otherwise valid authored water surface.
        if (polygonIndices.empty() ||
            polygon.size() >
                water_surface::performance_limits::kMaximumGeneratedVertices ||
            polygonIndices.size() >
                water_surface::performance_limits::kMaximumGeneratedIndices) {
            return false;
        }
        output.vertices.clear();
        output.indices.clear();
        output.vertices.reserve(polygon.size());
        const float surfaceOffset =
            source.surfaceHeight - polygon.front().z();
        for (const RenderVector& point : polygon) {
            const float tx = std::clamp(
                (point.x() - minimum.x()) / extentX, 0.0f, 1.0f);
            const float ty = std::clamp(
                (point.y() - minimum.y()) / extentY, 0.0f, 1.0f);
            const math::vec4 bottom = material.vertexColors[0] +
                (material.vertexColors[1] - material.vertexColors[0]) * tx;
            const math::vec4 top = material.vertexColors[2] +
                (material.vertexColors[3] - material.vertexColors[2]) * tx;
            const math::vec4 authoredColor = bottom + (top - bottom) * ty;
            StaticMeshVertex vertex;
            vertex.position = {
                point.x() - center.x(), point.y() - center.y(),
                point.z() + surfaceOffset,
            };
            vertex.normal = {0.0f, 0.0f, 1.0f};
            vertex.texcoord = {
                point.x() /
                    water_surface::visual_defaults::
                        kStandingWaterWorldUnitsPerRepeat,
                point.y() /
                    water_surface::visual_defaults::
                        kStandingWaterWorldUnitsPerRepeat,
            };
            vertex.detailTexcoord = {
                point.x() * material.skyTexelsPerUnit,
                point.y() * material.skyTexelsPerUnit,
            };
            vertex.color = {
                authoredColor.x(), authoredColor.y(), authoredColor.z(),
                std::clamp(authoredColor.w(), 0.0f, 1.0f),
            };
            output.vertices.push_back(vertex);
        }
        output.indices = polygonIndices;
        return true;
    };
    // Authored standing-water triggers are already the render surface in the
    // original W3D path.  Submit that polygon directly and let terrain depth
    // hide submerged fragments.  Cutting every authored polygon against the
    // heightfield changes shoreline topology and can leave a valid water area
    // with only tiny/disconnected pieces.  Keep the conforming path below for
    // the synthesized legacy full-map surface only; rivers and vertex water
    // have their own builders.
    if (!source.synthesizedLegacyWater) {
        return buildAuthoredPolygonFallback();
    }
    const bool variableSurfaceHeight = std::any_of(
        polygon.begin(), polygon.end(),
        [&polygon](const RenderVector& point) {
            return std::abs(point.z() - polygon.front().z()) > 0.0001f;
        });
    container::Vector<WaterEar> ears;
    ears.reserve(polygonIndices.size() / 3u);
    for (size_t index = 0; index < polygonIndices.size(); index += 3u) {
        WaterEar ear;
        ear.minimum = {std::numeric_limits<float>::max(),
                       std::numeric_limits<float>::max()};
        ear.maximum = {-std::numeric_limits<float>::max(),
                       -std::numeric_limits<float>::max()};
        for (size_t vertex = 0; vertex < 3; ++vertex) {
            ear.vertices[vertex] = polygon[polygonIndices[index + vertex]];
            ear.minimum = {
                std::min(ear.minimum.x(), ear.vertices[vertex].x()),
                std::min(ear.minimum.y(), ear.vertices[vertex].y()),
            };
            ear.maximum = {
                std::max(ear.maximum.x(), ear.vertices[vertex].x()),
                std::max(ear.maximum.y(), ear.vertices[vertex].y()),
            };
        }
        ears.push_back(ear);
    }

    const float inverseCellSize = 1.0f / terrain.cellWorldSize;
    const int32_t unclampedMinimumX = static_cast<int32_t>(std::floor(
        minimum.x() * inverseCellSize + static_cast<float>(terrain.borderSize)));
    const int32_t unclampedMinimumY = static_cast<int32_t>(std::floor(
        minimum.y() * inverseCellSize + static_cast<float>(terrain.borderSize)));
    const int32_t unclampedMaximumX = static_cast<int32_t>(std::floor(
        maximum.x() * inverseCellSize + static_cast<float>(terrain.borderSize)));
    const int32_t unclampedMaximumY = static_cast<int32_t>(std::floor(
        maximum.y() * inverseCellSize + static_cast<float>(terrain.borderSize)));
    if (unclampedMaximumX < 0 || unclampedMaximumY < 0 ||
        unclampedMinimumX >= terrain.width - 1 ||
        unclampedMinimumY >= terrain.height - 1) {
        return buildAuthoredPolygonFallback();
    }
    const int32_t minimumCellX = std::clamp(
        unclampedMinimumX, 0, terrain.width - 2);
    const int32_t minimumCellY = std::clamp(
        unclampedMinimumY, 0, terrain.height - 2);
    const int32_t maximumCellX = std::clamp(
        unclampedMaximumX, 0, terrain.width - 2);
    const int32_t maximumCellY = std::clamp(
        unclampedMaximumY, 0, terrain.height - 2);
    const uint64_t cellCount =
        static_cast<uint64_t>(maximumCellX - minimumCellX + 1) *
        static_cast<uint64_t>(maximumCellY - minimumCellY + 1);
    if (ears.empty() ||
        cellCount > std::numeric_limits<uint64_t>::max() /
            (2u * static_cast<uint64_t>(ears.size())) ||
        cellCount * 2u * static_cast<uint64_t>(ears.size()) >
            water_surface::performance_limits::kMaximumCandidateTrianglePairs) {
        return buildAuthoredPolygonFallback();
    }

    const float minimumVisibleDepth =
        water_surface::visual_defaults::kMinimumVisibleDepth;
    const float positionEpsilon = std::max(
        0.0001f, terrain.cellWorldSize * 0.00001f);
    const float areaEpsilon = positionEpsilon *
        std::max(terrain.cellWorldSize, 1.0f);
    const container::Array<math::vec2, 4> terrainCorners{{
        {terrain.worldPosition(0, 0).x(), terrain.worldPosition(0, 0).y()},
        {terrain.worldPosition(terrain.width - 1, 0).x(),
         terrain.worldPosition(terrain.width - 1, 0).y()},
        {terrain.worldPosition(terrain.width - 1, terrain.height - 1).x(),
         terrain.worldPosition(terrain.width - 1, terrain.height - 1).y()},
        {terrain.worldPosition(0, terrain.height - 1).x(),
         terrain.worldPosition(0, terrain.height - 1).y()},
    }};
    const bool polygonCoversTerrain = source.synthesizedLegacyWater ||
        std::all_of(
            terrainCorners.begin(), terrainCorners.end(),
            [&polygon, positionEpsilon](math::vec2 point) {
                return waterPolygonContainsPoint(
                    polygon, point, positionEpsilon);
            });

    const auto appendWaterVertex = [&](const WaterClipVertex& clipped) {
        if (output.vertices.size() >=
                water_surface::performance_limits::kMaximumGeneratedVertices ||
            output.vertices.size() >=
                static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
            return UINT32_MAX;
        }
        const float tx = std::clamp(
            (clipped.position.x() - minimum.x()) / extentX,
            0.0f, 1.0f);
        const float ty = std::clamp(
            (clipped.position.y() - minimum.y()) / extentY,
            0.0f, 1.0f);
        const math::vec4 bottom = material.vertexColors[0] +
            (material.vertexColors[1] - material.vertexColors[0]) * tx;
        const math::vec4 top = material.vertexColors[2] +
            (material.vertexColors[3] - material.vertexColors[2]) * tx;
        const math::vec4 authoredColor = bottom + (top - bottom) * ty;
        StaticMeshVertex vertex;
        vertex.position = {
            clipped.position.x() - center.x(),
            clipped.position.y() - center.y(),
            clipped.waterHeight,
        };
        vertex.normal = {0.0f, 0.0f, 1.0f};
        vertex.texcoord = {
            clipped.position.x() /
                water_surface::visual_defaults::kStandingWaterWorldUnitsPerRepeat,
            clipped.position.y() /
                water_surface::visual_defaults::kStandingWaterWorldUnitsPerRepeat,
        };
        vertex.detailTexcoord = {
            clipped.position.x() * material.skyTexelsPerUnit,
            clipped.position.y() * material.skyTexelsPerUnit,
        };
        vertex.color = {
            authoredColor.x(), authoredColor.y(), authoredColor.z(),
            waterSurfaceVertexOpacity(
                material, authoredColor.w(),
                clipped.waterHeight - clipped.terrainHeight,
                minimumVisibleDepth),
        };
        const uint32_t index = static_cast<uint32_t>(output.vertices.size());
        output.vertices.push_back(vertex);
        return index;
    };

    const auto appendPiece = [&](const container::Vector<WaterClipVertex>& piece) {
        if (output.vertices.size() + piece.size() >
                water_surface::performance_limits::kMaximumGeneratedVertices ||
            output.indices.size() + (piece.size() - 2u) * 3u >
                water_surface::performance_limits::kMaximumGeneratedIndices ||
            output.vertices.size() + piece.size() >
                static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
            output.vertices.clear();
            output.indices.clear();
            return false;
        }
        const uint32_t firstVertex =
            static_cast<uint32_t>(output.vertices.size());
        for (const WaterClipVertex& clipped : piece) {
            if (appendWaterVertex(clipped) == UINT32_MAX) {
                output.vertices.clear();
                output.indices.clear();
                return false;
            }
        }
        for (uint32_t vertex = 1;
             vertex + 1u < static_cast<uint32_t>(piece.size());
             ++vertex) {
            output.indices.insert(output.indices.end(), {
                firstVertex, firstVertex + vertex,
                firstVertex + vertex + 1u,
            });
        }
        return true;
    };

    container::Vector<WaterClipVertex> triangleInput;
    container::Vector<WaterClipVertex> wet;
    container::Vector<WaterClipVertex> pieceA;
    container::Vector<WaterClipVertex> pieceB;
    triangleInput.reserve(4);
    wet.reserve(5);
    pieceA.reserve(polygon.size() + 4u);
    pieceB.reserve(polygon.size() + 4u);
    container::Vector<uint32_t> sharedTerrainVertices;
    if (polygonCoversTerrain) {
        sharedTerrainVertices.assign(terrain.heights.size(), UINT32_MAX);
    }
    constexpr container::Array<int32_t, 4> cornerOffsetX{{0, 1, 1, 0}};
    constexpr container::Array<int32_t, 4> cornerOffsetY{{0, 0, 1, 1}};
    for (int32_t mapY = minimumCellY; mapY <= maximumCellY; ++mapY) {
        for (int32_t mapX = minimumCellX; mapX <= maximumCellX; ++mapX) {
            const container::Array<math::vec3, 4> corners{{
                terrain.worldPosition(mapX, mapY),
                terrain.worldPosition(mapX + 1, mapY),
                terrain.worldPosition(mapX + 1, mapY + 1),
                terrain.worldPosition(mapX, mapY + 1),
            }};
            bool cellHasWetCorner = variableSurfaceHeight;
            bool cellAllWet = !variableSurfaceHeight;
            if (!variableSurfaceHeight) {
                for (const math::vec3& corner : corners) {
                    const bool wetCorner = source.surfaceHeight - corner.z() >=
                        minimumVisibleDepth;
                    cellHasWetCorner = cellHasWetCorner || wetCorner;
                    cellAllWet = cellAllWet && wetCorner;
                }
            }
            if (!cellHasWetCorner) continue;
            // A completely submerged cell is a flat coplanar water quad, so
            // its diagonal has no shoreline/depth-classification effect.
            // Resolve the full authored terrain topology only for mixed
            // shoreline cells where the clipping plane depends on it.
            const bool flip = cellAllWet
                ? false
                : renderedTerrainCellFlip(terrain, mapX, mapY);
            const container::Array<container::Array<uint8_t, 3>, 2> triangleIndices = flip
                ? container::Array<container::Array<uint8_t, 3>, 2>{{
                      {{1, 3, 0}}, {{1, 2, 3}},
                  }}
                : container::Array<container::Array<uint8_t, 3>, 2>{{
                      {{0, 1, 2}}, {{0, 2, 3}},
                  }};
            const math::vec2 cellMinimum{corners[0].x(), corners[0].y()};
            const math::vec2 cellMaximum{corners[2].x(), corners[2].y()};
            for (const container::Array<uint8_t, 3>& triangle : triangleIndices) {
                if (variableSurfaceHeight) {
                    for (const WaterEar& ear : ears) {
                        if (ear.maximum.x() < cellMinimum.x() ||
                            ear.maximum.y() < cellMinimum.y() ||
                            ear.minimum.x() > cellMaximum.x() ||
                            ear.minimum.y() > cellMaximum.y()) {
                            continue;
                        }
                        triangleInput.clear();
                        for (const uint8_t corner : triangle) {
                            const math::vec2 position{
                                corners[corner].x(), corners[corner].y()};
                            triangleInput.push_back({
                                .position = position,
                                .terrainHeight = corners[corner].z(),
                                .waterHeight = waterEarHeightAt(ear, position),
                            });
                        }
                        pieceA = triangleInput;
                        pieceB.clear();
                        for (size_t edge = 0;
                             edge < 3u && pieceA.size() >= 3u; ++edge) {
                            clipWaterPolygonToEarEdge(
                                pieceA, pieceB, ear.vertices[edge],
                                ear.vertices[(edge + 1u) % 3u],
                                positionEpsilon);
                            compactWaterClipPolygon(pieceB, positionEpsilon);
                            pieceA.swap(pieceB);
                        }
                        if (pieceA.size() < 3u) continue;
                        clipWaterPolygonToDepth(
                            pieceA, wet, minimumVisibleDepth);
                        compactWaterClipPolygon(wet, positionEpsilon);
                        if (wet.size() < 3u ||
                            std::abs(waterClipPolygonArea(wet)) <= areaEpsilon) {
                            continue;
                        }
                        if (!appendPiece(wet)) {
                            return buildAuthoredPolygonFallback();
                        }
                    }
                    continue;
                }
                bool allWet = polygonCoversTerrain;
                bool anyWet = false;
                for (const uint8_t corner : triangle) {
                    const bool cornerWet =
                        source.surfaceHeight - corners[corner].z() >=
                            minimumVisibleDepth;
                    allWet = allWet && cornerWet;
                    anyWet = anyWet || cornerWet;
                }
                if (!anyWet) continue;
                if (allWet) {
                    if (output.indices.size() + 3u >
                        water_surface::performance_limits::kMaximumGeneratedIndices) {
                        output.vertices.clear();
                        output.indices.clear();
                        return buildAuthoredPolygonFallback();
                    }
                    container::Array<uint32_t, 3> sharedIndices{};
                    for (size_t vertex = 0; vertex < triangle.size(); ++vertex) {
                        const uint8_t corner = triangle[vertex];
                        const int32_t sampleX = mapX + cornerOffsetX[corner];
                        const int32_t sampleY = mapY + cornerOffsetY[corner];
                        const size_t sampleIndex = static_cast<size_t>(sampleY) *
                                static_cast<size_t>(terrain.width) +
                            static_cast<size_t>(sampleX);
                        uint32_t& sharedIndex = sharedTerrainVertices[sampleIndex];
                        if (sharedIndex == UINT32_MAX) {
                            sharedIndex = appendWaterVertex({
                                .position = {
                                    corners[corner].x(), corners[corner].y()},
                                .terrainHeight = corners[corner].z(),
                                .waterHeight = source.surfaceHeight,
                            });
                            if (sharedIndex == UINT32_MAX) {
                                output.vertices.clear();
                                output.indices.clear();
                                return buildAuthoredPolygonFallback();
                            }
                        }
                        sharedIndices[vertex] = sharedIndex;
                    }
                    output.indices.insert(
                        output.indices.end(), sharedIndices.begin(),
                        sharedIndices.end());
                    continue;
                }
                triangleInput.clear();
                for (const uint8_t corner : triangle) {
                    triangleInput.push_back({
                        .position = {corners[corner].x(), corners[corner].y()},
                        .terrainHeight = corners[corner].z(),
                        .waterHeight = source.surfaceHeight,
                    });
                }
                clipWaterPolygonToDepth(
                    triangleInput, wet, minimumVisibleDepth);
                compactWaterClipPolygon(wet, positionEpsilon);
                if (wet.size() < 3 ||
                    std::abs(waterClipPolygonArea(wet)) <= areaEpsilon) {
                    continue;
                }
                if (polygonCoversTerrain) {
                    if (!appendPiece(wet)) {
                        return buildAuthoredPolygonFallback();
                    }
                    continue;
                }
                for (const WaterEar& ear : ears) {
                    if (ear.maximum.x() < cellMinimum.x() ||
                        ear.maximum.y() < cellMinimum.y() ||
                        ear.minimum.x() > cellMaximum.x() ||
                        ear.minimum.y() > cellMaximum.y()) {
                        continue;
                    }
                    pieceA = wet;
                    pieceB.clear();
                    for (size_t edge = 0; edge < 3 && pieceA.size() >= 3; ++edge) {
                        clipWaterPolygonToEarEdge(
                            pieceA, pieceB, ear.vertices[edge],
                            ear.vertices[(edge + 1u) % 3u], positionEpsilon);
                        compactWaterClipPolygon(pieceB, positionEpsilon);
                        pieceA.swap(pieceB);
                    }
                    if (pieceA.size() < 3 ||
                        std::abs(waterClipPolygonArea(pieceA)) <= areaEpsilon) {
                        continue;
                    }
                    if (!appendPiece(pieceA)) {
                        return buildAuthoredPolygonFallback();
                    }
                }
            }
        }
    }
    return !output.indices.empty() || buildAuthoredPolygonFallback();
}

[[nodiscard]] bool buildTerrainVertexWaterMesh(
    const TerrainVertexWaterRenderData& source,
    const TerrainWaterMaterialRenderData& material,
    TerrainWaterMeshCpu& output) {
    if (!source.isValid()) return false;
    const uint64_t vertexCount =
        static_cast<uint64_t>(source.gridCellsX + 1u) *
        static_cast<uint64_t>(source.gridCellsY + 1u);
    const uint64_t indexCount =
        static_cast<uint64_t>(source.gridCellsX) *
        static_cast<uint64_t>(source.gridCellsY) * 6u;
    if (vertexCount >
            water_surface::performance_limits::kMaximumGeneratedVertices ||
        indexCount >
            water_surface::performance_limits::kMaximumGeneratedIndices ||
        vertexCount > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    const float sine = std::sin(source.angleRadians);
    const float cosine = std::cos(source.angleRadians);
    output.worldTransform = {};
    output.vertices.reserve(static_cast<size_t>(vertexCount));
    output.indices.reserve(static_cast<size_t>(indexCount));
    const bool hasIntegratedPointHeights =
        source.pointHeights.size() == static_cast<size_t>(vertexCount);
    for (uint32_t y = 0; y <= source.gridCellsY; ++y) {
        const float ty = static_cast<float>(y) /
            static_cast<float>(source.gridCellsY);
        for (uint32_t x = 0; x <= source.gridCellsX; ++x) {
            const float tx = static_cast<float>(x) /
                static_cast<float>(source.gridCellsX);
            const float localX = static_cast<float>(x) * source.gridSize;
            const float localY = static_cast<float>(y) * source.gridSize;
            const math::vec2 world{
                source.position.x() + localX * cosine - localY * sine,
                source.position.y() + localX * sine + localY * cosine,
            };
            const math::vec4 bottom = material.vertexColors[0] +
                (material.vertexColors[1] - material.vertexColors[0]) * tx;
            const math::vec4 top = material.vertexColors[2] +
                (material.vertexColors[3] - material.vertexColors[2]) * tx;
            const math::vec4 color = bottom + (top - bottom) * ty;
            StaticMeshVertex vertex;
            const size_t pointIndex = static_cast<size_t>(y) *
                (static_cast<size_t>(source.gridCellsX) + 1u) + x;
            float heightOffset = hasIntegratedPointHeights
                ? source.pointHeights[pointIndex] : 0.0f;
            vertex.position = {
                world.x(), world.y(), source.position.z() + heightOffset};
            vertex.normal = {0.0f, 0.0f, 1.0f};
            vertex.texcoord = {
                world.x() /
                    water_surface::visual_defaults::kStandingWaterWorldUnitsPerRepeat,
                world.y() /
                    water_surface::visual_defaults::kStandingWaterWorldUnitsPerRepeat,
            };
            vertex.detailTexcoord = {
                world.x() * material.skyTexelsPerUnit,
                world.y() * material.skyTexelsPerUnit,
            };
            vertex.color = color;
            output.vertices.push_back(vertex);
        }
    }
    const uint32_t rowStride = source.gridCellsX + 1u;
    for (uint32_t y = 0; y < source.gridCellsY; ++y) {
        for (uint32_t x = 0; x < source.gridCellsX; ++x) {
            const uint32_t first = y * rowStride + x;
            output.indices.insert(output.indices.end(), {
                first, first + 1u, first + rowStride + 1u,
                first, first + rowStride + 1u, first + rowStride,
            });
        }
    }
    return !output.indices.empty();
}


std::optional<TerrainWaterMeshInspection> inspectTerrainWaterMesh(
    const TerrainRenderSnapshot& terrain,
    const TerrainWaterRenderArea& area,
    const TerrainWaterMaterialRenderData& material) {
    TerrainWaterMeshCpu cpu;
    if (!buildTerrainWaterMesh(terrain, area, material, cpu)) {
        return std::nullopt;
    }
    TerrainWaterMeshInspection output;
    output.river = area.river;
    output.vertices.reserve(cpu.vertices.size());
    for (const StaticMeshVertex& vertex : cpu.vertices) {
        output.vertices.push_back({
            .worldPosition =
                cpu.worldTransform.transform_point(vertex.position),
            .texcoord = vertex.texcoord,
            .skyTexcoord = vertex.detailTexcoord,
            .alpha = vertex.color.w(),
        });
    }
    output.indices = std::move(cpu.indices);
    return output;
}

std::optional<TerrainWaterMeshInspection> inspectTerrainVertexWaterMesh(
    const TerrainVertexWaterRenderData& grid,
    const TerrainWaterMaterialRenderData& material) {
    TerrainWaterMeshCpu cpu;
    if (!buildTerrainVertexWaterMesh(grid, material, cpu)) {
        return std::nullopt;
    }
    TerrainWaterMeshInspection output;
    output.vertices.reserve(cpu.vertices.size());
    for (const StaticMeshVertex& vertex : cpu.vertices) {
        output.vertices.push_back({
            .worldPosition =
                cpu.worldTransform.transform_point(vertex.position),
            .texcoord = vertex.texcoord,
            .skyTexcoord = vertex.detailTexcoord,
            .alpha = vertex.color.w(),
        });
    }
    output.indices = std::move(cpu.indices);
    return output;
}

} // namespace engine::render
