#include "engine/renderer/world/pipeline/WorldRenderPipeline.h"

#include <algorithm>
#include <cmath>

namespace engine::render {

container::Span<const RenderMatrix> PreparedWorldFrame::pose(
    const PreparedRenderInstance& instance) const noexcept {
    if (!instance.poseReady || instance.poseOffset > poseArena.size() ||
        instance.poseCount > poseArena.size() - instance.poseOffset) {
        return {};
    }
    return container::Span<const RenderMatrix>(poseArena)
        .subspan(instance.poseOffset, instance.poseCount);
}

std::optional<size_t> PreparedWorldFrame::visibleInstanceIndexById(
    RenderEntityId id) const noexcept {
    const auto found = visibleInstanceIndicesById.find(id);
    if (found == visibleInstanceIndicesById.end() ||
        found->second >= visibleInstances.size()) {
        return std::nullopt;
    }
    return found->second;
}

container::Span<const uint8_t> PreparedWorldFrame::visibility(
    const PreparedRenderInstance& instance) const noexcept {
    if (!instance.visibilityReady ||
        instance.poseOffset > visibilityArena.size() ||
        instance.poseCount >
            visibilityArena.size() - instance.poseOffset) {
        return {};
    }
    return container::Span<const uint8_t>(visibilityArena)
        .subspan(instance.poseOffset, instance.poseCount);
}

container::Span<const std::optional<RenderMatrix>>
PreparedWorldFrame::particleEmitterBoneWorldTransforms(
    const PreparedRenderInstance& instance) const noexcept {
    if (instance.particleEmitterBoneOffset >
            particleEmitterBoneWorldTransformArena.size() ||
        instance.particleEmitterBoneCount >
            particleEmitterBoneWorldTransformArena.size() -
                instance.particleEmitterBoneOffset) {
        return {};
    }
    return container::Span<const std::optional<RenderMatrix>>(
        particleEmitterBoneWorldTransformArena).subspan(
            instance.particleEmitterBoneOffset,
            instance.particleEmitterBoneCount);
}

void PreparedWorldHotSoA::rebuild(
    container::Span<const PreparedRenderInstance> instances) {
    const size_t count = instances.size();
    ids.resize(count);
    positionX.resize(count);
    positionY.resize(count);
    positionZ.resize(count);
    boundingRadii.resize(count);

    for (size_t index = 0; index < count; ++index) {
        const PreparedRenderInstance& instance = instances[index];
        ids[index] = instance.id;
        boundingRadii[index] = std::isfinite(instance.boundingRadius)
            ? std::max(0.0f, instance.boundingRadius) : 0.0f;
        const RenderVector cullingCenter =
            instance.worldTransform.translation() +
            instance.cullingCenterOffset;
        positionX[index] = cullingCenter.x();
        positionY[index] = cullingCenter.y();
        positionZ[index] = cullingCenter.z();
    }
}

bool PreparedWorldHotSoA::validFor(size_t count) const noexcept {
    return ids.size() == count && positionX.size() == count &&
        positionY.size() == count && positionZ.size() == count &&
        boundingRadii.size() == count;
}

} // namespace engine::render
