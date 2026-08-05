#include "DX12RendererWorldAssetRuntime.h"
#include "core/debug/debug.h"
#include "engine/renderer/world/pipeline/WorldRenderer.h"

#include <algorithm>
#include <chrono>
#include <limits>

namespace engine {

size_t DX12Renderer::finalizeWorldFrameStats(
    const render::PreparedWorldFrame& frame,
    const render::ParticleRenderDrawList& particleDrawList,
    const WorldFrameFinalizeInput& input) {
    if (!m_worldAssets || !m_worldRenderer) return 0;
    const auto statsPublicationStart = std::chrono::steady_clock::now();
    const size_t drawCount = input.preparedDrawCount;
    const size_t projectileTrailSegmentCount =
        input.effects.projectileTrailSegmentCount;
    const size_t typedFxTriangleCount =
        input.effects.typedFxTriangleCount;
    const size_t particleDrawCount = input.effects.particleDrawCount;
    const size_t projectedShadowCount =
        input.effects.projectedShadowCount;
    const size_t smudgeDrawCount = input.post.smudgeDrawCount;
    const size_t snowflakeCount = input.post.snowflakeCount;
    const bool fxaaRendered = input.post.fxaaRendered;
    if (m_worldAssets->stats.assetSampleDue(frame.simulationFrame)) {
        // These diagnostics walk every model/texture/animation cache entry
        // and may inspect dependency graphs. Sample them at a bounded cadence
        // instead of repeating O(asset-count) work for every render frame.
        m_worldAssets->stats.publishAssetSample(
            frame.simulationFrame,
            m_worldAssets->residency.textures->stats(),
            m_worldAssets->residency.assets.stats(),
            m_worldAssets->residency.animations.stats());
    }
    const render::WorldTextureCache::Stats& textureCacheStats =
        m_worldAssets->stats.textureSample();
    const render::W3dAssetCacheStats& modelAssetStats =
        m_worldAssets->stats.modelSample();
    const render::W3dAnimationCacheStats& animationAssetStats =
        m_worldAssets->stats.animationSample();
    const render::W3dGpuUploadBatchStats& modelUploadStats =
        m_worldAssets->residency.assets.lastGpuUploadBatchStats();
    const fx::ParticleRuntimePhaseProfile particlePhase =
        m_worldAssets->fx.runtime
        ? m_worldAssets->fx.runtime->particles().lastPhaseProfile()
        : fx::ParticleRuntimePhaseProfile{};
    const render::GpuParticleSimulationRenderStats gpuParticleStats =
        m_worldAssets->particleRenderer.gpuSimulationStats();
    const render::ParticleRenderExecutionStats& particleExecutionStats =
        m_worldAssets->particleRenderer.executionStats();
    const render::SceneColorRenderStats sceneColorStats =
        m_worldRenderer->sceneColorStats() +
        m_worldAssets->particleRenderer.sceneColorStats();
    m_worldAssets->stats.publishRetainedScratchCapacity(
        m_worldAssets->retainedScratchCapacityBytes());
    const render::RetainedRenderScratchStats retainedScratchStats{
        .capacityBytes =
            m_worldAssets->stats.retainedScratchCapacity(),
        .lifetimeHighWaterBytes =
            m_worldAssets->stats.retainedScratchHighWater(),
        .capacityGrowthFrames =
            m_worldAssets->stats.retainedScratchGrowthFrames(),
    };
    const auto boundedU32 = [](size_t value) noexcept {
        return static_cast<uint32_t>(std::min<size_t>(
            value, std::numeric_limits<uint32_t>::max()));
    };
    render::RenderAssetKindLifecycleStats uiTextureLifecycle;
    uiTextureLifecycle.tracked = boundedU32(m_textureCache.size());
    uiTextureLifecycle.currentStates[static_cast<size_t>(
        render::RenderAssetLifecycleState::GpuResident)] =
        uiTextureLifecycle.tracked;
    uiTextureLifecycle.requests =
        m_uiTextureLifecycle.cacheHits + m_uiTextureLifecycle.cacheMisses;
    uiTextureLifecycle.cacheHits = m_uiTextureLifecycle.cacheHits;
    uiTextureLifecycle.cacheMisses = m_uiTextureLifecycle.cacheMisses;
    uiTextureLifecycle.published = m_uiTextureLifecycle.published;
    uiTextureLifecycle.failures = m_uiTextureLifecycle.failures;
    uiTextureLifecycle.evictions = m_uiTextureLifecycle.evictions;
    uiTextureLifecycle.resets = m_uiTextureLifecycle.resets;

    render::RenderAssetKindLifecycleStats glyphLifecycle;
    glyphLifecycle.tracked = boundedU32(m_glyphCache.size());
    glyphLifecycle.currentStates[static_cast<size_t>(
        render::RenderAssetLifecycleState::GpuResident)] =
        glyphLifecycle.tracked;
    glyphLifecycle.requests =
        m_uiGlyphLifecycle.cacheHits + m_uiGlyphLifecycle.cacheMisses;
    glyphLifecycle.cacheHits = m_uiGlyphLifecycle.cacheHits;
    glyphLifecycle.cacheMisses = m_uiGlyphLifecycle.cacheMisses;
    glyphLifecycle.published = m_uiGlyphLifecycle.published;
    glyphLifecycle.failures = m_uiGlyphLifecycle.failures;
    glyphLifecycle.evictions = m_uiGlyphLifecycle.evictions;
    glyphLifecycle.resets = m_uiGlyphLifecycle.resets;

    const render::RenderAssetLifecycleStats assetLifecycleStats =
        m_worldAssets->stats.projectAssetLifecycle(
            uiTextureLifecycle, glyphLifecycle,
            m_worldAssets->frame.modelGraphTraversalStats.cycleRejected,
            m_worldAssets->frame.modelGraphTraversalStats.depthRejected);
    const bool terrainReady = frame.terrain &&
        (m_worldAssets->terrain &&
         !m_worldAssets->terrainUploads.fallbackActive() &&
         m_worldAssets->terrain->revision() == frame.terrain->revision &&
         m_worldAssets->terrain->layoutRevision() ==
             frame.terrain->layoutRevision &&
         m_worldAssets->terrain->borderShroudRevision() ==
             frame.terrain->borderShroudRevision &&
         m_worldAssets->terrain->waterRevision() ==
             frame.terrain->waterRevision &&
         m_worldAssets->terrain->bridgeRevision() ==
             frame.terrain->bridgeRevision);
    const bool startupSceneFailed = frame.loadingRevision != 0u &&
        frame.sessionRevision != 0u &&
        m_worldAssets->terrainStartupFailure.matches(frame);
    // The initial terrain product is required. Bib geometry becomes a
    // required product only when this generation actually contains bibs;
    // models, clips and material textures retain ZH-compatible fallback
    // behavior inside the ticket owner.
    const bool bibRequired = frame.terrain && !frame.terrain->bibs.empty();
    const bool bibsReady = !bibRequired || (terrainReady &&
        m_worldAssets->terrain->bibsReady());
    const render::StartupSceneTicketRenderStats startupTicket =
        m_worldAssets->lifetime.startupSceneTicket.projectStats(
            frame, m_worldAssets->residency.assets, m_worldAssets->residency.animations,
            terrainReady, bibRequired, bibsReady, startupSceneFailed);
    const bool startupSceneReady =
        startupTicket.state == render::StartupSceneTicketState::Ready ||
        startupTicket.state == render::StartupSceneTicketState::Degraded;
    render::WorldFrameRenderStats& frameStats =
        m_worldAssets->stats.frame();
    frameStats = {
        .simulationFrame = frame.simulationFrame,
        .worldRevision = frame.stamp.worldRevision,
        .viewRevision = m_worldAssets->view.current.viewRevision,
        .interpolationAlpha =
            m_worldAssets->view.current.interpolationAlpha,
        .presentationEpoch = frame.presentationEpoch,
        .sessionRevision = frame.sessionRevision,
        .loadingRevision = frame.loadingRevision,
        .terrainRevision = m_worldAssets->terrain
            ? m_worldAssets->terrain->revision() : 0u,
        .startupSceneReady = startupSceneReady && !startupSceneFailed,
        .startupSceneFailed = startupSceneFailed,
        .startupSceneTicket = startupTicket,
        .passTimings = input.passTimings,
        .worldResources = m_d3d12.currentWorldResourceStateStats(),
        .gpuTimestamps = m_d3d12.gpuTimestampStats(),
        .renderBindings = m_d3d12.renderBindingStats(),
        .sceneColor = sceneColorStats,
        .retainedScratch = retainedScratchStats,
        .displayOutput = m_worldAssets->output.projectStats(),
        .assets = assetLifecycleStats,
        .preparation = m_worldAssets->pipeline.lastPreparationStats(),
        .staticMeshes = m_worldRenderer->lastStaticMeshStats(),
        .frameUpload = m_d3d12.currentFrameUploadStats(),
        .srvDescriptors = m_d3d12.srvDescriptorStats(),
        .gpuRetirement = m_d3d12.gpuRetirementStats(),
        .fxaaPasses = fxaaRendered ? 1u : 0u,
        .fxaaEnabled = m_worldAssets->quality.effectiveAntiAliasingMode ==
            RenderAntiAliasingMode::Fxaa,
        .terrainVisibleChunks = m_worldAssets->terrain
            ? static_cast<uint32_t>(std::min<size_t>(
                m_worldAssets->terrain->lastVisibleChunkCount(),
                std::numeric_limits<uint32_t>::max()))
            : 0,
        .terrainCulledChunks = m_worldAssets->terrain
            ? static_cast<uint32_t>(std::min<size_t>(
                m_worldAssets->terrain->lastCulledChunkCount(),
                std::numeric_limits<uint32_t>::max()))
            : 0,
        .terrainBridgeDrawPackets = static_cast<uint32_t>(std::min<size_t>(
            m_worldAssets->frame.bridgeDrawPackets.size(),
            std::numeric_limits<uint32_t>::max())),
        .terrainBibDrawPackets = static_cast<uint32_t>(std::min<size_t>(
            m_worldAssets->frame.bibDrawPackets.size(),
            std::numeric_limits<uint32_t>::max())),
        .projectileTrails = m_worldAssets->projectileTrailRenderer.stats().activeTrails,
        .projectileTrailSegments = static_cast<uint32_t>(std::min<size_t>(
            projectileTrailSegmentCount, std::numeric_limits<uint32_t>::max())),
        .trackMarkStreams = m_worldAssets->trackMarkRenderer.stats().activeStreams,
        .trackMarkSegments = m_worldAssets->trackMarkRenderer.stats().renderedSegments,
        .projectedShadows = static_cast<uint32_t>(std::min<size_t>(
            projectedShadowCount, std::numeric_limits<uint32_t>::max())),
        .projectedShadowDrawCalls =
            m_worldAssets->groundProjectorRenderer.stats().drawCalls,
        .groundProjectorTextureBatches =
            m_worldAssets->groundProjectorRenderer.stats().textureBatches,
        .groundProjectorResidentTextures =
            m_worldAssets->groundProjectorRenderer.stats().residentTextures,
        .groundProjectorBudgetRejected =
            m_worldAssets->groundProjectorRenderer.stats().budgetRejectedInstances,
        .snowflakeInstances = static_cast<uint32_t>(std::min<size_t>(
            snowflakeCount, std::numeric_limits<uint32_t>::max())),
        .visibilityHiddenEntities = frame.localVisibility.hiddenEntityCount,
        .visibilityHiddenProjectiles = frame.localVisibility.hiddenProjectileCount,
        .visibilityRejectedFxObjects =
            m_worldAssets->stats.visibilityRejectedFxObjects(),
        .visibilityRejectedFxInvocations =
            m_worldAssets->stats.visibilityRejectedFxInvocations(),
    };
    m_worldAssets->stats.projectParticleFrameStats(
        particleDrawList, particleDrawCount, smudgeDrawCount,
        particlePhase, gpuParticleStats, particleExecutionStats);
    m_worldAssets->stats.projectAssetCacheFrameStats(modelUploadStats);
    const render::W3dRestPaletteFrameStats& restPaletteStats =
        m_worldAssets->residency.restPalettes.stats();
    frameStats.staticMeshes.restPalettesBuilt =
        restPaletteStats.palettesBuilt;
    frameStats.staticMeshes.restPalettesReused =
        restPaletteStats.palettesReused;
    frameStats.staticMeshes.
        restPaletteJointsMaterialized = restPaletteStats.jointsMaterialized;
    frameStats.staticMeshes.
        poseBindingGenerationRejects =
            m_worldAssets->frame.poseBindingGenerationRejects;
    frameStats.passTimings.
        statsPublicationMicroseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() -
                statsPublicationStart).count());
    frameStats.passTimings.
        statsPublicationSkipped = false;
    m_worldAssets->stats.markSubmissionPending();
    return drawCount + particleDrawCount + smudgeDrawCount +
        projectileTrailSegmentCount +
        projectedShadowCount + typedFxTriangleCount;
}

} // namespace engine
