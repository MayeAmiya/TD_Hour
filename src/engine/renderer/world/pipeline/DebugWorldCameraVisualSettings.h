#pragma once

#include <algorithm>
#include <cmath>

namespace engine::debug_world_camera::visual_defaults {

inline constexpr float kMinimumNearClip = 0.1f;
inline constexpr float kMaximumNearClip = 128.0f;
inline constexpr float kNearClipDistanceScale = 0.01f;

[[nodiscard]] inline float nearClipForDistance(float distance) noexcept {
    if (!std::isfinite(distance) || distance <= 0.0f) {
        return kMinimumNearClip;
    }
    return std::clamp(
        distance * kNearClipDistanceScale,
        kMinimumNearClip,
        kMaximumNearClip);
}

} // namespace engine::debug_world_camera::visual_defaults
