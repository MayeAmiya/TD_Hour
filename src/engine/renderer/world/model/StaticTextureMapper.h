#pragma once

#include "core/container/container_types.h"
#include <cstdint>

namespace engine::render {

// Immutable backend-neutral representation of the W3D vertex mapper selected
// by one material texture stage. It is copied from the parsed asset into the
// CPU material, GPU material table, and transient draw packet; no legacy
// mapper object or mutable timer crosses those boundaries.
enum class StaticTextureMapperType : uint8_t {
    Uv = 0,
    Environment,
    CheapEnvironment,
    LinearOffset,
    Scale,
    Grid,
    Rotate,
    Unsupported,
};
// WorldRenderer's shader constants use these backend enum values directly.
static_assert(static_cast<uint8_t>(StaticTextureMapperType::Grid) == 5);
static_assert(static_cast<uint8_t>(StaticTextureMapperType::Rotate) == 6);
static_assert(static_cast<uint8_t>(StaticTextureMapperType::Unsupported) == 7);

struct StaticTextureMapperDesc final {
    StaticTextureMapperType type = StaticTextureMapperType::Uv;
    uint8_t sourceType = 0;
    bool clampFix = false;
    float uScale = 1.0f;
    float vScale = 1.0f;
    float uPerSecond = 0.0f;
    float vPerSecond = 0.0f;
    float uOffset = 0.0f;
    float vOffset = 0.0f;
    float uCenter = 0.0f;
    float vCenter = 0.0f;
    float gridFramesPerSecond = 1.0f;
    uint32_t gridWidthLog2 = 1;
    uint32_t gridLastFrame = 0;
    uint32_t gridOffset = 0;
    float turnsPerSecond = 0.1f;

    bool operator==(const StaticTextureMapperDesc&) const noexcept = default;
};

using StaticTextureMapperStages = container::Array<StaticTextureMapperDesc, 2>;

} // namespace engine::render
