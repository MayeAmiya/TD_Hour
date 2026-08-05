#pragma once

#include "engine/renderer/runtime/RenderPerformanceSettings.h"
#include "presentation/render/RenderGameDataSettings.h"

#include <cstddef>
#include <cstdint>

namespace engine::render {

// Resolved renderer policy and bounded per-frame upload/admission budgets.
// This is configuration state, not frame scratch or asset ownership.
struct WorldRenderQualityState final {
    size_t modelUploadsPerFrame = 1;
    size_t modelUploadsPerLoadingFrame = 16;
    uint64_t modelUploadBytesPerFrame = 8ull * 1024ull * 1024ull;
    uint64_t modelUploadBytesPerLoadingFrame = 64ull * 1024ull * 1024ull;
    uint64_t modelUploadMicrosecondsPerFrame = 2000;
    uint64_t modelUploadMicrosecondsPerLoadingFrame = 12000;
    uint32_t maximumGroundProjectorsPerFrame =
        ground_decals::performance_limits::kDefaultMaximumInstancesPerFrame;
    uint32_t maximumGroundProjectorTextures =
        ground_decals::performance_limits::kDefaultMaximumResidentTextures;
    uint32_t textureFilter = 2;
    uint32_t anisotropyLevel = 2;
    uint32_t textureReductionFactor = 0;
    float displayGamma = 1.0f;
    uint32_t worldSampleCount = 1;
    uint64_t featureRevision = 0;
    uint64_t displayRevision = 0;
    RenderAntiAliasingMode effectiveAntiAliasingMode =
        RenderAntiAliasingMode::Off;
    RenderDynamicLod dynamicLod = RenderDynamicLod::High;
    RenderParticleSimulationBackend requestedParticleSimulationBackend =
        RenderParticleSimulationBackend::Cpu;
    RenderParticleSimulationBackend particleSimulationBackend =
        RenderParticleSimulationBackend::Cpu;
    uint64_t gpuParticleAuthorityEpoch = 0;
    bool useCloudMap = true;
    bool useLightMap = true;
};

} // namespace engine::render
