#pragma once

#include "core/container/container_types.h"

#include "presentation/render/RenderOverlaySnapshot.h"
#include "presentation/render/RenderViewSnapshot.h"

#include <cstddef>
#include <cstdint>
namespace engine::render {

// A screen-space projection of the original camera-relative snow volume.
// DX12Renderer submits these through its existing alpha UI-quad batch before
// it records view filters/UI; the values remain renderer-owned and are never
// fed back to a GameSession.
struct WeatherSnowflake final {
    float x = 0.0f;
    float y = 0.0f;
    float size = 0.0f;
    float opacity = 0.0f;
};

// Applies the detached SET_TREE_SWAY state to one explicitly opted-in model
// world matrix.  Stable RenderEntityId hashing replaces RefCode's client RNG,
// so visual variation survives dropped render frames without advancing any
// simulation random stream.
[[nodiscard]] math::transform applyTreeSwayPresentation(
    const math::transform& world, const TreeSwayRenderState& sway,
    RenderEntityId entity, uint64_t simulationFrame) noexcept;

// Builds a bounded, camera-relative snow field for the current sealed world
// frame. It returns zero whenever Weather.ini did not enable snow or when
// SHOW_WEATHER made that configured effect invisible.
[[nodiscard]] size_t buildWeatherSnowflakes(
    const WeatherRenderState& weather, const RenderCameraSnapshot& camera,
    uint64_t simulationFrame, float viewportWidth, float viewportHeight,
    container::Span<WeatherSnowflake> output) noexcept;

} // namespace engine::render
