#include "DX12RendererWorldAssetRuntime.h"

#include "core/debug/debug.h"
#include "engine/renderer/world/pipeline/WorldRenderer.h"

#include <algorithm>

namespace engine {

bool DX12Renderer::fxaaAvailable() const noexcept {
    return m_worldRenderer && m_worldRenderer->fxaaAvailable();
}

void DX12Renderer::configureGpuParticlePresentationQualification(
    const render::GpuParticlePresentationQualification& qualification) {
    if (!m_worldAssets) return;
    m_worldAssets->particleRenderer
        .configureGpuPresentationQualification(qualification);
}

void DX12Renderer::configureFxContent(
    container::SharedPtr<const fx::ParticleSystemCatalog> particles,
    container::SharedPtr<const fx::FxListCatalog> fxLists) {
    RenderOperationalBudget legacyBudget;
    legacyBudget.maximumParticles =
        render_game_data_limits::kMaximumParticles;
    configureFxContent(
        std::move(particles), std::move(fxLists), legacyBudget);
}

void DX12Renderer::configureFxContent(
    container::SharedPtr<const fx::ParticleSystemCatalog> particles,
    container::SharedPtr<const fx::FxListCatalog> fxLists,
    const RenderOperationalBudget& budget) {
    if (m_worldAssets && m_worldAssets->quality.featureRevision == 0u) {
        const size_t maximumParticles = std::min<size_t>(
            budget.maximumParticles,
            render_game_data_limits::kMaximumParticles);
        if (m_worldAssets->fx.maximumParticles != maximumParticles) {
            m_worldAssets->fx.maximumParticles = maximumParticles;
            m_worldAssets->fx.runtime.reset();
        }
    }
    RenderGameDataSettings settings;
    settings.operational = budget;
    configureFxContent(std::move(particles), std::move(fxLists), settings);
}

void DX12Renderer::configureFxContent(
    container::SharedPtr<const fx::ParticleSystemCatalog> particles,
    container::SharedPtr<const fx::FxListCatalog> fxLists,
    const RenderGameDataSettings& settings) {
    if (!m_worldAssets) return;
    const RenderOperationalBudget& budget = settings.operational;
    // maximumParticles and mip policy come from the explicitly applied,
    // session-frozen Feature snapshot. This function owns catalogs plus
    // non-user operational budgets only.
    const size_t maximumParticles =
        m_worldAssets->fx.maximumParticles;
    const size_t maximumFieldParticles = std::min<size_t>(
        budget.maximumFieldParticles,
        render_game_data_limits::kMaximumFieldParticles);
    // There is no runtime particle LOD.  Invalid is the lowest admission
    // threshold in ParticleRuntime, so every authored priority is accepted;
    // a zero skip mask performs no generation thinning.
    constexpr fx::ParticlePriority minimumParticlePriority =
        fx::ParticlePriority::Invalid;
    constexpr fx::ParticlePriority minimumParticleSkipPriority =
        fx::ParticlePriority::Invalid;
    constexpr uint32_t particleSkipMask =
        render_lod_policy::kParticleSkipMask;
    const float particleScale = std::isfinite(settings.visual.particleScale)
        ? std::max(0.0f, settings.visual.particleScale)
        : 1.0f;
    const size_t maximumEmitters = std::min<size_t>(
        budget.maximumParticleEmitters,
        render_game_data_limits::kMaximumParticleEmitters);
    const size_t initialEmitterCapacity = std::min<size_t>(
        budget.initialParticleEmitterCapacity, maximumEmitters);
    const size_t particleDrawExpansionFactor = std::clamp<size_t>(
        budget.particleDrawExpansionFactor, 1,
        render_game_data_limits::kMaximumParticleDrawExpansionFactor);
    const size_t modelUploadsPerFrame = std::clamp<size_t>(
        budget.modelUploadsPerFrame, 1,
        render_game_data_limits::kMaximumModelUploadsPerFrame);
    const size_t modelUploadsPerLoadingFrame = std::clamp<size_t>(
        budget.modelUploadsPerLoadingFrame, 1,
        render_game_data_limits::kMaximumModelUploadsPerFrame);
    const uint64_t modelUploadBytesPerFrame = std::clamp<uint64_t>(
        budget.modelUploadBytesPerFrame, 1,
        render_game_data_limits::kMaximumModelUploadBytesPerFrame);
    const uint64_t modelUploadBytesPerLoadingFrame = std::clamp<uint64_t>(
        budget.modelUploadBytesPerLoadingFrame, 1,
        render_game_data_limits::kMaximumModelUploadBytesPerFrame);
    const uint64_t modelUploadMicrosecondsPerFrame = std::clamp<uint64_t>(
        budget.modelUploadMicrosecondsPerFrame, 1,
        render_game_data_limits::kMaximumModelUploadMicrosecondsPerFrame);
    const uint64_t modelUploadMicrosecondsPerLoadingFrame =
        std::clamp<uint64_t>(
            budget.modelUploadMicrosecondsPerLoadingFrame, 1,
            render_game_data_limits::kMaximumModelUploadMicrosecondsPerFrame);
    const size_t maximumAttachedEmitters = std::min<size_t>(
        budget.maximumAttachedFxEmitters,
        render_game_data_limits::kMaximumAttachedFxEmitters);
    const size_t maximumPresentationCommands = std::min<size_t>(
        budget.maximumFxPresentationCommands,
        render_game_data_limits::kMaximumFxPresentationCommands);
    const uint32_t maximumGroundProjectorsPerFrame = std::min(
        budget.maximumGroundProjectorsPerFrame,
        render_game_data_limits::kMaximumGroundProjectorsPerFrame);
    const uint32_t maximumGroundProjectorTextures = std::min(
        budget.maximumGroundProjectorTextures,
        render_game_data_limits::kMaximumGroundProjectorTextures);
    if (m_worldAssets->fx.particleCatalog == particles &&
        m_worldAssets->fx.listCatalog == fxLists && m_worldAssets->fx.runtime &&
        m_worldAssets->fx.maximumParticles == maximumParticles &&
        m_worldAssets->fx.maximumFieldParticles == maximumFieldParticles &&
        m_worldAssets->fx.minimumParticlePriority == minimumParticlePriority &&
        m_worldAssets->fx.minimumParticleSkipPriority ==
            minimumParticleSkipPriority &&
        m_worldAssets->fx.particleSkipMask == particleSkipMask &&
        m_worldAssets->fx.particleScale == particleScale &&
        m_worldAssets->fx.initialEmitterCapacity == initialEmitterCapacity &&
        m_worldAssets->fx.maximumEmitters == maximumEmitters &&
        m_worldAssets->fx.maximumAttachedEmitters == maximumAttachedEmitters &&
        m_worldAssets->fx.maximumPresentationCommands ==
            maximumPresentationCommands &&
        m_worldAssets->fx.particleDrawExpansionFactor ==
            particleDrawExpansionFactor &&
        m_worldAssets->quality.modelUploadsPerFrame == modelUploadsPerFrame &&
        m_worldAssets->quality.modelUploadsPerLoadingFrame ==
            modelUploadsPerLoadingFrame &&
        m_worldAssets->quality.modelUploadBytesPerFrame ==
            modelUploadBytesPerFrame &&
        m_worldAssets->quality.modelUploadBytesPerLoadingFrame ==
            modelUploadBytesPerLoadingFrame &&
        m_worldAssets->quality.modelUploadMicrosecondsPerFrame ==
            modelUploadMicrosecondsPerFrame &&
        m_worldAssets->quality.modelUploadMicrosecondsPerLoadingFrame ==
            modelUploadMicrosecondsPerLoadingFrame &&
        m_worldAssets->quality.maximumGroundProjectorsPerFrame ==
            maximumGroundProjectorsPerFrame &&
        m_worldAssets->quality.maximumGroundProjectorTextures ==
            maximumGroundProjectorTextures) {
        return;
    }
    m_worldAssets->fx.runtime.reset();
    m_worldAssets->fx.typed.reset();
    m_worldAssets->frame.fxBonePoseDemands.clear();
    m_worldAssets->frame.fxDeferredExecutionSnapshots.clear();
    m_worldAssets->fx.invalidateSkeletonBindings();
    m_worldAssets->stats.resetFirstFrame(
        render::WorldFirstFrameDiagnostic::Particle);
    m_worldAssets->stats.resetFirstFrame(
        render::WorldFirstFrameDiagnostic::Smudge);
    m_worldAssets->stats.resetFirstFrame(
        render::WorldFirstFrameDiagnostic::TypedFx);
    m_worldAssets->stats.resetFirstFrame(
        render::WorldFirstFrameDiagnostic::DynamicLight);
    m_worldAssets->fx.particleCatalog = std::move(particles);
    m_worldAssets->fx.listCatalog = std::move(fxLists);
    m_worldAssets->fx.maximumParticles = maximumParticles;
    m_worldAssets->fx.maximumFieldParticles = maximumFieldParticles;
    m_worldAssets->fx.minimumParticlePriority = minimumParticlePriority;
    m_worldAssets->fx.minimumParticleSkipPriority =
        minimumParticleSkipPriority;
    m_worldAssets->fx.particleSkipMask = particleSkipMask;
    m_worldAssets->fx.particleScale = particleScale;
    m_worldAssets->fx.initialEmitterCapacity = initialEmitterCapacity;
    m_worldAssets->fx.maximumEmitters = maximumEmitters;
    m_worldAssets->fx.maximumAttachedEmitters = maximumAttachedEmitters;
    m_worldAssets->fx.maximumPresentationCommands =
        maximumPresentationCommands;
    m_worldAssets->fx.particleDrawExpansionFactor = particleDrawExpansionFactor;
    m_worldAssets->quality.modelUploadsPerFrame = modelUploadsPerFrame;
    m_worldAssets->quality.modelUploadsPerLoadingFrame = modelUploadsPerLoadingFrame;
    m_worldAssets->quality.modelUploadBytesPerFrame = modelUploadBytesPerFrame;
    m_worldAssets->quality.modelUploadBytesPerLoadingFrame =
        modelUploadBytesPerLoadingFrame;
    m_worldAssets->quality.modelUploadMicrosecondsPerFrame =
        modelUploadMicrosecondsPerFrame;
    m_worldAssets->quality.modelUploadMicrosecondsPerLoadingFrame =
        modelUploadMicrosecondsPerLoadingFrame;
    m_worldAssets->quality.maximumGroundProjectorsPerFrame =
        maximumGroundProjectorsPerFrame;
    m_worldAssets->quality.maximumGroundProjectorTextures =
        maximumGroundProjectorTextures;
    m_worldAssets->groundProjectorRenderer.configureOperationalBudget(
        maximumGroundProjectorsPerFrame, maximumGroundProjectorTextures);
    if (m_worldAssets->fx.particleCatalog) {
        m_worldAssets->particleRenderer.configureGpuMaterialBins(
            *m_worldAssets->fx.particleCatalog);
    }
    if (m_worldAssets->fx.particleCatalog && m_worldAssets->fx.listCatalog) {
        m_worldAssets->fx.runtime = std::make_unique<fx::FxRuntime>(
            m_worldAssets->fx.particleCatalog, m_worldAssets->fx.listCatalog,
            render_game_data_limits::kMaximumParticles,
            initialEmitterCapacity, maximumEmitters,
            fx::ParticleAdmissionSettings{
                .ordinaryParticleLimit = maximumParticles,
                .fieldParticleLimit = maximumFieldParticles,
                .minimumPriority = minimumParticlePriority,
                .minimumSkipPriority = minimumParticleSkipPriority,
                .skipMask = particleSkipMask,
            }, maximumAttachedEmitters, maximumPresentationCommands);
        m_worldAssets->fx.runtime->particles().setParticleScale(particleScale);
        m_worldAssets->fx.runtime->particles().setGpuCommandCaptureEnabled(
            m_worldAssets->quality.requestedParticleSimulationBackend ==
                    RenderParticleSimulationBackend::GpuCompute &&
                m_worldAssets->particleRenderer
                    .gpuSimulationInfrastructureReady());
    }
}

void DX12Renderer::submitFxSnapshot(const fx::FxPresentationSnapshot& snapshot) {
    if (!m_worldAssets) return;
    if (!synchronizeWorldPresentationEpoch(snapshot.sessionEpoch, true)) {
        return;
    }

    auto& pending = m_worldAssets->fx.pendingSnapshots;
    if (pending.empty() ||
        pending.back().simulationFrame <= snapshot.simulationFrame) {
        pending.push_back(snapshot);
        return;
    }
    // Normal GameSession extraction is monotonic. Keep diagnostics/import
    // traffic deterministic as well without allowing one late value to block
    // every later confirmed FX frame at the front of the queue.
    const auto insertion = std::upper_bound(
        pending.begin(), pending.end(), snapshot.simulationFrame,
        [](uint64_t frame, const fx::FxPresentationSnapshot& candidate) {
            return frame < candidate.simulationFrame;
        });
    pending.insert(insertion, snapshot);
}

void DX12Renderer::consumeDisplayedFxSnapshot(
    const fx::FxPresentationSnapshot& snapshot) {
    if (!m_worldAssets) return;
    m_worldAssets->stats.setVisibilityRejectedFx(
        snapshot.visibilityRejectedObjects,
        snapshot.visibilityRejectedInvocations);
    if (m_worldAssets->fx.runtime) {
        container::Vector<fx::FxPresentationInvocation>& admitted =
            m_worldAssets->frame.fxAdmittedInvocations;
        m_worldAssets->fx.runtime->admitInvocationsInto(admitted, snapshot);
        // Discover sparse bone requirements before executing the batch. The
        // prepared-world pass below publishes those exact current-frame poses
        // and only then expands the invocation. This mirrors
        // W3DModelDraw::handleWeaponFireFX reading the current render object;
        // using the previous frame's demand set made a first/isolated tracer
        // originate at the object root.
        m_worldAssets->fx.runtime->collectBonePoseDemands(
            container::Span<const fx::FxPresentationInvocation>(admitted),
            m_worldAssets->frame.fxBonePoseDemands);
        if (!admitted.empty()) {
            // Execution only needs the confirmed clock, immutable lookup
            // handles and admitted invocation stream.  Do not duplicate the
            // snapshot's complete object/vehicle anchor columns into this
            // per-present scratch queue; admission already installed their
            // latest displayed state in FxRuntime.
            fx::FxPresentationSnapshot deferred{
                .sessionEpoch = snapshot.sessionEpoch,
                .simulationFrame = snapshot.simulationFrame,
                .logicFramesPerSecond = snapshot.logicFramesPerSecond,
                .groundHeights = snapshot.groundHeights,
                .legacyBeamTemplates = snapshot.legacyBeamTemplates,
                .invocations = std::move(admitted),
            };
            m_worldAssets->frame.fxDeferredExecutionSnapshots.push_back(
                std::move(deferred));
        }
    }
}

void DX12Renderer::releaseFxSnapshotsThrough(
    uint64_t presentationEpoch, uint64_t simulationFrame) {
    if (!m_worldAssets || presentationEpoch == 0u) return;
    auto& pending = m_worldAssets->fx.pendingSnapshots;
    while (!pending.empty()) {
        const fx::FxPresentationSnapshot& snapshot = pending.front();
        if (snapshot.sessionEpoch < presentationEpoch) {
            pending.pop_front();
            continue;
        }
        if (snapshot.sessionEpoch > presentationEpoch ||
            snapshot.simulationFrame > simulationFrame) {
            break;
        }
        consumeDisplayedFxSnapshot(snapshot);
        pending.pop_front();
    }
}

container::Vector<fx::FxSoundCommand> DX12Renderer::takeFxSoundCommands() {
    return m_worldAssets
        ? m_worldAssets->fx.typed.takeSounds()
        : container::Vector<fx::FxSoundCommand>{};
}

void DX12Renderer::clearFxPresentation() {
    if (m_worldAssets && m_worldAssets->fx.runtime) {
        m_worldAssets->fx.runtime->reset();
    }
    if (m_worldAssets) {
        m_worldAssets->fx.pendingSnapshots.clear();
        m_worldAssets->particleRenderer.requestGpuSimulationReset(0);
        m_worldAssets->quality.gpuParticleAuthorityEpoch = 0;
        m_worldAssets->particleRenderer.resetTextureCache();
        m_worldAssets->projectileTrailRenderer.resetTextureCache();
        m_worldAssets->fx.typed.reset();
        m_worldAssets->frame.fxBonePoseDemands.clear();
        m_worldAssets->frame.fxDeferredExecutionSnapshots.clear();
        m_worldAssets->projectileTrailRenderer.reset();
        m_worldAssets->stats.resetFirstFrame(
            render::WorldFirstFrameDiagnostic::ProjectileTrail);
        m_worldAssets->stats.resetFirstFrame(
            render::WorldFirstFrameDiagnostic::TypedFx);
        m_worldAssets->stats.resetFirstFrame(
            render::WorldFirstFrameDiagnostic::DynamicLight);
    }
}

void DX12Renderer::submitGroundDecalPresentation(
    const render::GroundDecalPresentationBatch& batch) {
    if (!m_worldAssets) return;
    if (batch.presentationEpoch == 0u ||
        (m_worldAssets->lifetime.retiredPresentationEpoch != 0u &&
         batch.presentationEpoch <=
             m_worldAssets->lifetime.retiredPresentationEpoch)) {
        return;
    }
    if (batch.presentationEpoch != 0 &&
        batch.presentationEpoch !=
            m_worldAssets->groundDecalPresentation.presentationEpoch()) {
        m_worldAssets->stats.resetFirstFrame(
            render::WorldFirstFrameDiagnostic::GroundDecal);
    }
    static_cast<void>(
        m_worldAssets->groundDecalPresentation.submit(batch));
}

void DX12Renderer::clearGroundDecalPresentation(
    uint64_t presentationEpoch) {
    if (m_worldAssets) {
        m_worldAssets->groundDecalPresentation.reset(presentationEpoch);
        m_worldAssets->stats.resetFirstFrame(
            render::WorldFirstFrameDiagnostic::GroundDecal);
    }
}

std::optional<render::RenderCameraSnapshot>
DX12Renderer::scriptCameraSlaveListenerOverride(
    uint64_t expectedPresentationEpoch) const noexcept {
    if (!m_worldAssets || expectedPresentationEpoch == 0 ||
        (m_worldAssets->lifetime.retiredPresentationEpoch != 0u &&
         expectedPresentationEpoch <=
             m_worldAssets->lifetime.retiredPresentationEpoch) ||
        !m_worldAssets->durablePresentation.cameraSlaveListener(
            expectedPresentationEpoch)) {
        return std::nullopt;
    }
    return m_worldAssets->durablePresentation.cameraSlaveListener(
        expectedPresentationEpoch);
}

std::optional<render::WorldFrameRenderStats>
DX12Renderer::lastWorldFrameStats() const noexcept {
    if (!m_worldAssets || !m_worldAssets->stats.hasFrame()) {
        return std::nullopt;
    }
    return m_worldAssets->stats.frame();
}

bool DX12Renderer::submitGroundDecal(
    uint64_t presentationEpoch, render::GroundProjectorInstance decal) {
    if (!m_worldAssets || presentationEpoch == 0) return false;
    if (m_worldAssets->lifetime.retiredPresentationEpoch != 0u &&
        presentationEpoch <= m_worldAssets->lifetime.retiredPresentationEpoch) {
        return false;
    }
    if (m_worldAssets->frame.generalGroundDecalEpoch != 0 &&
        presentationEpoch < m_worldAssets->frame.generalGroundDecalEpoch) {
        return false;
    }
    if (presentationEpoch != m_worldAssets->frame.generalGroundDecalEpoch) {
        m_worldAssets->frame.generalGroundDecals.clear();
        m_worldAssets->frame.generalGroundDecalEpoch = presentationEpoch;
    }
    if (m_worldAssets->frame.generalGroundDecals.size() >=
        ground_decals::performance_limits::kHardMaximumInstancesPerFrame) {
        return false;
    }
    m_worldAssets->frame.generalGroundDecals.push_back(std::move(decal));
    return true;
}

void DX12Renderer::clearGroundDecals(uint64_t presentationEpoch) {
    if (!m_worldAssets) return;
    if (presentationEpoch != 0 &&
        m_worldAssets->frame.generalGroundDecalEpoch != 0 &&
        presentationEpoch < m_worldAssets->frame.generalGroundDecalEpoch) {
        return;
    }
    m_worldAssets->frame.generalGroundDecals.clear();
    m_worldAssets->frame.generalGroundDecalEpoch = presentationEpoch;
}

} // namespace engine
