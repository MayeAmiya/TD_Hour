#pragma once

#include <algorithm>
#include <cstdint>

namespace engine::dynamic_lights::performance_limits {

// These are three different original policies. Runtime ownership needs a
// modern safety ceiling, terrain evaluates at most 20 lights, and each object
// keeps four diffuse contributors while accumulating ambient from every
// intersecting light. Do not collapse them back into one admission cap.
inline constexpr uint32_t kHardMaximumActiveLights = 256;
inline constexpr uint32_t kTerrainReceiverMaximumLights = 20;
inline constexpr uint32_t kObjectDiffuseMaximumLights = 4;
inline constexpr uint32_t kReferenceSimulationFramesPerSecond = 30;
inline constexpr uint32_t kMaximumAuthoredDurationMilliseconds =
    10u * 60u * 1000u;

[[nodiscard]] constexpr uint32_t framesFromMilliseconds(
    uint32_t milliseconds) noexcept {
    const uint32_t bounded = std::min(
        milliseconds, kMaximumAuthoredDurationMilliseconds);
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(bounded) *
             kReferenceSimulationFramesPerSecond +
         999u) /
        1000u);
}

} // namespace engine::dynamic_lights::performance_limits

namespace engine {

// Performance policy is deliberately separate from the light's visible
// radius/fade contract. A quality profile may lower this count, but can never
// expand the shader/runtime hard limit.
struct DynamicLightRenderBudget final {
    uint32_t maximumLights =
        dynamic_lights::performance_limits::kHardMaximumActiveLights;

    [[nodiscard]] constexpr uint32_t boundedMaximumLights() const noexcept {
        return std::clamp(
            maximumLights, 1u,
            dynamic_lights::performance_limits::kHardMaximumActiveLights);
    }
};

} // namespace engine
