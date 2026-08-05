#pragma once

#include <cmath>
#include <cstdint>

namespace engine::heat_vision::visual_settings {

// RefCode resets the second material pass to 1 whenever that object's real
// detector scan hits, then Drawable::draw() multiplies it by 0.8 each logic
// frame.  There is deliberately no fixed refresh period here: shipped
// DetectionRate values include 500, 900 and 1500 ms and each detector starts
// on an independent phase.
inline constexpr float kHeatVisionOpacityFadePerFrame = 0.8f;

[[nodiscard]] inline float heatVisionOpacityAfterFrames(
    uint64_t elapsed) noexcept {
    return std::pow(kHeatVisionOpacityFadePerFrame,
                    static_cast<float>(elapsed));
}

} // namespace engine::heat_vision::visual_settings
