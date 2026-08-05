#pragma once

namespace engine::presentation_defaults {

// Original WND files author coordinates in an 800x600 canvas. These values
// describe content, not the swap-chain or world-render resolution.
inline constexpr int AUTHORED_WIDTH = 800;
inline constexpr int AUTHORED_HEIGHT = 600;

// Current developer-launch default. Runtime rendering still observes the
// actual SDL pixel extent and recomputes its authored-to-output scale after
// every window/output change, so higher resolutions do not require code
// changes.
inline constexpr int DEFAULT_OUTPUT_WIDTH = 1600;
inline constexpr int DEFAULT_OUTPUT_HEIGHT = 1200;

// Transitional names used by the existing 2D drawing interfaces. They are
// the authored canvas extent, never the physical render-target extent.
inline constexpr int VIRTUAL_WIDTH = AUTHORED_WIDTH;
inline constexpr int VIRTUAL_HEIGHT = AUTHORED_HEIGHT;

} // namespace engine::presentation_defaults
