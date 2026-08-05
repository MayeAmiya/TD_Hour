#pragma once

#include <cstdint>

namespace engine::render {

// Sampling interpretation shared by CPU decode and GPU residency owners.
enum class WorldTextureVariant : uint8_t {
    ColorLegacyGamma,
    DataLinear,
};

} // namespace engine::render
