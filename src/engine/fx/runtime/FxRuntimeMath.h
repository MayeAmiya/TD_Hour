#pragma once

#include "presentation/fx/content/ParticleSystemCatalog.h"
#include "presentation/fx/runtime/FxPresentationSnapshot.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace engine::fx::runtime_detail {

[[nodiscard]] inline ParticleVector3 rotateEuler(
    ParticleVector3 value, float rotateX, float rotateY,
    float rotateZ) noexcept {
    const float cosineX = std::cos(rotateX);
    const float sineX = std::sin(rotateX);
    value = {
        value.x,
        value.y * cosineX - value.z * sineX,
        value.y * sineX + value.z * cosineX,
    };
    const float cosineY = std::cos(rotateY);
    const float sineY = std::sin(rotateY);
    value = {
        value.x * cosineY + value.z * sineY,
        value.y,
        -value.x * sineY + value.z * cosineY,
    };
    const float cosineZ = std::cos(rotateZ);
    const float sineZ = std::sin(rotateZ);
    return {
        value.x * cosineZ - value.y * sineZ,
        value.x * sineZ + value.y * cosineZ,
        value.z,
    };
}

[[nodiscard]] inline std::optional<float> sampleGroundHeight(
    const FxGroundHeightFieldSnapshot& field, float worldX,
    float worldY) noexcept {
    if (field.width < 2 || field.height < 2 ||
        field.heights.size() != static_cast<size_t>(field.width) *
            static_cast<size_t>(field.height) ||
        !std::isfinite(field.cellWorldSize) ||
        field.cellWorldSize <= 0.0f ||
        !std::isfinite(field.heightWorldScale) ||
        field.heightWorldScale <= 0.0f || !std::isfinite(worldX) ||
        !std::isfinite(worldY)) {
        return std::nullopt;
    }
    float gridX = worldX / field.cellWorldSize +
        static_cast<float>(field.borderSize);
    float gridY = worldY / field.cellWorldSize +
        static_cast<float>(field.borderSize);
    gridX = std::clamp(
        gridX, 0.0f, static_cast<float>(field.width - 1));
    gridY = std::clamp(
        gridY, 0.0f, static_cast<float>(field.height - 1));
    const int32_t cellX = std::min(
        static_cast<int32_t>(std::floor(gridX)), field.width - 2);
    const int32_t cellY = std::min(
        static_cast<int32_t>(std::floor(gridY)), field.height - 2);
    const float localX = gridX - static_cast<float>(cellX);
    const float localY = gridY - static_cast<float>(cellY);
    const auto heightAt = [&field](int32_t x, int32_t y) noexcept {
        return static_cast<float>(field.heights[
            static_cast<size_t>(y) * static_cast<size_t>(field.width) +
            static_cast<size_t>(x)]) * field.heightWorldScale;
    };
    const float h00 = heightAt(cellX, cellY);
    const float h10 = heightAt(cellX + 1, cellY);
    const float h11 = heightAt(cellX + 1, cellY + 1);
    const float h01 = heightAt(cellX, cellY + 1);
    return localY > localX
        ? h01 + (1.0f - localY) * (h00 - h01) +
              localX * (h11 - h01)
        : h10 + localY * (h11 - h10) +
              (1.0f - localX) * (h00 - h10);
}

[[nodiscard]] inline std::optional<float> sampleGroundHeight(
    const FxGroundHeightFieldSnapshot* field, float worldX,
    float worldY) noexcept {
    return field ? sampleGroundHeight(*field, worldX, worldY)
                 : std::nullopt;
}

} // namespace engine::fx::runtime_detail
