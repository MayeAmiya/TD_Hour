#pragma once

#include "presentation/camera/GameCameraState.h"
#include "game/terrain/MapHeightfieldLoader.h"
#include "presentation/render/RenderWorldDescriptorContracts.h"

#include <cstdint>
#include <optional>

namespace engine::selection {

struct LocalPlacementViewport final {
    uint32_t width = 0;
    uint32_t height = 0;
};

// Exact client-side equivalent of W3DView::screenToTerrain for the modern
// fixed p0->p2 heightfield. The pick ray uses the tactical viewport and tests
// the two real terrain triangles in front-to-back grid order; it does not
// intersect a flat Z plane or a bilinear surface.
[[nodiscard]] std::optional<render::RenderVector>
localPlacementScreenToTerrain(
    const GameCameraState& camera,
    const game::terrain::TerrainHeightfieldData& terrain,
    LocalPlacementViewport viewport,
    float screenX,
    float screenY) noexcept;

} // namespace engine::selection
