#pragma once

#include "presentation/render/TerrainRenderSnapshot.h"

#include <cstdint>

namespace engine::render {

enum class BridgeTowerCorner : uint8_t {
    FromLeft,
    FromRight,
    ToLeft,
    ToRight,
};

struct BridgeTowerPresentationInstance final {
    BridgeTowerCorner corner = BridgeTowerCorner::FromLeft;
    container::String modelAsset;
    RenderVector worldPosition{};
    float yawRadians = 0.0f;
    math::transform worldTransform{};
};

struct BridgeTowerPresentationPlan final {
    container::Vector<BridgeTowerPresentationInstance> instances;
};

// Pure renderer-side equivalent of W3DBridgeBuffer::updateTowerPos. The
// lateral extents come from the selected bridge W3D's BRIDGE_LEFT mesh, while
// each tower rests on the immutable heightfield rather than the elevated deck.
[[nodiscard]] BridgeTowerPresentationPlan buildBridgeTowerPresentationPlan(
    const TerrainBridgeRenderData& bridge,
    const TerrainRenderSnapshot& terrain,
    float sourceMinimumY,
    float sourceMaximumY);

} // namespace engine::render
