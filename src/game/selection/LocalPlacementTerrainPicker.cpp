#include "game/selection/LocalPlacementTerrainPicker.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::selection {
namespace {

struct Ray final {
    math::vec3 origin{};
    math::vec3 direction{};
    float minimum = 0.0f;
    float maximum = 0.0f;
};

[[nodiscard]] std::optional<float> rayTriangle(
    const Ray& ray, math::vec3 a, math::vec3 b, math::vec3 c) noexcept {
    const math::vec3 edge1 = b - a;
    const math::vec3 edge2 = c - a;
    const math::vec3 p = ray.direction.cross(edge2);
    const float determinant = edge1.dot(p);
    constexpr float epsilon = 1.0e-6f;
    if (!std::isfinite(determinant) || std::abs(determinant) <= epsilon) {
        return std::nullopt;
    }
    const float inverse = 1.0f / determinant;
    const math::vec3 translated = ray.origin - a;
    const float u = translated.dot(p) * inverse;
    if (!std::isfinite(u) || u < -epsilon || u > 1.0f + epsilon) {
        return std::nullopt;
    }
    const math::vec3 q = translated.cross(edge1);
    const float v = ray.direction.dot(q) * inverse;
    if (!std::isfinite(v) || v < -epsilon || u + v > 1.0f + epsilon) {
        return std::nullopt;
    }
    const float distance = edge2.dot(q) * inverse;
    if (!std::isfinite(distance) || distance < ray.minimum - epsilon ||
        distance > ray.maximum + epsilon) {
        return std::nullopt;
    }
    return std::clamp(distance, ray.minimum, ray.maximum);
}

[[nodiscard]] bool clipAxis(
    float origin, float direction, float minimum, float maximum,
    float& enter, float& leave) noexcept {
    if (std::abs(direction) <= math::EPSILON) {
        return origin >= minimum && origin <= maximum;
    }
    float first = (minimum - origin) / direction;
    float second = (maximum - origin) / direction;
    if (first > second) std::swap(first, second);
    enter = std::max(enter, first);
    leave = std::min(leave, second);
    return enter <= leave;
}

[[nodiscard]] std::optional<Ray> makeRay(
    const GameCameraState& source, LocalPlacementViewport viewport,
    float screenX, float screenY) noexcept {
    if (viewport.width == 0 || viewport.height == 0 ||
        !std::isfinite(screenX) || !std::isfinite(screenY)) {
        return std::nullopt;
    }
    const GameCameraState camera = source.sanitized();
    const float tacticalHeight = std::max(
        1.0f, std::round(static_cast<float>(viewport.height) *
                         camera.tacticalViewportHeightScale));
    if (screenX < 0.0f || screenY < 0.0f ||
        screenX >= static_cast<float>(viewport.width) ||
        screenY >= tacticalHeight) {
        return std::nullopt;
    }

    const math::vec3 forward = (camera.target - camera.position).normalized();
    math::vec3 right = forward.cross(camera.up).normalized();
    if (right.length_sq() <= math::EPSILON * math::EPSILON) {
        return std::nullopt;
    }
    const math::vec3 up = right.cross(forward).normalized();
    const float aspect = static_cast<float>(viewport.width) / tacticalHeight;
    float tangentHalfVertical = std::tan(camera.verticalFovRadians * 0.5f);
    float tangentHalfHorizontal = tangentHalfVertical * aspect;
    if (camera.horizontalFovRadians > 0.0f) {
        tangentHalfHorizontal = std::tan(camera.horizontalFovRadians * 0.5f);
        tangentHalfVertical = tangentHalfHorizontal / aspect;
    }
    const float ndcX = screenX / static_cast<float>(viewport.width) * 2.0f - 1.0f;
    const float ndcY = 1.0f - screenY / tacticalHeight * 2.0f;
    const math::vec3 direction =
        (forward + right * (ndcX * tangentHalfHorizontal) +
         up * (ndcY * tangentHalfVertical)).normalized();
    if (direction.length_sq() <= math::EPSILON * math::EPSILON) {
        return std::nullopt;
    }
    return Ray{
        .origin = camera.position,
        .direction = direction,
        .minimum = camera.nearClip,
        .maximum = camera.farClip,
    };
}

} // namespace

std::optional<render::RenderVector> localPlacementScreenToTerrain(
    const GameCameraState& camera,
    const game::terrain::TerrainHeightfieldData& terrain,
    LocalPlacementViewport viewport,
    float screenX,
    float screenY) noexcept {
    const std::optional<Ray> rayValue =
        makeRay(camera, viewport, screenX, screenY);
    size_t expectedSamples = 0;
    if (terrain.width > 0 && terrain.height > 0 &&
        static_cast<size_t>(terrain.width) <=
            std::numeric_limits<size_t>::max() /
                static_cast<size_t>(terrain.height)) {
        expectedSamples = static_cast<size_t>(terrain.width) *
            static_cast<size_t>(terrain.height);
    }
    if (!rayValue || terrain.width < 2 || terrain.height < 2 ||
        terrain.heights.size() != expectedSamples) {
        return std::nullopt;
    }
    const Ray& ray = *rayValue;
    const game::terrain::TerrainHeightfieldData& heightfield = terrain;
    const float cell = game::terrain::kMapCellWorldSize;
    const float minimumX =
        -static_cast<float>(heightfield.borderSize) * cell;
    const float minimumY =
        -static_cast<float>(heightfield.borderSize) * cell;
    const float maximumX = minimumX +
        static_cast<float>(heightfield.width - 1) * cell;
    const float maximumY = minimumY +
        static_cast<float>(heightfield.height - 1) * cell;
    float enter = ray.minimum;
    float leave = ray.maximum;
    if (!clipAxis(ray.origin.x(), ray.direction.x(), minimumX, maximumX,
                  enter, leave) ||
        !clipAxis(ray.origin.y(), ray.direction.y(), minimumY, maximumY,
                  enter, leave)) {
        return std::nullopt;
    }
    if (leave < ray.minimum || enter > ray.maximum) return std::nullopt;
    enter = std::max(enter, ray.minimum);
    leave = std::min(leave, ray.maximum);

    constexpr float nudge = 1.0e-4f;
    const float initialDistance = std::min(leave, enter + nudge);
    const math::vec3 initial = ray.origin + ray.direction * initialDistance;
    int32_t cellX = std::clamp(
        static_cast<int32_t>(std::floor(
            (initial.x() - minimumX) / cell)),
        0, heightfield.width - 2);
    int32_t cellY = std::clamp(
        static_cast<int32_t>(std::floor(
            (initial.y() - minimumY) / cell)),
        0, heightfield.height - 2);
    const int32_t stepX = ray.direction.x() > math::EPSILON ? 1
        : ray.direction.x() < -math::EPSILON ? -1 : 0;
    const int32_t stepY = ray.direction.y() > math::EPSILON ? 1
        : ray.direction.y() < -math::EPSILON ? -1 : 0;
    const float infinity = std::numeric_limits<float>::infinity();
    const float deltaX = stepX != 0
        ? cell / std::abs(ray.direction.x()) : infinity;
    const float deltaY = stepY != 0
        ? cell / std::abs(ray.direction.y()) : infinity;
    const auto nextBoundaryDistance = [&](int32_t coordinate, int32_t step,
                                          float origin, float direction,
                                          float minimum) noexcept {
        if (step == 0) return infinity;
        const float boundary = minimum + static_cast<float>(
            coordinate + (step > 0 ? 1 : 0)) * cell;
        return (boundary - origin) / direction;
    };
    float nextX = nextBoundaryDistance(
        cellX, stepX, ray.origin.x(), ray.direction.x(), minimumX);
    float nextY = nextBoundaryDistance(
        cellY, stepY, ray.origin.y(), ray.direction.y(), minimumY);

    const size_t width = static_cast<size_t>(heightfield.width);
    const auto point = [&](int32_t x, int32_t y) noexcept {
        const size_t index = static_cast<size_t>(y) * width +
            static_cast<size_t>(x);
        return math::vec3{
            minimumX + static_cast<float>(x) * cell,
            minimumY + static_cast<float>(y) * cell,
            static_cast<float>(heightfield.heights[index]) *
                game::terrain::kMapHeightWorldScale,
        };
    };
    const size_t maximumSteps = static_cast<size_t>(heightfield.width) +
        static_cast<size_t>(heightfield.height) + 4u;
    for (size_t step = 0; step < maximumSteps; ++step) {
        const math::vec3 p0 = point(cellX, cellY);
        const math::vec3 p1 = point(cellX + 1, cellY);
        const math::vec3 p2 = point(cellX + 1, cellY + 1);
        const math::vec3 p3 = point(cellX, cellY + 1);
        std::optional<float> hit = rayTriangle(ray, p0, p1, p2);
        const std::optional<float> other = rayTriangle(ray, p0, p2, p3);
        if (other && (!hit || *other < *hit)) hit = other;
        const float cellLeave = std::min({nextX, nextY, leave});
        if (hit && *hit >= enter - nudge && *hit <= cellLeave + nudge) {
            return ray.origin + ray.direction * *hit;
        }
        if (cellLeave >= leave) break;
        const bool crossesX = nextX <= nextY + nudge;
        const bool crossesY = nextY <= nextX + nudge;
        if (crossesX) {
            cellX += stepX;
            nextX += deltaX;
        }
        if (crossesY) {
            cellY += stepY;
            nextY += deltaY;
        }
        if (cellX < 0 || cellY < 0 ||
            cellX >= heightfield.width - 1 ||
            cellY >= heightfield.height - 1) {
            break;
        }
        enter = cellLeave;
    }
    return std::nullopt;
}

} // namespace engine::selection
