#pragma once

#include "presentation/render/TerrainRenderSnapshot.h"
#include "engine/renderer/world/model/W3dStaticModel.h"

#include <cstdint>
#include <optional>

namespace engine::render {

enum class BridgeW3dSectionKind : uint8_t {
    Left,
    Span,
    Right,
};

struct BridgeW3dSectionInstance final {
    BridgeW3dSectionKind kind = BridgeW3dSectionKind::Left;
    math::transform worldTransform{};
    container::Vector<RenderSubObjectVisibility> subObjectVisibility;
};

// Pure renderer-side equivalent of W3DBridge::getIndicesNVertices().  The
// source W3D remains immutable and shared; each returned instance selects one
// BRIDGE_LEFT/SPAN/RIGHT subobject and maps its authored X/Y/Z axes onto the
// bridge endpoints.  No GPU handle or ECS identity crosses this boundary.
struct BridgeW3dPresentationPlan final {
    container::Vector<BridgeW3dSectionInstance> sections;
    float sourceMinimumX = 0.0f;
    float sourceMaximumX = 0.0f;
    float sourceMinimumY = 0.0f;
    float sourceMaximumY = 0.0f;
    float bridgeWidth = 0.0f;
    float sourceLength = 0.0f;
    float spanLength = 0.0f;
    uint32_t spanCount = 0;
    bool sectional = false;
};

[[nodiscard]] std::optional<BridgeW3dPresentationPlan>
buildBridgeW3dPresentationPlan(
    const CpuStaticModel& model,
    const TerrainBridgeRenderData& bridge);

} // namespace engine::render
