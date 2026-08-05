#include "DX12RendererWorldAssetRuntime.h"
#include "core/debug/debug.h"
#include "engine/renderer/d3d12/runtime/D3D12QualitySettings.h"
#include "engine/renderer/world/pipeline/WorldRenderer.h"

#include <algorithm>
#include <cmath>

namespace engine {

void DX12Renderer::applyRenderFeatureQuality(
    const ResolvedRenderFeatureSnapshot& feature) {
    if (!m_worldAssets) return;
    const uint32_t textureReductionFactor = std::min(
        feature.requested.textureReductionFactor,
        render_game_data_limits::kMaximumTextureReductionFactor);
    const size_t maximumParticles = std::min<size_t>(
        feature.requested.maximumParticles,
        render_game_data_limits::kMaximumParticles);
    const bool textureReductionChanged =
        m_worldAssets->quality.textureReductionFactor != textureReductionFactor;
    const bool particleAdmissionChanged =
        m_worldAssets->fx.maximumParticles != maximumParticles ||
        m_worldAssets->quality.dynamicLod !=
            render_lod_policy::kDynamicProfile ||
        m_worldAssets->fx.minimumParticlePriority !=
            fx::ParticlePriority::Invalid ||
        m_worldAssets->fx.minimumParticleSkipPriority !=
            fx::ParticlePriority::Invalid ||
        m_worldAssets->fx.particleSkipMask !=
            render_lod_policy::kParticleSkipMask ||
        m_worldAssets->quality.requestedParticleSimulationBackend !=
            feature.requested.particleSimulationBackend;
    const bool terrainMacroChanged =
        m_worldAssets->quality.useCloudMap != feature.requested.useCloudMap ||
        m_worldAssets->quality.useLightMap != feature.requested.useLightMap;
    if (m_worldAssets->quality.featureRevision == feature.revision &&
        !textureReductionChanged && !particleAdmissionChanged &&
        !terrainMacroChanged) {
        return;
    }

    if (textureReductionChanged) {
        // Mip policy is a Feature/session variant. Retire every immutable
        // material owner before the cache changes interpretation; sampler
        // changes belong to Display and never enter this invalidation path.
        m_worldAssets->terrainUploads.invalidateForTexturePolicy();
        m_worldAssets->terrain.reset();
        m_worldAssets->residency.assets.clear();
        m_worldAssets->residency.skyboxTextureOverrides.reset(
            *m_worldAssets->residency.textures);
        m_worldAssets->residency.treeTextureOverrides.reset(
            *m_worldAssets->residency.textures);
        m_worldAssets->debugWorld.setModel({});
        m_worldAssets->residency.registeredModelRevisions.clear();
        m_worldAssets->particleRenderer.resetTextureCache();
        m_worldAssets->projectileTrailRenderer.resetTextureCache();
        m_worldAssets->trackMarkRenderer.resetTextureCache();
        m_worldAssets->waypointRenderer.resetTextureCache();
        m_worldAssets->fx.typed.worldRenderer().resetTextureCache();
        m_worldAssets->groundProjectorRenderer.shutdown();
        m_worldAssets->residency.textures->configureTextureReduction(
            textureReductionFactor);
        if (!m_worldAssets->groundProjectorRenderer.init(
                m_d3d12, m_worldAssets->residency.textures)) {
            TD_LOG_ERROR(
                "[RenderQuality] ground projector reinitialization failed after Feature mip change");
        }
    }
    m_worldAssets->fx.maximumParticles = maximumParticles;
    m_worldAssets->quality.textureReductionFactor = textureReductionFactor;
    m_worldAssets->quality.dynamicLod =
        render_lod_policy::kDynamicProfile;
    const bool gpuComputeRequested =
        feature.requested.particleSimulationBackend ==
        RenderParticleSimulationBackend::GpuCompute;
    const bool gpuComputeInfrastructureReady =
        m_worldAssets->particleRenderer.configureGpuSimulationInfrastructure(
            gpuComputeRequested);
    m_worldAssets->quality.requestedParticleSimulationBackend =
        feature.requested.particleSimulationBackend;
    // CPU stays authoritative until the birth/compact/indirect dispatcher is
    // complete. Do not report a partially connected Compute path as effective.
    m_worldAssets->quality.particleSimulationBackend =
        RenderParticleSimulationBackend::Cpu;
    if (m_worldAssets->fx.runtime) {
        m_worldAssets->fx.runtime->particles().setGpuCommandCaptureEnabled(
            gpuComputeRequested && gpuComputeInfrastructureReady);
    }
    m_worldAssets->quality.useCloudMap = feature.requested.useCloudMap;
    m_worldAssets->quality.useLightMap = feature.requested.useLightMap;
    if (particleAdmissionChanged && m_worldAssets->fx.runtime) {
        constexpr fx::ParticlePriority minimumPriority =
            fx::ParticlePriority::Invalid;
        constexpr fx::ParticlePriority minimumSkipPriority =
            fx::ParticlePriority::Invalid;
        constexpr uint32_t skipMask =
            render_lod_policy::kParticleSkipMask;
        m_worldAssets->fx.minimumParticlePriority = minimumPriority;
        m_worldAssets->fx.minimumParticleSkipPriority = minimumSkipPriority;
        m_worldAssets->fx.particleSkipMask = skipMask;
        m_worldAssets->fx.runtime->particles().setAdmissionSettings({
            .ordinaryParticleLimit = maximumParticles,
            .fieldParticleLimit =
                m_worldAssets->fx.maximumFieldParticles,
            .minimumPriority = minimumPriority,
            .minimumSkipPriority = minimumSkipPriority,
            .skipMask = skipMask,
        });
    }
    m_worldAssets->quality.featureRevision = feature.revision;
    TD_LOG_INFO(
        "[RenderQuality] Feature revision={} lod={}/{} particles={} textureReduction={} particleBackend(requested={},effective={},computeReady={}) terrainMacro(cloud={},light={})",
        feature.revision,
        static_cast<uint32_t>(feature.requested.staticLod),
        static_cast<uint32_t>(m_worldAssets->quality.dynamicLod),
        maximumParticles, textureReductionFactor,
        static_cast<uint32_t>(feature.requested.particleSimulationBackend),
        static_cast<uint32_t>(m_worldAssets->quality.particleSimulationBackend),
        gpuComputeInfrastructureReady,
        m_worldAssets->quality.useCloudMap,
        m_worldAssets->quality.useLightMap);
}

void DX12Renderer::applyRenderDisplaySettings(
    const ResolvedRenderDisplaySnapshot& display) {
    if (!m_worldAssets || !m_worldRenderer) return;
    const uint32_t textureFilter = std::min(
        display.effective.textureFilter, 4u);
    const uint32_t anisotropyLevel = std::clamp(
        display.effective.anisotropyLevel, 2u, 16u);
    const float displayGamma = std::clamp(
        std::isfinite(display.effective.displayGamma)
            ? display.effective.displayGamma : 1.0f,
        0.6f, 2.0f);
    constexpr uint32_t worldSampleCount = 1u;
    const uint32_t requestedWidth = std::max(1u, display.effective.width);
    const uint32_t requestedHeight = std::max(1u, display.effective.height);
    m_worldAssets->output.lastResolvedDisplay = display;
    m_worldAssets->output.hasResolvedDisplay = true;
    if (m_worldAssets->quality.displayRevision == display.revision &&
        m_worldAssets->quality.textureFilter == textureFilter &&
        m_worldAssets->quality.anisotropyLevel == anisotropyLevel &&
        m_worldAssets->quality.displayGamma == displayGamma &&
        m_worldAssets->quality.worldSampleCount == worldSampleCount &&
        m_worldAssets->output.verticalSync == display.effective.verticalSync) {
        return;
    }
    // Main owns SDL output application and publishes the observed result via
    // RenderWindowOutputState. This function applies render-device quality
    // only; requested output values are retained for diagnostics/comparison.
    m_worldAssets->output.displayWidth = requestedWidth;
    m_worldAssets->output.displayHeight = requestedHeight;
    m_worldAssets->output.displayMode = display.effective.displayMode;
    m_worldAssets->output.displayRefreshRateHz =
        display.effective.refreshRateHz;

    // The project deliberately does not use MSAA. Legacy AntiAliasing is
    // retained only for options migration and can never change this target.
    static_cast<void>(m_d3d12.configureWorldMultisampling(worldSampleCount));
    const bool samplingReady =
        m_worldRenderer->configureTextureSampling(
            textureFilter, anisotropyLevel, worldSampleCount) &&
        m_worldAssets->particleRenderer.configureTextureSampling(
            textureFilter, anisotropyLevel, worldSampleCount) &&
        m_worldAssets->projectileTrailRenderer.configureTextureSampling(
            textureFilter, anisotropyLevel, worldSampleCount) &&
        m_worldAssets->groundProjectorRenderer.configureTextureSampling(
            textureFilter, anisotropyLevel, worldSampleCount) &&
        m_worldAssets->fx.typed.worldRenderer().configureSampleCount(
            worldSampleCount);
    if (!samplingReady) {
        TD_LOG_ERROR(
            "[RenderQuality] Display sampling-dependent PSO rebuild failed");
        return;
    }
    const bool fxaaRequested = display.effective.antiAliasingMode ==
        RenderAntiAliasingMode::Fxaa;
    const bool fxaaReady = m_worldRenderer->configureFxaa(
        fxaaRequested, display.effective.fxaaSubpixel,
        display.effective.fxaaEdgeThreshold,
        display.effective.fxaaEdgeThresholdMin);
    if (fxaaRequested && !fxaaReady) {
        TD_LOG_WARN(
            "[RenderQuality] FXAA requested but renderer resources are unavailable; effective mode is Off");
    }
    static_cast<void>(m_d3d12.configureDisplayGamma(displayGamma));
    m_d3d12.configureVerticalSync(display.effective.verticalSync);
    m_worldAssets->particleRenderer.resetTextureCache();
    m_worldAssets->projectileTrailRenderer.resetTextureCache();
    m_worldAssets->quality.textureFilter = textureFilter;
    m_worldAssets->quality.anisotropyLevel = anisotropyLevel;
    m_worldAssets->quality.displayGamma = displayGamma;
    m_worldAssets->quality.worldSampleCount = worldSampleCount;
    m_worldAssets->output.verticalSync = display.effective.verticalSync;
    m_worldAssets->quality.effectiveAntiAliasingMode =
        fxaaRequested && fxaaReady
        ? RenderAntiAliasingMode::Fxaa
        : RenderAntiAliasingMode::Off;
    m_worldAssets->quality.displayRevision = display.revision;
    TD_LOG_INFO(
        "[RenderQuality] Display revision={} requested={}x{}:{}@{} effective={}x{}:{}@{} applied={}x{}:{}@{} pixel={}x{} appliedRev={} attemptRev={} outputOk={} state={}/{} filter={} anisotropy={}x aa={} fallback=0x{:X} changes=0x{:X}",
        display.revision, display.requested.width, display.requested.height,
        static_cast<uint32_t>(display.requested.displayMode),
        display.requested.refreshRateHz,
        display.effective.width, display.effective.height,
        static_cast<uint32_t>(display.effective.displayMode),
        display.effective.refreshRateHz,
        m_worldAssets->output.appliedOutputWidth,
        m_worldAssets->output.appliedOutputHeight,
        static_cast<uint32_t>(m_worldAssets->output.appliedOutputMode),
        m_worldAssets->output.appliedOutputRefreshRateHz,
        m_worldAssets->output.displayPixelWidth,
        m_worldAssets->output.displayPixelHeight,
        m_worldAssets->output.appliedOutputRevision,
        m_worldAssets->output.lastOutputAttemptRevision,
        m_worldAssets->output.lastOutputApplySucceeded,
        m_worldAssets->output.hasAppliedOutput,
        m_worldAssets->output.displayPixelExtentValid,
        textureFilter,
        anisotropyLevel,
        static_cast<uint32_t>(
            m_worldAssets->quality.effectiveAntiAliasingMode),
        static_cast<uint32_t>(display.fallbackMask),
        static_cast<uint32_t>(display.changeMask));
}

} // namespace engine
