#pragma once

#include <cstdint>

namespace engine::render {

// CPU-authoritative sparse-slot membership. Generation prevents a stale
// frame decision from admitting a newer particle that reused the same slot.
// Object/system visibility is resolved before emission; ordinary camera
// clipping remains a renderer/GPU concern and is not encoded in this list.
struct GpuParticleVisibilityGeneration final {
    uint32_t stateSlot = 0;
    uint32_t particleGeneration = 0;
};

} // namespace engine::render
