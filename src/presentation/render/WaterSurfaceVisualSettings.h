#pragma once

#include <cstdint>

namespace engine::water_surface::visual_defaults {

// The clipped shoreline ends where terrain remains this far below the water
// plane. It is a presentation separation, not a simulation water-level or a
// renderer resource budget.
inline constexpr float kMinimumVisibleDepth = 0.0f;
// Standing-water UVs are anchored in world space in W3DWater. Keeping this
// separate from WaterRepeatCount prevents adjacent triggers from restarting
// texture phase at their independent bounding boxes.
inline constexpr float kStandingWaterWorldUnitsPerRepeat = 150.0f;

// INI::parseVelocityReal converts authored units/second to the legacy
// 30-Hz client update step before WaveGuideUpdate reaches W3DWater.
inline constexpr float kLegacyWaterUpdatesPerSecond = 30.0f;
[[nodiscard]] constexpr float legacyWaterVelocityPerUpdate(
    float unitsPerSecond) noexcept {
    return unitsPerSecond / kLegacyWaterUpdatesPerSecond;
}
[[nodiscard]] constexpr float legacyWaterGravityPerUpdate(
    float unitsPerSecondSquared) noexcept {
    return unitsPerSecondSquared /
        (kLegacyWaterUpdatesPerSecond * kLegacyWaterUpdatesPerSecond);
}

// RefCode W3DWater's fixed-step, per-vertex oscillator constants.  These are
// presentation behaviour rather than renderer budgets: changing them changes
// the authored wave response and settling time.
inline constexpr float kVertexWaterVelocityDamping = 0.93f;
inline constexpr float kVertexWaterGravityMultiplier = 3.0f;
inline constexpr float kVertexWaterPreferredHeightFudge = 1.0f;
inline constexpr float kVertexWaterRestVelocityFudge = 1.0f;
// W3DWater::WaterMeshData stores preferredHeight as UnsignedByte even though
// addVelocity accepts Real.  Preserve that truncating storage boundary
// explicitly, while callers reject values outside the representable range.
inline constexpr float kVertexWaterPreferredHeightMinimum = 0.0f;
inline constexpr float kVertexWaterPreferredHeightMaximum = 255.0f;

// RefCode W3DWater.cpp:1383-1385 normalizes the authored field by the
// WaterTexture level-zero width.  Keep the slightly surprising water-texture
// dependency explicit instead of silently substituting SkyTexture dimensions.
[[nodiscard]] constexpr float normalizedSkyTexelsPerUnit(
    float authoredTexelsPerUnit, uint32_t waterTextureWidth) noexcept {
    return waterTextureWidth != 0u
        ? authoredTexelsPerUnit / static_cast<float>(waterTextureWidth)
        : authoredTexelsPerUnit;
}

static_assert(kMinimumVisibleDepth >= 0.0f);
static_assert(kStandingWaterWorldUnitsPerRepeat > 0.0f);
static_assert(kLegacyWaterUpdatesPerSecond > 0.0f);
static_assert(kVertexWaterVelocityDamping > 0.0f &&
              kVertexWaterVelocityDamping < 1.0f);
static_assert(kVertexWaterGravityMultiplier > 0.0f);
static_assert(kVertexWaterPreferredHeightFudge > 0.0f);
static_assert(kVertexWaterRestVelocityFudge > 0.0f);

} // namespace engine::water_surface::visual_defaults
