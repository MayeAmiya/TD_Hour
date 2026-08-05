#include "engine/renderer/world/pipeline/WorldRenderPipeline.h"

#include "engine/renderer/runtime/RenderParallelExecutor.h"
#include "engine/renderer/world/model/W3dStaticModel.h"

#include <utility>

namespace engine::render {

WorldRenderPipeline::WorldRenderPipeline() = default;

WorldRenderPipeline::~WorldRenderPipeline() {
    if (m_preparing) {
        m_pendingWork.wait();
        m_preparing = false;
    }
}

void WorldRenderPipeline::recordPreparationTaskStart() noexcept {
    const int workerId = parallelExecutor().this_worker_id();
    if (workerId >= 0 && workerId < 64) {
        m_preparationWorkerMask.fetch_or(
            uint64_t{1} << static_cast<uint32_t>(workerId),
            std::memory_order_relaxed);
    }
}

void WorldRenderPipeline::recordPreparationTaskCompletion() noexcept {
    m_completedPreparationTaskCount.fetch_add(
        1u, std::memory_order_relaxed);
}

void WorldRenderPipeline::registerSkeleton(
    container::String modelAsset,
    container::SharedPtr<const Skeleton> skeleton) {
    if (m_preparing) finishPreparation();
    RegisteredModel& registered = m_models[std::move(modelAsset)];
    registered.hierarchyName.clear();
    registered.resolutionError.clear();
    registered.skeleton = std::move(skeleton);
    registered.animations.clear();
    registered.animationErrors.clear();
}

bool WorldRenderPipeline::registerAnimation(
    container::String modelAsset, container::String animationState,
    container::SharedPtr<const AnimationClip> animation) {
    if (m_preparing) finishPreparation();
    RegisteredModel& registered = m_models[std::move(modelAsset)];
    container::String error;
    if (!animation) {
        error = "animation clip is null";
    } else if (!registered.skeleton || registered.skeleton->empty()) {
        error = "model has no resolved skeleton";
    }
    // WW3D does not require HAnim::Get_HName() to equal the model HTree name.
    // Animation is bound by pivot index: model pivots without motion keep
    // their base pose and animation pivots beyond the model are ignored by
    // pose evaluation. Authored assets intentionally rely on this behavior.
    if (!error.empty()) {
        registered.animations.erase(animationState);
        registered.animationErrors[animationState] = std::move(error);
        return false;
    }
    registered.animationErrors.erase(animationState);
    registered.animations[std::move(animationState)] = std::move(animation);
    return true;
}

container::String WorldRenderPipeline::animationRegistrationError(
    container::StringView modelAsset,
    container::StringView animationState) const {
    const auto model = m_models.find(container::String(modelAsset));
    if (model == m_models.end()) return "model is not registered";
    const auto error = model->second.animationErrors.find(
        container::String(animationState));
    return error == model->second.animationErrors.end()
        ? container::String{} : error->second;
}

void WorldRenderPipeline::recordModelResolution(
    container::String modelAsset, container::String diagnostic) {
    if (m_preparing) finishPreparation();
    RegisteredModel& registered = m_models[std::move(modelAsset)];
    if (!registered.skeleton) {
        registered.resolutionError = std::move(diagnostic);
    }
}

void WorldRenderPipeline::recordAnimationResolution(
    container::String modelAsset, container::String animationState,
    container::String diagnostic) {
    if (m_preparing) finishPreparation();
    RegisteredModel& registered = m_models[std::move(modelAsset)];
    if (registered.animations.contains(animationState)) return;
    if (diagnostic.empty()) {
        registered.animationErrors.erase(animationState);
    } else {
        registered.animationErrors[std::move(animationState)] =
            std::move(diagnostic);
    }
}

void WorldRenderPipeline::registerW3dModel(
    container::String modelAsset, const CpuStaticModel& model) {
    if (m_preparing) finishPreparation();
    RegisteredModel& registered = m_models[std::move(modelAsset)];
    registered.hierarchyName = model.hierarchyName;
    registered.resolutionError.clear();
    registered.skeleton = model.skeleton;
    registered.animations.clear();
    registered.animationErrors.clear();
    for (const CpuStaticModel::Animation& animation : model.animations) {
        if (animation.name.empty() || !animation.clip) continue;
        if (registered.skeleton && !registered.skeleton->empty()) {
            registered.animations.emplace(animation.name, animation.clip);
        } else {
            registered.animationErrors.emplace(
                animation.name,
                "model has no resolved skeleton");
        }
    }
}

void WorldRenderPipeline::removeModel(container::StringView modelAsset) {
    if (m_preparing) finishPreparation();
    m_models.erase(container::String(modelAsset));
}

void WorldRenderPipeline::resetPresentationEpoch(
    uint64_t presentationEpoch) {
    if (m_preparing) static_cast<void>(finishPreparation());

    m_models.clear();
    m_localVisibilityMemory.clear();
    m_localVisibilityMemoryEpoch = presentationEpoch;
    m_localVisibilityMemoryFrame = 0;
    m_snapshot = {};
    m_preparedByInput.clear();
    m_resolvedPresentationsByInput.clear();
    m_preparationPoseArena.clear();
    m_preparationVisibilityArena.clear();
    m_previousPoseById.clear();
    m_animationCompletionsByInput.clear();
    m_completionFallbacksByInput.clear();
    m_preparedParticleEmitterInstances.clear();
    m_preparedParticleEmitterBoneWorldTransformArena.clear();
    m_frozenParticleEmitterAnchors.clear();
    m_liveFrozenParticleEmitterAnchors.clear();
    m_staleFrozenParticleEmitterAnchors.clear();
    m_preparedTrackMarks.clear();
    m_preparedCameraSlave = {};
    m_cameraVisualScratch = {};
    m_preparedCameraSlaveTargetPresent = false;
    m_preparedCameraSlaveBoneWorldTransform.reset();
    m_completedFrame = {};
    m_completedFrame.presentationEpoch = presentationEpoch;
    m_previousFrame = {};
    m_hasCompletedFrame = false;
    m_hasPreviousFrame = false;
    m_lastPreparationStats = {};
    m_preparing = false;
}

} // namespace engine::render
