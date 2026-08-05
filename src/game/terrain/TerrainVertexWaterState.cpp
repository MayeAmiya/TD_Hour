#include "game/terrain/TerrainVertexWaterState.h"

#include "presentation/render/WaterSurfacePerformanceSettings.h"
#include "presentation/render/WaterSurfaceVisualSettings.h"

#include <cmath>
#include <bit>
#include <limits>

namespace engine {
namespace {

[[nodiscard]] bool finiteConfig(
    const TerrainVertexWaterGridConfig& config) noexcept {
    return std::isfinite(config.positionX) &&
        std::isfinite(config.positionY) &&
        std::isfinite(config.positionZ) &&
        std::isfinite(config.angleRadians) &&
        std::isfinite(config.gridSize) && config.gridSize > 0.0f &&
        std::isfinite(config.influenceRange) &&
        config.influenceRange >= 0.0f && config.cellsX > 0u &&
        config.cellsY > 0u &&
        config.cellsX <= water_surface::performance_limits::
            kMaximumVertexWaterGridCellsPerAxis &&
        config.cellsY <= water_surface::performance_limits::
            kMaximumVertexWaterGridCellsPerAxis;
}

[[nodiscard]] uint32_t clampedFloor(float value,
                                    uint32_t maximum) noexcept {
    if (!(value > 0.0f)) return 0u;
    if (value >= static_cast<float>(maximum)) return maximum;
    return static_cast<uint32_t>(std::floor(value));
}

[[nodiscard]] uint32_t clampedCeil(float value,
                                   uint32_t maximum) noexcept {
    if (!(value > 0.0f)) return 0u;
    if (value >= static_cast<float>(maximum)) return maximum;
    return static_cast<uint32_t>(std::ceil(value));
}

[[nodiscard]] bool sameFloatBits(float left, float right) noexcept {
    return std::bit_cast<uint32_t>(left) == std::bit_cast<uint32_t>(right);
}

[[nodiscard]] bool sameConfig(
    const TerrainVertexWaterGridConfig& left,
    const TerrainVertexWaterGridConfig& right) noexcept {
    return left.cellsX == right.cellsX && left.cellsY == right.cellsY &&
        sameFloatBits(left.positionX, right.positionX) &&
        sameFloatBits(left.positionY, right.positionY) &&
        sameFloatBits(left.positionZ, right.positionZ) &&
        sameFloatBits(left.angleRadians, right.angleRadians) &&
        sameFloatBits(left.gridSize, right.gridSize) &&
        sameFloatBits(left.influenceRange, right.influenceRange);
}

} // namespace

bool TerrainVertexWaterState::configure(
    const TerrainVertexWaterGridConfig& config) {
    if (!finiteConfig(config)) return false;

    const float rangeInGridCells = config.influenceRange / config.gridSize;
    if (!std::isfinite(rangeInGridCells)) return false;

    const std::size_t verticesX =
        static_cast<std::size_t>(config.cellsX) + 1u;
    const std::size_t verticesY =
        static_cast<std::size_t>(config.cellsY) + 1u;
    if (verticesX > std::numeric_limits<std::size_t>::max() / verticesY) {
        return false;
    }

    container::Vector<TerrainVertexWaterPoint> newPoints;
    try {
        newPoints.resize(verticesX * verticesY);
    } catch (...) {
        return false;
    }

    const float cosine = std::cos(config.angleRadians);
    const float sine = std::sin(config.angleRadians);
    if (!std::isfinite(cosine) || !std::isfinite(sine)) return false;

    config_ = config;
    points_ = std::move(newPoints);
    directionXx_ = cosine;
    directionXy_ = sine;
    directionYx_ = -sine;
    directionYy_ = cosine;
    configured_ = true;
    inMotion_ = false;
    return true;
}

void TerrainVertexWaterState::reset() noexcept {
    for (TerrainVertexWaterPoint& point : points_) {
        point = {};
    }
    inMotion_ = false;
}

void TerrainVertexWaterState::clear() noexcept {
    config_ = {};
    container::Vector<TerrainVertexWaterPoint>{}.swap(points_);
    directionXx_ = 1.0f;
    directionXy_ = 0.0f;
    directionYx_ = 0.0f;
    directionYy_ = 1.0f;
    configured_ = false;
    inMotion_ = false;
}

bool TerrainVertexWaterState::addVelocity(float worldX,
                                          float worldY,
                                          float velocity,
                                          float preferredHeight) noexcept {
    using namespace water_surface::visual_defaults;
    if (!configured_ || !std::isfinite(worldX) || !std::isfinite(worldY) ||
        !std::isfinite(velocity) || !std::isfinite(preferredHeight) ||
        preferredHeight < kVertexWaterPreferredHeightMinimum ||
        preferredHeight > kVertexWaterPreferredHeightMaximum) {
        return false;
    }

    const float dx = worldX - config_.positionX;
    const float dy = worldY - config_.positionY;
    const float gridX =
        (dx * directionXx_ + dy * directionXy_) / config_.gridSize;
    const float gridY =
        (dx * directionYx_ + dy * directionYy_) / config_.gridSize;
    if (!std::isfinite(gridX) || !std::isfinite(gridY) || gridX < 0.0f ||
        gridY < 0.0f ||
        gridX > static_cast<float>(config_.cellsX - 1u) ||
        gridY > static_cast<float>(config_.cellsY - 1u)) {
        return false;
    }

    const float range = config_.influenceRange / config_.gridSize;
    const uint32_t minX = clampedFloor(gridX - range, config_.cellsX);
    const uint32_t maxX = clampedCeil(gridX + range, config_.cellsX);
    const uint32_t minY = clampedFloor(gridY - range, config_.cellsY);
    const uint32_t maxY = clampedCeil(gridY + range, config_.cellsY);
    const uint8_t preferredHeightByte =
        static_cast<uint8_t>(preferredHeight);

    for (uint32_t y = minY; y <= maxY; ++y) {
        for (uint32_t x = minX; x <= maxX; ++x) {
            TerrainVertexWaterPoint& point = points_[pointIndex(x, y)];
            point.preferredHeight = preferredHeightByte;
            point.velocity += velocity;
            point.status = TerrainVertexWaterPointStatus::InMotion;
        }
    }
    inMotion_ = true;
    return true;
}

bool TerrainVertexWaterState::advance(float gravityPerUpdate) noexcept {
    using namespace water_surface::visual_defaults;
    if (!configured_ || !std::isfinite(gravityPerUpdate)) return false;
    if (!inMotion_) return true;

    inMotion_ = false;
    for (TerrainVertexWaterPoint& point : points_) {
        if (!point.inMotion()) continue;

        point.velocity *= kVertexWaterVelocityDamping;
        const float preferredHeight =
            static_cast<float>(point.preferredHeight);
        if (point.height < preferredHeight) {
            point.velocity -=
                gravityPerUpdate * kVertexWaterGravityMultiplier;
        } else {
            point.velocity +=
                gravityPerUpdate * kVertexWaterGravityMultiplier;
        }
        point.height += point.velocity;

        if (std::abs(point.height - preferredHeight) <
                kVertexWaterPreferredHeightFudge &&
            std::abs(point.velocity) < kVertexWaterRestVelocityFudge) {
            point.height = preferredHeight;
            point.velocity = 0.0f;
            point.status = TerrainVertexWaterPointStatus::AtRest;
        } else {
            inMotion_ = true;
        }
    }
    return true;
}

bool TerrainVertexWaterState::restore(
    const TerrainVertexWaterGridConfig& expectedConfig,
    container::Vector<TerrainVertexWaterPoint> points) {
    if (!configured_ || !sameConfig(config_, expectedConfig) ||
        points.size() != points_.size()) {
        return false;
    }
    bool anyMotion = false;
    for (const TerrainVertexWaterPoint& point : points) {
        const uint8_t status = static_cast<uint8_t>(point.status);
        if (!std::isfinite(point.height) || !std::isfinite(point.velocity) ||
            status > static_cast<uint8_t>(
                         TerrainVertexWaterPointStatus::InMotion)) {
            return false;
        }
        anyMotion = anyMotion || point.inMotion();
    }
    points_.swap(points);
    inMotion_ = anyMotion;
    return true;
}

const TerrainVertexWaterPoint* TerrainVertexWaterState::point(
    uint32_t x, uint32_t y) const noexcept {
    if (!configured_ || x > config_.cellsX || y > config_.cellsY) {
        return nullptr;
    }
    return &points_[pointIndex(x, y)];
}

std::size_t TerrainVertexWaterState::pointIndex(uint32_t x,
                                                uint32_t y) const noexcept {
    return static_cast<std::size_t>(y) *
            (static_cast<std::size_t>(config_.cellsX) + 1u) +
        static_cast<std::size_t>(x);
}

} // namespace engine
