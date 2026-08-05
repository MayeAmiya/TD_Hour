#include "DX12RendererWorldAssetRuntime.h"
#include "core/debug/debug.h"
#include "engine/texture/TextureManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>

namespace engine {

bool DX12Renderer::synchronizeWorldPresentationEpoch(
    uint64_t presentationEpoch, bool fxIngress, bool forceReset) {
    if (!m_worldAssets) return false;

    const uint64_t acceptedEpoch =
        m_worldAssets->lifetime.modelUploadPresentationEpoch;
    if (!forceReset &&
        m_worldAssets->lifetime.retiredPresentationEpoch != 0u &&
        presentationEpoch <= m_worldAssets->lifetime.retiredPresentationEpoch) {
        uint64_t& rejected = fxIngress
            ? m_worldAssets->lifetime.staleFxSnapshotRejectCount
            : m_worldAssets->lifetime.staleWorldSnapshotRejectCount;
        ++rejected;
        if (rejected == 1u || (rejected % 256u) == 0u) {
            TD_LOG_WARN(
                "[DX12Renderer] Rejected retired {} presentation epoch={} tombstone={} rejects={}",
                fxIngress ? "FX" : "world", presentationEpoch,
                m_worldAssets->lifetime.retiredPresentationEpoch, rejected);
        }
        return false;
    }
    if (presentationEpoch == 0 && !forceReset) {
        if (acceptedEpoch == 0) return true;
        uint64_t& rejected = fxIngress
            ? m_worldAssets->lifetime.staleFxSnapshotRejectCount
            : m_worldAssets->lifetime.staleWorldSnapshotRejectCount;
        ++rejected;
        if (rejected == 1u) {
            TD_LOG_WARN(
                "[DX12Renderer] Rejected unscoped {} presentation after epoch {} was accepted",
                fxIngress ? "FX" : "world", acceptedEpoch);
        }
        return false;
    }
    if (acceptedEpoch != 0 && presentationEpoch < acceptedEpoch) {
        uint64_t& rejected = fxIngress
            ? m_worldAssets->lifetime.staleFxSnapshotRejectCount
            : m_worldAssets->lifetime.staleWorldSnapshotRejectCount;
        ++rejected;
        if (rejected == 1u || (rejected % 256u) == 0u) {
            TD_LOG_WARN(
                "[DX12Renderer] Rejected stale {} presentation epoch={} accepted={} rejects={}",
                fxIngress ? "FX" : "world", presentationEpoch,
                acceptedEpoch, rejected);
        }
        return false;
    }
    if (presentationEpoch == acceptedEpoch && !forceReset) return true;

    // Epoch changes are rare safe-boundary operations. Join both CPU
    // preparation and prior GPU use before releasing owners whose resources
    // are not individually fence-retired (notably terrain render targets).
    if (m_worldAssets->debugWorld.preparationPending() ||
        m_worldAssets->submittedPreparationPending ||
        m_worldAssets->pipeline.isPreparing()) {
        static_cast<void>(m_worldAssets->pipeline.finishPreparation());
    }
    m_worldAssets->debugWorld.cancelPreparation();
    m_worldAssets->submittedPreparationPending = false;
    if (!m_d3d12.waitIdle()) {
        TD_LOG_WARN(
            "[DX12Renderer] GPU idle wait failed during presentation epoch reset {} -> {}; continuing degraded retirement",
            acceptedEpoch, presentationEpoch);
    }

    m_pendingAnimationCompletions.clear();
    m_pendingAnimationEndpointAdmissions.clear();
    m_worldAssets->pipeline.resetPresentationEpoch(presentationEpoch);
    if (forceReset || !m_worldAssets->view.latest ||
        m_worldAssets->view.latest->sourceWorld.presentationEpoch !=
            presentationEpoch) {
        m_worldAssets->view.latest.reset();
    }
    m_worldAssets->view.presentationCameraOverride.reset();
    m_worldAssets->view.current = {};
    m_worldAssets->view.nextRevision = 1u;
    m_worldAssets->view.worldInterpolation.reset();
    m_worldAssets->view.cameraInterpolation.reset();
    if (m_worldRenderer) {
        m_worldRenderer->resetPresentationEpoch(presentationEpoch);
    }

    // Remove frame-local/shared owners before advancing any cache generation.
    m_worldAssets->frame.particleDrawList.instances.clear();
    m_worldAssets->frame.particleDrawList.batches.clear();
    m_worldAssets->frame.particleDrawList.smudgeInstances.clear();
    m_worldAssets->frame.particleDrawList.gpuReferenceSampleCount = 0;
    m_worldAssets->frame.drawPackets.clear();
    m_worldAssets->frame.bridgeDrawPackets.clear();
    m_worldAssets->frame.reflectionDrawPackets.clear();
    m_worldAssets->frame.bridgeRadarGeometry.clear();
    m_worldAssets->frame.overlayDrawPackets.clear();
    m_worldAssets->frame.bibDrawPackets.clear();
    m_worldAssets->frame.groundProjectors.clear();
    m_worldAssets->frame.mapScorchProjectors.clear();
    m_worldAssets->frame.mapScorchTerrainRevision = 0;
    m_worldAssets->frame.mapScorchSourceCount = 0;
    m_worldAssets->frame.mapScorchSourceCursor = 0;
    m_worldAssets->frame.typedScorchBuildSources.clear();
    m_worldAssets->frame.typedScorchProjectors.clear();
    m_worldAssets->frame.typedScorchTerrainRevision = 0;
    m_worldAssets->frame.typedScorchSourceCursor = 0;
    m_worldAssets->frame.generalGroundDecals.clear();
    m_worldAssets->frame.generalGroundDecalEpoch = presentationEpoch;
    m_worldAssets->residency.restPalettes.clear();
    m_worldAssets->frame.modelGraphTraversalStats = {};

    if (m_worldAssets->fx.runtime) {
        m_worldAssets->fx.runtime->reset();
    }
    m_worldAssets->fx.pendingSnapshots.clear();
    m_worldAssets->fx.typed.reset(presentationEpoch);
    m_worldAssets->frame.fxBonePoseDemands.clear();
    m_worldAssets->frame.fxDeferredExecutionSnapshots.clear();
    m_worldAssets->particleRenderer.requestGpuSimulationReset(
        presentationEpoch);
    m_worldAssets->quality.gpuParticleAuthorityEpoch = presentationEpoch;
    m_worldAssets->projectileTrailRenderer.reset();
    m_worldAssets->trackMarkRenderer.reset();
    m_worldAssets->groundDecalPresentation.reset(presentationEpoch);
    m_worldAssets->frame.policeLights.clear();
    m_worldAssets->frame.policeLightPresentationEpoch = presentationEpoch;
    m_worldAssets->frame.policeLightSimulationFrame = 0;

    m_worldAssets->clientOptionsPresentation.reset();
    m_worldAssets->objectIconOverlay.reset();
    m_worldAssets->objectUiOverlay.reset();
    m_worldAssets->selectionFlash.reset(presentationEpoch);
    m_worldAssets->tacticalRadar.reset();

    m_worldAssets->terrainUploads.resetState();
    m_worldAssets->terrain.reset();
    m_worldAssets->residency.terrainTextureResolver.reset();
    m_worldAssets->residency.skyboxTextureOverrides.reset(
        *m_worldAssets->residency.textures);
    m_worldAssets->residency.treeTextureOverrides.reset(
        *m_worldAssets->residency.textures);
    m_worldAssets->particleRenderer.resetTextureCache();
    m_worldAssets->projectileTrailRenderer.resetTextureCache();
    m_worldAssets->trackMarkRenderer.resetTextureCache();
    m_worldAssets->waypointRenderer.resetTextureCache();
    m_worldAssets->fx.typed.worldRenderer().resetTextureCache();
    m_worldAssets->groundProjectorRenderer.resetTextureCache();
    m_worldAssets->fx.invalidateSkeletonBindings();
    m_worldAssets->residency.assets.clear();
    m_worldAssets->residency.animations.clear();
    m_worldAssets->residency.textures->resetSourceCache();

    m_worldAssets->debugWorld.clearSceneAssets();
    m_worldAssets->residency.registeredModelRevisions.clear();
    m_worldAssets->residency.reportedAssetFailures.clear();
    m_worldAssets->residency.reportedAnimationFailures.clear();
    m_worldAssets->durablePresentation.reset(presentationEpoch);

    m_worldAssets->stats.reset();

    m_worldAssets->lifetime.modelUploadPresentationEpoch = presentationEpoch;
    m_worldAssets->lifetime.started = false;
    m_worldAssets->lifetime.loading = false;
    ++m_worldAssets->lifetime.resetCount;
    TD_LOG_INFO(
        "[DX12Renderer] Presentation epoch reset {} -> {} ingress={} resets={} staleWorld={} staleFx={}",
        acceptedEpoch, presentationEpoch, fxIngress ? "FX" : "world",
        m_worldAssets->lifetime.resetCount,
        m_worldAssets->lifetime.staleWorldSnapshotRejectCount,
        m_worldAssets->lifetime.staleFxSnapshotRejectCount);
    return true;
}

void DX12Renderer::retireWorldPresentation(
    render::WorldPreparationStamp retiredWorld) {
    if (!m_worldAssets || retiredWorld.presentationEpoch == 0u ||
        retiredWorld.sessionRevision == 0u) {
        return;
    }
    m_worldAssets->lifetime.retiredPresentationEpoch = std::max(
        m_worldAssets->lifetime.retiredPresentationEpoch,
        retiredWorld.presentationEpoch);
    m_worldAssets->lifetime.retiredSessionRevision = std::max(
        m_worldAssets->lifetime.retiredSessionRevision,
        retiredWorld.sessionRevision);

    // An already accepted newer domain must survive a delayed retirement
    // command for its predecessor. Otherwise retire to the tombstone itself,
    // making the accepted epoch floor explicit even when no frame completed.
    if (m_worldAssets->lifetime.modelUploadPresentationEpoch >
        retiredWorld.presentationEpoch) {
        return;
    }

    // Render-view ingress is independent from world-snapshot ingress. A
    // delayed retirement can therefore encounter a view for the next domain
    // even when the next world endpoint has not reached the backend yet.
    // Preserve that newer view across the old domain's forced reset.
    std::optional<render::RenderViewState> preservedRenderView;
    if (m_worldAssets->view.latest &&
        m_worldAssets->view.latest->sourceWorld.presentationEpoch >
            m_worldAssets->lifetime.retiredPresentationEpoch &&
        m_worldAssets->view.latest->sourceWorld.sessionRevision >
            m_worldAssets->lifetime.retiredSessionRevision) {
        preservedRenderView = std::move(m_worldAssets->view.latest);
    }
    static_cast<void>(synchronizeWorldPresentationEpoch(
        m_worldAssets->lifetime.retiredPresentationEpoch, false, true));
    if (preservedRenderView) {
        setRenderViewState(std::move(*preservedRenderView));
    }
}

uint64_t DX12Renderer::worldPresentationEpoch() const noexcept {
    return m_worldAssets
        ? m_worldAssets->lifetime.modelUploadPresentationEpoch
        : 0u;
}

void DX12Renderer::pumpWorldCpuResourceCompletions() {
    if (!m_worldAssets) return;
    const bool loading = m_worldAssets->lifetime.loading;
    m_worldAssets->residency.assets.processCpuLoads(render::RenderAssetReadyBudget{
        .maxItems = loading
            ? render::performance_limits::kModelReadyPublishesPerLoadingFrame
            : render::performance_limits::kModelReadyPublishesPerFrame,
        .maxBytes = loading
            ? render::performance_limits::kModelReadyBytesPerLoadingFrame
            : render::performance_limits::kModelReadyBytesPerFrame,
        .maxElapsedMicroseconds = loading
            ? render::performance_limits::kModelReadyMicrosecondsPerLoadingFrame
            : render::performance_limits::kModelReadyMicrosecondsPerFrame,
    });
    m_worldAssets->residency.animations.processLoads(render::RenderAssetReadyBudget{
        .maxItems = loading
            ? render::performance_limits::kAnimationReadyPublishesPerLoadingFrame
            : render::performance_limits::kAnimationReadyPublishesPerFrame,
        .maxBytes = loading
            ? render::performance_limits::kAnimationReadyBytesPerLoadingFrame
            : render::performance_limits::kAnimationReadyBytesPerFrame,
        .maxElapsedMicroseconds = loading
            ? render::performance_limits::kAnimationReadyMicrosecondsPerLoadingFrame
            : render::performance_limits::kAnimationReadyMicrosecondsPerFrame,
    });
}

bool DX12Renderer::prepareWorldSnapshot(render::WorldRenderSnapshot snapshot) {
    if (!m_worldAssets) return false;
    if ((m_worldAssets->lifetime.retiredPresentationEpoch != 0u &&
         snapshot.presentationEpoch <=
             m_worldAssets->lifetime.retiredPresentationEpoch) ||
        (m_worldAssets->lifetime.retiredSessionRevision != 0u &&
         snapshot.sessionRevision <=
             m_worldAssets->lifetime.retiredSessionRevision)) {
        ++m_worldAssets->lifetime.staleWorldSnapshotRejectCount;
        return false;
    }
    if (m_worldAssets->debugWorld.preparationPending() ||
        m_worldAssets->submittedPreparationPending ||
        m_worldAssets->pipeline.isPreparing()) {
        return false;
    }
    m_worldAssets->debugWorld.cancelPreparation();
    m_worldAssets->submittedPreparationPending = false;
    if (!synchronizeWorldPresentationEpoch(
            snapshot.presentationEpoch, false)) {
        return false;
    }
    m_worldAssets->lifetime.loading =
        snapshot.loadingRevision != 0u;
    const bool loading = snapshot.loadingRevision != 0u;
#if TD_DEBUG_ENABLED
    m_worldAssets->debugVisualTraceObjectId =
        snapshot.debugVisualTraceObjectId;
#endif
    if (loading) {
        m_worldAssets->lifetime.startupSceneTicket.begin(
            snapshot.presentationEpoch, snapshot.sessionRevision,
            snapshot.loadingRevision);
    } else {
        m_worldAssets->lifetime.startupSceneTicket.reset();
    }
    // A map does not expose a separate skybox resource in the sealed source
    // model. Use a renderer-owned horizon clear derived from its environment
    // instead of retaining the old hard black backdrop. This runs before
    // beginFrame, so it affects exactly the frame that consumes this snapshot.
    math::vec3 horizon{0.035f, 0.075f, 0.135f};
    if (snapshot.camera.fogEnabled) {
        horizon = snapshot.camera.fogColor;
    } else if (snapshot.terrain && snapshot.terrain->globalLighting) {
        const auto& global = *snapshot.terrain->globalLighting;
        const math::vec3 ambient = global.objectLights[global.terrainLightSlot()].front().ambient;
        if (std::isfinite(ambient.x()) && std::isfinite(ambient.y()) && std::isfinite(ambient.z())) {
            horizon = {
                0.035f + std::max(ambient.x(), 0.0f) * 0.28f,
                0.075f + std::max(ambient.y(), 0.0f) * 0.32f,
                0.135f + std::max(ambient.z(), 0.0f) * 0.42f,
            };
        }
    }
    m_d3d12.setClearColor({horizon.x(), horizon.y(), horizon.z(), 1.0f});
    const auto prepareModelPhase = [this, loading](
            const container::String& modelAsset,
            const container::String& animationState) {
            if (modelAsset.empty()) return;
            const render::W3dModelHandle handle =
                m_worldAssets->residency.assets.requestAsync(
                    modelAsset, true,
                    render::RenderAssetPriority::Visible);
            if (!handle) return;
            if (loading) {
                m_worldAssets->lifetime.startupSceneTicket.addModel(handle);
                m_worldAssets->lifetime.startupSceneTicket.addAnimation(
                    animationState);
            }
            const std::optional<render::W3dAssetVersion> version =
                m_worldAssets->residency.assets.version(handle);
            if (!version) return;
            const auto model = m_worldAssets->residency.assets.cpuModel(handle);
            if (!model) {
                const std::optional<render::W3dAssetState> state =
                    m_worldAssets->residency.assets.state(handle);
                container::String diagnostic;
                if (state == render::W3dAssetState::Failed) {
                    diagnostic = m_worldAssets->residency.assets.error(handle);
                    if (diagnostic.empty()) {
                        diagnostic = "model asset failed";
                    }
                }
                m_worldAssets->pipeline.recordModelResolution(
                    modelAsset, std::move(diagnostic));
                return;
            }
            if (model &&
                m_worldAssets->residency.registeredModelRevisions[modelAsset] !=
                    *version) {
                m_worldAssets->pipeline.registerW3dModel(modelAsset, *model);
                m_worldAssets->residency.registeredModelRevisions[modelAsset] =
                    *version;
            }
            if (!model || animationState.empty()) return;
            auto animation = m_worldAssets->residency.animations.findLoaded(
                animationState);
            if (!animation) {
                m_worldAssets->residency.animations.requestAsync(
                    animationState,
                    render::RenderAssetPriority::Visible);
            }
            render::W3dAnimationDependency dependency =
                m_worldAssets->residency.animations.dependency(animationState);

            // Some shipped ZH ModelConditionState entries contain a malformed
            // HAnim clip suffix even though the model's own W3D contains the
            // compatible animation (for example NBNMissle_A3ENSS).  Once the
            // authored source has definitively failed, try the model-self
            // identity and register that clip under the authored state key.
            // Pending authored loads never take this path, so they cannot be
            // mistaken for terminal failure or complete a transition early.
            if (!animation && !dependency.diagnostic.empty()) {
                container::String fallbackAnimation = modelAsset;
                fallbackAnimation.push_back('.');
                fallbackAnimation.append(modelAsset);
                if (fallbackAnimation != animationState) {
                    animation = m_worldAssets->residency.animations.findLoaded(
                        fallbackAnimation);
                    if (!animation) {
                        m_worldAssets->residency.animations.requestAsync(
                            fallbackAnimation,
                            render::RenderAssetPriority::Visible);
                    }
                    const render::W3dAnimationDependency fallbackDependency =
                        m_worldAssets->residency.animations.dependency(
                            fallbackAnimation);
                    if (animation) {
                        dependency = fallbackDependency;
                    } else if (!fallbackDependency.diagnostic.empty()) {
                        dependency.diagnostic +=
                            "; model-self fallback failed: " +
                            fallbackDependency.diagnostic;
                    } else {
                        // The fallback is pending.  Keep resolution pending
                        // instead of publishing the malformed authored error.
                        dependency.diagnostic.clear();
                    }
                }
            }
            bool registered = false;
            container::String diagnostic = dependency.diagnostic;
            if (animation) {
                registered = m_worldAssets->pipeline.registerAnimation(
                    modelAsset, animationState, animation);
                if (!registered) {
                    diagnostic = m_worldAssets->pipeline.animationRegistrationError(
                        modelAsset, animationState);
                }
            }
            if (!registered) {
                m_worldAssets->pipeline.recordAnimationResolution(
                    modelAsset, animationState, diagnostic);
            }
            m_worldAssets->residency.assets.recordAnimationDependency(
                handle, animationState,
                dependency.hierarchyName, dependency.sourcePath,
                dependency.revision, registered, diagnostic);
            const container::String diagnosticKey = modelAsset + "|" + animationState;
            if (!registered && !diagnostic.empty() &&
                m_worldAssets->residency.reportedAnimationFailures.insert(diagnosticKey).second) {
                TD_LOG_WARN("[DX12Renderer] Animation '{}' rejected for model '{}': {}",
                    animationState, modelAsset, diagnostic);
            } else if (registered) {
                m_worldAssets->residency.reportedAnimationFailures.erase(diagnosticKey);
            }
    };
    for (const render::RenderModelPhaseDependency& dependency :
         snapshot.visualAssetDependencies) {
        prepareModelPhase(
            dependency.modelAsset, dependency.animationState);
    }
    for (const render::RenderEntitySnapshot& instance : snapshot.entities) {
        prepareModelPhase(instance.modelAsset, instance.visual.animationState);
        if (instance.visual.debris.enabled) {
            prepareModelPhase(
                instance.modelAsset,
                instance.visual.debris.initialAnimation);
            prepareModelPhase(
                instance.modelAsset,
                instance.visual.debris.flyingAnimation);
            prepareModelPhase(
                instance.modelAsset,
                instance.visual.debris.finalAnimation);
        }
        if (instance.animationCompletionTarget) {
            prepareModelPhase(
                instance.animationCompletionTarget->modelAsset,
                instance.animationCompletionTarget->animationState);
        }
        if (instance.animationFinalTarget) {
            prepareModelPhase(
                instance.animationFinalTarget->modelAsset,
                instance.animationFinalTarget->animationState);
        }
    }
#if TD_DEBUG_ENABLED
    if (snapshot.debugVisualTraceObjectId != 0u) {
        const auto combine = [](size_t& seed, size_t value) noexcept {
            seed ^= value + size_t{0x9E3779B9u} + (seed << 6u) +
                (seed >> 2u);
        };
        for (const render::RenderEntitySnapshot& instance : snapshot.entities) {
            if (instance.objectId != snapshot.debugVisualTraceObjectId)
                continue;
            const render::W3dModelHandle handle =
                m_worldAssets->residency.assets.requestAsync(
                    instance.modelAsset, true,
                    render::RenderAssetPriority::Visible);
            const std::optional<render::W3dAssetState> modelState =
                handle ? m_worldAssets->residency.assets.state(handle) : std::nullopt;
            const render::W3dAnimationDependency animation =
                m_worldAssets->residency.animations.dependency(
                    instance.visual.animationState);
            const container::String transitionModel =
                instance.animationCompletionTarget
                    ? instance.animationCompletionTarget->modelAsset
                    : container::String{};
            const container::String transitionAnimation =
                instance.animationCompletionTarget
                    ? instance.animationCompletionTarget->animationState
                    : container::String{};
            const container::String finalModel = instance.animationFinalTarget
                ? instance.animationFinalTarget->modelAsset
                : container::String{};
            const container::String finalAnimation =
                instance.animationFinalTarget
                    ? instance.animationFinalTarget->animationState
                    : container::String{};
            container::String launchBones;
            for (const container::String& bone : instance.weaponLaunchBones) {
                if (!launchBones.empty()) launchBones.push_back('|');
                launchBones.append(bone);
            }
            size_t fingerprint = 0;
            const auto addString = [&combine, &fingerprint](
                    const container::String& value) {
                combine(fingerprint, std::hash<container::String>{}(value));
            };
            combine(fingerprint, static_cast<size_t>(snapshot.presentationEpoch));
            combine(fingerprint, static_cast<size_t>(instance.channelIndex));
            combine(fingerprint, static_cast<size_t>(
                instance.visual.modelConditionFlags[0]));
            combine(fingerprint, static_cast<size_t>(
                instance.visual.modelConditionFlags[1]));
            addString(instance.modelAsset);
            addString(instance.visual.animationState);
            addString(transitionModel);
            addString(transitionAnimation);
            addString(finalModel);
            addString(finalAnimation);
            addString(launchBones);
            addString(animation.diagnostic);
            combine(fingerprint, std::hash<float>{}(
                instance.visual.animationTimeSeconds));
            combine(fingerprint, static_cast<size_t>(
                instance.visual.animationStart.generation));
            combine(fingerprint, static_cast<size_t>(
                instance.visual.animationResourcePendingGeneration));
            combine(fingerprint, static_cast<size_t>(
                instance.visual.animationResourcePendingPhase));
            combine(fingerprint, static_cast<size_t>(instance.visual.hidden));
            combine(fingerprint, static_cast<size_t>(
                instance.hiddenByLocalVisibility));
            combine(fingerprint, static_cast<size_t>(
                instance.localVisibilityState));
            combine(fingerprint, static_cast<size_t>(
                modelState ? static_cast<uint8_t>(*modelState) : UINT8_MAX));
            combine(fingerprint, static_cast<size_t>(animation.ready));
            const auto found =
                m_worldAssets->debugVisualTraceHashes.find(instance.id);
            if (found != m_worldAssets->debugVisualTraceHashes.end() &&
                found->second == fingerprint) {
                continue;
            }
            m_worldAssets->debugVisualTraceHashes.insert_or_assign(
                instance.id, fingerprint);
            TD_LOG_INFO(
                "[VisualTrace] tick={} object={} channel={} conditions={:016X}:{:016X} model='{}' modelState={} animation='{}' animationReady={} animationDiagnostic='{}' time={:.4f} generation={} pendingGeneration={} pendingPhase={} completionPhase={} transition='{}|{}' final='{}|{}' launchBones='{}' hidden={} shroudHidden={} visibility={}",
                snapshot.simulationFrame, instance.objectId,
                instance.channelIndex,
                instance.visual.modelConditionFlags[1],
                instance.visual.modelConditionFlags[0],
                instance.modelAsset,
                modelState ? static_cast<int>(*modelState) : -1,
                instance.visual.animationState, animation.ready,
                animation.diagnostic,
                instance.visual.animationTimeSeconds,
                instance.visual.animationStart.generation,
                instance.visual.animationResourcePendingGeneration,
                instance.visual.animationResourcePendingPhase,
                static_cast<int>(instance.animationCompletionPhase),
                transitionModel, transitionAnimation,
                finalModel, finalAnimation, launchBones,
                instance.visual.hidden,
                instance.hiddenByLocalVisibility,
                static_cast<int>(instance.localVisibilityState));
        }
    }
#endif
    m_worldAssets->pipeline.beginPreparation(
        std::move(snapshot), m_d3d12.width(), m_d3d12.height());
    m_worldAssets->submittedPreparationPending = true;
    return true;
}

void DX12Renderer::setRenderViewState(render::RenderViewState view) {
    if (!m_worldAssets || view.sourceWorld.presentationEpoch == 0u ||
        view.sourceWorld.sessionRevision == 0u) {
        return;
    }
    if ((m_worldAssets->lifetime.retiredPresentationEpoch != 0u &&
         view.sourceWorld.presentationEpoch <=
             m_worldAssets->lifetime.retiredPresentationEpoch) ||
        (m_worldAssets->lifetime.retiredSessionRevision != 0u &&
         view.sourceWorld.sessionRevision <=
             m_worldAssets->lifetime.retiredSessionRevision)) {
        return;
    }
    if (m_worldAssets->view.latest) {
        const render::RenderViewState& accepted =
            *m_worldAssets->view.latest;
        const bool olderSession = view.sourceWorld.sessionRevision <
            accepted.sourceWorld.sessionRevision;
        const bool sameSession = view.sourceWorld.sessionRevision ==
            accepted.sourceWorld.sessionRevision;
        const bool olderEpoch = sameSession &&
            view.sourceWorld.presentationEpoch <
                accepted.sourceWorld.presentationEpoch;
        const bool sameDomain = sameSession &&
            view.sourceWorld.presentationEpoch ==
                accepted.sourceWorld.presentationEpoch;
        if (olderSession || olderEpoch ||
            (sameDomain && view.viewRevision <= accepted.viewRevision)) {
            return;
        }
    }
    m_worldAssets->view.latest = std::move(view);
}

void DX12Renderer::setPresentationCameraOverride(
    render::PresentationCameraOverride cameraOverride) {
    if (!m_worldAssets || cameraOverride.sourceWorld.sessionRevision == 0u) {
        return;
    }
    if (!cameraOverride.active) {
        if (m_worldAssets->view.presentationCameraOverride &&
            m_worldAssets->view.presentationCameraOverride->sourceWorld.sessionRevision ==
                cameraOverride.sourceWorld.sessionRevision) {
            m_worldAssets->view.presentationCameraOverride.reset();
        }
        return;
    }
    if (cameraOverride.sourceWorld.presentationEpoch == 0u ||
        (m_worldAssets->lifetime.retiredPresentationEpoch != 0u &&
         cameraOverride.sourceWorld.presentationEpoch <=
             m_worldAssets->lifetime.retiredPresentationEpoch) ||
        (m_worldAssets->lifetime.retiredSessionRevision != 0u &&
         cameraOverride.sourceWorld.sessionRevision <=
             m_worldAssets->lifetime.retiredSessionRevision)) {
        return;
    }
    m_worldAssets->view.presentationCameraOverride = std::move(cameraOverride);
}

std::optional<render::RenderViewState>
DX12Renderer::currentRenderViewState() const noexcept {
    if (!m_worldAssets ||
        m_worldAssets->view.current.viewRevision == 0u ||
        m_worldAssets->view.current.sourceWorld.presentationEpoch == 0u) {
        return std::nullopt;
    }
    return m_worldAssets->view.current;
}

bool DX12Renderer::hasPreparedWorld() const noexcept {
    return m_worldAssets && m_worldAssets->pipeline.hasCompletedFrame();
}

DX12Renderer::PreparedWorldRenderResult
DX12Renderer::renderPreparedWorldWithStatus(
    TextureManager* objectIconTextures) {
    PreparedWorldRenderResult result;
    const bool preparationWasPending = m_worldAssets &&
        m_worldAssets->submittedPreparationPending;
    result.renderedInstances = renderPreparedWorld(objectIconTextures);
    if (!m_worldAssets) return result;

    // Observe submission state only after the render attempt. Endpoint
    // publication and pending-state observation are therefore one operation
    // from the subsystem's point of view, without a ready/render TOCTOU gap.
    if (m_worldAssets->submittedPreparationPending) {
        result.state = PreparedWorldRenderState::PreparationPending;
    } else if (!m_worldAssets->pipeline.hasCompletedFrame()) {
        result.state = PreparedWorldRenderState::Unavailable;
    } else if (preparationWasPending) {
        result.state = PreparedWorldRenderState::PublishedEndpoint;
    } else {
        result.state = PreparedWorldRenderState::ReusedEndpoint;
    }
    return result;
}

} // namespace engine
