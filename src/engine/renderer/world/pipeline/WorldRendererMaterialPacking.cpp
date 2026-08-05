#include "engine/renderer/world/pipeline/WorldRendererMaterialPacking.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace engine::render::world_renderer_detail {
namespace {

[[nodiscard]] float finiteOr(float value, float fallback) noexcept {
    return std::isfinite(value) ? value : fallback;
}

} // namespace

void packTextureMapper(
    const StaticTextureMapperDesc& source,
    float (&scaleOffset)[4],
    float (&motionCenter)[4],
    uint32_t& type,
    uint32_t& clampFix) noexcept {
    scaleOffset[0] = finiteOr(source.uScale, 1.0f);
    scaleOffset[1] = finiteOr(source.vScale, 1.0f);
    scaleOffset[2] = finiteOr(source.uOffset, 0.0f);
    scaleOffset[3] = finiteOr(source.vOffset, 0.0f);
    if (source.type == StaticTextureMapperType::Grid) {
        motionCenter[0] = finiteOr(source.gridFramesPerSecond, 1.0f);
        // Grid uses these float lanes as uint payloads. Bit-casting retains
        // authored Last/Offset values above float's 24-bit integer range.
        motionCenter[1] = std::bit_cast<float>(
            std::min<uint32_t>(source.gridWidthLog2, 15u));
        motionCenter[2] = std::bit_cast<float>(source.gridLastFrame);
        motionCenter[3] = std::bit_cast<float>(source.gridOffset);
    } else {
        motionCenter[0] = source.type == StaticTextureMapperType::Rotate
            ? finiteOr(source.turnsPerSecond, 0.1f)
            : finiteOr(source.uPerSecond, 0.0f);
        motionCenter[1] = finiteOr(source.vPerSecond, 0.0f);
        motionCenter[2] = finiteOr(source.uCenter, 0.0f);
        motionCenter[3] = finiteOr(source.vCenter, 0.0f);
    }
    type = source.type == StaticTextureMapperType::Unsupported
        ? static_cast<uint32_t>(StaticTextureMapperType::Uv)
        : static_cast<uint32_t>(source.type);
    clampFix = source.clampFix ? 1u : 0u;
}

} // namespace engine::render::world_renderer_detail
