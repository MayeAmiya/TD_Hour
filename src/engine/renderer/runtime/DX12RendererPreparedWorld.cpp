#include "DX12RendererWorldAssetRuntime.h"
#include "core/debug/debug.h"
#include "engine/texture/TextureManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <optional>

namespace engine {

namespace render {
namespace {

[[nodiscard]] RenderMatrix interpolateEndpointTransform(
    const RenderMatrix& previous, const RenderMatrix& current,
    float alpha) noexcept {
    if (!(alpha > 0.0f)) return previous;
    if (alpha >= 1.0f) return current;
    DirectX::XMVECTOR previousScale;
    DirectX::XMVECTOR previousRotation;
    DirectX::XMVECTOR previousTranslation;
    DirectX::XMVECTOR currentScale;
    DirectX::XMVECTOR currentRotation;
    DirectX::XMVECTOR currentTranslation;
    if (!DirectX::XMMatrixDecompose(
            &previousScale, &previousRotation, &previousTranslation,
            previous.load()) ||
        !DirectX::XMMatrixDecompose(
            &currentScale, &currentRotation, &currentTranslation,
            current.load())) {
        return current;
    }
    return RenderMatrix::from_trs(
        RenderVector{DirectX::XMVectorLerp(
            previousScale, currentScale, alpha)},
        RenderQuaternion{DirectX::XMQuaternionSlerp(
            previousRotation, currentRotation, alpha)},
        RenderVector{DirectX::XMVectorLerp(
            previousTranslation, currentTranslation, alpha)});
}

[[nodiscard]] RenderCameraSnapshot interpolateEndpointCamera(
    const RenderCameraSnapshot& previous,
    const RenderCameraSnapshot& current,
    float alpha) noexcept {
    if (!(alpha > 0.0f)) return previous;
    if (alpha >= 1.0f) return current;
    const auto interpolateVector = [alpha](
        const RenderVector& from, const RenderVector& to) noexcept {
        return from + (to - from) * alpha;
    };
    const auto interpolateScalar = [alpha](float from, float to) noexcept {
        return std::lerp(from, to, alpha);
    };
    RenderCameraSnapshot result = current;
    result.position = interpolateVector(previous.position, current.position);
    result.target = interpolateVector(previous.target, current.target);
    result.up = interpolateVector(previous.up, current.up);
    result.up = result.up.length_sq() > math::EPSILON * math::EPSILON
        ? result.up.normalized() : current.up;
    result.visibilityDistance = interpolateScalar(
        previous.visibilityDistance, current.visibilityDistance);
    result.verticalFovRadians = interpolateScalar(
        previous.verticalFovRadians, current.verticalFovRadians);
    result.horizontalFovRadians = interpolateScalar(
        previous.horizontalFovRadians, current.horizontalFovRadians);
    result.tacticalViewportHeightScale = interpolateScalar(
        previous.tacticalViewportHeightScale,
        current.tacticalViewportHeightScale);
    result.nearClip = interpolateScalar(previous.nearClip, current.nearClip);
    result.farClip = interpolateScalar(previous.farClip, current.farClip);
    result.fogColor = interpolateVector(previous.fogColor, current.fogColor);
    result.fogStartDistance = interpolateScalar(
        previous.fogStartDistance, current.fogStartDistance);
    result.fogEndDistance = interpolateScalar(
        previous.fogEndDistance, current.fogEndDistance);
    return result;
}

[[nodiscard]] bool cameraEndpointMatches(
    const RenderCameraSnapshot& base,
    const RenderCameraSnapshot& settled) noexcept {
    const auto closeScalar = [](float lhs, float rhs) noexcept {
        const float scale = std::max({1.0f, std::abs(lhs), std::abs(rhs)});
        return std::abs(lhs - rhs) <= scale * 0.00001f;
    };
    const auto closeVector = [&closeScalar](
        const RenderVector& lhs, const RenderVector& rhs) noexcept {
        return closeScalar(lhs.x(), rhs.x()) &&
            closeScalar(lhs.y(), rhs.y()) &&
            closeScalar(lhs.z(), rhs.z());
    };
    return base.cameraCutRevision == settled.cameraCutRevision &&
        closeVector(base.position, settled.position) &&
        closeVector(base.target, settled.target) &&
        closeVector(base.up, settled.up) &&
        closeScalar(base.visibilityDistance, settled.visibilityDistance) &&
        closeScalar(base.verticalFovRadians, settled.verticalFovRadians) &&
        closeScalar(base.horizontalFovRadians, settled.horizontalFovRadians) &&
        closeScalar(base.tacticalViewportHeightScale,
                    settled.tacticalViewportHeightScale) &&
        closeScalar(base.nearClip, settled.nearClip) &&
        closeScalar(base.farClip, settled.farClip) &&
        base.fogEnabled == settled.fogEnabled &&
        closeVector(base.fogColor, settled.fogColor) &&
        closeScalar(base.fogStartDistance, settled.fogStartDistance) &&
        closeScalar(base.fogEndDistance, settled.fogEndDistance);
}

} // namespace
} // namespace render

size_t DX12Renderer::renderPreparedWorld(TextureManager* objectIconTextures) {
    if (!m_worldAssets) return 0;
    m_worldAssets->view.worldInterpolation.setMaximumIntermediateSamples(
        m_worldAssets->stats.interpolationIntermediateSamples());
    const auto renderNow = std::chrono::steady_clock::now();
    bool publishedNewWorld = false;
    const render::PreparedWorldFrame* preparedFrame = nullptr;
    if (m_worldAssets->submittedPreparationPending &&
        !m_worldAssets->pipeline.isPreparing() &&
        m_worldAssets->view.worldInterpolation.endpointRotationReady(
            m_worldAssets->pipeline.hasCompletedFrame(), renderNow)) {
        preparedFrame = &m_worldAssets->pipeline.finishPreparation();
        m_worldAssets->submittedPreparationPending = false;
        m_worldAssets->debugWorld.cancelPreparation();
        publishedNewWorld = true;
    } else {
        preparedFrame = m_worldAssets->pipeline.completedFrame();
    }
    if (!preparedFrame) return 0;
    const render::PreparedWorldFrame& prepared = *preparedFrame;
    if (publishedNewWorld) {
        const render::PreparedWorldFrame* previous =
            m_worldAssets->pipeline.previousFrame();
        m_worldAssets->view.worldInterpolation.beginEndpoint(
            previous ? &previous->stamp : nullptr, prepared.stamp,
            prepared.objectUi.logicFramesPerSecond, renderNow);
        m_worldAssets->view.cameraInterpolation.beginEndpoint(
            previous ? &previous->stamp : nullptr, prepared.stamp,
            prepared.objectUi.logicFramesPerSecond, renderNow);
    }
    uint64_t viewRevision = m_worldAssets->view.nextRevision++;
    if (viewRevision == 0u) {
        viewRevision = m_worldAssets->view.nextRevision++;
    }
    m_worldAssets->view.current = prepared.sourceView;
    if (m_worldAssets->view.latest) {
        const render::RenderViewState& latest =
            *m_worldAssets->view.latest;
        const bool sameWorldDomain =
            latest.sourceWorld.presentationEpoch ==
                prepared.stamp.presentationEpoch &&
            latest.sourceWorld.sessionRevision ==
                prepared.stamp.sessionRevision &&
            latest.sourceWorld.loadingRevision ==
                prepared.stamp.loadingRevision;
        // Snapshot extraction currently publishes one camera sample beside
        // each world endpoint. Preparation can lag by one or more endpoints,
        // so a merely same-session latest value may belong to a future world;
        // applying it now and then rotating endpoints makes the camera jump
        // forward/back every present frame. Independent presentation-camera
        // samples may relax this once they carry an explicit reusable-world
        // contract. Endpoint cameras must match exactly today.
        const bool sameWorldRevision =
            latest.sourceWorld.worldRevision ==
                prepared.stamp.worldRevision;
        if (sameWorldDomain && sameWorldRevision) {
            m_worldAssets->view.current = latest;
        }
    }
    const render::PreparedWorldFrame* previousPrepared =
        m_worldAssets->pipeline.previousFrame();
    // Publish the logical interpolation endpoints, not the physical slots.
    // The timeline collapses incompatible loading/session/epoch pairs to
    // B/B; exposing pipeline.previousFrame() here would make picking and UI
    // search the current domain for an A revision that belongs to a retired
    // domain, producing a persistent authoritative no-hit.
    m_worldAssets->view.current.worldARevision =
        m_worldAssets->view.worldInterpolation.worldARevision();
    m_worldAssets->view.current.worldBRevision =
        m_worldAssets->view.worldInterpolation.worldBRevision();
    m_worldAssets->view.current.viewRevision = viewRevision;
    int logicalWidth = 0;
    int logicalHeight = 0;
    getWindowSize(logicalWidth, logicalHeight);
    m_worldAssets->view.current.viewport = {
        .pixelWidth = m_d3d12.width(),
        .pixelHeight = m_d3d12.height(),
        .logicalWidth = static_cast<uint32_t>(std::max(logicalWidth, 1)),
        .logicalHeight = static_cast<uint32_t>(std::max(logicalHeight, 1)),
        .virtualWidth = static_cast<float>(std::max(m_virtualW, 1)),
        .virtualHeight = static_cast<float>(std::max(m_virtualH, 1)),
    };
    m_worldAssets->view.current.interpolationAlpha =
        m_worldAssets->view.worldInterpolation.alpha(renderNow);
    if (previousPrepared &&
        previousPrepared->stamp.presentationEpoch ==
            prepared.stamp.presentationEpoch &&
        previousPrepared->stamp.sessionRevision ==
            prepared.stamp.sessionRevision &&
        previousPrepared->stamp.loadingRevision ==
            prepared.stamp.loadingRevision &&
        previousPrepared->camera.cameraCutRevision ==
            prepared.camera.cameraCutRevision) {
        m_worldAssets->view.current.camera =
            render::interpolateEndpointCamera(
                previousPrepared->sourceView.camera,
                m_worldAssets->view.current.camera,
                m_worldAssets->view.cameraInterpolation.alpha(renderNow));
    }
    // A smooth authored move retains its cut revision, so revision equality
    // alone can match the preceding scene.  Retire only after the immutable
    // base endpoint itself has caught up to the completed presentation pose.
    if (m_worldAssets->view.presentationCameraOverride &&
        m_worldAssets->view.presentationCameraOverride->releaseWhenBaseMatches &&
        render::cameraEndpointMatches(
            m_worldAssets->view.current.camera,
            m_worldAssets->view.presentationCameraOverride->camera)) {
        m_worldAssets->view.presentationCameraOverride.reset();
    }
    if (m_worldAssets->view.presentationCameraOverride) {
        const render::PresentationCameraOverride& cameraOverride =
            *m_worldAssets->view.presentationCameraOverride;
        const bool sameWorldDomain =
            cameraOverride.sourceWorld.presentationEpoch ==
                prepared.stamp.presentationEpoch &&
            cameraOverride.sourceWorld.sessionRevision ==
                prepared.stamp.sessionRevision &&
            cameraOverride.sourceWorld.loadingRevision ==
                prepared.stamp.loadingRevision;
        if (sameWorldDomain) {
            m_worldAssets->view.current.camera = cameraOverride.camera;
        }
    }
    // FX is deterministic presentation state belonging to confirmed logic
    // frames. While A->B is still in flight, the visible confirmed boundary
    // remains A; admitting B's particles/one-shots earlier makes trails finish
    // before the model reaches B. A discontinuity or disabled interpolation
    // collapses to B/B and therefore releases B immediately.
    uint64_t displayedFxFrame = prepared.simulationFrame;
    if (m_worldAssets->view.current.interpolationAlpha < 1.0f &&
        previousPrepared &&
        m_worldAssets->view.current.worldARevision ==
            previousPrepared->stamp.worldRevision &&
        m_worldAssets->view.current.worldBRevision ==
            prepared.stamp.worldRevision) {
        displayedFxFrame = previousPrepared->simulationFrame;
    }
    m_worldAssets->frame.displayedSimulationFrame = displayedFxFrame;
    // This is per-present transient storage. releaseFxSnapshotsThrough()
    // appends only newly admitted confirmed batches for the displayed
    // endpoint; no invocation is retained or replayed across presents.
    m_worldAssets->frame.fxDeferredExecutionSnapshots.clear();
    releaseFxSnapshotsThrough(
        prepared.presentationEpoch, displayedFxFrame);
    constexpr size_t kMaximumPendingAnimationCompletions = 4096;
    if (publishedNewWorld) {
        for (const render::RenderAnimationEndpointAdmission& admission :
             prepared.animationEndpointAdmissions) {
            if (prepared.presentationEpoch == 0 || admission.objectId == 0 ||
                admission.objectId > UINT32_MAX || admission.generation == 0) {
                continue;
            }
            const uint64_t key =
                (admission.objectId << 32u) | admission.channelIndex;
            render::RenderAnimationCompletionFeedback feedback{
                .presentationEpoch = prepared.presentationEpoch,
                .simulationFrame = prepared.simulationFrame,
                .objectId = admission.objectId,
                .channelIndex = admission.channelIndex,
                .generation = admission.generation,
                .kind = render::RenderAnimationFeedbackKind::EndpointPublished,
            };
            auto [entry, inserted] =
                m_pendingAnimationEndpointAdmissions.try_emplace(
                    key, feedback);
            if (!inserted &&
                (feedback.generation > entry->second.generation ||
                 (feedback.generation == entry->second.generation &&
                  feedback.simulationFrame >
                      entry->second.simulationFrame))) {
                entry->second = feedback;
            }
        }
        for (const render::RenderAnimationCompletionFeedback& completion :
             prepared.animationCompletions) {
            if (completion.presentationEpoch == 0 || completion.objectId == 0 ||
                completion.generation == 0) {
                continue;
            }
            if (m_pendingAnimationCompletions.size() >=
                kMaximumPendingAnimationCompletions) {
                m_pendingAnimationCompletions.erase(
                    m_pendingAnimationCompletions.begin());
            }
            m_pendingAnimationCompletions.push_back(completion);
        }
#if TD_DEBUG_ENABLED
        if (m_worldAssets->debugVisualTraceObjectId != 0u) {
            for (const render::PreparedRenderInstance& instance :
                 prepared.visibleInstances) {
                if (instance.objectId !=
                    m_worldAssets->debugVisualTraceObjectId) {
                    continue;
                }
                uint8_t launchBoneMask = 0;
                for (size_t slot = 0;
                     slot < instance.weaponLaunchBoneWorldTransforms.size();
                     ++slot) {
                    if (instance.weaponLaunchBoneWorldTransforms[slot]) {
                        launchBoneMask |= static_cast<uint8_t>(1u << slot);
                    }
                }
                size_t fingerprint = static_cast<size_t>(
                    instance.skeletonGeneration);
                fingerprint ^= static_cast<size_t>(launchBoneMask) << 8u;
                fingerprint ^= static_cast<size_t>(instance.poseReady) << 16u;
                fingerprint ^=
                    static_cast<size_t>(instance.visibilityReady) << 17u;
                const auto found =
                    m_worldAssets->debugVisualPreparedHashes.find(instance.id);
                if (found !=
                        m_worldAssets->debugVisualPreparedHashes.end() &&
                    found->second == fingerprint) {
                    continue;
                }
                m_worldAssets->debugVisualPreparedHashes.insert_or_assign(
                    instance.id, fingerprint);
                TD_LOG_INFO(
                    "[VisualTrace.Prepared] tick={} object={} channel={} model='{}' skeletonGeneration={} poseReady={} visibilityReady={} launchBoneResolvedMask=0x{:02X}",
                    prepared.simulationFrame, instance.objectId,
                    instance.channelIndex, instance.modelAsset,
                    instance.skeletonGeneration, instance.poseReady,
                    instance.visibilityReady, launchBoneMask);
            }
        }
#endif
    }
    if (m_worldAssets->fx.runtime) {
        container::Vector<fx::FxPresentationBonePose>& poses =
            m_worldAssets->frame.fxPresentationBonePoses;
        size_t poseCount = 0;
        container::Vector<fx::FxBonePoseDemand>& boneDemands =
            m_worldAssets->frame.fxBonePoseDemands;
        m_worldAssets->fx.runtime->appendActiveBonePoseDemands(boneDemands);
        m_worldAssets->fx.typed.worldRenderer().appendActiveBonePoseDemands(
            boneDemands);
        std::erase_if(boneDemands, [](const fx::FxBonePoseDemand& demand) {
            return !demand.valid();
        });
        std::sort(
            boneDemands.begin(), boneDemands.end(),
            [](const fx::FxBonePoseDemand& left,
               const fx::FxBonePoseDemand& right) {
                if (left.objectKey != right.objectKey) {
                    return left.objectKey < right.objectKey;
                }
                if (left.boneName != right.boneName) {
                    return left.boneName < right.boneName;
                }
                if (left.numberedPointLimit != right.numberedPointLimit) {
                    return left.numberedPointLimit < right.numberedPointLimit;
                }
                return left.includeBare < right.includeBare;
            });
        boneDemands.erase(
            std::unique(
                boneDemands.begin(), boneDemands.end(),
                [](const fx::FxBonePoseDemand& left,
                   const fx::FxBonePoseDemand& right) {
                    return left.objectKey == right.objectKey &&
                        left.boneName == right.boneName &&
                        left.numberedPointLimit == right.numberedPointLimit &&
                        left.includeBare == right.includeBare;
                }),
            boneDemands.end());
        const size_t estimatedPoseCount = std::min(
            fx::kMaximumFxBonePoseSamples, boneDemands.size() * 4u);
        if (poses.capacity() < estimatedPoseCount) {
            poses.reserve(estimatedPoseCount);
        }
        container::Vector<fx::FxModelParticleEmitterPose>& modelEmitters =
            m_worldAssets->frame.fxModelParticleEmitterPoses;
        size_t modelEmitterCount = 0;
        modelEmitters.reserve(prepared.particleEmitterInstances.size());
        const auto presentationAnchor = [](uint64_t objectKey,
                                           const render::RenderMatrix& transform) {
            const math::vec3 localX =
                transform.transform_dir({1.0f, 0.0f, 0.0f}).normalized();
            const math::vec3 localY =
                transform.transform_dir({0.0f, 1.0f, 0.0f}).normalized();
            const math::vec3 localZ =
                transform.transform_dir({0.0f, 0.0f, 1.0f}).normalized();
            const float horizontal = std::hypot(localX.x(), localX.y());
            // ParticleRuntime applies Rx(-roll) * Ry(pitch) * Rz(yaw), the
            // same row-vector order used by the render transform. Preserve
            // all three axes: forward elevation alone has the opposite sign
            // for Ry and loses the roll needed by side/down-offset emitters
            // such as FallingShellsGattlingTilted.
            return fx::FxPresentationAnchor{
                .objectKey = objectKey,
                .position = {
                    transform.translation().x(),
                    transform.translation().y(),
                    transform.translation().z(),
                },
                .rollRadians = -std::atan2(localY.z(), localZ.z()),
                .pitchRadians = std::atan2(-localX.z(), horizontal),
                .yawRadians = std::atan2(localX.y(), localX.x()),
            };
        };
        const float endpointAlpha =
            m_worldAssets->view.current.interpolationAlpha;
        bool poseBudgetExhausted = false;
        for (const fx::FxBonePoseDemand& demand : boneDemands) {
            const std::optional<size_t> instanceIndex =
                prepared.visibleInstanceIndexById(demand.objectKey);
            if (!instanceIndex) continue;
            const render::PreparedRenderInstance& instance =
                prepared.visibleInstances[*instanceIndex];
            if (!instance.skeleton ||
                instance.skeletonGeneration !=
                    instance.skeleton->generation()) {
                continue;
            }
            const container::Span<const render::RenderMatrix> currentPose =
                prepared.pose(instance);
            container::Span<const render::RenderMatrix> previousPose;
            const bool interpolatePose = endpointAlpha < 1.0f &&
                previousPrepared &&
                *instanceIndex < prepared.interpolationEligible.size() &&
                prepared.interpolationEligible[*instanceIndex] != 0u &&
                *instanceIndex < prepared.previousEndpointIndices.size();
            if (interpolatePose) {
                const uint32_t previousIndex =
                    prepared.previousEndpointIndices[*instanceIndex];
                if (previousIndex != UINT32_MAX &&
                    previousIndex <
                        previousPrepared->visibleInstances.size()) {
                    previousPose = previousPrepared->pose(
                        previousPrepared->visibleInstances[previousIndex]);
                    if (previousPose.size() != currentPose.size()) {
                        previousPose = {};
                    }
                }
            }
            const auto poseTransform = [&](size_t jointIndex)
                -> std::optional<render::RenderMatrix> {
                if (jointIndex >= currentPose.size()) return std::nullopt;
                if (jointIndex < previousPose.size()) {
                    return render::interpolateEndpointTransform(
                        previousPose[jointIndex], currentPose[jointIndex],
                        endpointAlpha);
                }
                return currentPose[jointIndex];
            };
            const auto appendPose = [&](size_t jointIndex) {
                if (jointIndex >= currentPose.size() ||
                    poseCount >= fx::kMaximumFxBonePoseSamples) {
                    poseBudgetExhausted =
                        poseCount >= fx::kMaximumFxBonePoseSamples;
                    return;
                }
                const std::optional<render::RenderMatrix> transform =
                    poseTransform(jointIndex);
                if (!transform) return;
                if (poseCount == poses.size()) poses.emplace_back();
                fx::FxPresentationBonePose& pose = poses[poseCount++];
                pose.objectKey = instance.id;
                pose.boneName = instance.skeleton->joints()[jointIndex].name;
                pose.anchor = presentationAnchor(instance.id, *transform);
            };
            if (demand.numberedPointLimit == 0u || demand.includeBare) {
                appendPose(m_worldAssets->fx.namedJoint(
                    instance.skeleton, demand.boneName));
            }
            if (demand.numberedPointLimit != 0u && !poseBudgetExhausted) {
                container::Span<const size_t> numbered =
                    m_worldAssets->fx.numberedJoints(
                        instance.skeleton, demand.boneName);
                if (numbered.size() > demand.numberedPointLimit) {
                    numbered = numbered.first(demand.numberedPointLimit);
                }
                for (const size_t jointIndex : numbered) {
                    if (jointIndex >= currentPose.size()) break;
                    appendPose(jointIndex);
                    if (poseBudgetExhausted) break;
                }
            }
            if (poseBudgetExhausted) break;
        }
        if (poseBudgetExhausted ||
            boneDemands.size() >= fx::kMaximumFxBonePoseDemands) {
            m_worldAssets->fx.typed.noteRejected();
        }
        for (const render::PreparedRenderInstance& instance :
             prepared.particleEmitterInstances) {
            const container::Span<const std::optional<render::RenderMatrix>>
                emitterTransforms =
                    prepared.particleEmitterBoneWorldTransforms(instance);
            for (size_t emitterIndex = 0;
                 emitterIndex < instance.visual.particleSystemBones.size();
                 ++emitterIndex) {
                const render::RenderParticleSystemBone& emitter =
                    instance.visual.particleSystemBones[emitterIndex];
                // This list is prepared independently of mesh culling. A
                // live non-hidden state therefore keeps its emitter across
                // distance/frustum changes; only state/visibility/object
                // lifetime removes its stable emitter key.
                std::optional<render::RenderMatrix> transform =
                    instance.worldTransform;
                if (emitterIndex < emitterTransforms.size() &&
                    emitterTransforms[emitterIndex]) {
                    transform = emitterTransforms[emitterIndex];
                }
                if (!transform) continue;
                if (modelEmitterCount == modelEmitters.size()) {
                    modelEmitters.emplace_back();
                }
                fx::FxModelParticleEmitterPose& modelEmitter =
                    modelEmitters[modelEmitterCount++];
                modelEmitter.emitterKey = emitter.identity;
                modelEmitter.objectKey = instance.id;
                modelEmitter.boneName = emitter.boneName;
                modelEmitter.particleSystem = emitter.particleSystem;
                modelEmitter.anchor = presentationAnchor(
                    instance.id, *transform);
#if TD_DEBUG_ENABLED
                if (publishedNewWorld &&
                    instance.objectId ==
                        m_worldAssets->debugVisualTraceObjectId) {
                    const render::RenderVector root =
                        instance.worldTransform.translation();
                    const render::RenderVector camera =
                        m_worldAssets->view.current.camera.position;
                    const float deltaX =
                        modelEmitter.anchor.position.x - camera.x();
                    const float deltaY =
                        modelEmitter.anchor.position.y - camera.y();
                    const float deltaZ =
                        modelEmitter.anchor.position.z - camera.z();
                    TD_LOG_INFO(
                        "[VisualTrace.Emitter] tick={} object={} channel={} emitter=0x{:016X} system='{}' bone='{}' root=({:.3f},{:.3f},{:.3f}) anchor=({:.3f},{:.3f},{:.3f}) camera=({:.3f},{:.3f},{:.3f}) distance={:.3f}",
                        prepared.simulationFrame, instance.objectId,
                        instance.channelIndex, emitter.identity,
                        emitter.particleSystem, emitter.boneName,
                        root.x(), root.y(), root.z(),
                        modelEmitter.anchor.position.x,
                        modelEmitter.anchor.position.y,
                        modelEmitter.anchor.position.z,
                        camera.x(), camera.y(), camera.z(),
                        std::sqrt(deltaX * deltaX + deltaY * deltaY +
                                  deltaZ * deltaZ));
                }
#endif
            }
        }
        poses.resize(poseCount);
        modelEmitters.resize(modelEmitterCount);
        m_worldAssets->fx.runtime->updateBonePosesRetained(
            container::Span<const fx::FxPresentationBonePose>(poses));
        boneDemands.clear();
        m_worldAssets->fx.runtime->updateModelParticleEmitters(modelEmitters);
        for (const fx::FxPresentationSnapshot& deferred :
             m_worldAssets->frame.fxDeferredExecutionSnapshots) {
            m_worldAssets->fx.runtime->submitDeferredInvocations(
                deferred, true);
        }
        m_worldAssets->frame.fxDeferredExecutionSnapshots.clear();
        m_worldAssets->fx.runtime->completeDeferredInvocationBarrier();
        m_worldAssets->fx.typed.consume(
            m_worldAssets->fx.runtime->takeCommands(),
            m_worldAssets->residency.assets,
            m_worldAssets->fx.maximumPresentationCommands);
    }
    return renderWorldFrame(prepared, objectIconTextures);
}

container::Vector<render::RenderAnimationCompletionFeedback>
DX12Renderer::takeAnimationCompletions() {
    container::Vector<render::RenderAnimationCompletionFeedback> result;
    result.reserve(m_pendingAnimationEndpointAdmissions.size() +
                   m_pendingAnimationCompletions.size());
    for (auto& [key, admission] :
         m_pendingAnimationEndpointAdmissions) {
        static_cast<void>(key);
        result.push_back(std::move(admission));
    }
    m_pendingAnimationEndpointAdmissions.clear();
    std::sort(result.begin(), result.end(),
        [](const render::RenderAnimationCompletionFeedback& left,
           const render::RenderAnimationCompletionFeedback& right) {
            if (left.objectId != right.objectId) {
                return left.objectId < right.objectId;
            }
            return left.channelIndex < right.channelIndex;
        });
    result.insert(
        result.end(),
        std::make_move_iterator(m_pendingAnimationCompletions.begin()),
        std::make_move_iterator(m_pendingAnimationCompletions.end()));
    m_pendingAnimationCompletions.clear();
    return result;
}

} // namespace engine
