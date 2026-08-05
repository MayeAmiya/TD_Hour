#include "DX12RendererWorldAssetRuntime.h"

#include <limits>

namespace engine {

uint64_t DX12Renderer::WorldAssetRuntime::retainedScratchCapacityBytes()
    const noexcept {
    uint64_t total = 0;
    const auto addVector = [&total]<typename Value>(
                               const container::Vector<Value>& values) {
        const uint64_t capacity = static_cast<uint64_t>(values.capacity());
        const uint64_t elementBytes = sizeof(Value);
        const uint64_t bytes = elementBytes != 0 &&
                capacity > std::numeric_limits<uint64_t>::max() /
                    elementBytes
            ? std::numeric_limits<uint64_t>::max()
            : capacity * elementBytes;
        total = bytes > std::numeric_limits<uint64_t>::max() - total
            ? std::numeric_limits<uint64_t>::max()
            : total + bytes;
    };

    addVector(frame.drawPackets);
    addVector(frame.worldViewVisibility);
    addVector(frame.worldViewTaskVisibleCounts);
    addVector(frame.bridgeDrawPackets);
    addVector(frame.reflectionDrawPackets);
    addVector(frame.bridgeRadarGeometry);
    addVector(frame.overlayDrawPackets);
    addVector(frame.bibDrawPackets);
    addVector(frame.materialTextureOverrideScratch);
    addVector(frame.groundProjectors);
    addVector(frame.mapScorchProjectors);
    addVector(frame.typedScorchBuildSources);
    addVector(frame.typedScorchProjectors);
    addVector(frame.generalGroundDecals);

    const uint64_t typedFxBytes = fx.typed.retainedQueueCapacityBytes();
    total = typedFxBytes > std::numeric_limits<uint64_t>::max() - total
        ? std::numeric_limits<uint64_t>::max()
        : total + typedFxBytes;

    addVector(frame.policeLightKeys);
    addVector(frame.combinedDynamicPointLights);
    addVector(frame.trackMarkSources);
    addVector(frame.interpolatedProjectiles);
    addVector(frame.trackMarkDrawList.vertices);
    addVector(frame.trackMarkDrawList.indices);
    addVector(frame.trackMarkDrawList.batches);
    addVector(frame.particleDrawList.instances);
    addVector(frame.particleDrawList.batches);
    addVector(frame.particleDrawList.gpuVisibilityGenerations);
    addVector(frame.particleDrawList.smudgeInstances);
    addVector(frame.fxBonePoseDemands);
    addVector(frame.fxPresentationBonePoses);
    addVector(frame.fxModelParticleEmitterPoses);
    addVector(frame.fxAdmittedInvocations);
    addVector(frame.fxDeferredExecutionSnapshots);
    return total;
}

} // namespace engine
