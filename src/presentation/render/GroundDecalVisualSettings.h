#pragma once

namespace engine::ground_decals::visual_defaults {

inline constexpr float kTerrainOffset = 0.05f;
// RefCode queueSimpleDecal() moves projected object shadows one world unit
// along the sampled terrain normal. This is deliberately separate from the
// much smaller terrain decal/scorch offset.
inline constexpr float kProjectedShadowTerrainNormalOffset = 1.0f;
inline constexpr float kRadialEdgeSoftness = 0.30f;

} // namespace engine::ground_decals::visual_defaults
