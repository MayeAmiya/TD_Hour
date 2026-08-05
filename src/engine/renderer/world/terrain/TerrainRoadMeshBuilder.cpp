#include "engine/renderer/world/terrain/TerrainRoadMeshBuilder.h"

#include "engine/renderer/world/terrain/TerrainTileMeshBuilder.h"
#include "presentation/render/RoadMeshPerformanceSettings.h"
#include "presentation/render/RoadSurfaceVisualSettings.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <utility>

namespace engine::render {
namespace {

constexpr uint8_t kBlendInvertedMask = 0x1;
constexpr uint8_t kBlendForcedFlipMask = 0x2;
constexpr float kMinimumRoadHalfWidth = 0.125f;
constexpr float kMaximumRoadHalfWidth = 256.0f;

[[nodiscard]] bool roadTerrainCellFlip(
    const TerrainRenderSnapshot& terrain,
    int32_t mapX,
    int32_t mapY) noexcept {
    const TerrainMaterialRenderData* materials =
        terrain.materials &&
            terrain.materials->isValidFor(terrain.heights.size())
        ? &*terrain.materials
        : nullptr;
    if (!materials) return false;

    const size_t sampleIndex = static_cast<size_t>(mapY) *
            static_cast<size_t>(terrain.width) +
        static_cast<size_t>(mapX);
    if (sampleIndex >= materials->baseTileIndices.size() ||
        sampleIndex >= materials->blendTileIndices.size()) {
        return false;
    }

    const int16_t primarySelector = materials->blendTileIndices[sampleIndex];
    const TerrainBlendDefinitionRenderData* primaryDefinition =
        primarySelector > 0 &&
            static_cast<size_t>(primarySelector) <
                materials->blendDefinitions.size()
        ? &materials->blendDefinitions[static_cast<size_t>(primarySelector)]
        : nullptr;
    const int32_t tileIndex = primaryDefinition
        ? primaryDefinition->blendTileIndex
        : materials->baseTileIndices[sampleIndex];
    const std::optional<TerrainTileUvResolution> resolvedUv =
        resolveTerrainTileUv(
            terrain, *materials, sampleIndex, tileIndex);

    const bool ordinaryFlip = [primaryDefinition]() noexcept {
        if (!primaryDefinition ||
            primaryDefinition->customEdgeTextureClass >= 0) {
            return false;
        }
        const bool inverted =
            (primaryDefinition->inverted & kBlendInvertedMask) != 0;
        return
            (primaryDefinition->inverted & kBlendForcedFlipMask) != 0 ||
            (primaryDefinition->rightDiagonal && !inverted) ||
            (primaryDefinition->leftDiagonal && inverted);
    }();

    return resolvedUv && resolvedUv->requiresHeightTriangleFlip
        ? resolveTerrainAuthoredCliffTriangleFlip(
              terrain, mapX, mapY)
        : ordinaryFlip;
}

[[nodiscard]] bool renderedTerrainCellFlip(
    const TerrainRenderSnapshot& terrain,
    int32_t mapX,
    int32_t mapY) noexcept {
    return roadTerrainCellFlip(terrain, mapX, mapY);
}

struct RoadMeshEndpointSection final {
    math::vec2 negativeSide{};
    math::vec2 positiveSide{};
    bool exactMiter = false;
};

struct RoadMeshPlan final {
    float startTrim = 0.0f;
    float endTrim = 0.0f;
    RoadMeshEndpointSection startSection{};
    RoadMeshEndpointSection endSection{};
    uint32_t materialPass = 0;
};

struct RoadJunctionArm final {
    size_t roadIndex = 0;
    bool atStart = false;
    math::vec2 direction{};
    float halfWidth = 0.0f;
};

enum class RoadJunctionKind : uint8_t {
    Generic,
    Tee,
    FourWay,
    ThreeWayY,
    ThreeWayH,
    ThreeWayHFlip,
};

struct RoadJunctionPlan final {
    math::vec2 center{};
    container::Vector<RoadJunctionArm> arms;
    size_t ownerRoadIndex = 0;
    float trimRadius = 0.0f;
    RoadJunctionKind kind = RoadJunctionKind::Generic;
    container::Array<math::vec2, 4> authoredCorners{};
    math::vec2 authoredForward{};
    math::vec2 authoredNormal{};
    float authoredAtlasU = 0.0f;
    float authoredAtlasV = 0.0f;
    float authoredUvScale = 1.0f;
};

struct RoadCornerPlan final {
    math::vec2 center{};
    math::vec2 curveCenter{};
    container::Array<RoadJunctionArm, 2> arms{};
    size_t ownerRoadIndex = 0;
    float trimRadius = 0.0f;
    float curveRadius = 0.0f;
    uint32_t curveSliceCount = 0;
    bool miter = false;
    bool tight = false;
};

struct RoadCrossJoinPlan final {
    size_t ownerRoadIndex = 0;
    math::vec2 endpoint{};
    math::vec2 targetDirection{};
    float ownerHalfWidth = 0.0f;
    float targetWidth = 0.0f;
};

struct RoadJunctionGraph final {
    container::Vector<RoadMeshPlan> roads;
    container::Vector<RoadJunctionPlan> junctions;
    container::Vector<RoadCornerPlan> corners;
    container::Vector<RoadCrossJoinPlan> crossJoins;
    container::Vector<container::Vector<size_t>> junctionsByOwner;
    container::Vector<container::Vector<size_t>> cornersByOwner;
    container::Vector<container::Vector<size_t>> crossJoinsByOwner;
};

[[nodiscard]] float roadVectorCross(const math::vec2& left,
                                    const math::vec2& right) noexcept {
    return left.x() * right.y() - left.y() * right.x();
}

[[nodiscard]] std::optional<math::vec2> intersectRoadOffsetLines(
    const math::vec2& firstPoint,
    const math::vec2& firstDirection,
    const math::vec2& secondPoint,
    const math::vec2& secondDirection) noexcept {
    const float denominator = roadVectorCross(
        firstDirection, secondDirection);
    // RefCode LineSegClass::Find_Intersection rejects only an exactly
    // parallel pair; near-parallel authored roads keep their distant miter.
    if (!std::isfinite(denominator) || denominator == 0.0f) {
        return std::nullopt;
    }
    const float distance = roadVectorCross(
        secondPoint - firstPoint, secondDirection) / denominator;
    const math::vec2 intersection = firstPoint + firstDirection * distance;
    if (!std::isfinite(intersection.x()) ||
        !std::isfinite(intersection.y())) {
        return std::nullopt;
    }
    return intersection;
}

void assignRoadMiterEndpointSection(
    RoadMeshPlan& plan,
    const RoadJunctionArm& arm,
    const math::vec2& outwardNegative,
    const math::vec2& outwardPositive) noexcept {
    RoadMeshEndpointSection section;
    // buildRoadMesh's side vector follows the source record direction. At an
    // authored end that direction is opposite the graph arm's outward
    // direction, so the two edge labels must be swapped there.
    if (arm.atStart) {
        section.negativeSide = outwardNegative;
        section.positiveSide = outwardPositive;
    } else {
        section.negativeSide = outwardPositive;
        section.positiveSide = outwardNegative;
    }
    section.exactMiter = true;
    if (arm.atStart) plan.startSection = section;
    else plan.endSection = section;
}

void assignRoadEndpointCenter(RoadMeshPlan& plan,
                              const RoadJunctionArm& arm,
                              const math::vec2& endpointCenter) noexcept {
    const math::vec2 sourceDirection = arm.atStart
        ? arm.direction : -arm.direction;
    const math::vec2 sourceSide{
        -sourceDirection.y(), sourceDirection.x()};
    RoadMeshEndpointSection section;
    section.negativeSide = endpointCenter - sourceSide * arm.halfWidth;
    section.positiveSide = endpointCenter + sourceSide * arm.halfWidth;
    section.exactMiter = true;
    if (arm.atStart) plan.startSection = section;
    else plan.endSection = section;
}

[[nodiscard]] bool renderedTerrainCellFlip(
    const TerrainRenderSnapshot& terrain,
    int32_t mapX,
    int32_t mapY) noexcept;

[[nodiscard]] std::optional<float> terrainSurfaceHeight(
    const TerrainRenderSnapshot& terrain,
    float worldX,
    float worldY) noexcept {
    if (!terrain.isValid() || !std::isfinite(worldX) || !std::isfinite(worldY)) {
        return std::nullopt;
    }
    const float gridX = worldX / terrain.cellWorldSize +
        static_cast<float>(terrain.borderSize);
    const float gridY = worldY / terrain.cellWorldSize +
        static_cast<float>(terrain.borderSize);
    if (gridX < 0.0f || gridY < 0.0f ||
        gridX > static_cast<float>(terrain.width - 1) ||
        gridY > static_cast<float>(terrain.height - 1)) return std::nullopt;
    const int32_t x0 = static_cast<int32_t>(std::floor(gridX));
    const int32_t y0 = static_cast<int32_t>(std::floor(gridY));
    const int32_t x1 = std::min(x0 + 1, terrain.width - 1);
    const int32_t y1 = std::min(y0 + 1, terrain.height - 1);
    const float tx = gridX - static_cast<float>(x0);
    const float ty = gridY - static_cast<float>(y0);
    const float h00 = terrain.heightWorld(x0, y0);
    const float h10 = terrain.heightWorld(x1, y0);
    const float h11 = terrain.heightWorld(x1, y1);
    const float h01 = terrain.heightWorld(x0, y1);
    if (x0 == x1 || y0 == y1) {
        const float lower = h00 + (h10 - h00) * tx;
        const float upper = h01 + (h11 - h01) * tx;
        return lower + (upper - lower) * ty;
    }

    // Match the exact two triangles emitted by buildChunk().  Bilinear
    // sampling can pass through a terrain ridge which does not exist in the
    // rendered mesh and is therefore unsuitable for road conformance.
    if (!renderedTerrainCellFlip(terrain, x0, y0)) {
        if (ty <= tx) {
            return h00 + (h10 - h00) * tx + (h11 - h10) * ty;
        }
        return h00 + (h11 - h01) * tx + (h01 - h00) * ty;
    }
    if (tx + ty <= 1.0f) {
        return h00 + (h10 - h00) * tx + (h01 - h00) * ty;
    }
    return h11 + (h01 - h11) * (1.0f - tx) +
        (h10 - h11) * (1.0f - ty);
}

[[nodiscard]] std::optional<float> terrainCellMaximumHeight(
    const TerrainRenderSnapshot& terrain,
    float worldX,
    float worldY) noexcept;

[[nodiscard]] std::optional<float> roadCrossSectionMaximumHeight(
    const TerrainRenderSnapshot& terrain,
    math::vec2 left,
    math::vec2 right) {
    using namespace game::road_surface;
    if (!terrain.isValid()) return std::nullopt;
    const float width = (right - left).length();
    if (!std::isfinite(width)) return std::nullopt;
    // ZH W3DRoadBuffer::loadFloat4PtSection samples a regular cross-road grid
    // at MAP_XY_FACTOR spacing, caps it to 100 rows, and raises the collapsed
    // output column to the maximum sampled cell height. It does not intersect
    // the road with every terrain triangle edge.
    const uint32_t sampleCount = std::clamp<uint32_t>(
        static_cast<uint32_t>(width /
            std::max(terrain.cellWorldSize, math::EPSILON)) + 1u,
        2u, kRoadMeshMaximumLateralSamples);
    float maximum = -std::numeric_limits<float>::max();
    for (uint32_t sample = 0u; sample < sampleCount; ++sample) {
        const float t = static_cast<float>(sample) /
            static_cast<float>(sampleCount - 1u);
        const math::vec2 point = left + (right - left) * t;
        const std::optional<float> height = terrainCellMaximumHeight(
            terrain, point.x(), point.y());
        if (!height) return std::nullopt;
        maximum = std::max(maximum, *height);
    }
    return maximum;
}

[[nodiscard]] std::optional<float> terrainCellMaximumHeight(
    const TerrainRenderSnapshot& terrain,
    float worldX,
    float worldY) noexcept {
    if (!terrain.isValid() || !std::isfinite(worldX) ||
        !std::isfinite(worldY)) return std::nullopt;
    const float gridX = worldX / terrain.cellWorldSize +
        static_cast<float>(terrain.borderSize);
    const float gridY = worldY / terrain.cellWorldSize +
        static_cast<float>(terrain.borderSize);
    if (gridX < 0.0f || gridY < 0.0f ||
        gridX > static_cast<float>(terrain.width - 1) ||
        gridY > static_cast<float>(terrain.height - 1)) return std::nullopt;
    const int32_t x0 = std::min(
        static_cast<int32_t>(std::floor(gridX)), terrain.width - 2);
    const int32_t y0 = std::min(
        static_cast<int32_t>(std::floor(gridY)), terrain.height - 2);
    return std::max({
        terrain.heightWorld(x0, y0),
        terrain.heightWorld(x0 + 1, y0),
        terrain.heightWorld(x0 + 1, y0 + 1),
        terrain.heightWorld(x0, y0 + 1),
    });
}

[[nodiscard]] math::vec2 rotateRoadVector(const math::vec2& value,
                                          float radians) noexcept {
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    return {
        value.x() * cosine - value.y() * sine,
        value.x() * sine + value.y() * cosine,
    };
}

[[nodiscard]] std::optional<RoadJunctionPlan> buildAuthoredRoadJunction(
    const TerrainRenderSnapshot& terrain,
    const math::vec2& center,
    const container::Vector<RoadJunctionArm>& arms,
    container::Vector<RoadMeshPlan>& roadPlans,
    bool requireTerrainConform) {
    using namespace game::road_surface;
    if (arms.size() != 3u && arms.size() != 4u) return std::nullopt;

    size_t ownerRoadIndex = std::numeric_limits<size_t>::max();
    for (const RoadJunctionArm& arm : arms) {
        ownerRoadIndex = std::min(ownerRoadIndex, arm.roadIndex);
    }
    // insertY/insert3Way/insert4Way author their synthetic road from index1,
    // the first map-order segment at the endpoint. Unequal-width arms must
    // not silently promote the patch to the widest later segment.
    const TerrainRoadRenderSegment& authoredOwner =
        terrain.roads[ownerRoadIndex];
    const float scale = std::max(authoredOwner.width, 0.25f);
    const float widthInTexture = std::clamp(
        authoredOwner.widthInTexture, 0.01f, 32.0f);
    container::Vector<math::vec2> endpointCenters(arms.size(), center);
    RoadJunctionPlan plan;
    plan.center = center;
    plan.arms = arms;
    plan.ownerRoadIndex = ownerRoadIndex;
    plan.authoredUvScale = scale;
    const auto setRectangle = [&plan](const math::vec2& bottomLeft,
                                      const math::vec2& bottomRight,
                                      const math::vec2& topRight,
                                      const math::vec2& topLeft) {
        plan.authoredCorners = {{
            bottomLeft, bottomRight, topRight, topLeft,
        }};
    };
    const auto normalizedPerpendicular = [](const math::vec2& direction) {
        return math::vec2{-direction.y(), direction.x()};
    };

    if (arms.size() == 3u) {
        bool yRejected = false;
        for (size_t first = 0u; first < 3u; ++first) {
            for (size_t second = first + 1u; second < 3u; ++second) {
                if (arms[first].direction.dot(arms[second].direction) <
                    kRoadYRejectOppositeDot) {
                    yRejected = true;
                }
            }
        }
        size_t yStem = 3u;
        float bestYScore = std::numeric_limits<float>::max();
        if (!yRejected) {
            for (size_t stem = 0u; stem < 3u; ++stem) {
                const size_t firstLeg = (stem + 1u) % 3u;
                const size_t secondLeg = (stem + 2u) % 3u;
                const float firstCross = roadVectorCross(
                    arms[stem].direction, arms[firstLeg].direction);
                const float secondCross = roadVectorCross(
                    arms[stem].direction, arms[secondLeg].direction);
                const math::vec2 stemLeft = normalizedPerpendicular(
                    arms[stem].direction);
                if (firstCross * secondCross >= 0.0f ||
                    roadVectorCross(stemLeft, arms[firstLeg].direction) <= 0.0f ||
                    roadVectorCross(stemLeft, arms[secondLeg].direction) <= 0.0f) {
                    continue;
                }
                const float score = std::abs(
                    arms[stem].direction.dot(arms[firstLeg].direction) -
                        kRoadYIdealLegDot) +
                    std::abs(
                    arms[stem].direction.dot(arms[secondLeg].direction) -
                        kRoadYIdealLegDot);
                if (score < bestYScore) {
                    bestYScore = score;
                    yStem = stem;
                }
            }
        }
        if (yStem < 3u) {
            plan.kind = RoadJunctionKind::ThreeWayY;
            const math::vec2 upVector =
                arms[yStem].direction * (scale * 0.5f);
            const math::vec2 forward =
                rotateRoadVector(upVector, -std::numbers::pi_v<float> * 0.5f)
                    .normalized();
            const math::vec2 normal = normalizedPerpendicular(forward);
            const math::vec2 roadVector =
                forward * (scale * kRoadYLengthFactor);
            const math::vec2 roadNormal = normal * scale;
            const math::vec2 topLeft = center +
                roadNormal * kRoadYTopOffsetFactor - roadVector * 0.5f;
            const math::vec2 bottomLeft =
                topLeft - roadNormal * kRoadYWidthFactor;
            setRectangle(bottomLeft, bottomLeft + roadVector,
                         topLeft + roadVector, topLeft);
            plan.authoredForward = forward;
            plan.authoredNormal = normal;
            plan.authoredAtlasU = kRoadYAtlasU;
            plan.authoredAtlasV = kRoadYAtlasV;
            endpointCenters[yStem] = center +
                upVector * kRoadYStemEndpointFactor;
            for (size_t index = 0u; index < 3u; ++index) {
                if (index == yStem) continue;
                const float angle = roadVectorCross(
                    arms[yStem].direction, arms[index].direction) > 0.0f
                    ? std::numbers::pi_v<float> * 0.75f
                    : -std::numbers::pi_v<float> * 0.75f;
                endpointCenters[index] = center +
                    rotateRoadVector(upVector, angle) *
                        kRoadYLegEndpointFactor;
            }
        } else {
            size_t firstPair = 0u;
            size_t secondPair = 1u;
            float mostOpposite = arms[0].direction.dot(arms[1].direction);
            for (size_t first = 0u; first < 3u; ++first) {
                for (size_t second = first + 1u; second < 3u; ++second) {
                    const float dot =
                        arms[first].direction.dot(arms[second].direction);
                    if (dot < mostOpposite) {
                        mostOpposite = dot;
                        firstPair = first;
                        secondPair = second;
                    }
                }
            }
            const size_t branch = 3u - firstPair - secondPair;
            const math::vec2 upDirection =
                (arms[secondPair].direction - arms[firstPair].direction)
                    .normalized();
            const math::vec2 upVector = upDirection * (scale * 0.5f);
            const float cross = roadVectorCross(
                upDirection, arms[branch].direction);
            const bool mirror = cross < 0.0f;
            const math::vec2 teeVector = rotateRoadVector(
                upVector, mirror ? -std::numbers::pi_v<float> * 0.5f
                                 : std::numbers::pi_v<float> * 0.5f);
            const math::vec2 forward = teeVector.normalized();
            const math::vec2 normal = normalizedPerpendicular(forward);
            const bool slanted = std::abs(
                upDirection.dot(arms[branch].direction)) >
                kRoadSlantedTeeDot;
            if (!slanted) {
                plan.kind = RoadJunctionKind::Tee;
                const float teeFactor =
                    scale * kRoadTeeWidthAdjustment * 0.5f;
                const float left = scale * widthInTexture * 0.5f;
                const math::vec2 bottomLeft =
                    center - forward * left - normal * teeFactor;
                const math::vec2 bottomRight =
                    center + forward * teeFactor - normal * teeFactor;
                setRectangle(bottomLeft, bottomRight,
                             bottomRight + normal * (teeFactor * 2.0f),
                             bottomLeft + normal * (teeFactor * 2.0f));
                plan.authoredForward = forward;
                plan.authoredNormal = normal;
                plan.authoredAtlasU = kRoadTeeAtlasU;
                plan.authoredAtlasV = kRoadTeeAtlasV;
                endpointCenters[firstPair] = center - upVector;
                endpointCenters[secondPair] = center + upVector;
                endpointCenters[branch] = center + teeVector;
            } else {
                const bool flip = roadVectorCross(
                    teeVector, arms[branch].direction) > 0.0f;
                plan.kind = flip
                    ? RoadJunctionKind::ThreeWayHFlip
                    : RoadJunctionKind::ThreeWayH;
                const math::vec2 roadVector = forward * scale;
                math::vec2 roadNormal =
                    normal * (scale * kRoadHNormalFactor);
                const float bottomOffset = flip
                    ? kRoadHFlipBottomOffset : kRoadHBottomOffset;
                const math::vec2 bottomLeft = center -
                    roadNormal * bottomOffset -
                    roadVector * (widthInTexture * 0.5f);
                const math::vec2 bottomRight = bottomLeft +
                    roadVector *
                        (widthInTexture * 0.5f + kRoadHExtensionFactor);
                setRectangle(bottomLeft, bottomRight,
                             bottomRight + roadNormal,
                             bottomLeft + roadNormal);
                plan.authoredForward = forward;
                plan.authoredNormal = flip ? -normal : normal;
                plan.authoredAtlasU = kRoadHAtlasU;
                plan.authoredAtlasV = kRoadHAtlasV;
                if (flip != mirror) {
                    endpointCenters[firstPair] = center -
                        upVector * kRoadHLongEndpointFactor;
                    endpointCenters[secondPair] = center +
                        upVector * kRoadHShortEndpointFactor;
                } else {
                    endpointCenters[firstPair] = center -
                        upVector * kRoadHShortEndpointFactor;
                    endpointCenters[secondPair] = center +
                        upVector * kRoadHLongEndpointFactor;
                }
                const math::vec2 branchArm = rotateRoadVector(
                    teeVector, flip ? std::numbers::pi_v<float> * 0.25f
                                    : -std::numbers::pi_v<float> * 0.25f);
                endpointCenters[branch] = center +
                    branchArm * kRoadHArmEndpointFactor;
            }
        }
    } else {
        plan.kind = RoadJunctionKind::FourWay;
        size_t firstPair = 0u;
        size_t secondPair = 1u;
        float mostOpposite = arms[0].direction.dot(arms[1].direction);
        for (size_t first = 0u; first < 4u; ++first) {
            for (size_t second = first + 1u; second < 4u; ++second) {
                const float dot = arms[first].direction.dot(arms[second].direction);
                if (dot < mostOpposite) {
                    mostOpposite = dot;
                    firstPair = first;
                    secondPair = second;
                }
            }
        }
        container::Array<size_t, 2> remaining{};
        size_t remainingCount = 0u;
        for (size_t index = 0u; index < 4u; ++index) {
            if (index != firstPair && index != secondPair) {
                remaining[remainingCount++] = index;
            }
        }
        math::vec2 alignVector =
            (arms[secondPair].direction - arms[firstPair].direction)
                .normalized() * (scale * 0.5f);
        math::vec2 teeVector = rotateRoadVector(
            alignVector,
            roadVectorCross(alignVector, arms[remaining[0]].direction) < 0.0f
                ? -std::numbers::pi_v<float> * 0.5f
                : std::numbers::pi_v<float> * 0.5f);
        endpointCenters[firstPair] = center - alignVector;
        endpointCenters[secondPair] = center + alignVector;
        endpointCenters[remaining[0]] = center + teeVector;
        endpointCenters[remaining[1]] = center - teeVector;
        if (alignVector.x() < 0.0f) alignVector = -alignVector;
        const math::vec2 forward = alignVector.normalized();
        const math::vec2 normal = normalizedPerpendicular(forward);
        const float teeFactor = scale * kRoadTeeWidthAdjustment * 0.5f;
        setRectangle(center - forward * teeFactor - normal * teeFactor,
                     center + forward * teeFactor - normal * teeFactor,
                     center + forward * teeFactor + normal * teeFactor,
                     center - forward * teeFactor + normal * teeFactor);
        plan.authoredForward = forward;
        plan.authoredNormal = normal;
        plan.authoredAtlasU = kRoadFourWayAtlasU;
        plan.authoredAtlasV = kRoadFourWayAtlasV;
    }

    if (plan.kind == RoadJunctionKind::Generic) return std::nullopt;
    if (requireTerrainConform) {
        for (const math::vec2& corner : plan.authoredCorners) {
            if (!terrainCellMaximumHeight(
                    terrain, corner.x(), corner.y()).has_value()) {
                return std::nullopt;
            }
        }
    }
    for (size_t index = 0u; index < arms.size(); ++index) {
        assignRoadEndpointCenter(
            roadPlans[arms[index].roadIndex], arms[index],
            endpointCenters[index]);
    }
    return plan;
}

[[nodiscard]] RoadJunctionGraph buildRoadJunctionGraph(
    const TerrainRenderSnapshot& terrain,
    bool requireTerrainConform = true) {
    using namespace game::road_surface;

    struct PendingNode final {
        container::String styleName;
        uint32_t styleIdentity = 0;
        math::vec2 position{};
        container::Vector<RoadJunctionArm> arms;
    };

    RoadJunctionGraph graph;
    graph.roads.resize(terrain.roads.size());
    container::Vector<PendingNode> nodes;
    nodes.reserve(terrain.roads.size());
    container::HashMap<uint64_t, container::Vector<size_t>> nodeBuckets;
    const auto endpointBucketKey = [](const TerrainRoadRenderSegment& road,
                                      math::vec2 position) noexcept {
        uint64_t hash = 1469598103934665603ull;
        const auto mix = [&hash](uint64_t value) noexcept {
            constexpr uint64_t prime = 1099511628211ull;
            for (uint32_t byte = 0; byte < 8u; ++byte) {
                hash ^= static_cast<uint8_t>(value >> (byte * 8u));
                hash *= prime;
            }
        };
        mix(road.styleIdentity);
        if (road.styleIdentity == 0u) {
            for (const char value : road.styleName) {
                mix(static_cast<uint8_t>(value));
            }
        }
        mix(std::bit_cast<uint32_t>(position.x()));
        mix(std::bit_cast<uint32_t>(position.y()));
        return hash;
    };

    for (size_t roadIndex = 0; roadIndex < terrain.roads.size(); ++roadIndex) {
        const TerrainRoadRenderSegment& road = terrain.roads[roadIndex];
        const math::vec2 start{road.start.x(), road.start.y()};
        const math::vec2 end{road.end.x(), road.end.y()};
        const math::vec2 roadDelta = end - start;
        const float roadLength = roadDelta.length();
        if (!std::isfinite(roadLength) || roadLength <= math::EPSILON ||
            !std::isfinite(road.width) || road.width <= 0.0f) {
            continue;
        }
        const math::vec2 forward = roadDelta / roadLength;
        for (size_t endpoint = 0; endpoint < 2u; ++endpoint) {
            const bool atStart = endpoint == 0u;
            const math::vec2 position = atStart ? start : end;
            if (!std::isfinite(position.x()) || !std::isfinite(position.y())) continue;
            const float physicalWidth = road.width *
                std::clamp(road.widthInTexture, 0.01f, 32.0f);
            const RoadJunctionArm arm{
                .roadIndex = roadIndex,
                .atStart = atStart,
                .direction = atStart ? forward : -forward,
                .halfWidth = std::clamp(
                    physicalWidth * 0.5f,
                    kMinimumRoadHalfWidth,
                    kMaximumRoadHalfWidth),
            };

            const uint64_t bucketKey = endpointBucketKey(road, position);
            size_t matchingNodeIndex = nodes.size();
            if (const auto bucket = nodeBuckets.find(bucketKey);
                bucket != nodeBuckets.end()) {
                for (const size_t nodeIndex : bucket->second) {
                    PendingNode& node = nodes[nodeIndex];
                    const bool sameStyle = road.styleIdentity != 0u &&
                            node.styleIdentity != 0u
                        ? node.styleIdentity == road.styleIdentity
                        : node.styleName == road.styleName;
                    if (!sameStyle || node.arms.empty()) continue;
                    // Hashing only narrows candidates. Keep RefCode's exact
                    // authored Vector2 equality as the final contract.
                    if (position.x() == node.position.x() &&
                        position.y() == node.position.y()) {
                        matchingNodeIndex = nodeIndex;
                        break;
                    }
                }
            }
            if (matchingNodeIndex == nodes.size()) {
                PendingNode node;
                node.styleName = road.styleName;
                node.styleIdentity = road.styleIdentity;
                node.position = position;
                node.arms.push_back(arm);
                nodes.push_back(std::move(node));
                nodeBuckets[bucketKey].push_back(nodes.size() - 1u);
            } else {
                nodes[matchingNodeIndex].arms.push_back(arm);
            }
        }
    }

    container::Vector<container::Array<uint32_t, 2>>
        sameStyleEndpointConnections(terrain.roads.size());
    for (const PendingNode& node : nodes) {
        const uint32_t connectionCount = node.arms.empty()
            ? 0u : static_cast<uint32_t>(node.arms.size() - 1u);
        for (const RoadJunctionArm& arm : node.arms) {
            sameStyleEndpointConnections[arm.roadIndex][arm.atStart ? 0u : 1u] =
                connectionCount;
        }
    }

    for (PendingNode& node : nodes) {
        if (node.arms.size() == 2u) {
            const RoadJunctionArm& first = node.arms[0];
            const RoadJunctionArm& second = node.arms[1];
            const float outwardDot = std::clamp(
                first.direction.dot(second.direction), -1.0f, 1.0f);
            const float turnAngle = std::acos(std::clamp(
                -outwardDot, -1.0f, 1.0f));
            if (outwardDot < kRoadJunctionParallelDirectionDot &&
                turnAngle > 0.001f) {
                const TerrainRoadRenderSegment& firstRoad =
                    terrain.roads[first.roadIndex];
                const TerrainRoadRenderSegment& secondRoad =
                    terrain.roads[second.roadIndex];
                const auto endpointAngled = [](const TerrainRoadRenderSegment& road,
                                                bool atStart) noexcept {
                    return atStart ? road.startAngled : road.endAngled;
                };
                bool miter =
                    turnAngle < kRoadCurveStepRadians * 0.9f ||
                    endpointAngled(firstRoad, first.atStart) ||
                    endpointAngled(secondRoad, second.atStart);
                const bool tight = firstRoad.tightCorner ||
                    secondRoad.tightCorner;
                const float authoredWidth = std::max(
                    std::max(firstRoad.width, 0.25f),
                    std::max(secondRoad.width, 0.25f));
                const float curveRadius = authoredWidth * (tight
                    ? kRoadTightCornerRadiusInWidths
                    : kRoadNormalCornerRadiusInWidths);
                float trimRadius = miter
                    ? std::max(first.halfWidth, second.halfWidth)
                    : curveRadius * std::tan(turnAngle * 0.5f);
                const auto armLength = [&terrain](const RoadJunctionArm& arm) {
                    const TerrainRoadRenderSegment& road =
                        terrain.roads[arm.roadIndex];
                    return math::vec2{
                        road.end.x() - road.start.x(),
                        road.end.y() - road.start.y()}.length();
                };
                const float shortestArm = std::min(
                    armLength(first), armLength(second));
                // RefCode rejects a circular trim that leaves less than its
                // DOT_LIMIT (0.5 world unit) on either source segment, then
                // falls back to the exact miter path.
                if (!miter && (!std::isfinite(trimRadius) ||
                        trimRadius + 0.5f >= shortestArm)) {
                    miter = true;
                    trimRadius = std::max(
                        first.halfWidth, second.halfWidth);
                }
                if (miter) {
                    const math::vec2 firstNormal{
                        -first.direction.y(), first.direction.x()};
                    const math::vec2 secondNormal{
                        -second.direction.y(), second.direction.x()};
                    container::Array<math::vec2, 2> intersections{};
                    bool hasMiterIntersections = true;
                    for (size_t sideIndex = 0u; sideIndex < 2u;
                         ++sideIndex) {
                        const float firstSign = sideIndex == 0u
                            ? -1.0f : 1.0f;
                        // Both graph arms point away from the joint. Boundary
                        // continuity therefore pairs one arm's top line with
                        // the other arm's bottom line.
                        const std::optional<math::vec2> intersection =
                            intersectRoadOffsetLines(
                                node.position + firstNormal *
                                    (first.halfWidth * firstSign),
                                first.direction,
                                node.position + secondNormal *
                                    (second.halfWidth * -firstSign),
                                second.direction);
                        if (!intersection) {
                            hasMiterIntersections = false;
                            break;
                        }
                        intersections[sideIndex] = *intersection;
                    }
                    if (!hasMiterIntersections) continue;
                    bool safeMiter = true;
                    if (safeMiter && requireTerrainConform) {
                        safeMiter = roadCrossSectionMaximumHeight(
                            terrain, intersections[0], intersections[1])
                            .has_value();
                    }
                    // An exactly parallel/non-finite pair keeps square ends;
                    // otherwise preserve RefCode's infinite-line result.
                    if (!safeMiter) continue;

                    RoadMeshPlan& firstPlan = graph.roads[first.roadIndex];
                    RoadMeshPlan& secondPlan = graph.roads[second.roadIndex];
                    assignRoadMiterEndpointSection(
                        firstPlan, first, intersections[0], intersections[1]);
                    assignRoadMiterEndpointSection(
                        secondPlan, second, intersections[1], intersections[0]);
                    continue;
                }
                const math::vec2 center = node.position;
                bool patchFitsTerrain = std::isfinite(trimRadius) &&
                    trimRadius >= kRoadJunctionMinimumTrim;
                if (patchFitsTerrain && requireTerrainConform) {
                    patchFitsTerrain = terrainSurfaceHeight(
                        terrain, center.x(), center.y()).has_value();
                    for (const RoadJunctionArm& arm : node.arms) {
                        const math::vec2 side{
                            -arm.direction.y(), arm.direction.x()};
                        const math::vec2 endCenter = center +
                            arm.direction * trimRadius;
                        patchFitsTerrain &= roadCrossSectionMaximumHeight(
                            terrain,
                            endCenter - side * arm.halfWidth,
                            endCenter + side * arm.halfWidth).has_value();
                    }
                }
                if (patchFitsTerrain) {
                    math::vec2 curveCenter = center;
                    uint32_t curveSliceCount = 0u;
                    if (!miter) {
                        const math::vec2 bisector =
                            first.direction + second.direction;
                        const float bisectorLength = bisector.length();
                        const float cosineHalfTurn =
                            std::cos(turnAngle * 0.5f);
                        if (bisectorLength <= math::EPSILON ||
                            cosineHalfTurn <= math::EPSILON) {
                            patchFitsTerrain = false;
                        } else {
                            curveCenter = center + bisector / bisectorLength *
                                (curveRadius / cosineHalfTurn);
                            curveSliceCount = std::max(1u,
                                static_cast<uint32_t>(std::ceil(
                                    turnAngle / kRoadCurveStepRadians)));
                        }
                    }
                    if (!patchFitsTerrain) continue;
                    for (const RoadJunctionArm& arm : node.arms) {
                        RoadMeshPlan& roadPlan = graph.roads[arm.roadIndex];
                        if (arm.atStart) roadPlan.startTrim = trimRadius;
                        else roadPlan.endTrim = trimRadius;
                    }
                    graph.corners.push_back({
                        .center = center,
                        .curveCenter = curveCenter,
                        .arms = {first, second},
                        .ownerRoadIndex = std::min(
                            first.roadIndex, second.roadIndex),
                        .trimRadius = trimRadius,
                        .curveRadius = curveRadius,
                        .curveSliceCount = curveSliceCount,
                        .miter = miter,
                        .tight = tight,
                    });
                }
            }
            continue;
        }
        if (node.arms.size() < kRoadMinimumJunctionDegree ||
            node.arms.size() > kRoadMaximumJunctionDegree) {
            continue;
        }

        bool hasDuplicateDirection = false;
        for (size_t first = 0; first < node.arms.size() && !hasDuplicateDirection; ++first) {
            for (size_t second = first + 1u; second < node.arms.size(); ++second) {
                if (node.arms[first].direction.dot(node.arms[second].direction) >
                    kRoadJunctionParallelDirectionDot) {
                    hasDuplicateDirection = true;
                    break;
                }
            }
        }
        // Duplicate outward arms generally mean malformed authored endpoints
        // or overlapping road records. Independent strips are a safer fallback
        // than emitting a self-overlapping junction polygon.
        if (hasDuplicateDirection) continue;

        const math::vec2 center = node.position;
        if ((node.arms.size() == 3u || node.arms.size() == 4u)) {
            if (std::optional<RoadJunctionPlan> authored =
                    buildAuthoredRoadJunction(
                        terrain, center, node.arms, graph.roads,
                        requireTerrainConform)) {
                graph.junctions.push_back(std::move(*authored));
                continue;
            }
        }

        float maximumHalfWidth = 0.0f;
        float shortestArmLength = std::numeric_limits<float>::max();
        size_t ownerRoadIndex = std::numeric_limits<size_t>::max();
        for (const RoadJunctionArm& arm : node.arms) {
            maximumHalfWidth = std::max(maximumHalfWidth, arm.halfWidth);
            ownerRoadIndex = std::min(ownerRoadIndex, arm.roadIndex);
            const TerrainRoadRenderSegment& road = terrain.roads[arm.roadIndex];
            const math::vec2 delta{road.end.x() - road.start.x(),
                                   road.end.y() - road.start.y()};
            shortestArmLength = std::min(shortestArmLength, delta.length());
        }
        float trimRadius = std::max(kRoadJunctionMinimumTrim,
            maximumHalfWidth * kRoadJunctionTrimWidthFactor);
        trimRadius = std::min(trimRadius,
            shortestArmLength * kRoadMaximumEndpointTrimFraction);
        if (!std::isfinite(trimRadius) || trimRadius < kRoadJunctionMinimumTrim) continue;

        bool patchFitsTerrain = true;
        if (requireTerrainConform) {
            patchFitsTerrain = terrainCellMaximumHeight(
                terrain, center.x(), center.y()).has_value();
            for (const RoadJunctionArm& arm : node.arms) {
                const math::vec2 side{-arm.direction.y(), arm.direction.x()};
                const math::vec2 endCenter = center + arm.direction * trimRadius;
                for (float sign : {-1.0f, 1.0f}) {
                    const math::vec2 point = endCenter + side * (arm.halfWidth * sign);
                    patchFitsTerrain &= terrainCellMaximumHeight(
                        terrain, point.x(), point.y()).has_value();
                }
            }
        }
        // Do not trim the contributing strips unless every patch sample can
        // be conformed. This preserves the old independent-strip result at a
        // terrain boundary instead of leaving an intersection-shaped hole.
        if (!patchFitsTerrain) continue;

        for (const RoadJunctionArm& arm : node.arms) {
            RoadMeshPlan& roadPlan = graph.roads[arm.roadIndex];
            if (arm.atStart) roadPlan.startTrim = trimRadius;
            else roadPlan.endTrim = trimRadius;
        }
        RoadJunctionPlan junction;
        junction.center = center;
        junction.arms = std::move(node.arms);
        junction.ownerRoadIndex = ownerRoadIndex;
        junction.trimRadius = trimRadius;
        graph.junctions.push_back(std::move(junction));
    }

    // ROAD_JOIN is deliberately cross-material.  It does not require an
    // exact endpoint match: the flagged open end may land anywhere inside a
    // different road strip.  Preserve that authored relationship as a small
    // fading connector and promote the owner material above the target style.
    struct JoinOrdering final { size_t owner = 0; size_t target = 0; };
    container::Vector<JoinOrdering> ordering;
    constexpr float kJoinSpatialCellSize = 64.0f;
    constexpr int64_t kMaximumIndexedCellsPerRoad = 4096;
    const auto spatialCoordinate = [](float value) noexcept {
        const double cell = std::floor(
            static_cast<double>(value) /
            static_cast<double>(kJoinSpatialCellSize));
        return static_cast<int32_t>(std::clamp(
            cell,
            static_cast<double>(std::numeric_limits<int32_t>::min()),
            static_cast<double>(std::numeric_limits<int32_t>::max())));
    };
    const auto spatialKey = [](int32_t x, int32_t y) noexcept {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32u) |
            static_cast<uint32_t>(y);
    };
    container::HashMap<uint64_t, container::Vector<size_t>> roadsByCell;
    container::Vector<size_t> globallyIndexedRoads;
    for (size_t targetIndex = 0; targetIndex < terrain.roads.size();
         ++targetIndex) {
        const TerrainRoadRenderSegment& target = terrain.roads[targetIndex];
        const math::vec2 targetStart{target.start.x(), target.start.y()};
        const math::vec2 targetEnd{target.end.x(), target.end.y()};
        if (!std::isfinite(targetStart.x()) ||
            !std::isfinite(targetStart.y()) ||
            !std::isfinite(targetEnd.x()) ||
            !std::isfinite(targetEnd.y())) {
            continue;
        }
        const float halfWidth = std::max(target.width, 0.25f) * 0.5f;
        const int32_t minimumCellX = spatialCoordinate(
            std::min(targetStart.x(), targetEnd.x()) - halfWidth);
        const int32_t minimumCellY = spatialCoordinate(
            std::min(targetStart.y(), targetEnd.y()) - halfWidth);
        const int32_t maximumCellX = spatialCoordinate(
            std::max(targetStart.x(), targetEnd.x()) + halfWidth);
        const int32_t maximumCellY = spatialCoordinate(
            std::max(targetStart.y(), targetEnd.y()) + halfWidth);
        const int64_t cellWidth =
            static_cast<int64_t>(maximumCellX) - minimumCellX + 1;
        const int64_t cellHeight =
            static_cast<int64_t>(maximumCellY) - minimumCellY + 1;
        if (cellWidth <= 0 || cellHeight <= 0 ||
            cellWidth > kMaximumIndexedCellsPerRoad ||
            cellHeight > kMaximumIndexedCellsPerRoad ||
            cellWidth * cellHeight > kMaximumIndexedCellsPerRoad) {
            globallyIndexedRoads.push_back(targetIndex);
            continue;
        }
        for (int32_t y = minimumCellY;; ++y) {
            for (int32_t x = minimumCellX;; ++x) {
                roadsByCell[spatialKey(x, y)].push_back(targetIndex);
                if (x == maximumCellX) break;
            }
            if (y == maximumCellY) break;
        }
    }
    for (size_t roadIndex = 0; roadIndex < terrain.roads.size(); ++roadIndex) {
        const TerrainRoadRenderSegment& road = terrain.roads[roadIndex];
        const math::vec2 roadStart{road.start.x(), road.start.y()};
        const math::vec2 roadEnd{road.end.x(), road.end.y()};
        const math::vec2 roadDelta = roadEnd - roadStart;
        const float roadLength = roadDelta.length();
        if (!std::isfinite(roadLength) || roadLength <= math::EPSILON) continue;
        const math::vec2 outwardStart = roadDelta / roadLength;
        for (uint32_t endpointIndex = 0; endpointIndex < 2u; ++endpointIndex) {
            const bool atStart = endpointIndex == 0u;
            if (!(atStart ? road.startJoin : road.endJoin)) continue;
            // RefCode inserts ROAD_JOIN only for a flagged open endpoint.
            // A same-type exact endpoint connection consumes the flag through
            // ordinary curve/miter/junction handling instead.
            if (sameStyleEndpointConnections[roadIndex][endpointIndex] != 0u) {
                continue;
            }
            const math::vec2 endpoint = atStart ? roadStart : roadEnd;
            const math::vec2 ownerJoinDirection = atStart
                ? outwardStart : -outwardStart;
            size_t bestTarget = terrain.roads.size();
            math::vec2 bestTargetDirection{};
            float bestTargetWidth = 0.0f;
            const auto considerTarget = [&](size_t targetIndex) {
                if (targetIndex >= bestTarget || targetIndex == roadIndex) {
                    return;
                }
                const TerrainRoadRenderSegment& target =
                    terrain.roads[targetIndex];
                const bool sameStyle = road.styleIdentity != 0u &&
                        target.styleIdentity != 0u
                    ? road.styleIdentity == target.styleIdentity
                    : target.styleName == road.styleName;
                if (sameStyle) return;
                const math::vec2 targetStart{
                    target.start.x(), target.start.y()};
                const math::vec2 targetEnd{target.end.x(), target.end.y()};
                const math::vec2 targetDelta = targetEnd - targetStart;
                const float lengthSquared = targetDelta.length_sq();
                if (!std::isfinite(lengthSquared) ||
                    lengthSquared <= math::EPSILON * math::EPSILON) return;
                const float targetWidth = std::max(target.width, 0.25f);
                const float halfTargetWidth = targetWidth * 0.5f;
                if (endpoint.x() < std::min(targetStart.x(), targetEnd.x()) -
                        halfTargetWidth ||
                    endpoint.x() > std::max(targetStart.x(), targetEnd.x()) +
                        halfTargetWidth ||
                    endpoint.y() < std::min(targetStart.y(), targetEnd.y()) -
                        halfTargetWidth ||
                    endpoint.y() > std::max(targetStart.y(), targetEnd.y()) +
                        halfTargetWidth) {
                    return;
                }
                const float t = std::clamp(
                    (endpoint - targetStart).dot(targetDelta) /
                        lengthSquared,
                    0.0f, 1.0f);
                const math::vec2 closest = targetStart + targetDelta * t;
                const float distance = (closest - endpoint).length();
                if (distance >= targetWidth * 0.55f) return;
                bestTarget = targetIndex;
                bestTargetDirection = targetDelta / std::sqrt(lengthSquared);
                bestTargetWidth = targetWidth;
            };
            if (const auto candidates = roadsByCell.find(spatialKey(
                    spatialCoordinate(endpoint.x()),
                    spatialCoordinate(endpoint.y())));
                candidates != roadsByCell.end()) {
                for (const size_t targetIndex : candidates->second) {
                    considerTarget(targetIndex);
                }
            }
            for (const size_t targetIndex : globallyIndexedRoads) {
                considerTarget(targetIndex);
            }
            math::vec2 fadeDirection = ownerJoinDirection;
            if (bestTarget != terrain.roads.size()) {
                const math::vec2 normal{
                    -bestTargetDirection.y(), bestTargetDirection.x()};
                fadeDirection = normal *
                    (normal.dot(ownerJoinDirection) >= 0.0f ? 1.0f : -1.0f);
            }
            const float ownerHalfWidth = std::clamp(
                road.width * std::clamp(
                    road.widthInTexture, 0.01f, 32.0f) * 0.5f,
                kMinimumRoadHalfWidth, kMaximumRoadHalfWidth);
            const math::vec2 ownerSide{
                -outwardStart.y(), outwardStart.x()};
            const math::vec2 joinLineDirection{
                -fadeDirection.y(), fadeDirection.x()};
            const std::optional<math::vec2> clippedNegative =
                intersectRoadOffsetLines(
                    endpoint - ownerSide * ownerHalfWidth,
                    outwardStart, endpoint, joinLineDirection);
            const std::optional<math::vec2> clippedPositive =
                intersectRoadOffsetLines(
                    endpoint + ownerSide * ownerHalfWidth,
                    outwardStart, endpoint, joinLineDirection);
            float alphaJoinHalfWidth = ownerHalfWidth;
            if (clippedNegative && clippedPositive) {
                RoadMeshEndpointSection section;
                section.negativeSide = *clippedNegative;
                section.positiveSide = *clippedPositive;
                section.exactMiter = true;
                RoadMeshPlan& ownerPlan = graph.roads[roadIndex];
                if (atStart) ownerPlan.startSection = section;
                else ownerPlan.endSection = section;
                alphaJoinHalfWidth =
                    (*clippedPositive - *clippedNegative).length() * 0.5f;
            }
            graph.crossJoins.push_back({
                .ownerRoadIndex = roadIndex,
                .endpoint = endpoint,
                .targetDirection = fadeDirection,
                .ownerHalfWidth = alphaJoinHalfWidth,
                .targetWidth = bestTargetWidth,
            });
            if (bestTarget != terrain.roads.size()) {
                ordering.push_back({roadIndex, bestTarget});
            }
        }
    }
    container::Vector<size_t> roadStyleIndices(terrain.roads.size(), 0u);
    container::HashMap<uint32_t, size_t> styleIndexByIdentity;
    container::HashMap<container::String, size_t> styleIndexByName;
    size_t styleCount = 0u;
    for (size_t roadIndex = 0; roadIndex < terrain.roads.size(); ++roadIndex) {
        const TerrainRoadRenderSegment& road = terrain.roads[roadIndex];
        size_t styleIndex = 0u;
        if (road.styleIdentity != 0u) {
            const auto [iterator, inserted] =
                styleIndexByIdentity.emplace(road.styleIdentity, styleCount);
            if (inserted) ++styleCount;
            styleIndex = iterator->second;
        } else {
            const auto [iterator, inserted] =
                styleIndexByName.emplace(road.styleName, styleCount);
            if (inserted) ++styleCount;
            styleIndex = iterator->second;
        }
        roadStyleIndices[roadIndex] = styleIndex;
    }
    container::Vector<uint32_t> stylePasses(styleCount, 0u);
    for (size_t iteration = 0; iteration < styleCount; ++iteration) {
        bool changed = false;
        for (const JoinOrdering& edge : ordering) {
            uint32_t& ownerPass = stylePasses[roadStyleIndices[edge.owner]];
            const uint32_t desired = std::min<uint32_t>(
                stylePasses[roadStyleIndices[edge.target]] + 1u,
                static_cast<uint32_t>(styleCount));
            if (ownerPass < desired) {
                ownerPass = desired;
                changed = true;
            }
        }
        if (!changed) break;
    }
    for (size_t roadIndex = 0; roadIndex < terrain.roads.size(); ++roadIndex) {
        graph.roads[roadIndex].materialPass =
            stylePasses[roadStyleIndices[roadIndex]];
    }
    graph.junctionsByOwner.resize(terrain.roads.size());
    graph.cornersByOwner.resize(terrain.roads.size());
    graph.crossJoinsByOwner.resize(terrain.roads.size());
    for (size_t index = 0; index < graph.junctions.size(); ++index) {
        const size_t owner = graph.junctions[index].ownerRoadIndex;
        if (owner < graph.junctionsByOwner.size()) {
            graph.junctionsByOwner[owner].push_back(index);
        }
    }
    for (size_t index = 0; index < graph.corners.size(); ++index) {
        const size_t owner = graph.corners[index].ownerRoadIndex;
        if (owner < graph.cornersByOwner.size()) {
            graph.cornersByOwner[owner].push_back(index);
        }
    }
    for (size_t index = 0; index < graph.crossJoins.size(); ++index) {
        const size_t owner = graph.crossJoins[index].ownerRoadIndex;
        if (owner < graph.crossJoinsByOwner.size()) {
            graph.crossJoinsByOwner[owner].push_back(index);
        }
    }
    return graph;
}

bool buildRoadMesh(const TerrainRenderSnapshot& terrain,
                   const TerrainRoadRenderSegment& source,
                   const RoadMeshPlan& plan,
                   TerrainRoadMeshCpu& output) {
    using namespace game::road_surface;
    const math::vec2 authoredStart{source.start.x(), source.start.y()};
    const math::vec2 authoredEnd{source.end.x(), source.end.y()};
    math::vec2 direction = authoredEnd - authoredStart;
    const float authoredLength = direction.length();
    if (!std::isfinite(authoredLength) || authoredLength <= math::EPSILON ||
        !std::isfinite(source.width) || source.width <= 0.0f) return false;
    direction = direction / authoredLength;
    float startTrim = std::max(0.0f, plan.startTrim);
    float endTrim = std::max(0.0f, plan.endTrim);
    // Every curve plan already validates its authored trim against its source
    // arm. Re-scaling here would move the straight strip away from the exact
    // curve boundary and reopen a crack.
    if (startTrim + endTrim >= authoredLength) return false;
    const math::vec2 start = authoredStart + direction * startTrim;
    const math::vec2 end = authoredEnd - direction * endTrim;
    const float length = (end - start).length();
    if (!std::isfinite(length) || length <= math::EPSILON) return false;
    const math::vec2 side{-direction.y(), direction.x()};
    const float widthInTexture = std::clamp(
        source.widthInTexture, 0.01f, 32.0f);
    const float halfWidth = std::clamp(
        source.width * widthInTexture * 0.5f,
        kMinimumRoadHalfWidth,
        kMaximumRoadHalfWidth);
    const math::vec2 startNegative = plan.startSection.exactMiter
        ? plan.startSection.negativeSide
        : start - side * halfWidth;
    const math::vec2 startPositive = plan.startSection.exactMiter
        ? plan.startSection.positiveSide
        : start + side * halfWidth;
    const math::vec2 endNegative = plan.endSection.exactMiter
        ? plan.endSection.negativeSide
        : end - side * halfWidth;
    const math::vec2 endPositive = plan.endSection.exactMiter
        ? plan.endSection.positiveSide
        : end + side * halfWidth;
    const float sampleDistance = std::max(
        terrain.cellWorldSize * kRoadMeshSampleDistanceInCells, 1.0f);
    const uint32_t segmentCount = std::clamp<uint32_t>(
        static_cast<uint32_t>(std::ceil(length / sampleDistance)), 1u,
        kRoadMeshMaximumSubdivisions);
    container::Vector<float> longitudinalParameters;
    longitudinalParameters.reserve(segmentCount + 1u);
    for (uint32_t sample = 0; sample <= segmentCount; ++sample) {
        longitudinalParameters.push_back(
            static_cast<float>(sample) / static_cast<float>(segmentCount));
    }
    output.vertices.reserve(longitudinalParameters.size() * 2u);
    output.indices.reserve(
        (longitudinalParameters.size() - 1u) * 6u);
    for (size_t sample = 0; sample < longitudinalParameters.size(); ++sample) {
        const float t = longitudinalParameters[sample];
        const math::vec2 left = startNegative +
            (endNegative - startNegative) * t;
        const math::vec2 right = startPositive +
            (endPositive - startPositive) * t;
        const std::optional<float> columnHeight =
            roadCrossSectionMaximumHeight(terrain, left, right);
        if (!columnHeight) return false;
        const float distanceUv = (startTrim + length * t) /
            (std::max(source.width, 0.25f) * kRoadAtlasWorldScale);
        const float sideUv = halfWidth /
            (std::max(source.width, 0.25f) * kRoadAtlasWorldScale);
        StaticMeshVertex leftVertex;
        leftVertex.position = {
            left.x(), left.y(), *columnHeight + kRoadSurfaceZOffset};
        leftVertex.normal = {0.0f, 0.0f, 1.0f};
        leftVertex.texcoord = {
            kRoadStraightAtlasU + distanceUv,
            kRoadStraightAtlasV + sideUv,
        };
        leftVertex.color = evaluateTerrainVertexColorCpu(
            terrain, {0.0f, 0.0f, 1.0f}, leftVertex.position, 1.0f);
        StaticMeshVertex rightVertex = leftVertex;
        rightVertex.position = {
            right.x(), right.y(), *columnHeight + kRoadSurfaceZOffset};
        rightVertex.texcoord = {
            kRoadStraightAtlasU + distanceUv,
            kRoadStraightAtlasV - sideUv,
        };
        rightVertex.color = evaluateTerrainVertexColorCpu(
            terrain, {0.0f, 0.0f, 1.0f}, rightVertex.position, 1.0f);
        output.vertices.push_back(leftVertex);
        output.vertices.push_back(rightVertex);
        if (sample + 1u == longitudinalParameters.size()) continue;
        const uint32_t base = static_cast<uint32_t>(sample * 2u);
        output.indices.insert(output.indices.end(), {
            base, base + 2u, base + 3u,
            base, base + 3u, base + 1u,
        });
    }
    return !output.indices.empty();
}

bool appendAuthoredRoadQuad(
    const TerrainRenderSnapshot& terrain,
    const container::Array<math::vec2, 4>& corners,
    const math::vec2& uvOrigin,
    const math::vec2& uvForward,
    const math::vec2& uvNormal,
    float atlasU,
    float atlasV,
    float uvUScale,
    float uvVScale,
    float alpha,
    TerrainRoadMeshCpu& output) {
    using namespace game::road_surface;
    if (!std::isfinite(uvUScale) || uvUScale <= 0.0f ||
        !std::isfinite(uvVScale) || uvVScale <= 0.0f) {
        return false;
    }
    const float bottomLength = (corners[1] - corners[0]).length();
    const float topLength = (corners[2] - corners[3]).length();
    const float maximumLength = std::max(bottomLength, topLength);
    if (!std::isfinite(maximumLength) || maximumLength <= math::EPSILON) {
        return false;
    }
    const float sampleDistance = std::max(
        terrain.cellWorldSize * kRoadMeshSampleDistanceInCells, 1.0f);
    const uint32_t segmentCount = std::clamp<uint32_t>(
        static_cast<uint32_t>(std::ceil(maximumLength / sampleDistance)),
        1u, kRoadMeshMaximumSubdivisions);
    container::Vector<float> parameters;
    parameters.reserve(segmentCount + 1u);
    for (uint32_t sample = 0u; sample <= segmentCount; ++sample) {
        parameters.push_back(
            static_cast<float>(sample) / static_cast<float>(segmentCount));
    }

    const uint32_t firstVertex =
        static_cast<uint32_t>(output.vertices.size());
    const size_t firstIndex = output.indices.size();
    const float inverseU = 1.0f /
        (uvUScale * kRoadAtlasWorldScale);
    const float inverseV = 1.0f /
        (uvVScale * kRoadAtlasWorldScale);
    for (size_t column = 0u; column < parameters.size(); ++column) {
        const float parameter = parameters[column];
        const math::vec2 bottom =
            corners[0] + (corners[1] - corners[0]) * parameter;
        const math::vec2 top =
            corners[3] + (corners[2] - corners[3]) * parameter;
        const std::optional<float> height =
            roadCrossSectionMaximumHeight(terrain, bottom, top);
        if (!height) {
            output.vertices.resize(firstVertex);
            output.indices.resize(firstIndex);
            return false;
        }
        for (const math::vec2& point : {bottom, top}) {
            const math::vec2 local = point - uvOrigin;
            StaticMeshVertex vertex;
            vertex.position = {
                point.x(), point.y(), *height + kRoadSurfaceZOffset,
            };
            vertex.normal = {0.0f, 0.0f, 1.0f};
            vertex.texcoord = {
                atlasU + local.dot(uvForward) * inverseU,
                atlasV - local.dot(uvNormal) * inverseV,
            };
            vertex.color = evaluateTerrainVertexColorCpu(
                terrain, vertex.normal, vertex.position, alpha);
            output.vertices.push_back(vertex);
        }
        if (column == 0u) continue;
        const uint32_t base = firstVertex +
            static_cast<uint32_t>((column - 1u) * 2u);
        output.indices.insert(output.indices.end(), {
            base, base + 2u, base + 3u,
            base, base + 3u, base + 1u,
        });
    }
    return output.vertices.size() > firstVertex;
}

bool appendRoadJunctionMesh(const TerrainRenderSnapshot& terrain,
                            const TerrainRoadRenderSegment& owner,
                            const RoadJunctionPlan& junction,
                            TerrainRoadMeshCpu& output) {
    using namespace game::road_surface;
    struct BoundaryPoint final {
        math::vec2 position{};
        float angle = 0.0f;
    };

    if (junction.arms.size() < kRoadMinimumJunctionDegree ||
        !std::isfinite(junction.trimRadius) || junction.trimRadius <= 0.0f) {
        if (junction.kind == RoadJunctionKind::Generic) return false;
    }
    if (junction.kind != RoadJunctionKind::Generic) {
        return appendAuthoredRoadQuad(
            terrain, junction.authoredCorners, junction.center,
            junction.authoredForward, junction.authoredNormal,
            junction.authoredAtlasU, junction.authoredAtlasV,
            junction.authoredUvScale, junction.authoredUvScale,
            1.0f, output);
    }
    container::Vector<BoundaryPoint> boundary;
    boundary.reserve(junction.arms.size() * 2u);
    for (const RoadJunctionArm& arm : junction.arms) {
        const math::vec2 side{-arm.direction.y(), arm.direction.x()};
        const math::vec2 endCenter = junction.center +
            arm.direction * junction.trimRadius;
        for (float sign : {-1.0f, 1.0f}) {
            const math::vec2 position = endCenter + side * (arm.halfWidth * sign);
            const math::vec2 local = position - junction.center;
            boundary.push_back({
                .position = position,
                .angle = std::atan2(local.y(), local.x()),
            });
        }
    }
    std::sort(boundary.begin(), boundary.end(),
              [](const BoundaryPoint& left, const BoundaryPoint& right) {
                  return left.angle < right.angle;
              });

    const std::optional<float> centerHeight = terrainCellMaximumHeight(
        terrain, junction.center.x(), junction.center.y());
    if (!centerHeight) return false;
    const uint32_t centerIndex = static_cast<uint32_t>(output.vertices.size());
    float atlasU = kRoadYAtlasU;
    float atlasV = kRoadYAtlasV;
    math::vec2 uvForward = junction.arms.front().direction;
    float mostOppositeDot = 1.0f;
    for (size_t first = 0; first < junction.arms.size(); ++first) {
        for (size_t second = first + 1u; second < junction.arms.size(); ++second) {
            const float dot = junction.arms[first].direction.dot(
                junction.arms[second].direction);
            if (dot < mostOppositeDot) {
                mostOppositeDot = dot;
                uvForward = (junction.arms[first].direction -
                    junction.arms[second].direction).normalized();
            }
        }
    }
    if (junction.arms.size() == 4u) {
        atlasU = kRoadFourWayAtlasU;
        atlasV = kRoadFourWayAtlasV;
    } else if (junction.arms.size() == 3u &&
               mostOppositeDot <= kRoadStraightPairDot) {
        atlasU = kRoadTeeAtlasU;
        atlasV = kRoadTeeAtlasV;
    }
    const float uvScale = 1.0f /
        (std::max(owner.width, 0.25f) * kRoadJunctionAtlasWorldScale);
    const math::vec2 uvSide{-uvForward.y(), uvForward.x()};

    StaticMeshVertex centerVertex;
    centerVertex.position = {junction.center.x(), junction.center.y(),
                             *centerHeight + kRoadSurfaceZOffset};
    centerVertex.normal = {0.0f, 0.0f, 1.0f};
    centerVertex.texcoord = {atlasU, atlasV};
    centerVertex.color = evaluateTerrainVertexColorCpu(
        terrain, centerVertex.normal, centerVertex.position, 1.0f);
    output.vertices.push_back(centerVertex);

    for (const BoundaryPoint& point : boundary) {
        const std::optional<float> height = terrainCellMaximumHeight(
            terrain, point.position.x(), point.position.y());
        if (!height) {
            output.vertices.resize(centerIndex);
            return false;
        }
        const math::vec2 local = point.position - junction.center;
        StaticMeshVertex vertex = centerVertex;
        vertex.position = {point.position.x(), point.position.y(),
                           *height + kRoadSurfaceZOffset};
        vertex.texcoord = {
            atlasU + local.dot(uvSide) * uvScale,
            atlasV - local.dot(uvForward) * uvScale,
        };
        vertex.color = evaluateTerrainVertexColorCpu(
            terrain, vertex.normal, vertex.position, 1.0f);
        output.vertices.push_back(vertex);
    }
    for (uint32_t index = 0; index < static_cast<uint32_t>(boundary.size()); ++index) {
        const uint32_t current = centerIndex + 1u + index;
        const uint32_t next = centerIndex + 1u +
            ((index + 1u) % static_cast<uint32_t>(boundary.size()));
        output.indices.insert(output.indices.end(), {centerIndex, current, next});
    }
    return true;
}

bool appendRoadCornerMesh(const TerrainRenderSnapshot& terrain,
                          const TerrainRoadRenderSegment& owner,
                          const RoadCornerPlan& corner,
                          TerrainRoadMeshCpu& output) {
    using namespace game::road_surface;
    if (!std::isfinite(corner.trimRadius) ||
        corner.trimRadius <= 0.0f) return false;
    const math::vec2 start = corner.center +
        corner.arms[0].direction * corner.trimRadius;
    const math::vec2 end = corner.center +
        corner.arms[1].direction * corner.trimRadius;
    const float outwardDot = std::clamp(
        corner.arms[0].direction.dot(corner.arms[1].direction),
        -1.0f, 1.0f);
    const float turnAngle = std::acos(std::clamp(
        -outwardDot, -1.0f, 1.0f));
    if (corner.miter) {
        const math::vec2 firstNormal{
            -corner.arms[0].direction.y(),
            corner.arms[0].direction.x()};
        const math::vec2 secondNormal{
            -corner.arms[1].direction.y(),
            corner.arms[1].direction.x()};
        const std::optional<math::vec2> miterMinus =
            intersectRoadOffsetLines(
                corner.center - firstNormal * corner.arms[0].halfWidth,
                corner.arms[0].direction,
                corner.center - secondNormal * corner.arms[1].halfWidth,
                corner.arms[1].direction);
        const std::optional<math::vec2> miterPlus =
            intersectRoadOffsetLines(
                corner.center + firstNormal * corner.arms[0].halfWidth,
                corner.arms[0].direction,
                corner.center + secondNormal * corner.arms[1].halfWidth,
                corner.arms[1].direction);
        if (!miterMinus || !miterPlus) return false;

        const uint32_t firstVertex =
            static_cast<uint32_t>(output.vertices.size());
        const size_t firstIndex = output.indices.size();
        const float uvScale = 1.0f /
            (std::max(owner.width, 0.25f) * kRoadAtlasWorldScale);
        const auto appendMiterHalf = [&](const RoadJunctionArm& arm) {
            const math::vec2 normal{-arm.direction.y(), arm.direction.x()};
            const math::vec2 armCenter =
                corner.center + arm.direction * corner.trimRadius;
            const math::vec2 armMinus = armCenter - normal * arm.halfWidth;
            const math::vec2 armPlus = armCenter + normal * arm.halfWidth;
            const std::optional<float> armHeight =
                roadCrossSectionMaximumHeight(terrain, armMinus, armPlus);
            const std::optional<float> miterHeight =
                roadCrossSectionMaximumHeight(
                    terrain, *miterMinus, *miterPlus);
            if (!armHeight || !miterHeight) return false;
            const container::Array<math::vec2, 4> points{{
                armMinus, *miterMinus, *miterPlus, armPlus,
            }};
            const container::Array<float, 4> heights{{
                *armHeight, *miterHeight, *miterHeight, *armHeight,
            }};
            const uint32_t base =
                static_cast<uint32_t>(output.vertices.size());
            for (size_t pointIndex = 0; pointIndex < points.size(); ++pointIndex) {
                const math::vec2 local = points[pointIndex] - corner.center;
                StaticMeshVertex vertex;
                vertex.position = {
                    points[pointIndex].x(), points[pointIndex].y(),
                    heights[pointIndex] + kRoadSurfaceZOffset,
                };
                vertex.normal = {0.0f, 0.0f, 1.0f};
                vertex.texcoord = {
                    kRoadStraightAtlasU + local.dot(arm.direction) * uvScale,
                    kRoadStraightAtlasV - local.dot(normal) * uvScale,
                };
                vertex.color = evaluateTerrainVertexColorCpu(
                    terrain, vertex.normal, vertex.position, 1.0f);
                output.vertices.push_back(vertex);
            }
            const float winding = roadVectorCross(
                points[1] - points[0], points[2] - points[0]);
            if (winding >= 0.0f) {
                output.indices.insert(output.indices.end(), {
                    base, base + 1u, base + 2u,
                    base, base + 2u, base + 3u,
                });
            } else {
                output.indices.insert(output.indices.end(), {
                    base, base + 2u, base + 1u,
                    base, base + 3u, base + 2u,
                });
            }
            return true;
        };
        if (!appendMiterHalf(corner.arms[0]) ||
            !appendMiterHalf(corner.arms[1])) {
            output.vertices.resize(firstVertex);
            output.indices.resize(firstIndex);
            return false;
        }
        return true;
    }
    if (!corner.miter) {
        const math::vec2 bisector =
            corner.arms[0].direction + corner.arms[1].direction;
        const float bisectorLength = bisector.length();
        const float cosineHalfTurn = std::cos(turnAngle * 0.5f);
        if (!std::isfinite(corner.curveRadius) || corner.curveRadius <= 0.0f ||
            !std::isfinite(bisectorLength) ||
            bisectorLength <= math::EPSILON ||
            !std::isfinite(cosineHalfTurn) ||
            cosineHalfTurn <= math::EPSILON) {
            return false;
        }
        const math::vec2 curveCenter = corner.curveCenter;
        const math::vec2 startVector = start - curveCenter;
        const math::vec2 endVector = end - curveCenter;
        const float signedTurn = std::atan2(
            startVector.x() * endVector.y() -
                startVector.y() * endVector.x(),
            startVector.dot(endVector));
        if (!std::isfinite(signedTurn) ||
            std::abs(signedTurn) <= math::EPSILON) {
            return false;
        }

        container::Vector<math::vec2> centers;
        centers.push_back(start);
        float rotated = 0.0f;
        while (std::abs(signedTurn - rotated) > 1.0e-5f) {
            const float remaining = signedTurn - rotated;
            rotated += std::copysign(
                std::min(std::abs(remaining), kRoadCurveStepRadians),
                remaining);
            const float cosine = std::cos(rotated);
            const float sine = std::sin(rotated);
            const math::vec2 sample{
                startVector.x() * cosine - startVector.y() * sine,
                startVector.x() * sine + startVector.y() * cosine,
            };
            centers.push_back(curveCenter + sample);
        }
        centers.back() = end;

        const uint32_t firstVertex =
            static_cast<uint32_t>(output.vertices.size());
        const size_t firstIndex = output.indices.size();
        const float authoredWidth = std::max(owner.width, 0.25f);
        const float halfWidth = authoredWidth *
            std::clamp(owner.widthInTexture, 0.01f, 32.0f) * 0.5f;
        const float atlasV = corner.tight
            ? kRoadTightCurveAtlasV : kRoadCurveAtlasV;
        for (size_t slice = 0; slice + 1u < centers.size(); ++slice) {
            const math::vec2 loc = centers[slice];
            const math::vec2 chord = centers[slice + 1u] - loc;
            const float chordLength = chord.length();
            if (!std::isfinite(chordLength) ||
                chordLength <= math::EPSILON) {
                output.vertices.resize(firstVertex);
                output.indices.resize(firstIndex);
                return false;
            }
            const math::vec2 direction = chord / chordLength;
            // W3DRoadBuffer::loadCurve uses loc1/loc2 only to obtain the
            // slice direction.  It then deliberately replaces the chord
            // length with the authored RoadWidth before constructing the
            // atlas quad.  The resulting overlap between consecutive
            // 30-degree patches hides their shared edge.  Using the shorter
            // circle chord here leaves a visible hole, most obviously on the
            // high-contrast railroad texture.
            const math::vec2 roadVector = direction * authoredWidth;
            const math::vec2 roadNormal{
                -direction.y() * halfWidth,
                direction.x() * halfWidth,
            };
            math::vec2 bottomLeft = loc - roadNormal;
            math::vec2 bottomRight;
            math::vec2 topRight;
            math::vec2 topLeft;
            if (corner.tight) {
                bottomRight = bottomLeft + roadVector * 0.5f;
                topRight = bottomRight + roadNormal * 2.0f;
                topLeft = bottomLeft + roadNormal * 2.0f;
                bottomRight += roadVector * 0.1f + roadNormal * 0.2f;
                bottomLeft -= roadNormal * 0.1f + roadVector * 0.02f;
                topLeft -= roadVector * 0.02f;
                topRight += -roadVector * 0.4f + roadNormal * 0.2f;
            } else {
                bottomRight = bottomLeft + roadVector;
                topRight = bottomRight + roadNormal * 2.0f;
                topLeft = bottomLeft + roadNormal * 2.0f;
                bottomRight += roadVector * 0.1f + roadNormal * 0.4f;
                bottomLeft -= roadNormal * 0.2f + roadVector * 0.02f;
                topLeft -= roadVector * 0.02f;
                topRight += -roadVector * 0.4f + roadNormal * 0.4f;
            }
            const math::vec2 normal = roadNormal / halfWidth;
            const container::Array<math::vec2, 4> points{{
                bottomLeft, bottomRight, topRight, topLeft,
            }};
            if (!appendAuthoredRoadQuad(
                    terrain, points, loc, direction, normal,
                    kRoadCurveAtlasU, atlasV,
                    authoredWidth, authoredWidth, 1.0f, output)) {
                output.vertices.resize(firstVertex);
                output.indices.resize(firstIndex);
                return false;
            }
        }
        return output.vertices.size() > firstVertex;
    }
    return false;
}

bool appendRoadCrossJoinMesh(const TerrainRenderSnapshot& terrain,
                             const TerrainRoadRenderSegment& owner,
                             const RoadCrossJoinPlan& join,
                             TerrainRoadMeshCpu& output) {
    using namespace game::road_surface;
    math::vec2 joinDirection = join.targetDirection;
    const float directionLength = joinDirection.length();
    const float authoredWidth = std::max(owner.width, 0.25f);
    if (!std::isfinite(directionLength) ||
        directionLength <= math::EPSILON ||
        !std::isfinite(join.ownerHalfWidth) || join.ownerHalfWidth <= 0.0f) {
        return false;
    }
    joinDirection = joinDirection / directionLength;
    const math::vec2 joinSide{-joinDirection.y(), joinDirection.x()};
    const float joinLength =
        authoredWidth * kRoadAlphaJoinLengthInRoadWidths;
    // insertCrossTypeJoins stores the clipped edge length through
    // scaleAdjustment, then loadAlphaJoin divides it by the original
    // RoadWidthInTexture before applying the eight-pixel expansion.
    const float adjustedVScale = join.ownerHalfWidth * 2.0f /
        std::clamp(owner.widthInTexture, 0.01f, 32.0f);
    const float expandedHalfWidth = adjustedVScale *
        kRoadAlphaJoinWidthExpansion * 0.5f;
    const math::vec2 start = join.endpoint - joinDirection *
        (joinLength * kRoadAlphaJoinLongitudinalOffset);
    const math::vec2 end = start + joinDirection * joinLength;
    const container::Array<math::vec2, 4> corners{{
        start - joinSide * expandedHalfWidth,
        end - joinSide * expandedHalfWidth,
        end + joinSide * expandedHalfWidth,
        start + joinSide * expandedHalfWidth,
    }};
    return appendAuthoredRoadQuad(
        terrain, corners, join.endpoint, joinDirection, joinSide,
        kRoadAlphaJoinAtlasU, kRoadAlphaJoinAtlasV,
        authoredWidth, adjustedVScale, 1.0f, output);
}

} // namespace

struct TerrainRoadMeshPlan::Impl final {
    RoadJunctionGraph graph;
};

TerrainRoadMeshPlan::TerrainRoadMeshPlan() = default;
TerrainRoadMeshPlan::~TerrainRoadMeshPlan() = default;
TerrainRoadMeshPlan::TerrainRoadMeshPlan(TerrainRoadMeshPlan&&) noexcept =
    default;
TerrainRoadMeshPlan& TerrainRoadMeshPlan::operator=(
    TerrainRoadMeshPlan&&) noexcept = default;

TerrainRoadMeshPlan buildTerrainRoadMeshPlan(
    const TerrainRenderSnapshot& terrain,
    bool requireTerrainConform) {
    TerrainRoadMeshPlan result;
    result.m_impl = std::make_unique<TerrainRoadMeshPlan::Impl>();
    result.m_impl->graph =
        buildRoadJunctionGraph(terrain, requireTerrainConform);
    return result;
}

namespace {

struct TerrainRoadPlanCacheKey final {
    uint64_t first = 0;
    uint64_t second = 0;
    uint32_t roadCount = 0;
    int32_t width = 0;
    int32_t height = 0;

    [[nodiscard]] bool operator==(
        const TerrainRoadPlanCacheKey&) const noexcept = default;
};

[[nodiscard]] TerrainRoadPlanCacheKey terrainRoadPlanCacheKey(
    const TerrainRenderSnapshot& terrain) noexcept {
    TerrainRoadPlanCacheKey key{
        .first = 14695981039346656037ull,
        .second = 1099511628211ull,
        .roadCount = static_cast<uint32_t>(std::min<size_t>(
            terrain.roads.size(), std::numeric_limits<uint32_t>::max())),
        .width = terrain.width,
        .height = terrain.height,
    };
    const auto mix = [&key](uint64_t value) noexcept {
        key.first ^= value + 0x9E3779B97F4A7C15ull +
            (key.first << 6u) + (key.first >> 2u);
        key.first *= 1099511628211ull;
        key.second ^= value + 0xD6E8FEB86659FD93ull +
            (key.second << 7u) + (key.second >> 3u);
        key.second *= 14029467366897019727ull;
    };
    const auto mixString = [&mix](container::StringView value) noexcept {
        mix(value.size());
        for (const unsigned char byte : value) mix(byte);
    };
    mix(static_cast<uint32_t>(terrain.width));
    mix(static_cast<uint32_t>(terrain.height));
    mix(static_cast<uint32_t>(terrain.borderSize));
    mix(std::bit_cast<uint32_t>(terrain.cellWorldSize));
    mix(std::bit_cast<uint32_t>(terrain.heightWorldScale));
    mix(terrain.roads.size());
    for (const TerrainRoadRenderSegment& road : terrain.roads) {
        mix(road.styleIdentity);
        mixString(road.styleName);
        mixString(road.textureName);
        mix(std::bit_cast<uint32_t>(road.start.x()));
        mix(std::bit_cast<uint32_t>(road.start.y()));
        mix(std::bit_cast<uint32_t>(road.end.x()));
        mix(std::bit_cast<uint32_t>(road.end.y()));
        mix(std::bit_cast<uint32_t>(road.width));
        mix(std::bit_cast<uint32_t>(road.widthInTexture));
        mix(static_cast<uint64_t>(road.startAngled) |
            (static_cast<uint64_t>(road.endAngled) << 1u) |
            (static_cast<uint64_t>(road.startJoin) << 2u) |
            (static_cast<uint64_t>(road.endJoin) << 3u) |
            (static_cast<uint64_t>(road.tightCorner) << 4u));
    }
    return key;
}

struct TerrainRoadPlanCacheEntry final {
    TerrainRoadPlanCacheKey key;
    container::SharedPtr<const TerrainRoadMeshPlan> plan;
    uint64_t lastUse = 0;
};

} // namespace

container::SharedPtr<const TerrainRoadMeshPlan>
findOrBuildCachedTerrainRoadMeshPlan(const TerrainRenderSnapshot& terrain) {
    constexpr size_t kMaximumCachedPlans = 4u;
    static std::mutex cacheMutex;
    static container::Vector<TerrainRoadPlanCacheEntry> cache;
    static uint64_t useSequence = 0;
    const TerrainRoadPlanCacheKey key = terrainRoadPlanCacheKey(terrain);

    std::scoped_lock lock(cacheMutex);
    ++useSequence;
    for (TerrainRoadPlanCacheEntry& entry : cache) {
        if (entry.key == key) {
            entry.lastUse = useSequence;
            return entry.plan;
        }
    }
    auto plan = std::make_shared<const TerrainRoadMeshPlan>(
        buildTerrainRoadMeshPlan(terrain));
    if (cache.size() == kMaximumCachedPlans) {
        const auto oldest = std::min_element(
            cache.begin(), cache.end(), [](const auto& left, const auto& right) {
                return left.lastUse < right.lastUse;
            });
        *oldest = {.key = key, .plan = plan, .lastUse = useSequence};
    } else {
        cache.push_back({.key = key, .plan = plan, .lastUse = useSequence});
    }
    return plan;
}

std::optional<TerrainRoadMeshCpu> buildBasicTerrainRoadMesh(
    const TerrainRenderSnapshot& terrain, size_t roadIndex) {
    if (roadIndex >= terrain.roads.size()) return std::nullopt;
    TerrainRoadMeshCpu output;
    if (!buildRoadMesh(
            terrain, terrain.roads[roadIndex], RoadMeshPlan{}, output)) {
        return std::nullopt;
    }
    return output;
}

std::optional<TerrainRoadMeshCpu> buildTerrainRoadMesh(
    const TerrainRenderSnapshot& terrain,
    const TerrainRoadMeshPlan& plan,
    size_t roadIndex) {
    if (!plan.m_impl || roadIndex >= terrain.roads.size() ||
        roadIndex >= plan.m_impl->graph.roads.size()) {
        return std::nullopt;
    }

    const RoadJunctionGraph& graph = plan.m_impl->graph;
    const TerrainRoadRenderSegment& source = terrain.roads[roadIndex];
    TerrainRoadMeshCpu output;
    if (!buildRoadMesh(
            terrain, source, graph.roads[roadIndex], output)) {
        return std::nullopt;
    }
    for (const size_t index : graph.junctionsByOwner[roadIndex]) {
        appendRoadJunctionMesh(
            terrain, source, graph.junctions[index], output);
    }
    for (const size_t index : graph.cornersByOwner[roadIndex]) {
        appendRoadCornerMesh(
            terrain, source, graph.corners[index], output);
    }
    for (const size_t index : graph.crossJoinsByOwner[roadIndex]) {
        appendRoadCrossJoinMesh(
            terrain, source, graph.crossJoins[index], output);
    }
    output.materialPass = graph.roads[roadIndex].materialPass;
    return output;
}

TerrainRoadJunctionTopology analyzeTerrainRoadJunctions(
    const TerrainRenderSnapshot& terrain) {
    const RoadJunctionGraph graph = buildRoadJunctionGraph(terrain, false);
    TerrainRoadJunctionTopology topology;
    for (const RoadJunctionPlan& junction : graph.junctions) {
        if (junction.arms.size() == 3u) ++topology.threeWayCount;
        else if (junction.arms.size() == 4u) ++topology.fourWayCount;
        else ++topology.multiWayCount;
        topology.joinedEndpointCount += junction.arms.size();
        if (junction.kind == RoadJunctionKind::Generic) {
            topology.patchVertexCount += 1u + junction.arms.size() * 2u;
            topology.patchTriangleCount += junction.arms.size() * 2u;
        } else {
            topology.patchVertexCount += 4u;
            topology.patchTriangleCount += 2u;
        }
    }
    return topology;
}

std::optional<TerrainRoadMeshInspection> inspectTerrainRoadMesh(
    const TerrainRenderSnapshot& terrain,
    size_t roadIndex) {
    if (roadIndex >= terrain.roads.size()) return std::nullopt;

    TerrainRoadMeshPlan plan = buildTerrainRoadMeshPlan(terrain);
    std::optional<TerrainRoadMeshCpu> cpu =
        buildTerrainRoadMesh(terrain, plan, roadIndex);
    if (!cpu) return std::nullopt;

    const RoadJunctionGraph& graph = plan.m_impl->graph;
    TerrainRoadMeshInspection output;
    output.materialPass = cpu->materialPass;
    output.authoredCornerCount = static_cast<size_t>(std::count_if(
        graph.corners.begin(), graph.corners.end(),
        [roadIndex](const RoadCornerPlan& corner) {
            return corner.ownerRoadIndex == roadIndex;
        }));
    output.crossMaterialJoinCount = static_cast<size_t>(std::count_if(
        graph.crossJoins.begin(), graph.crossJoins.end(),
        [roadIndex](const RoadCrossJoinPlan& join) {
            return join.ownerRoadIndex == roadIndex;
        }));
    output.vertices.reserve(cpu->vertices.size());
    for (const StaticMeshVertex& vertex : cpu->vertices) {
        output.vertices.push_back({
            .worldPosition = vertex.position,
            .texcoord = vertex.texcoord,
            .alpha = vertex.color.w(),
        });
    }
    output.indices = std::move(cpu->indices);
    return output;
}

} // namespace engine::render
