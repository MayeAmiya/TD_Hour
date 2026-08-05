#include "DX12RendererWorldAssetRuntime.h"
#include <algorithm>
#include <limits>

namespace engine {

void DX12Renderer::updateWorldParticlePass(
    const render::PreparedWorldFrame& frame,
    float renderDeltaSeconds,
    render::ParticleRenderDrawList& particleDrawList) {
    particleDrawList.stats = {};
    particleDrawList.textureBindingGeneration = 0;
    particleDrawList.instances.clear();
    particleDrawList.batches.clear();
    particleDrawList.smudgeInstances.clear();
    particleDrawList.gpuReferenceSampleCount = 0;
    if (!m_worldAssets || !m_worldAssets->fx.runtime ||
        !m_worldAssets->fx.particleCatalog) {
        return;
    }
    // Dynamic LOD is intentionally absent: every authored priority is
    // admitted and no generation skip mask is applied.  The independent hard
    // pool ceiling still bounds memory.
    m_worldAssets->fx.runtime->particles().setDynamicAdmissionPolicy(
        fx::ParticlePriority::Invalid,
        fx::ParticlePriority::Invalid,
        render_lod_policy::kParticleSkipMask);
    const float particleScale = frame.renderGameDataSettings
        ? frame.renderGameDataSettings->visual.particleScale
        : 1.0f;
    m_worldAssets->fx.runtime->particles().setParticleScale(particleScale);
    // FX admission and world-frame preparation are independent buffered
    // streams. A prepared frame can therefore be older than an invocation
    // that has already advanced the FX runtime. Feeding that stale cursor to
    // ParticleRuntime is a same-epoch rewind, which correctly resets its
    // state for replay—but here no replay follows, so every other draw list
    // can become empty. Keep event admission ordered by confirmedFrame and
    // only clamp this renderer-side presentation cursor forward.
    const uint64_t particleSimulationFrame = std::max(
        m_worldAssets->frame.displayedSimulationFrame,
        m_worldAssets->fx.runtime->lastSubmittedSimulationFrame());
    m_worldAssets->fx.runtime->synchronizeSimulationFrame(
        particleSimulationFrame);
    m_worldAssets->fx.runtime->updateSeconds(renderDeltaSeconds);
    const fx::ParticleRuntimePhaseProfile& particlePhase =
        m_worldAssets->fx.runtime->particles().lastPhaseProfile();
    m_worldAssets->particleRenderer.queueGpuSimulationCommands(
        m_worldAssets->fx.runtime->particles().takeGpuCommands(),
        particlePhase.sampleOrdinal, particlePhase.authoredFrames);
}

void DX12Renderer::prepareWorldParticleDrawPass(
    const render::PreparedWorldFrame& frame,
    const render::RenderCameraSnapshot& presentationCamera,
    render::ParticleRenderDrawList& particleDrawList) {
    if (!m_worldAssets) return;
    size_t gpuGateParticleCount = 0;
    size_t gpuGateParticleBudget = 0;
    if (m_worldAssets->fx.runtime && m_worldAssets->fx.particleCatalog) {
        gpuGateParticleCount =
            m_worldAssets->fx.runtime->particles().particleCount();
        const size_t particleBudget = gpuGateParticleCount;
        gpuGateParticleBudget = particleBudget;
        const size_t maximumParticleExpansion =
            m_worldAssets->fx.particleDrawExpansionFactor;
        const size_t expandedParticleBudget = particleBudget >
                std::numeric_limits<size_t>::max() /
                    maximumParticleExpansion
            ? std::numeric_limits<size_t>::max()
            : particleBudget * maximumParticleExpansion;
        // FX extraction already applies object/system-level shroud and
        // lifetime admission. Do not run a second exact camera test for every
        // detached particle: rasterization clips billboards/streaks, while
        // existing particles remain visible as they drift away from an
        // emitter that has stopped spawning behind shroud, matching ZH.
        m_worldAssets->particleRenderer.buildDrawListIntoRetained(
            particleDrawList,
            m_worldAssets->fx.runtime->particles(),
            *m_worldAssets->fx.particleCatalog,
            presentationCamera.position, expandedParticleBudget,
            m_worldAssets->fx.runtime->particles().interpolationAlpha(),
            particleBudget, frame.localVisibility);
        m_worldAssets->particleRenderer.prepareTextureBindings(
            particleDrawList);
        static_cast<void>(m_worldAssets->particleRenderer
            .publishGpuVisibilityAuthority(particleDrawList));
    } else {
        particleDrawList.stats = {};
        particleDrawList.instances.clear();
        particleDrawList.batches.clear();
        particleDrawList.smudgeInstances.clear();
        particleDrawList.gpuVisibilityGenerations.clear();
        particleDrawList.gpuReferenceSampleCount = 0;
        static_cast<void>(m_worldAssets->particleRenderer
            .publishGpuVisibilityAuthority(particleDrawList));
    }
    m_worldAssets->particleRenderer.updateGpuPresentationGate(
        m_worldAssets->quality.requestedParticleSimulationBackend ==
            RenderParticleSimulationBackend::GpuCompute,
        gpuGateParticleCount, gpuGateParticleBudget);
}

} // namespace engine
