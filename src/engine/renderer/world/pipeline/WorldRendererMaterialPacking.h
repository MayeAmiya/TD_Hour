#pragma once

#include "engine/renderer/world/model/StaticTextureMapper.h"

#include <cstdint>

namespace engine::render::world_renderer_detail {

void packTextureMapper(
    const StaticTextureMapperDesc& source,
    float (&scaleOffset)[4],
    float (&motionCenter)[4],
    uint32_t& type,
    uint32_t& clampFix) noexcept;

} // namespace engine::render::world_renderer_detail
