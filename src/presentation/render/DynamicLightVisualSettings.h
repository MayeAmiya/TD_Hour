#pragma once

namespace engine::dynamic_lights::visual_defaults {

// W3DDisplay::createLightPulse uses a fixed inner radius of one and treats
// the authored Radius as attenuation width. The target outer radius is thus
// inner + authored width; both colour and outer radius share the fade factor.
inline constexpr float kInnerRadius = 1.0f;
inline constexpr float kMinimumAttenuationWidth = 0.01f;
inline constexpr float kMaximumAttenuationWidth = 4096.0f;
inline constexpr float kPathfindCellSize = 10.0f;
// W3DDisplay rejects inner + attenuationWidth < 2 * cell + 1. With the
// fixed inner radius of one this admits authored Radius=20 exactly and drops
// the stock Radius=10/15 pulses.
inline constexpr float kMinimumOriginalOuterRadius =
    2.0f * kPathfindCellSize + 1.0f;

} // namespace engine::dynamic_lights::visual_defaults
