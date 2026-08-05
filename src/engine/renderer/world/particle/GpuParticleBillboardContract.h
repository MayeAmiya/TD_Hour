#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::render {

// Must remain byte-for-byte compatible with the b0 cbuffer in
// particle_gpu_billboard.hlsl. The indirect argument's StartInstanceLocation
// is consumed through SV_InstanceID; particleCapacity bounds both SRV
// accesses. This is a graphics-only contract and does not grant the renderer
// ownership of particle simulation or gameplay state.
struct GpuParticleBillboardConstants final {
    float viewProjection[16]{};
    float cameraRight[4]{};
    float cameraUp[4]{};
    uint32_t particleCapacity = 0;
    float interpolationAlpha = 1.0f;
    uint32_t reserved[2]{};
    float playableMinimum[2]{};
    float playableMaximum[2]{};
    uint32_t playableBoundsEnabled = 0;
    uint32_t playablePadding[3]{};
};

static_assert(sizeof(GpuParticleBillboardConstants) == 144u);
static_assert(offsetof(GpuParticleBillboardConstants, viewProjection) == 0u);
static_assert(offsetof(GpuParticleBillboardConstants, cameraRight) == 64u);
static_assert(offsetof(GpuParticleBillboardConstants, cameraUp) == 80u);
static_assert(offsetof(GpuParticleBillboardConstants, particleCapacity) == 96u);
static_assert(offsetof(GpuParticleBillboardConstants, interpolationAlpha) == 100u);
static_assert(offsetof(GpuParticleBillboardConstants, playableMinimum) == 112u);
static_assert(offsetof(GpuParticleBillboardConstants, playableMaximum) == 120u);
static_assert(offsetof(GpuParticleBillboardConstants,
                       playableBoundsEnabled) == 128u);

} // namespace engine::render
