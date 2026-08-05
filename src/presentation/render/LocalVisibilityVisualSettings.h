#pragma once

namespace engine::local_visibility::visual_defaults {

// Presentation policy for the three-state tactical visibility texture.
// Shrouded means never explored and must conceal terrain/material detail
// completely; explored fog remains visible but heavily darkened.
inline constexpr float kExploredBrightness = 127.0f / 255.0f;
inline constexpr float kShroudedBrightness = 0.0f;

static_assert(kExploredBrightness > kShroudedBrightness);
static_assert(kExploredBrightness < 1.0f);

} // namespace engine::local_visibility::visual_defaults
