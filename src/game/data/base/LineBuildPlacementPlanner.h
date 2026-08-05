#pragma once

#include "core/container/container_types.h"
#include "core/math/fixed/q32_32.h"

#include <cstdint>

namespace engine {

struct LineBuildPosition final {
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
};

// Pure boundaries supplied by the caller's frozen terrain and placement
// evaluator. Both are invoked in line order; returning false terminates the
// legal prefix without considering any later tile.
using LineBuildTerrainHeightCallback = bool (*)(
    void* context, math::q32_32 x, math::q32_32 y,
    math::q32_32& height) noexcept;
using LineBuildLegalityCallback = bool (*)(
    void* context, const LineBuildPosition& position,
    uint32_t tileIndex) noexcept;

struct LineBuildPlacementRequest final {
    LineBuildPosition start;
    LineBuildPosition end;
    math::q32_32 geometryMajorRadius{};
    uint32_t maxTiles = 50;
    void* callbackContext = nullptr;
    LineBuildTerrainHeightCallback terrainHeight = nullptr;
    LineBuildLegalityCallback isLegal = nullptr;
};

struct LineBuildPlacementPlan final {
    // majorRadius * 2, matching BuildAssistant::buildObjectLineNow.
    math::q32_32 spacing{};
    // Count before legality truncation, already bounded by maxTiles.
    uint32_t boundedTileCount = 0;
    container::Vector<LineBuildPosition> legalPrefix;
};

// Reproduces the original BuildAssistant tiling rule in XY:
// trunc(length / (majorRadius * 2)) + 1, capped by maxTiles. End Z never
// affects length/direction. Every output Z is re-sampled from authoritative
// terrain before legality is evaluated.
[[nodiscard]] LineBuildPlacementPlan planLineBuildPlacement(
    const LineBuildPlacementRequest& request);

} // namespace engine
