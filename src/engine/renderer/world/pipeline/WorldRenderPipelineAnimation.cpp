#include "engine/renderer/world/pipeline/WorldRenderPipeline.h"

#include "engine/renderer/world/pipeline/WorldRenderPipelineMath.h"
#include "presentation/render/SupportDrawPresentation.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace engine::render {
namespace {

using world_pipeline_detail::finiteVector;
using world_pipeline_detail::floatSwayOrientation;
using world_pipeline_detail::makeEntityTransform;

SkeletonEvaluationScratch& workerSkeletonScratch() {
    // Taskflow workers are stable across frames. Retain temporary pose arrays
    // per executor thread so each 64-entity range reuses its previous peak
    // capacity without sharing mutable state between concurrent tasks.
    thread_local SkeletonEvaluationScratch scratch;
    return scratch;
}

[[nodiscard]] bool sameQuaternion(const RenderQuaternion& lhs,
                                  const RenderQuaternion& rhs) noexcept {
    return lhs.x() == rhs.x() && lhs.y() == rhs.y() &&
           lhs.z() == rhs.z() && lhs.w() == rhs.w();
}

[[nodiscard]] bool sameBoneControls(
    container::Span<const RenderBoneControl> lhs,
    container::Span<const RenderBoneControl> rhs) noexcept {
    if (lhs.size() != rhs.size()) return false;
    for (size_t index = 0; index < lhs.size(); ++index) {
        const RenderBoneControl& left = lhs[index];
        const RenderBoneControl& right = rhs[index];
        if (left.boneName != right.boneName ||
            !(left.translation == right.translation) ||
            !sameQuaternion(left.rotation, right.rotation) ||
            left.boneNameIsPrefix != right.boneNameIsPrefix ||
            left.boneNameSequenceOrdinal != right.boneNameSequenceOrdinal ||
            left.boneNamePrefixFallsBackToBare !=
                right.boneNamePrefixFallsBackToBare) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool sameSupplyBoneState(
    const RenderSupplyBoneState& lhs,
    const RenderSupplyBoneState& rhs) noexcept {
    return lhs.prefix == rhs.prefix &&
           lhs.currentSupply == rhs.currentSupply &&
           lhs.maximumSupply == rhs.maximumSupply &&
           lhs.enabled == rhs.enabled;
}

[[nodiscard]] std::optional<size_t> resolveWeaponLaunchJoint(
    const Skeleton& skeleton, container::StringView prefix,
    uint32_t sequenceOrdinal) {
    container::Span<const size_t> numbered =
        skeleton.numberedJointIndicesInsensitive(prefix);
    if (numbered.size() > 99u) numbered = numbered.first(99u);
    if (!numbered.empty()) {
        const uint32_t sequence = std::max<uint32_t>(1u, sequenceOrdinal);
        return numbered[(sequence - 1u) % numbered.size()];
    }
    return skeleton.findJointIndexInsensitive(prefix);
}


} // namespace


container::Span<RenderMatrix> WorldRenderPipeline::preparationPose(
    const PreparedRenderInstance& instance) noexcept {
    if (instance.poseOffset > m_preparationPoseArena.size() ||
        instance.poseCount >
            m_preparationPoseArena.size() - instance.poseOffset) {
        return {};
    }
    return container::Span<RenderMatrix>(m_preparationPoseArena)
        .subspan(instance.poseOffset, instance.poseCount);
}

container::Span<uint8_t> WorldRenderPipeline::preparationVisibility(
    const PreparedRenderInstance& instance) noexcept {
    if (instance.poseOffset > m_preparationVisibilityArena.size() ||
        instance.poseCount >
            m_preparationVisibilityArena.size() - instance.poseOffset) {
        return {};
    }
    return container::Span<uint8_t>(m_preparationVisibilityArena)
        .subspan(instance.poseOffset, instance.poseCount);
}

container::Span<std::optional<RenderMatrix>>
WorldRenderPipeline::preparationParticleEmitterBoneWorldTransforms(
    const PreparedRenderInstance& instance) noexcept {
    if (instance.particleEmitterBoneOffset >
            m_preparedParticleEmitterBoneWorldTransformArena.size() ||
        instance.particleEmitterBoneCount >
            m_preparedParticleEmitterBoneWorldTransformArena.size() -
                instance.particleEmitterBoneOffset) {
        return {};
    }
    return container::Span<std::optional<RenderMatrix>>(
        m_preparedParticleEmitterBoneWorldTransformArena).subspan(
            instance.particleEmitterBoneOffset,
            instance.particleEmitterBoneCount);
}

bool WorldRenderPipeline::evaluatePose(
    const RenderPoseInput& input,
    container::Span<RenderMatrix> poseOutput,
    container::Span<uint8_t> visibilityOutput) {
    if (!input.skeleton || input.skeleton->empty()) {
        return false;
    }
    if (poseOutput.size() < input.skeleton->joints().size() ||
        (!visibilityOutput.empty() &&
         visibilityOutput.size() < input.skeleton->joints().size())) {
        return false;
    }

    m_poseEvaluationCount.fetch_add(1u, std::memory_order_relaxed);
    m_poseJointCount.fetch_add(
        input.skeleton->joints().size(), std::memory_order_relaxed);
    m_poseControlCount.fetch_add(
        input.boneControls.size(), std::memory_order_relaxed);
    if (input.animation) {
        const uint64_t channelCount = input.animation->channels().size() +
            (!visibilityOutput.empty()
                 ? input.animation->visibilityChannels().size()
                 : 0u);
        m_poseAnimationChannelCount.fetch_add(
            channelCount, std::memory_order_relaxed);
    }

    std::atomic<uint32_t>* evaluationCounter = nullptr;
    std::atomic<uint64_t>* elapsedCounter = nullptr;
    switch (input.consumer) {
    case PoseConsumer::Ordinary:
        evaluationCounter = &m_ordinaryPoseEvaluationCount;
        elapsedCounter = &m_ordinaryPoseNanoseconds;
        break;
    case PoseConsumer::Camera:
        evaluationCounter = &m_cameraPoseEvaluationCount;
        elapsedCounter = &m_cameraPoseNanoseconds;
        break;
    case PoseConsumer::ParticleEmitter:
        evaluationCounter = &m_emitterPoseEvaluationCount;
        elapsedCounter = &m_emitterPoseNanoseconds;
        break;
    case PoseConsumer::TrackMark:
        evaluationCounter = &m_trackMarkPoseEvaluationCount;
        elapsedCounter = &m_trackMarkPoseNanoseconds;
        break;
    }
    evaluationCounter->fetch_add(1u, std::memory_order_relaxed);

    const auto started = std::chrono::steady_clock::now();
    const bool evaluated = evaluateSkeletonPoseIntoSpan(
        *input.skeleton, input.animation.get(), input.animationTimeSeconds,
        input.entityTransform, input.animationMode, input.animationRate,
        input.boneControls, poseOutput,
        visibilityOutput, workerSkeletonScratch());
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count();
    elapsedCounter->fetch_add(
        static_cast<uint64_t>(std::max<int64_t>(0, elapsed)),
        std::memory_order_relaxed);
    return evaluated;
}

bool WorldRenderPipeline::tryReusePreviousPose(
    PreparedRenderInstance& prepared,
    const CompiledPoseSample& sample,
    container::Span<RenderMatrix> poseOutput,
    container::Span<uint8_t> visibilityOutput) {
    prepared.poseAnimationGeneration = sample.animation
        ? sample.animation->generation() : 0u;
    prepared.poseSampleTimeSeconds = sample.timeSeconds;
    prepared.poseSampleMode = sample.mode;
    prepared.poseSampleRate = sample.rate;
    prepared.poseSampleDurationSeconds = sample.animation
        ? sample.animation->completionTimeSeconds(sample.rate) : 0.0f;
    if (!prepared.poseCacheReusable || poseOutput.empty() ||
        visibilityOutput.size() != poseOutput.size()) {
        return false;
    }

    const auto cached = m_previousPoseById.find(prepared.id);
    if (cached == m_previousPoseById.end() ||
        cached->second >= m_completedFrame.visibleInstances.size()) {
        return false;
    }
    const PreparedRenderInstance& previous =
        m_completedFrame.visibleInstances[cached->second];
    if (!previous.poseCacheReusable || !previous.poseReady ||
        !previous.visibilityReady ||
        previous.skeletonGeneration != prepared.skeletonGeneration ||
        previous.poseAnimationGeneration !=
            prepared.poseAnimationGeneration ||
        previous.poseSampleTimeSeconds != prepared.poseSampleTimeSeconds ||
        previous.poseSampleMode != prepared.poseSampleMode ||
        previous.poseSampleRate != prepared.poseSampleRate ||
        previous.poseSampleDurationSeconds !=
            prepared.poseSampleDurationSeconds ||
        !(previous.worldTransform == prepared.worldTransform) ||
        !sameBoneControls(previous.visual.boneControls,
                          prepared.visual.boneControls) ||
        !sameSupplyBoneState(previous.visual.supplyBones,
                             prepared.visual.supplyBones)) {
        return false;
    }

    const container::Span<const RenderMatrix> previousPose =
        m_completedFrame.pose(previous);
    const container::Span<const uint8_t> previousVisibility =
        m_completedFrame.visibility(previous);
    if (previousPose.size() != poseOutput.size() ||
        previousVisibility.size() != visibilityOutput.size()) {
        return false;
    }
    std::copy(previousPose.begin(), previousPose.end(), poseOutput.begin());
    std::copy(previousVisibility.begin(), previousVisibility.end(),
              visibilityOutput.begin());
    prepared.poseReady = true;
    prepared.visibilityReady = true;
    m_poseReuseCount.fetch_add(1u, std::memory_order_relaxed);
    return true;
}

WorldRenderPipeline::CompiledPoseSample
WorldRenderPipeline::compilePoseSample(
    RenderVisualState& visual, const RegisteredModel& model,
    RenderEntityId objectId, uint32_t channelIndex) {
    if (visual.debris.enabled) {
        std::optional<float> initialDuration;
        const auto initial = model.animations.find(
            visual.debris.initialAnimation);
        if (initial != model.animations.end() && initial->second) {
            initialDuration = initial->second->completionTimeSeconds();
        }
        const ResolvedDebrisAnimation debris = resolveDebrisAnimation(
            visual.debris, initialDuration);
        visual.animationState = debris.animation;
        visual.animationTimeSeconds = debris.timeSeconds;
        visual.animationMode = debris.mode;
        visual.animationManualFrame = debris.manualFrame;
        visual.animationRate = 1.0f;
        // The debris clip replaces whatever the condition state selected, so
        // any distanceCovered published for that clip no longer describes it.
        visual.animationSpeedSyncDurationSeconds = 0.0f;
    }

    CompiledPoseSample sample{
        .timeSeconds = visual.animationTimeSeconds,
        .mode = visual.animationMode,
        .rate = visual.animationRate,
    };
    const auto animation = model.animations.find(visual.animationState);
    if (animation == model.animations.end() || !animation->second) {
        visual.animationCompleted = false;
        return sample;
    }
    sample.animation = animation->second;

    if (visual.policeCar.enabled && sample.animation->frameRate() != 0u) {
        visual.policeCar.animationFrame = policeCarAnimationFrame(
            objectId, channelIndex, visual.policeCar.simulationFrame,
            static_cast<float>(sample.animation->frameCount()));
        visual.policeCar.diffuseColor = policeCarLightColor(
            visual.policeCar.animationFrame);
        visual.policeCar.ambientColor =
            visual.policeCar.diffuseColor * 0.5f;
        visual.animationTimeSeconds =
            visual.policeCar.animationFrame /
            static_cast<float>(sample.animation->frameRate());
        visual.animationMode = RenderAnimationMode::Once;
        visual.animationRate = 1.0f;
        visual.animationSpeedSyncDurationSeconds = 0.0f;
    }

    const float requestedDuration = visual.animationLoopDurationSeconds;
    // RefCode order of precedence: setAnimationLoopDuration() is a gameplay
    // request applied when the duration changes, while
    // adjustAnimSpeedToMovementSpeed() re-derives the multiplier from the live
    // speed every client frame afterwards. The presentation resolver already
    // installed that speed-derived rate, so an authored loop duration must not
    // overwrite it here.
    const bool movementSpeedSynchronized =
        visual.animationSpeedSyncDurationSeconds > 0.0f &&
        std::isfinite(visual.animationSpeedSyncDurationSeconds);
    if (!movementSpeedSynchronized && requestedDuration > 0.0f &&
        std::isfinite(requestedDuration) &&
        visual.animationMode != RenderAnimationMode::Manual) {
        const float authoredDuration =
            sample.animation->completionTimeSeconds(1.0f);
        if (authoredDuration > 0.0f && std::isfinite(authoredDuration)) {
            visual.animationRate = authoredDuration / requestedDuration;
        }
    }
    visual.animationCompleted = sample.animation->isComplete(
        visual.animationTimeSeconds, visual.animationMode,
        visual.animationRate);
    if (visual.animationCompleted) {
        const float completionTime = sample.animation->completionTimeSeconds(
            visual.animationRate);
        if (std::isfinite(completionTime)) {
            visual.animationTimeSeconds = std::min(
                std::max(0.0f, visual.animationTimeSeconds),
                completionTime);
        }
    }

    sample.timeSeconds = visual.animationTimeSeconds;
    sample.mode = visual.animationMode;
    sample.rate = visual.animationRate;
    if (sample.mode == RenderAnimationMode::Manual &&
        sample.animation->frameRate() != 0u) {
        // Drawable::setAnimationFrame is a literal frame override. Do not
        // reinterpret it through a seconds clock or playback-rate scaling.
        sample.timeSeconds = static_cast<float>(visual.animationManualFrame) /
            static_cast<float>(sample.animation->frameRate());
        sample.rate = 1.0f;
    }
    return sample;
}

WorldRenderPipeline::PrepareOutcome WorldRenderPipeline::prepareInstance(size_t index) {
    const auto& instance = m_snapshot.entities[index];
    // Completion is presentation-clock state, not visibility state. Resolve
    // it before hidden/distance/frustum exits so culling cannot postpone a
    // Once/Transition acknowledgement or change merge order.
    ResolvedAnimationPresentation& presentation =
        m_resolvedPresentationsByInput[index];
    resolveAnimationPresentationInto(presentation, instance);
    m_animationCompletionsByInput[index] = presentation.completions;
    m_completionFallbacksByInput[index] =
        presentation.terminalFallbackCompletions;
    if (instance.id == 0 || !finiteVector(instance.transform.position)) {
        return PrepareOutcome::Invalid;
    }
    if (instance.visual.hidden) return PrepareOutcome::Hidden;
    // Camera/range/frustum visibility is a render-view decision. Preparing
    // every valid non-hidden entity here lets one pose/attachment result serve
    // multiple render frames and camera revisions.
    PreparedRenderInstance& prepared = m_preparedByInput[index];
    prepared.id = instance.id;
    prepared.objectId = instance.objectId != 0 ? instance.objectId : instance.id;
    prepared.channelIndex = instance.channelIndex;
    prepared.modelAsset = presentation.modelAsset;
    prepared.visual = presentation.visual;
    RenderTransform presentedTransform = instance.transform;
    if (presentation.visual.floatSwayEnabled) {
        presentedTransform.orientation = floatSwayOrientation(
            presentation.visual.floatSwayBaseYawRadians,
            presentation.visual.floatSwaySampleTick);
    }
    prepared.worldTransform = makeEntityTransform(presentedTransform);
    prepared.boundingRadius = instance.boundingRadius;
    prepared.cullingCenterOffset = instance.cullingCenterOffset;
    prepared.directionalLightScale = instance.directionalLightScale;
    prepared.interpolationDisabled = instance.interpolationDisabled;
    prepared.shadow = instance.shadow;
    prepared.skeleton.reset();
    prepared.skeletonGeneration = 0;
    // Both attachment forms rewrite this channel's pose palette in place after
    // preparation, so the published palette is no longer a pure function of the
    // sampled animation. Reusing it next frame would compose the attachment
    // delta a second time.
    prepared.poseCacheReusable =
        (instance.containerObjectId == 0u ||
         instance.attachToBoneInContainer.empty()) &&
        instance.attachToBoneInAnotherModule.empty();
    prepared.poseReady = false;
    prepared.visibilityReady = false;
    prepared.particleEmitterBoneOffset = 0;
    prepared.particleEmitterBoneCount = 0;
    prepared.weaponLaunchBoneWorldTransforms = {};

    bool posed = false;
    const auto modelIt = m_models.find(prepared.modelAsset);
    if (modelIt != m_models.end() && modelIt->second.skeleton && !modelIt->second.skeleton->empty()) {
        prepared.skeleton = modelIt->second.skeleton;
        prepared.skeletonGeneration = prepared.skeleton->generation();
        const size_t jointCount = modelIt->second.skeleton->joints().size();
        if (prepared.poseCount < jointCount) {
            return PrepareOutcome::PreparedStatic;
        }
        prepared.poseCount = jointCount;
        const CompiledPoseSample sample = compilePoseSample(
            prepared.visual, modelIt->second, prepared.objectId,
            prepared.channelIndex);
        container::Span<RenderMatrix> pose = preparationPose(prepared);
        container::Span<uint8_t> visibility =
            preparationVisibility(prepared);
        if (!tryReusePreviousPose(prepared, sample, pose, visibility)) {
            prepared.poseReady = evaluatePose(
                RenderPoseInput{
                    .consumer = PoseConsumer::Ordinary,
                    .skeleton = modelIt->second.skeleton,
                    .animation = sample.animation,
                    .animationTimeSeconds = sample.timeSeconds,
                    // Evaluate the pose from the SWAYED transform.  When a model
                    // has a skeleton, appendDrawPackets uses the pose's
                    // skinPalette entry as the packet transform, so feeding the
                    // unswayed instance.transform here made float sway invisible
                    // on every such object — and the per-tick swaying
                    // worldTransform still defeated tryReusePreviousPose, forcing
                    // a full skeleton re-evaluation every frame for a pose that
                    // never actually changed.
                    .entityTransform = presentedTransform,
                    .animationMode = sample.mode,
                    .animationRate = sample.rate,
                    .boneControls = prepared.visual.boneControls,
                },
                pose, visibility);
            prepared.visibilityReady = prepared.poseReady;
        }
        if (prepared.visual.supplyBones.enabled &&
            !prepared.visual.supplyBones.prefix.empty()) {
            const container::Span<const size_t> supplyBones =
                modelIt->second.skeleton->numberedJointIndicesInsensitive(
                    prepared.visual.supplyBones.prefix);
            const uint32_t shown = supplyBonesToShow(
                static_cast<uint32_t>(supplyBones.size()),
                prepared.visual.supplyBones.currentSupply,
                prepared.visual.supplyBones.maximumSupply);
            for (size_t ordinal = 0; ordinal < supplyBones.size(); ++ordinal) {
                const size_t bone = supplyBones[ordinal];
                if (bone < visibility.size()) {
                    visibility[bone] = ordinal < shown ? 1u : 0u;
                }
            }
        }
        for (size_t slot = 0; slot < presentation.weaponLaunchBones.size(); ++slot) {
            if (presentation.weaponLaunchBones[slot].empty()) continue;
            const std::optional<size_t> boneIndex = resolveWeaponLaunchJoint(
                *modelIt->second.skeleton,
                presentation.weaponLaunchBones[slot],
                presentation.weaponLaunchBoneSequenceOrdinals[slot]);
            if (boneIndex && *boneIndex < pose.size()) {
                prepared.weaponLaunchBoneWorldTransforms[slot] =
                    pose[*boneIndex];
            }
        }
        posed = prepared.poseReady;
    }
    return posed ? PrepareOutcome::PreparedAnimated : PrepareOutcome::PreparedStatic;
}

void WorldRenderPipeline::prepareRange(size_t beginIndex, size_t endIndex) {
    container::Array<uint32_t, 3> counts{};
    for (size_t index = beginIndex; index < endIndex; ++index) {
        switch (prepareInstance(index)) {
        case PrepareOutcome::PreparedStatic:
            break;
        case PrepareOutcome::PreparedAnimated:
            ++counts[0];
            break;
        case PrepareOutcome::Invalid:
            ++counts[1];
            break;
        case PrepareOutcome::Hidden:
            ++counts[2];
            break;
        }
    }
    m_animatedPreparedCount.fetch_add(counts[0], std::memory_order_relaxed);
    m_invalidCount.fetch_add(counts[1], std::memory_order_relaxed);
    m_hiddenCount.fetch_add(counts[2], std::memory_order_relaxed);
}

} // namespace engine::render
