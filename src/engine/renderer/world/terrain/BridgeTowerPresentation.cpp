#include "engine/renderer/world/terrain/BridgeTowerPresentation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace engine::render {
namespace {

[[nodiscard]] std::optional<float> terrainHeightAt(
    const TerrainRenderSnapshot& terrain,
    float worldX, float worldY) noexcept {
    if (terrain.width < 2 || terrain.height < 2 ||
        !std::isfinite(terrain.cellWorldSize) ||
        terrain.cellWorldSize <= 0.0f ||
        !std::isfinite(terrain.heightWorldScale) ||
        terrain.heightWorldScale <= 0.0f ||
        !std::isfinite(worldX) || !std::isfinite(worldY)) {
        return std::nullopt;
    }
    const size_t width = static_cast<size_t>(terrain.width);
    const size_t height = static_cast<size_t>(terrain.height);
    if (width > std::numeric_limits<size_t>::max() / height ||
        terrain.heights.size() != width * height) {
        return std::nullopt;
    }
    const float sampleX = worldX / terrain.cellWorldSize +
        static_cast<float>(terrain.borderSize);
    const float sampleY = worldY / terrain.cellWorldSize +
        static_cast<float>(terrain.borderSize);
    if (sampleX < 0.0f || sampleY < 0.0f ||
        sampleX > static_cast<float>(terrain.width - 1) ||
        sampleY > static_cast<float>(terrain.height - 1)) {
        return std::nullopt;
    }
    const int32_t x0 = static_cast<int32_t>(std::floor(sampleX));
    const int32_t y0 = static_cast<int32_t>(std::floor(sampleY));
    const int32_t cellX = std::min(x0, terrain.width - 2);
    const int32_t cellY = std::min(y0, terrain.height - 2);
    const float amountX = sampleX - static_cast<float>(cellX);
    const float amountY = sampleY - static_cast<float>(cellY);
    const auto heightAt = [&terrain, width](int32_t x, int32_t y) {
        return static_cast<float>(terrain.heights[
            static_cast<size_t>(y) * width + static_cast<size_t>(x)]) *
            terrain.heightWorldScale;
    };
    const float p0 = heightAt(cellX, cellY);
    const float p1 = heightAt(cellX + 1, cellY);
    const float p2 = heightAt(cellX + 1, cellY + 1);
    const float p3 = heightAt(cellX, cellY + 1);
    // TerrainMap::sampleSurface and the original gameplay height query split
    // every cell along p0->p2. Tower feet must rest on one of those two
    // planes; bilinear interpolation cuts through a surface that is never
    // rendered and can visibly float on saddle-shaped cells.
    if (amountY > amountX) {
        return p3 + (1.0f - amountY) * (p0 - p3) +
            amountX * (p2 - p3);
    }
    return p1 + amountY * (p2 - p1) +
        (1.0f - amountX) * (p0 - p1);
}

} // namespace

BridgeTowerPresentationPlan buildBridgeTowerPresentationPlan(
    const TerrainBridgeRenderData& bridge,
    const TerrainRenderSnapshot& terrain,
    float sourceMinimumY,
    float sourceMaximumY) {
    BridgeTowerPresentationPlan output;
    if (!std::isfinite(bridge.scale) || bridge.scale <= 0.0f ||
        !std::isfinite(sourceMinimumY) ||
        !std::isfinite(sourceMaximumY) ||
        sourceMaximumY < sourceMinimumY) {
        return output;
    }
    const float deltaX = bridge.end.x() - bridge.start.x();
    const float deltaY = bridge.end.y() - bridge.start.y();
    const float length = std::hypot(deltaX, deltaY);
    if (!std::isfinite(length) || length <= math::EPSILON) return output;
    const float forwardX = deltaX / length;
    const float forwardY = deltaY / length;
    const float sideX = -forwardY;
    const float sideY = forwardX;
    const float toYaw = std::atan2(deltaY, deltaX);

    output.instances.reserve(bridge.towerModelAssets.size());
    for (size_t index = 0; index < bridge.towerModelAssets.size(); ++index) {
        const container::String& modelAsset = bridge.towerModelAssets[index];
        if (modelAsset.empty()) continue;
        const bool fromSide = index < 2u;
        const bool leftSide = (index % 2u) == 0u;
        const RenderVector& endpoint = fromSide ? bridge.start : bridge.end;
        const float lateral = (leftSide ? sourceMaximumY : sourceMinimumY) *
            bridge.scale;
        RenderVector position{
            endpoint.x() + sideX * lateral,
            endpoint.y() + sideY * lateral,
            0.0f,
        };
        const std::optional<float> ground = terrainHeightAt(
            terrain, position.x(), position.y());
        if (!ground) continue;
        position[2] = *ground;
        const float yaw = toYaw + (fromSide ? math::PI : 0.0f);
        output.instances.push_back({
            .corner = static_cast<BridgeTowerCorner>(index),
            .modelAsset = modelAsset,
            .worldPosition = position,
            .yawRadians = yaw,
            .worldTransform = math::transform::from_axes(
                {std::cos(yaw), std::sin(yaw), 0.0f},
                {-std::sin(yaw), std::cos(yaw), 0.0f},
                {0.0f, 0.0f, 1.0f}, position),
        });
    }
    return output;
}

} // namespace engine::render
