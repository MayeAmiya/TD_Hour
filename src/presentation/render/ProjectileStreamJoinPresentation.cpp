#include "core/container/container_types.h"
#include "presentation/render/ProjectileStreamJoinPresentation.h"

#include <algorithm>
#include <cmath>

namespace engine::render {
namespace {

constexpr float kParallelPlaneFactor = 0.9f;
constexpr float kMergeAbortFactor = 1.5f;

struct EyePoint final {
    RenderVector position{};
    container::Array<float, 4> color{};
    float textureV = 0.0f;
};

struct Segment final {
    RenderVector startPlane{};
    container::Array<RenderVector, 2> edgePlane{};
};

struct Intersection final {
    size_t pointCount = 1;
    size_t nextSegmentId = 0;
    RenderVector direction{};
    RenderVector point{};
    container::Array<float, 4> color{};
    float textureV = 0.0f;
    bool fold = false;
    bool parallel = false;
};

struct JoinWorkspace final {
    container::Vector<EyePoint> points;
    container::Vector<Segment> segments;
    container::Array<container::Vector<Intersection>, 2> intersections;
};

JoinWorkspace& joinWorkspace() {
    // Production recording is owner-threaded; pure helper callers can still
    // run elsewhere. Per-thread retained scratch avoids shared mutation and
    // does not introduce a central allocator/cache owner.
    thread_local JoinWorkspace workspace;
    return workspace;
}

[[nodiscard]] bool finiteVector(const RenderVector& value) noexcept {
    return std::isfinite(value.x()) && std::isfinite(value.y()) &&
           std::isfinite(value.z());
}

[[nodiscard]] bool normalizeFinite(RenderVector& value) noexcept {
    const float lengthSquared = value.length_sq();
    if (!finiteVector(value) || !std::isfinite(lengthSquared) ||
        lengthSquared <= math::EPSILON * math::EPSILON) {
        return false;
    }
    value = value / std::sqrt(lengthSquared);
    return finiteVector(value);
}

[[nodiscard]] RenderVector stablePerpendicular(
    const RenderVector& direction) noexcept {
    RenderVector perpendicular = direction.cross({0.0f, 0.0f, 1.0f});
    if (!normalizeFinite(perpendicular)) {
        perpendicular = direction.cross({0.0f, 1.0f, 0.0f});
    }
    if (!normalizeFinite(perpendicular)) return {1.0f, 0.0f, 0.0f};
    return perpendicular;
}

[[nodiscard]] RenderVector directionOnPlane(
    const RenderVector& point, const RenderVector& plane,
    const RenderVector& fallback) noexcept {
    RenderVector direction = point - plane * plane.dot(point);
    if (!normalizeFinite(direction)) direction = fallback;
    if (!normalizeFinite(direction)) direction = {1.0f, 0.0f, 0.0f};
    return direction;
}

[[nodiscard]] RenderVector intersectPlanes(
    const RenderVector& firstPlane, const RenderVector& secondPlane,
    const RenderVector& point, bool& parallel,
    const RenderVector& fallback) noexcept {
    const float planeDot = firstPlane.dot(secondPlane);
    parallel = std::abs(planeDot) >= kParallelPlaneFactor;
    RenderVector direction{};
    if (!parallel) {
        direction = firstPlane.cross(secondPlane);
    } else {
        RenderVector plane = planeDot > 0.0f
            ? firstPlane + secondPlane : firstPlane - secondPlane;
        if (!normalizeFinite(plane)) plane = firstPlane;
        direction = point - plane * plane.dot(point);
    }
    if (!normalizeFinite(direction)) direction = fallback;
    if (!normalizeFinite(direction)) direction = {1.0f, 0.0f, 0.0f};
    if (direction.dot(point) < 0.0f) direction = -direction;
    return direction;
}

[[nodiscard]] ProjectileStreamJoinVertex makeVertex(
    const Intersection& intersection, const RenderVector& sourcePoint,
    float textureU, const RenderVector& cameraPosition) noexcept {
    return {
        .worldPosition = intersection.direction *
                sourcePoint.dot(intersection.direction) +
            cameraPosition,
        .color = intersection.color,
        .textureU = textureU,
        .textureV = intersection.textureV,
    };
}

void appendTriangle(
    ProjectileStreamJoinMesh& output,
    const ProjectileStreamJoinVertex& first,
    const ProjectileStreamJoinVertex& second,
    const ProjectileStreamJoinVertex& third) {
    output.vertices.push_back(first);
    output.vertices.push_back(second);
    output.vertices.push_back(third);
}

} // namespace

ProjectileStreamJoinMesh buildProjectileStreamJoinMesh(
    container::Span<const ProjectileStreamJoinPoint> sourcePoints,
    const RenderVector& cameraPosition, float width) {
    ProjectileStreamJoinMesh output;
    buildProjectileStreamJoinMeshInto(
        output, sourcePoints, cameraPosition, width);
    return output;
}

void buildProjectileStreamJoinMeshInto(
    ProjectileStreamJoinMesh& output,
    container::Span<const ProjectileStreamJoinPoint> sourcePoints,
    const RenderVector& cameraPosition, float width) {
    output.vertices.clear();
    output.logicalSegmentCount = 0;
    output.foldCount = 0;
    output.mergedEdgeIntersectionCount = 0;
    if (sourcePoints.size() < 2 || !finiteVector(cameraPosition) ||
        !std::isfinite(width) || width <= 0.0f) {
        return;
    }

    JoinWorkspace& workspace = joinWorkspace();
    container::Vector<EyePoint>& points = workspace.points;
    points.clear();
    points.reserve(sourcePoints.size());
    for (const ProjectileStreamJoinPoint& source : sourcePoints) {
        if (!finiteVector(source.worldPosition) ||
            !std::isfinite(source.textureV) ||
            !std::all_of(source.color.begin(), source.color.end(),
                         [](float channel) { return std::isfinite(channel); })) {
            output.vertices.clear();
            output.logicalSegmentCount = 0;
            return;
        }
        points.push_back({
            .position = source.worldPosition - cameraPosition,
            .color = source.color,
            .textureV = source.textureV,
        });
    }

    const size_t pointCount = points.size();
    const size_t realSegmentCount = pointCount - 1u;
    output.logicalSegmentCount = static_cast<uint32_t>(realSegmentCount);
    const float radius = width * 0.5f;
    container::Vector<Segment>& segments = workspace.segments;
    segments.assign(pointCount + 1u, Segment{});
    auto& intersections = workspace.intersections;
    for (auto& edge : intersections) {
        edge.assign(pointCount, Intersection{});
    }

    bool switchEdges = false;
    for (size_t segmentIndex = 1; segmentIndex < pointCount; ++segmentIndex) {
        RenderVector& current = points[segmentIndex - 1u].position;
        RenderVector& next = points[segmentIndex].position;
        RenderVector segmentDirection = next - current;
        if (!normalizeFinite(segmentDirection)) {
            // RefCode perturbs equal adjacent points by +0.001 eye-space X.
            next[0] += 0.001f;
            segmentDirection = next - current;
            if (!normalizeFinite(segmentDirection)) {
                output.vertices.clear();
                output.logicalSegmentCount = 0;
                output.foldCount = 0;
                output.mergedEdgeIntersectionCount = 0;
                return;
            }
        }
        segments[segmentIndex].startPlane = segmentDirection;

        const RenderVector nearest = current -
            segmentDirection * segmentDirection.dot(current);
        RenderVector offset = segmentDirection.cross(nearest);
        if (!normalizeFinite(offset)) offset = stablePerpendicular(segmentDirection);
        const RenderVector top = current + offset * radius;
        const RenderVector bottom = current - offset * radius;
        RenderVector topNormal = top.cross(segmentDirection);
        RenderVector bottomNormal = segmentDirection.cross(bottom);
        if (!normalizeFinite(topNormal)) topNormal = offset;
        if (!normalizeFinite(bottomNormal)) bottomNormal = -offset;

        if (segmentIndex > 1u) {
            RenderVector previousPlane =
                points[segmentIndex - 2u].position.cross(current);
            RenderVector currentPlane = current.cross(next);
            const bool validPlanes = normalizeFinite(previousPlane) &&
                normalizeFinite(currentPlane);
            const bool fold = validPlanes &&
                previousPlane.dot(currentPlane) < 0.0f;
            intersections[0][segmentIndex - 1u].fold = fold;
            intersections[1][segmentIndex - 1u].fold = fold;
            if (fold) {
                switchEdges = !switchEdges;
                ++output.foldCount;
            }
        }
        if (switchEdges) {
            segments[segmentIndex].edgePlane[0] = -bottomNormal;
            segments[segmentIndex].edgePlane[1] = -topNormal;
        } else {
            segments[segmentIndex].edgePlane[0] = topNormal;
            segments[segmentIndex].edgePlane[1] = bottomNormal;
        }
    }

    for (size_t edge = 0; edge < 2; ++edge) {
        Intersection& first = intersections[edge].front();
        first.nextSegmentId = 1;
        first.point = points.front().position;
        first.color = points.front().color;
        first.textureV = points.front().textureV;
        first.fold = true;
        first.direction = directionOnPlane(
            first.point, segments[1].edgePlane[edge],
            stablePerpendicular(segments[1].startPlane));

        Intersection& last = intersections[edge].back();
        last.nextSegmentId = realSegmentCount + 1u;
        last.point = points.back().position;
        last.color = points.back().color;
        last.textureV = points.back().textureV;
        last.fold = true;
        last.direction = directionOnPlane(
            last.point, segments[realSegmentCount].edgePlane[edge],
            stablePerpendicular(segments[realSegmentCount].startPlane));
    }

    RenderVector startPlane = intersections[0].front().direction.cross(
        intersections[1].front().direction);
    if (!normalizeFinite(startPlane)) startPlane = segments[1].startPlane;
    if (segments[1].startPlane.dot(startPlane) <= 0.0f) startPlane = -startPlane;
    segments[0].startPlane = startPlane;
    segments[0].edgePlane[0] = startPlane;
    segments[0].edgePlane[1] = startPlane;
    segments[1].startPlane = startPlane;

    RenderVector endPlane = intersections[0].back().direction.cross(
        intersections[1].back().direction);
    if (!normalizeFinite(endPlane)) {
        endPlane = segments[realSegmentCount].startPlane;
    }
    if (segments[realSegmentCount].startPlane.dot(endPlane) <= 0.0f) {
        endPlane = -endPlane;
    }
    segments[realSegmentCount + 1u].startPlane = endPlane;
    segments[realSegmentCount + 1u].edgePlane[0] = endPlane;
    segments[realSegmentCount + 1u].edgePlane[1] = endPlane;

    for (size_t pointIndex = 1; pointIndex + 1u < pointCount; ++pointIndex) {
        for (size_t edge = 0; edge < 2; ++edge) {
            Intersection& intersection = intersections[edge][pointIndex];
            intersection.nextSegmentId = pointIndex + 1u;
            intersection.point = points[pointIndex].position;
            intersection.color = points[pointIndex].color;
            intersection.textureV = points[pointIndex].textureV;
            intersection.direction = intersectPlanes(
                segments[pointIndex].edgePlane[edge],
                segments[pointIndex + 1u].edgePlane[edge],
                intersection.point, intersection.parallel,
                intersections[edge][pointIndex - 1u].direction);
        }
        RenderVector plane = intersections[0][pointIndex].direction.cross(
            intersections[1][pointIndex].direction);
        if (!normalizeFinite(plane)) {
            plane = segments[pointIndex + 1u].startPlane;
        }
        if (segments[pointIndex + 1u].startPlane.dot(plane) <= 0.0f) {
            plane = -plane;
        }
        segments[pointIndex + 1u].startPlane = plane;
    }

    for (size_t edge = 0; edge < 2; ++edge) {
        bool merged = true;
        while (merged) {
            merged = false;
            auto& edgeIntersections = intersections[edge];
            for (size_t index = 0; index + 1u < edgeIntersections.size();) {
                Intersection& current = edgeIntersections[index];
                Intersection& next = edgeIntersections[index + 1u];
                const size_t previousSegmentId = index == 0
                    ? 0u : edgeIntersections[index - 1u].nextSegmentId;
                const Segment& previousSegment = segments[previousSegmentId];
                const Segment& currentSegment = segments[current.nextSegmentId];
                const Segment& nextSegment = segments[next.nextSegmentId];
                const bool overlapsNext = !next.fold &&
                    current.direction.dot(nextSegment.startPlane) > 0.0f &&
                    current.direction.dot(nextSegment.edgePlane[edge]) > 0.0f;
                const bool overlapsPrevious = !current.fold &&
                    next.direction.dot(-currentSegment.startPlane) > 0.0f &&
                    next.direction.dot(previousSegment.edgePlane[edge]) > 0.0f;
                if (!overlapsNext && !overlapsPrevious) {
                    ++index;
                    continue;
                }

                const size_t newCount = current.pointCount + next.pointCount;
                const float currentWeight = static_cast<float>(current.pointCount) /
                    static_cast<float>(newCount);
                const float nextWeight = static_cast<float>(next.pointCount) /
                    static_cast<float>(newCount);
                const RenderVector newPoint = current.point * currentWeight +
                    next.point * nextWeight;
                container::Array<float, 4> newColor{};
                for (size_t channel = 0; channel < newColor.size(); ++channel) {
                    newColor[channel] = current.color[channel] * currentWeight +
                        next.color[channel] * nextWeight;
                }
                const float newTextureV = current.textureV * currentWeight +
                    next.textureV * nextWeight;
                const float planeDot = previousSegment.edgePlane[edge].dot(
                    nextSegment.edgePlane[edge]);
                const bool newParallel =
                    std::abs(planeDot) >= kParallelPlaneFactor;
                RenderVector newDirection{};
                if (!newParallel) {
                    newDirection = previousSegment.edgePlane[edge].cross(
                        nextSegment.edgePlane[edge]);
                } else {
                    RenderVector averagePlane = planeDot > 0.0f
                        ? previousSegment.edgePlane[edge] +
                              nextSegment.edgePlane[edge]
                        : previousSegment.edgePlane[edge] -
                              nextSegment.edgePlane[edge];
                    if (!normalizeFinite(averagePlane)) {
                        averagePlane = previousSegment.edgePlane[edge];
                    }
                    // RefCode's intended parallel branches either project
                    // the existing intersection direction onto the average
                    // plane, or intersect that plane with the skipped edge.
                    // Use current.direction explicitly instead of preserving
                    // the original uninitialized-local typo.
                    newDirection = current.parallel
                        ? current.direction - averagePlane *
                              averagePlane.dot(current.direction)
                        : currentSegment.edgePlane[edge].cross(averagePlane);
                }
                if (!normalizeFinite(newDirection)) {
                    newDirection = current.direction;
                }
                if (!normalizeFinite(newDirection)) {
                    newDirection = {1.0f, 0.0f, 0.0f};
                }
                if (newDirection.dot(newPoint) < 0.0f) {
                    newDirection = -newDirection;
                }

                const float abortDistance = radius * kMergeAbortFactor;
                const float abortDistanceSquared = abortDistance * abortDistance;
                const RenderVector currentOffset = current.point - newDirection *
                    current.point.dot(newDirection);
                const RenderVector nextOffset = next.point - newDirection *
                    next.point.dot(newDirection);
                if (currentOffset.length_sq() > abortDistanceSquared ||
                    nextOffset.length_sq() > abortDistanceSquared) {
                    ++index;
                    continue;
                }

                current.direction = newDirection;
                current.parallel = newParallel;
                current.point = newPoint;
                current.color = newColor;
                current.textureV = newTextureV;
                current.pointCount = newCount;
                current.nextSegmentId = next.nextSegmentId;
                current.fold = current.fold || next.fold;
                edgeIntersections.erase(edgeIntersections.begin() + index + 1u);
                ++output.mergedEdgeIntersectionCount;
                merged = true;
            }
        }
    }

    size_t topIndex = 0;
    size_t bottomIndex = 0;
    size_t pointIndex = 0;
    size_t residualTop = intersections[0][0].pointCount;
    size_t residualBottom = intersections[1][0].pointCount;
    size_t delta = std::min(residualTop, residualBottom) - 1u;
    residualTop -= delta;
    residualBottom -= delta;
    pointIndex += delta;
    ProjectileStreamJoinVertex lastTop = makeVertex(
        intersections[0][0], points[0].position, 0.0f,
        cameraPosition);
    ProjectileStreamJoinVertex lastBottom = makeVertex(
        intersections[1][0], points[0].position, 1.0f,
        cameraPosition);

    for (;;) {
        if (residualTop == 1u && residualBottom == 1u) {
            ++topIndex;
            ++bottomIndex;
            ++pointIndex;
            if (topIndex >= intersections[0].size() ||
                bottomIndex >= intersections[1].size() ||
                pointIndex >= pointCount) {
                break;
            }
            residualTop = intersections[0][topIndex].pointCount;
            residualBottom = intersections[1][bottomIndex].pointCount;
            const ProjectileStreamJoinVertex nextTop = makeVertex(
                intersections[0][topIndex], points[pointIndex].position, 0.0f,
                cameraPosition);
            const ProjectileStreamJoinVertex nextBottom = makeVertex(
                intersections[1][bottomIndex], points[pointIndex].position, 1.0f,
                cameraPosition);
            appendTriangle(output, lastTop, lastBottom, nextTop);
            appendTriangle(output, lastBottom, nextBottom, nextTop);
            lastTop = nextTop;
            lastBottom = nextBottom;
        } else if (residualTop > 1u) {
            --residualTop;
            ++bottomIndex;
            ++pointIndex;
            if (bottomIndex >= intersections[1].size() ||
                pointIndex >= pointCount) break;
            residualBottom = intersections[1][bottomIndex].pointCount;
            const ProjectileStreamJoinVertex nextBottom = makeVertex(
                intersections[1][bottomIndex], points[pointIndex].position, 1.0f,
                cameraPosition);
            appendTriangle(output, lastTop, lastBottom, nextBottom);
            lastBottom = nextBottom;
        } else {
            --residualBottom;
            ++topIndex;
            ++pointIndex;
            if (topIndex >= intersections[0].size() ||
                pointIndex >= pointCount) break;
            residualTop = intersections[0][topIndex].pointCount;
            const ProjectileStreamJoinVertex nextTop = makeVertex(
                intersections[0][topIndex], points[pointIndex].position, 0.0f,
                cameraPosition);
            appendTriangle(output, lastTop, lastBottom, nextTop);
            lastTop = nextTop;
        }

        delta = std::min(residualTop, residualBottom) - 1u;
        residualTop -= delta;
        residualBottom -= delta;
        pointIndex += delta;
        const bool topComplete = topIndex + 1u >= intersections[0].size() &&
            residualTop == 1u;
        const bool bottomComplete = bottomIndex + 1u >= intersections[1].size() &&
            residualBottom == 1u;
        if (topComplete || bottomComplete) break;
    }
}

bool projectileStreamOwnerVisible(
    LocalVisibilityRenderCellState ownerAnchorState,
    bool localVisibilityValid,
    bool visibilityExempt) noexcept {
    return visibilityExempt || !localVisibilityValid ||
        ownerAnchorState == LocalVisibilityRenderCellState::Visible;
}

ProjectileStreamExtractionPolicy projectileStreamExtractionPolicy(
    bool projectileVisible, bool streamAuthored,
    bool ownerAnchorVisible) noexcept {
    const bool trailEnabled = streamAuthored && ownerAnchorVisible;
    return {
        .retainProjectileSnapshot = projectileVisible || trailEnabled,
        .trailEnabled = trailEnabled,
        .shadowEnabled = projectileVisible,
    };
}

uint32_t projectileStreamLogicFramesPerSecond(
    int sessionFramesPerSecond) noexcept {
    return static_cast<uint32_t>(std::max(1, sessionFramesPerSecond));
}

} // namespace engine::render
