#pragma once

#include "presentation/render/RenderOverlaySnapshot.h"

#include <cstdint>
#include <optional>

namespace engine::render {

struct TacticalRadarEventColors final {
    uint32_t primary = 0xffffffffu;
    uint32_t secondary = 0xffffffffu;
};

[[nodiscard]] TacticalRadarEventColors tacticalRadarEventColors(
    int32_t eventType, uint8_t alpha = 255u) noexcept;

struct TacticalRadarEventTriangle final {
    container::Array<math::vec2, 3> vertices{};
    float radius = 0.0f;
    float rotationRadians = 0.0f;
    uint32_t startColor = 0u;
    uint32_t endColor = 0u;
};

// Pure-value form of W3DRadar's event marker. Generic events start at half
// the displayed radar width; BEACON_PULSE (type 5) starts at one tenth and
// spins in the opposite direction. Both contract over 1.5 confirmed seconds
// to the original six-pixel floor and fade only during their authored tail.
[[nodiscard]] std::optional<TacticalRadarEventTriangle>
tacticalRadarEventTriangle(
    const TacticalRadarEventRenderSnapshot& event,
    math::vec2 center, float radarWidth,
    uint64_t simulationFrame,
    uint32_t logicFramesPerSecond) noexcept;

} // namespace engine::render
