#pragma once

#include <cstdint>

namespace engine::render::world_renderer_shadow {

inline constexpr uint32_t kMapSize = 2048;
inline constexpr float kDepthBias = 0.0012f;
inline constexpr float kStrength = 0.82f;
inline constexpr int32_t kRasterDepthBias = 1500;
inline constexpr float kRasterSlopeScaledDepthBias = 1.5f;

} // namespace engine::render::world_renderer_shadow
