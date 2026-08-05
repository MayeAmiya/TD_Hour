#include "engine/renderer/world/pipeline/WorldRenderPipeline.h"

#include "engine/renderer/runtime/RenderParallelExecutor.h"
#include "engine/renderer/world/pipeline/WorldRenderPipelineMath.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <utility>

namespace engine::render {
namespace {

using world_pipeline_detail::finiteVector;

[[nodiscard]] bool sameSubObjectVisibility(
    container::Span<const RenderSubObjectVisibility> lhs,
    container::Span<const RenderSubObjectVisibility> rhs) noexcept {
    if (lhs.size() != rhs.size()) return false;
    for (size_t index = 0; index < lhs.size(); ++index) {
        const RenderSubObjectVisibility& left = lhs[index];
        const RenderSubObjectVisibility& right = rhs[index];
        if (left.name != right.name || left.visible != right.visible ||
            left.nameIsPrefix != right.nameIsPrefix ||
            left.nameSequenceOrdinal != right.nameSequenceOrdinal ||
            left.namePrefixFallsBackToBare !=
                right.namePrefixFallsBackToBare) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool crossesAnimationLoopBoundary(
    const PreparedRenderInstance& previous,
    const PreparedRenderInstance& current) noexcept {
    if (current.poseAnimationGeneration == 0u) return false;
    if (current.poseSampleMode != RenderAnimationMode::Loop &&
        current.poseSampleMode != RenderAnimationMode::LoopBackwards) {
        return false;
    }
    const float duration = current.poseSampleDurationSeconds;
    if (!std::isfinite(duration) || duration <= 0.0f ||
        !std::isfinite(previous.poseSampleTimeSeconds) ||
        !std::isfinite(current.poseSampleTimeSeconds)) {
        return false;
    }
    if (current.poseSampleTimeSeconds < previous.poseSampleTimeSeconds) {
        return true;
    }
    const double inverseDuration = 1.0 / static_cast<double>(duration);
    const double previousCycle = std::floor(
        static_cast<double>(std::max(0.0f, previous.poseSampleTimeSeconds)) *
        inverseDuration);
    const double currentCycle = std::floor(
        static_cast<double>(std::max(0.0f, current.poseSampleTimeSeconds)) *
        inverseDuration);
    return previousCycle != currentCycle;
}

} // namespace

void WorldRenderPipeline::beginPreparation(WorldRenderSnapshot snapshot,
                                           uint32_t viewportWidth,
                                           uint32_t viewportHeight) {
    if (m_preparing) finishPreparation();
    m_containerCapacityGrowths = 0;
    const auto countCapacityGrowth = [this](size_t before,
                                             size_t after) noexcept {
        if (after > before &&
            m_containerCapacityGrowths !=
                std::numeric_limits<uint32_t>::max()) {
            ++m_containerCapacityGrowths;
        }
    };
    snapshot.sealSharedColumns();
    applyLocalVisibilityObjectMemory(snapshot);
    m_snapshot = std::move(snapshot);
    static_cast<void>(viewportWidth);
    static_cast<void>(viewportHeight);
    // Keep the completed world intact while workers prepare the next endpoint.
    // The renderer can therefore present A repeatedly while B is in flight.
    // finishPreparation() swaps old completed buffers back into these scratch
    // vectors only at the publication boundary.
    m_preparationPoseArena.clear();
    m_preparationVisibilityArena.clear();
    m_previousPoseById.clear();
    const bool previousPoseCacheCompatible = m_hasCompletedFrame &&
        m_completedFrame.presentationEpoch == m_snapshot.presentationEpoch &&
        m_completedFrame.sessionRevision == m_snapshot.sessionRevision;
    if (previousPoseCacheCompatible) {
        m_previousPoseById.reserve(
            m_completedFrame.visibleInstances.size());
        for (size_t index = 0;
             index < m_completedFrame.visibleInstances.size(); ++index) {
            const PreparedRenderInstance& previous =
                m_completedFrame.visibleInstances[index];
            if (previous.id != 0u && previous.poseReady &&
                previous.visibilityReady && previous.poseCacheReusable) {
                m_previousPoseById.try_emplace(previous.id, index);
            }
        }
    }
    // m_preparedByInput is a dedicated in-flight store. It must not steal the
    // currently displayed instance payloads from m_completedFrame.
    const size_t preparedCapacityBefore = m_preparedByInput.capacity();
    m_preparedByInput.resize(m_snapshot.entities.size());
    countCapacityGrowth(preparedCapacityBefore, m_preparedByInput.capacity());
    for (PreparedRenderInstance& prepared : m_preparedByInput) {
        // Culled/invalid inputs never reach prepareInstance's writeback.  A
        // zero identity keeps their retained payload strictly scratch-only.
        prepared.id = 0;
        prepared.poseOffset = 0;
        prepared.poseCount = 0;
        prepared.poseReady = false;
        prepared.visibilityReady = false;
        prepared.poseAnimationGeneration = 0;
        prepared.poseSampleTimeSeconds = 0.0f;
        prepared.poseSampleRate = 1.0f;
        prepared.poseSampleDurationSeconds = 0.0f;
        prepared.poseSampleMode = RenderAnimationMode::Loop;
        prepared.poseCacheReusable = false;
        prepared.particleEmitterBoneOffset = 0;
        prepared.particleEmitterBoneCount = 0;
        prepared.interpolationDisabled = false;
        prepared.weaponLaunchBoneWorldTransforms = {};
    }
    const size_t resolvedCapacityBefore =
        m_resolvedPresentationsByInput.capacity();
    m_resolvedPresentationsByInput.resize(m_snapshot.entities.size());
    countCapacityGrowth(
        resolvedCapacityBefore, m_resolvedPresentationsByInput.capacity());
    for (ResolvedAnimationPresentation& resolved :
         m_resolvedPresentationsByInput) {
        resolved.completions.clear();
        resolved.terminalFallbackCompletions = 0;
    }

    size_t poseCount = 0;
    m_poseArenaRequestedJoints = 0;
    m_poseArenaRejectedInstances = 0;
    const size_t jointLimit = std::min({
        performance_limits::kMaximumFramePoseJoints,
        m_preparationPoseArena.max_size(),
        m_preparationVisibilityArena.max_size()});
    const auto skeletonJointCount = [this](
        const container::String& modelAsset) noexcept {
        const auto model = m_models.find(modelAsset);
        return model != m_models.end() && model->second.skeleton
            ? model->second.skeleton->joints().size() : size_t{0};
    };
    for (size_t index = 0; index < m_preparedByInput.size(); ++index) {
        const RenderEntitySnapshot& entity = m_snapshot.entities[index];
        // Ordinary pose slices are not needed for inputs that prepareInstance
        // will reject before touching the ordinary arena. CameraSlave,
        // ParticleSysBone and TrackMark consumers have their own preparation
        // streams and must not be reintroduced into this admission filter.
        if (entity.id == 0 || entity.visual.hidden ||
            !finiteVector(entity.transform.position)) {
            continue;
        }
        size_t jointCount = skeletonJointCount(entity.modelAsset);
        if (entity.animationCompletionTarget) {
            jointCount = std::max(jointCount, skeletonJointCount(
                entity.animationCompletionTarget->modelAsset));
        }
        if (entity.animationFinalTarget) {
            jointCount = std::max(jointCount, skeletonJointCount(
                entity.animationFinalTarget->modelAsset));
        }
        if (jointCount == 0u) continue;
        m_poseArenaRequestedJoints = jointCount >
                std::numeric_limits<uint64_t>::max() -
                    m_poseArenaRequestedJoints
            ? std::numeric_limits<uint64_t>::max()
            : m_poseArenaRequestedJoints + jointCount;
        if (jointCount > jointLimit - poseCount) {
            if (m_poseArenaRejectedInstances !=
                std::numeric_limits<uint32_t>::max()) {
                ++m_poseArenaRejectedInstances;
            }
            continue;
        }
        PreparedRenderInstance& prepared = m_preparedByInput[index];
        prepared.poseOffset = poseCount;
        prepared.poseCount = jointCount;
        poseCount += jointCount;
    }
    const size_t poseCapacityBefore = m_preparationPoseArena.capacity();
    const size_t visibilityCapacityBefore =
        m_preparationVisibilityArena.capacity();
    if (poseCapacityBefore < performance_limits::kInitialFramePoseJoints) {
        m_preparationPoseArena.reserve(
            performance_limits::kInitialFramePoseJoints);
    }
    if (visibilityCapacityBefore <
        performance_limits::kInitialFramePoseJoints) {
        m_preparationVisibilityArena.reserve(
            performance_limits::kInitialFramePoseJoints);
    }
    m_preparationPoseArena.resize(poseCount);
    m_preparationVisibilityArena.resize(poseCount);
    m_poseArenaGrowths = m_preparationPoseArena.capacity() > poseCapacityBefore
        ? 1u : 0u;
    m_visibilityArenaGrowths =
        m_preparationVisibilityArena.capacity() > visibilityCapacityBefore
        ? 1u : 0u;
    countCapacityGrowth(
        poseCapacityBefore, m_preparationPoseArena.capacity());
    countCapacityGrowth(
        visibilityCapacityBefore, m_preparationVisibilityArena.capacity());
    m_poseArenaCapacityHighWaterBytes = std::max(
        m_poseArenaCapacityHighWaterBytes,
        static_cast<uint64_t>(m_preparationPoseArena.capacity()) *
            sizeof(RenderMatrix));
    m_visibilityArenaCapacityHighWaterBytes = std::max(
        m_visibilityArenaCapacityHighWaterBytes,
        static_cast<uint64_t>(m_preparationVisibilityArena.capacity()) *
            sizeof(uint8_t));
    const size_t completionInputCapacityBefore =
        m_animationCompletionsByInput.capacity();
    const size_t fallbackInputCapacityBefore =
        m_completionFallbacksByInput.capacity();
    m_animationCompletionsByInput.resize(m_snapshot.entities.size());
    m_completionFallbacksByInput.resize(m_snapshot.entities.size());
    countCapacityGrowth(
        completionInputCapacityBefore,
        m_animationCompletionsByInput.capacity());
    countCapacityGrowth(
        fallbackInputCapacityBefore,
        m_completionFallbacksByInput.capacity());
    m_preparedParticleEmitterBoneWorldTransformArena.clear();
    m_preparedParticleEmitterInstances.clear();
    const size_t preparedTrackCapacityBefore = m_preparedTrackMarks.capacity();
    m_snapshot.trackMarks.copyTo(m_preparedTrackMarks);
    countCapacityGrowth(
        preparedTrackCapacityBefore, m_preparedTrackMarks.capacity());
    m_preparedCameraSlave = m_snapshot.cameraSlave;
    m_preparedCameraSlaveTargetPresent = false;
    m_preparedCameraSlaveBoneWorldTransform.reset();
    m_animatedPreparedCount.store(0, std::memory_order_relaxed);
    m_hiddenCount.store(0, std::memory_order_relaxed);
    m_invalidCount.store(0, std::memory_order_relaxed);
    m_poseEvaluationCount.store(0, std::memory_order_relaxed);
    m_poseReuseCount.store(0, std::memory_order_relaxed);
    m_ordinaryPoseEvaluationCount.store(0, std::memory_order_relaxed);
    m_cameraPoseEvaluationCount.store(0, std::memory_order_relaxed);
    m_emitterPoseEvaluationCount.store(0, std::memory_order_relaxed);
    m_trackMarkPoseEvaluationCount.store(0, std::memory_order_relaxed);
    m_cameraPoseReuseCount.store(0, std::memory_order_relaxed);
    m_emitterPoseReuseCount.store(0, std::memory_order_relaxed);
    m_completionFallbackCount = 0;
    m_cameraPoseFallbackCount.store(0, std::memory_order_relaxed);
    m_emitterRootFallbackCount.store(0, std::memory_order_relaxed);
    m_poseJointCount.store(0, std::memory_order_relaxed);
    m_poseAnimationChannelCount.store(0, std::memory_order_relaxed);
    m_poseControlCount.store(0, std::memory_order_relaxed);
    m_ordinaryPoseNanoseconds.store(0, std::memory_order_relaxed);
    m_cameraPoseNanoseconds.store(0, std::memory_order_relaxed);
    m_emitterPoseNanoseconds.store(0, std::memory_order_relaxed);
    m_trackMarkPoseNanoseconds.store(0, std::memory_order_relaxed);
    m_preparationStarted = std::chrono::steady_clock::now();
    m_taskflow.clear();
    m_preparationTaskCount = 0;
    m_scheduledPreparationTaskCount = 0;
    m_completedPreparationTaskCount.store(0, std::memory_order_relaxed);
    m_preparationWorkerMask.store(0, std::memory_order_relaxed);
    const size_t entityTaskCapacityBefore =
        m_entityPreparationTasks.capacity();
    m_entityPreparationTasks.clear();
    m_entityPreparationTasks.reserve(
        (m_snapshot.entities.size() +
         performance_limits::kWorldPreparationEntityGrain - 1u) /
        performance_limits::kWorldPreparationEntityGrain);
    countCapacityGrowth(
        entityTaskCapacityBefore, m_entityPreparationTasks.capacity());
    for (size_t beginIndex = 0; beginIndex < m_snapshot.entities.size();
         beginIndex += performance_limits::kWorldPreparationEntityGrain) {
        const size_t endIndex = std::min(
            beginIndex + performance_limits::kWorldPreparationEntityGrain,
            m_snapshot.entities.size());
        m_entityPreparationTasks.push_back(
            m_taskflow.emplace([this, beginIndex, endIndex] {
                const platform::runtime::ThreadRoleScope role(
                    platform::runtime::ThreadRole::RenderWorker);
                recordPreparationTaskStart();
                prepareRange(beginIndex, endIndex);
                recordPreparationTaskCompletion();
            }));
        ++m_preparationTaskCount;
        ++m_scheduledPreparationTaskCount;
    }
    // Camera-slave evaluation is intentionally independent of ordinary
    // visibility. A cinematic camera bone may be outside the prior tactical
    // frustum or belong to a hidden drawable, yet W3DView still samples it.
    tf::Task cameraSlaveTask = m_taskflow.emplace([this] {
        const platform::runtime::ThreadRoleScope role(
            platform::runtime::ThreadRole::RenderWorker);
        recordPreparationTaskStart();
        prepareCameraSlave();
        recordPreparationTaskCompletion();
    });
    ++m_scheduledPreparationTaskCount;
    // Track history is likewise independent of camera and drawable culling.
    // A vehicle leaving the current frustum must retain its renderer-owned
    // trail, so resolve authored width from the sealed entity/rest pose in a
    // separate preparation stream rather than from visibleInstances.
    tf::Task trackMarkTask = m_taskflow.emplace([this] {
        const platform::runtime::ThreadRoleScope role(
            platform::runtime::ThreadRole::RenderWorker);
        recordPreparationTaskStart();
        prepareTrackMarks();
        recordPreparationTaskCompletion();
    });
    ++m_scheduledPreparationTaskCount;
    // ParticleSysBone lifetime follows live presentation state, not ordinary
    // mesh visibility. Prepare its root/optional animated-bone poses from the
    // complete sealed entity set in a distinct stream.
    tf::Task particleEmitterTask = m_taskflow.emplace([this] {
        const platform::runtime::ThreadRoleScope role(
            platform::runtime::ThreadRole::RenderWorker);
        recordPreparationTaskStart();
        prepareParticleEmitterInstances();
        recordPreparationTaskCompletion();
    });
    ++m_scheduledPreparationTaskCount;
    // Camera and ParticleSysBone may reuse a visible channel's completed pose.
    // The single camera demand precedes the emitter stream so an off-screen
    // channel requested by both also writes its arena slice only once.
    for (tf::Task& task : m_entityPreparationTasks) {
        task.precede(cameraSlaveTask);
        task.precede(trackMarkTask);
    }
    cameraSlaveTask.precede(particleEmitterTask);
    m_pendingWork = parallelExecutor().run(m_taskflow);
    m_preparing = true;
}


bool WorldRenderPipeline::isPreparing() const {
    return m_preparing && m_pendingWork.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

const PreparedWorldFrame& WorldRenderPipeline::finishPreparation() {
    if (!m_preparing) return m_completedFrame;
    m_pendingWork.get();
    // Rotate three physical world slots without copying their large vectors:
    // previous A <- completed B <- freshly prepared C. The retired A storage
    // becomes the new completed destination and its buffers are exchanged back
    // into preparation scratch as C is published.
    if (m_hasCompletedFrame) {
        if (m_hasPreviousFrame) {
            PreparedWorldFrame retired = std::move(m_previousFrame);
            m_previousFrame = std::move(m_completedFrame);
            m_completedFrame = std::move(retired);
        } else {
            m_previousFrame = std::move(m_completedFrame);
            m_completedFrame = {};
        }
        m_hasPreviousFrame = true;
    }
    m_completedFrame.stamp = {
        .worldRevision = m_snapshot.simulationFrame,
        .simulationFrame = m_snapshot.simulationFrame,
        .presentationEpoch = m_snapshot.presentationEpoch,
        .sessionRevision = m_snapshot.sessionRevision,
        .loadingRevision = m_snapshot.loadingRevision,
    };
    m_completedFrame.sourceView = {
        .sourceWorld = m_completedFrame.stamp,
        .viewRevision = 0,
        .worldARevision = m_snapshot.simulationFrame,
        .worldBRevision = m_snapshot.simulationFrame,
        .camera = m_snapshot.camera,
        .viewport = {},
        .interpolationAlpha = 1.0f,
    };
    m_completedFrame.simulationFrame = m_snapshot.simulationFrame;
    m_completedFrame.presentationEpoch = m_snapshot.presentationEpoch;
    m_completedFrame.sessionRevision = m_snapshot.sessionRevision;
    m_completedFrame.loadingRevision = m_snapshot.loadingRevision;
    m_completedFrame.renderGameDataSettings =
        m_snapshot.renderGameDataSettings;
    m_completedFrame.renderFeatureQuality =
        m_snapshot.renderFeatureQuality;
    m_completedFrame.camera = m_snapshot.camera;
    m_completedFrame.screenFade = m_snapshot.screenFade;
    m_completedFrame.blackAndWhite = m_snapshot.blackAndWhite;
    m_completedFrame.motionBlur = std::move(m_snapshot.motionBlur);
    m_completedFrame.cameraSlave = std::move(m_preparedCameraSlave);
    m_completedFrame.cameraSlaveTargetPresent = m_preparedCameraSlaveTargetPresent;
    m_completedFrame.cameraSlaveBoneWorldTransform =
        std::move(m_preparedCameraSlaveBoneWorldTransform);
    m_completedFrame.skybox = m_snapshot.skybox;
    m_completedFrame.treeSway = m_snapshot.treeSway;
    m_completedFrame.weather = std::move(m_snapshot.weather);
    m_completedFrame.screenShake = std::move(m_snapshot.screenShake);
    m_completedFrame.clientOptions = m_snapshot.clientOptions;
    m_completedFrame.objectIcons = std::move(m_snapshot.objectIcons);
    m_completedFrame.worldFeedback = std::move(m_snapshot.worldFeedback);
    m_completedFrame.objectUi = std::move(m_snapshot.objectUi);
    m_completedFrame.tacticalRadar = std::move(m_snapshot.tacticalRadar);
    m_snapshot.terrainBibs.copyTo(m_completedFrame.terrainBibs);
    m_completedFrame.viewCompatibility = m_snapshot.viewCompatibility;
    m_completedFrame.localVisibility = std::move(m_snapshot.localVisibility);
    m_completedFrame.terrain = std::move(m_snapshot.terrain);
    m_snapshot.animationEndpointAdmissions.copyTo(
        m_completedFrame.animationEndpointAdmissions);
    // Keep both sides of the frame handoff as retained buffers. The completed
    // frame receives this frame's values, while the old completed capacity
    // returns to preparation scratch for the next frame instead of being
    // discarded with a moved-from vector.
    m_completedFrame.trackMarks.swap(m_preparedTrackMarks);
    const auto countCapacityGrowth = [this](size_t before,
                                             size_t after) noexcept {
        if (after > before &&
            m_containerCapacityGrowths !=
                std::numeric_limits<uint32_t>::max()) {
            ++m_containerCapacityGrowths;
        }
    };
    // Intra-object Draw-module attachment first: a dependent channel outside
    // this object may then be parented to a bone that has already followed its
    // own sibling module, exactly as RefCode's per-drawable module order does.
    resolveModuleAttachments();
    resolveContainerAttachments();
    for (const uint32_t fallbackCount : m_completionFallbacksByInput) {
        m_completionFallbackCount = fallbackCount >
                std::numeric_limits<uint32_t>::max() -
                    m_completionFallbackCount
            ? std::numeric_limits<uint32_t>::max()
            : m_completionFallbackCount + fallbackCount;
    }
    const size_t completionCapacityBefore =
        m_completedFrame.animationCompletions.capacity();
    m_completedFrame.animationCompletions.clear();
    for (auto& completions : m_animationCompletionsByInput) {
        m_completedFrame.animationCompletions.insert(
            m_completedFrame.animationCompletions.end(),
            std::make_move_iterator(completions.begin()),
            std::make_move_iterator(completions.end()));
    }
    countCapacityGrowth(
        completionCapacityBefore,
        m_completedFrame.animationCompletions.capacity());
    const size_t visibleCapacityBefore =
        m_completedFrame.visibleInstances.capacity();
    m_completedFrame.visibleInstances.clear();
    m_completedFrame.visibleInstances.reserve(m_preparedByInput.size());
    countCapacityGrowth(
        visibleCapacityBefore,
        m_completedFrame.visibleInstances.capacity());
    for (auto& prepared : m_preparedByInput)
    {
        if (prepared.id != 0) m_completedFrame.visibleInstances.push_back(std::move(prepared));
    }
    m_completedFrame.hot.rebuild(m_completedFrame.visibleInstances);
    m_completedFrame.visibleInstanceIndicesById.clear();
    m_completedFrame.visibleInstanceIndicesById.reserve(
        m_completedFrame.visibleInstances.size());
    for (size_t index = 0;
         index < m_completedFrame.visibleInstances.size(); ++index) {
        m_completedFrame.visibleInstanceIndicesById.try_emplace(
            m_completedFrame.visibleInstances[index].id,
            index);
    }
    m_completedFrame.previousEndpointIndices.assign(
        m_completedFrame.visibleInstances.size(), UINT32_MAX);
    m_completedFrame.retiredPreviousEndpointIndices.clear();
    const bool previousEndpointCompatible = m_hasPreviousFrame &&
        m_previousFrame.stamp.presentationEpoch ==
            m_completedFrame.stamp.presentationEpoch &&
        m_previousFrame.stamp.sessionRevision ==
            m_completedFrame.stamp.sessionRevision &&
        m_previousFrame.hot.validFor(
            m_previousFrame.visibleInstances.size()) &&
        m_completedFrame.hot.validFor(
            m_completedFrame.visibleInstances.size()) &&
        m_previousFrame.visibleInstanceIndicesById.size() ==
            m_previousFrame.visibleInstances.size() &&
        m_previousFrame.visibleInstances.size() <=
            static_cast<size_t>(UINT32_MAX);
    if (previousEndpointCompatible) {
        container::Vector<uint8_t> previousMatched(
            m_previousFrame.hot.ids.size(), 0u);
        for (size_t index = 0; index < m_completedFrame.hot.ids.size();
             ++index) {
            const auto previous =
                m_previousFrame.visibleInstanceIndicesById.find(
                m_completedFrame.hot.ids[index]);
            if (previous !=
                m_previousFrame.visibleInstanceIndicesById.end()) {
                m_completedFrame.previousEndpointIndices[index] =
                    static_cast<uint32_t>(previous->second);
                previousMatched[previous->second] = 1u;
            }
        }
        m_completedFrame.retiredPreviousEndpointIndices.reserve(
            previousMatched.size());
        for (size_t index = 0; index < previousMatched.size(); ++index) {
            if (previousMatched[index] == 0u) {
                m_completedFrame.retiredPreviousEndpointIndices.push_back(
                    static_cast<uint32_t>(index));
            }
        }
    }
    m_completedFrame.particleEmitterInstances.swap(
        m_preparedParticleEmitterInstances);
    m_completedFrame.particleEmitterBoneWorldTransformArena.swap(
        m_preparedParticleEmitterBoneWorldTransformArena);
    m_completedFrame.poseArena.swap(m_preparationPoseArena);
    m_completedFrame.visibilityArena.swap(
        m_preparationVisibilityArena);
    m_completedFrame.interpolationEligible.assign(
        m_completedFrame.visibleInstances.size(), 0u);
    if (previousEndpointCompatible) {
        for (size_t index = 0;
             index < m_completedFrame.visibleInstances.size(); ++index) {
            const uint32_t previousIndex =
                m_completedFrame.previousEndpointIndices[index];
            if (previousIndex == UINT32_MAX ||
                previousIndex >= m_previousFrame.visibleInstances.size()) {
                continue;
            }
            const PreparedRenderInstance& current =
                m_completedFrame.visibleInstances[index];
            const PreparedRenderInstance& previous =
                m_previousFrame.visibleInstances[previousIndex];
            if (current.interpolationDisabled ||
                previous.interpolationDisabled ||
                current.visual.animationStateEnterTick !=
                    previous.visual.animationStateEnterTick ||
                !sameSubObjectVisibility(
                    current.visual.subObjectVisibility,
                    previous.visual.subObjectVisibility)) {
                continue;
            }
            if (current.id != previous.id ||
                current.modelAsset != previous.modelAsset ||
                current.skeletonGeneration != previous.skeletonGeneration ||
                current.poseAnimationGeneration !=
                    previous.poseAnimationGeneration ||
                current.poseSampleMode != previous.poseSampleMode ||
                current.poseSampleRate != previous.poseSampleRate ||
                current.poseSampleDurationSeconds !=
                    previous.poseSampleDurationSeconds ||
                crossesAnimationLoopBoundary(previous, current) ||
                current.visual.animationState !=
                    previous.visual.animationState) {
                continue;
            }
            const container::Span<const RenderMatrix> currentPose =
                m_completedFrame.pose(current);
            const container::Span<const RenderMatrix> previousPose =
                m_previousFrame.pose(previous);
            if (currentPose.size() != previousPose.size()) continue;
            const bool rootChanged =
                !(current.worldTransform == previous.worldTransform);
            const math::vec3 displacement =
                current.worldTransform.translation() -
                previous.worldTransform.translation();
            const float snapDistance = std::max(
                performance_limits::kWorldInterpolationMinimumSnapDistance,
                std::max(current.boundingRadius, previous.boundingRadius) *
                    performance_limits::kWorldInterpolationRadiusSnapFactor);
            if (!std::isfinite(displacement.length_sq()) ||
                displacement.length_sq() > snapDistance * snapDistance) {
                continue;
            }
            const bool poseChanged = !std::equal(
                currentPose.begin(), currentPose.end(),
                previousPose.begin(), previousPose.end());
            m_completedFrame.interpolationEligible[index] =
                rootChanged || poseChanged ? 1u : 0u;
        }
    }
    const size_t projectileCapacityBefore =
        m_completedFrame.projectiles.capacity();
    prepareProjectiles();
    countCapacityGrowth(
        projectileCapacityBefore,
        m_completedFrame.projectiles.capacity());
    const uint64_t preparedPaletteBytes = static_cast<uint64_t>(
        m_completedFrame.poseArena.size()) * sizeof(RenderMatrix);
    const uint64_t preparedVisibilityBytes = static_cast<uint64_t>(
        m_completedFrame.visibilityArena.size()) * sizeof(uint8_t);
    const auto capacityBytes = [](size_t capacity,
                                  size_t elementBytes) noexcept {
        const uint64_t count = static_cast<uint64_t>(capacity);
        const uint64_t bytes = static_cast<uint64_t>(elementBytes);
        return bytes != 0 && count >
                std::numeric_limits<uint64_t>::max() / bytes
            ? std::numeric_limits<uint64_t>::max()
            : count * bytes;
    };
    uint64_t retainedContainerCapacityBytes = 0;
    const auto addRetainedCapacity =
        [&retainedContainerCapacityBytes](uint64_t bytes) noexcept {
            retainedContainerCapacityBytes = bytes >
                    std::numeric_limits<uint64_t>::max() -
                        retainedContainerCapacityBytes
                ? std::numeric_limits<uint64_t>::max()
                : retainedContainerCapacityBytes + bytes;
        };
    addRetainedCapacity(capacityBytes(
        m_snapshot.entities.capacity(), sizeof(RenderEntitySnapshot)));
    addRetainedCapacity(capacityBytes(
        m_snapshot.projectiles.capacity(), sizeof(ProjectileRenderSnapshot)));
    addRetainedCapacity(capacityBytes(
        m_preparedByInput.capacity(), sizeof(PreparedRenderInstance)));
    addRetainedCapacity(capacityBytes(
        m_resolvedPresentationsByInput.capacity(),
        sizeof(ResolvedAnimationPresentation)));
    addRetainedCapacity(capacityBytes(
        m_completedFrame.visibleInstances.capacity(),
        sizeof(PreparedRenderInstance)));
    addRetainedCapacity(capacityBytes(
        m_preparedParticleEmitterInstances.capacity(),
        sizeof(PreparedRenderInstance)));
    addRetainedCapacity(capacityBytes(
        m_completedFrame.particleEmitterInstances.capacity(),
        sizeof(PreparedRenderInstance)));
    addRetainedCapacity(capacityBytes(
        m_preparedParticleEmitterBoneWorldTransformArena.capacity(),
        sizeof(std::optional<RenderMatrix>)));
    addRetainedCapacity(capacityBytes(
        m_completedFrame.particleEmitterBoneWorldTransformArena.capacity(),
        sizeof(std::optional<RenderMatrix>)));
    addRetainedCapacity(capacityBytes(
        m_completedFrame.projectiles.capacity(),
        sizeof(PreparedProjectileRenderSnapshot)));
    addRetainedCapacity(capacityBytes(
        m_preparedTrackMarks.capacity(), sizeof(TrackMarkRenderInput)));
    addRetainedCapacity(capacityBytes(
        m_completedFrame.trackMarks.capacity(), sizeof(TrackMarkRenderInput)));
    addRetainedCapacity(capacityBytes(
        m_animationCompletionsByInput.capacity(),
        sizeof(container::Vector<RenderAnimationCompletionFeedback>)));
    addRetainedCapacity(capacityBytes(
        m_completionFallbacksByInput.capacity(), sizeof(uint32_t)));
    addRetainedCapacity(capacityBytes(
        m_completedFrame.animationCompletions.capacity(),
        sizeof(RenderAnimationCompletionFeedback)));
    addRetainedCapacity(capacityBytes(
        m_completedFrame.poseArena.capacity(), sizeof(RenderMatrix)));
    addRetainedCapacity(capacityBytes(
        m_completedFrame.visibilityArena.capacity(), sizeof(uint8_t)));
    addRetainedCapacity(capacityBytes(
        m_entityPreparationTasks.capacity(), sizeof(tf::Task)));
    addRetainedCapacity(capacityBytes(
        m_attachmentResolutionScratch.capacity(),
        sizeof(AttachmentResolution)));
    addRetainedCapacity(capacityBytes(
        m_attachmentParentScratch.capacity(), sizeof(size_t)));
    addRetainedCapacity(capacityBytes(
        m_attachmentDepthScratch.capacity(), sizeof(size_t)));
    addRetainedCapacity(capacityBytes(
        m_attachmentPathScratch.capacity(), sizeof(size_t)));
    addRetainedCapacity(capacityBytes(
        m_attachmentOrderScratch.capacity(), sizeof(size_t)));
    addRetainedCapacity(capacityBytes(
        m_attachmentLayerOffsetsScratch.capacity(), sizeof(size_t)));
    addRetainedCapacity(capacityBytes(
        m_attachmentLayerCursorsScratch.capacity(), sizeof(size_t)));
    addRetainedCapacity(capacityBytes(
        m_attachmentTasksScratch.capacity(), sizeof(std::future<void>)));
    addRetainedCapacity(capacityBytes(
        m_attachmentInputIndexScratch.bucket_count(), sizeof(void*)));
    addRetainedCapacity(capacityBytes(
        m_attachmentInputIndexScratch.size(),
        sizeof(std::pair<RenderEntityId, size_t>)));
    addRetainedCapacity(capacityBytes(
        m_attachmentRenderIndexScratch.bucket_count(), sizeof(void*)));
    addRetainedCapacity(capacityBytes(
        m_attachmentRenderIndexScratch.size(),
        sizeof(std::pair<RenderEntityId, size_t>)));
    addRetainedCapacity(capacityBytes(
        m_currentVisibilityEntities.bucket_count(), sizeof(void*)));
    addRetainedCapacity(capacityBytes(
        m_currentVisibilityEntities.size(), sizeof(RenderEntityId)));
    addRetainedCapacity(capacityBytes(
        m_bodyParticleSelectionCounts.bucket_count(), sizeof(void*)));
    addRetainedCapacity(capacityBytes(
        m_bodyParticleSelectionCounts.size(),
        sizeof(std::pair<uint64_t, uint32_t>)));
    constexpr uint64_t kNestedCapacitySampleIntervalFrames = 60u;
    const bool nestedSampleRewound = m_nestedCapacitySampleValid &&
        m_snapshot.simulationFrame < m_nestedCapacityLastSampleFrame;
    const bool nestedSampleDue = !m_nestedCapacitySampleValid ||
        nestedSampleRewound ||
        m_snapshot.simulationFrame - m_nestedCapacityLastSampleFrame >=
            kNestedCapacitySampleIntervalFrames;
    uint64_t retainedNestedCapacityBytes = m_retainedNestedCapacityBytes;
    if (nestedSampleDue) {
        retainedNestedCapacityBytes = 0;
    const auto addNestedCapacity =
        [&retainedNestedCapacityBytes](uint64_t bytes) noexcept {
            retainedNestedCapacityBytes = bytes >
                    std::numeric_limits<uint64_t>::max() -
                        retainedNestedCapacityBytes
                ? std::numeric_limits<uint64_t>::max()
                : retainedNestedCapacityBytes + bytes;
        };
    const auto addStringCapacity = [&addNestedCapacity](
            const container::String& value) noexcept {
        addNestedCapacity(static_cast<uint64_t>(value.capacity()));
    };
    const auto addVisualCapacity =
        [&addNestedCapacity, &addStringCapacity, &capacityBytes](
            const RenderVisualState& visual) noexcept {
            addStringCapacity(visual.animationState);
            addStringCapacity(visual.animationStart.sourceModelAsset);
            addStringCapacity(visual.animationStart.sourceAnimationState);
            addNestedCapacity(capacityBytes(
                visual.subObjectVisibility.capacity(),
                sizeof(RenderSubObjectVisibility)));
            for (const RenderSubObjectVisibility& value :
                 visual.subObjectVisibility) {
                addStringCapacity(value.name);
            }
            addNestedCapacity(capacityBytes(
                visual.boneControls.capacity(), sizeof(RenderBoneControl)));
            for (const RenderBoneControl& value : visual.boneControls) {
                addStringCapacity(value.boneName);
            }
            addNestedCapacity(capacityBytes(
                visual.particleSystemBones.capacity(),
                sizeof(RenderParticleSystemBone)));
            for (const RenderParticleSystemBone& value :
                 visual.particleSystemBones) {
                addStringCapacity(value.boneName);
                addStringCapacity(value.particleSystem);
            }
            addStringCapacity(visual.supplyBones.prefix);
            addStringCapacity(visual.debris.initialAnimation);
            addStringCapacity(visual.debris.flyingAnimation);
            addStringCapacity(visual.debris.finalAnimation);
            addStringCapacity(visual.treeTextureAsset);
        };
    const auto addCompletionTargetCapacity =
        [&addNestedCapacity, &addStringCapacity, &capacityBytes](
            const RenderAnimationCompletionTarget& target) noexcept {
            addStringCapacity(target.modelAsset);
            addStringCapacity(target.animationState);
            addStringCapacity(target.animationStart.sourceModelAsset);
            addStringCapacity(target.animationStart.sourceAnimationState);
            addNestedCapacity(capacityBytes(
                target.subObjectVisibility.capacity(),
                sizeof(RenderSubObjectVisibility)));
            for (const RenderSubObjectVisibility& value :
                 target.subObjectVisibility) {
                addStringCapacity(value.name);
            }
            addNestedCapacity(capacityBytes(
                target.boneControls.capacity(), sizeof(RenderBoneControl)));
            for (const RenderBoneControl& value : target.boneControls) {
                addStringCapacity(value.boneName);
            }
            addNestedCapacity(capacityBytes(
                target.particleSystemBones.capacity(),
                sizeof(RenderParticleSystemBone)));
            for (const RenderParticleSystemBone& value :
                 target.particleSystemBones) {
                addStringCapacity(value.boneName);
                addStringCapacity(value.particleSystem);
            }
            for (const container::String& value : target.weaponLaunchBones) {
                addStringCapacity(value);
            }
        };
    const auto addPreparedCapacity =
        [&addVisualCapacity, &addStringCapacity](
            const PreparedRenderInstance& prepared) noexcept {
            addStringCapacity(prepared.modelAsset);
            addVisualCapacity(prepared.visual);
            addStringCapacity(prepared.shadow.textureName);
        };
    for (const RenderEntitySnapshot& entity : m_snapshot.entities) {
        addStringCapacity(entity.modelAsset);
        addVisualCapacity(entity.visual);
        for (const container::String& value : entity.weaponLaunchBones) {
            addStringCapacity(value);
        }
        if (entity.animationCompletionTarget) {
            addCompletionTargetCapacity(*entity.animationCompletionTarget);
        }
        if (entity.animationFinalTarget) {
            addCompletionTargetCapacity(*entity.animationFinalTarget);
        }
        addStringCapacity(entity.shadow.textureName);
        addStringCapacity(entity.attachToBoneInContainer);
        addStringCapacity(entity.attachToBoneInAnotherModule);
    }
    for (const ProjectileRenderSnapshot& projectile :
         m_snapshot.projectiles) {
        addStringCapacity(projectile.trailStreamName);
        addStringCapacity(projectile.trailTexture);
        addStringCapacity(projectile.shadow.textureName);
    }
    for (const TrackMarkRenderInput& track : m_snapshot.trackMarks) {
        addStringCapacity(track.textureName);
        addStringCapacity(track.leftWidthBone);
        addStringCapacity(track.rightWidthBone);
    }
    for (const ResolvedAnimationPresentation& resolved :
         m_resolvedPresentationsByInput) {
        addStringCapacity(resolved.modelAsset);
        addVisualCapacity(resolved.visual);
        for (const container::String& value : resolved.weaponLaunchBones) {
            addStringCapacity(value);
        }
        addNestedCapacity(capacityBytes(
            resolved.completions.capacity(),
            sizeof(RenderAnimationCompletionFeedback)));
    }
    for (const PreparedRenderInstance& prepared :
         m_completedFrame.visibleInstances) {
        addPreparedCapacity(prepared);
    }
    for (const PreparedRenderInstance& prepared :
         m_completedFrame.particleEmitterInstances) {
        addPreparedCapacity(prepared);
    }
    for (const PreparedRenderInstance& prepared : m_preparedByInput) {
        addPreparedCapacity(prepared);
    }
    addVisualCapacity(m_cameraVisualScratch);
    }
    if (nestedSampleDue) {
        if (retainedNestedCapacityBytes > m_retainedNestedCapacityBytes &&
            m_nestedCapacityGrowthFrames !=
                std::numeric_limits<uint32_t>::max()) {
            ++m_nestedCapacityGrowthFrames;
        }
        m_retainedNestedCapacityBytes = retainedNestedCapacityBytes;
        m_retainedNestedCapacityHighWaterBytes = std::max(
            m_retainedNestedCapacityHighWaterBytes,
            retainedNestedCapacityBytes);
        m_nestedCapacityLastSampleFrame = m_snapshot.simulationFrame;
        m_nestedCapacitySampleValid = true;
    }
    m_retainedContainerCapacityHighWaterBytes = std::max(
        m_retainedContainerCapacityHighWaterBytes,
        retainedContainerCapacityBytes);
    m_lastPreparationStats = {
        .simulationFrame = m_snapshot.simulationFrame,
        .inputEntities = static_cast<uint32_t>(std::min<size_t>(
            m_snapshot.entities.size(), std::numeric_limits<uint32_t>::max())),
        .chunkTaskCount = m_preparationTaskCount,
        .scheduledTaskCount = m_scheduledPreparationTaskCount,
        .completedTaskCount = m_completedPreparationTaskCount.load(
            std::memory_order_relaxed),
        .executorWorkerCapacity = static_cast<uint32_t>(std::min<size_t>(
            parallelExecutor().num_workers(),
            std::numeric_limits<uint32_t>::max())),
        .activeWorkerCount = static_cast<uint32_t>(std::popcount(
            m_preparationWorkerMask.load(std::memory_order_relaxed))),
        .activeWorkerObservationClamped =
            parallelExecutor().num_workers() > 64u,
        .entityGrain = static_cast<uint32_t>(std::min<size_t>(
            performance_limits::kWorldPreparationEntityGrain,
            std::numeric_limits<uint32_t>::max())),
        .preparedInstances = static_cast<uint32_t>(std::min<size_t>(
            m_completedFrame.visibleInstances.size(), std::numeric_limits<uint32_t>::max())),
        .animatedInstances = m_animatedPreparedCount.load(std::memory_order_relaxed),
        .hiddenInstances = m_hiddenCount.load(std::memory_order_relaxed),
        .invalidInstances = m_invalidCount.load(std::memory_order_relaxed),
        .distanceCulledInstances = 0,
        .frustumCulledInstances = 0,
        .poseEvaluations = m_poseEvaluationCount.load(
            std::memory_order_relaxed),
        .poseReuses = m_poseReuseCount.load(std::memory_order_relaxed),
        .ordinaryPoseEvaluations = m_ordinaryPoseEvaluationCount.load(
            std::memory_order_relaxed),
        .cameraPoseEvaluations = m_cameraPoseEvaluationCount.load(
            std::memory_order_relaxed),
        .emitterPoseEvaluations = m_emitterPoseEvaluationCount.load(
            std::memory_order_relaxed),
        .trackMarkPoseEvaluations = m_trackMarkPoseEvaluationCount.load(
            std::memory_order_relaxed),
        .cameraPoseReuses = m_cameraPoseReuseCount.load(
            std::memory_order_relaxed),
        .emitterPoseReuses = m_emitterPoseReuseCount.load(
            std::memory_order_relaxed),
        .completionFallbacks = m_completionFallbackCount,
        .cameraPoseFallbacks = m_cameraPoseFallbackCount.load(
            std::memory_order_relaxed),
        .emitterRootFallbacks = m_emitterRootFallbackCount.load(
            std::memory_order_relaxed),
        .poseJointsEvaluated = m_poseJointCount.load(
            std::memory_order_relaxed),
        .poseAnimationChannelsSampled = m_poseAnimationChannelCount.load(
            std::memory_order_relaxed),
        .poseControlsApplied = m_poseControlCount.load(
            std::memory_order_relaxed),
        .poseEvaluationMicroseconds =
            (m_ordinaryPoseNanoseconds.load(std::memory_order_relaxed) +
             m_cameraPoseNanoseconds.load(std::memory_order_relaxed) +
             m_emitterPoseNanoseconds.load(std::memory_order_relaxed) +
             m_trackMarkPoseNanoseconds.load(std::memory_order_relaxed)) /
            1000u,
        .ordinaryPoseMicroseconds = m_ordinaryPoseNanoseconds.load(
            std::memory_order_relaxed) / 1000u,
        .cameraPoseMicroseconds = m_cameraPoseNanoseconds.load(
            std::memory_order_relaxed) / 1000u,
        .emitterPoseMicroseconds = m_emitterPoseNanoseconds.load(
            std::memory_order_relaxed) / 1000u,
        .trackMarkPoseMicroseconds = m_trackMarkPoseNanoseconds.load(
            std::memory_order_relaxed) / 1000u,
        .poseArenaRequestedJoints = m_poseArenaRequestedJoints,
        .poseArenaAllocatedJoints = m_completedFrame.poseArena.size(),
        .poseArenaRejectedInstances = m_poseArenaRejectedInstances,
        .poseArenaGrowths = m_poseArenaGrowths,
        .visibilityArenaGrowths = m_visibilityArenaGrowths,
        .poseArenaCapacityBytes = capacityBytes(
            m_completedFrame.poseArena.capacity(), sizeof(RenderMatrix)),
        .visibilityArenaCapacityBytes = capacityBytes(
            m_completedFrame.visibilityArena.capacity(), sizeof(uint8_t)),
        .poseArenaCapacityHighWaterBytes =
            m_poseArenaCapacityHighWaterBytes,
        .visibilityArenaCapacityHighWaterBytes =
            m_visibilityArenaCapacityHighWaterBytes,
        .preparedPaletteBytes = preparedPaletteBytes,
        .preparedVisibilityBytes = preparedVisibilityBytes,
        .retainedContainerCapacityBytes =
            retainedContainerCapacityBytes,
        .retainedContainerCapacityHighWaterBytes =
            m_retainedContainerCapacityHighWaterBytes,
        .containerCapacityGrowths = m_containerCapacityGrowths,
        .retainedNestedCapacityBytes =
            retainedNestedCapacityBytes,
        .retainedNestedCapacityHighWaterBytes =
            m_retainedNestedCapacityHighWaterBytes,
        .nestedCapacityGrowthFrames =
            m_nestedCapacityGrowthFrames,
        .elapsedMicroseconds = static_cast<uint64_t>(std::max<int64_t>(0,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - m_preparationStarted).count())),
    };
    m_hasCompletedFrame = true;
    m_preparing = false;
    return m_completedFrame;
}

} // namespace engine::render
