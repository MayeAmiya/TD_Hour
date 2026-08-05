#include "DX12RendererWorldAssetRuntime.h"

#include "core/debug/debug.h"
#include "engine/renderer/world/effects/EnvironmentPresentationRender.h"

#include <algorithm>
#include <cmath>

namespace engine {

size_t DX12Renderer::renderWorldOpaqueGeometryPass(
    const render::PreparedWorldFrame& frame,
    const render::RenderCameraSnapshot& presentationCamera,
    const render::WorldLightEnvironment& lightEnvironment,
    float visualTimeSeconds,
    size_t drawCount) {
    if (!m_worldAssets || !m_worldRenderer) return 0;
    m_d3d12.flushStaticBufferUploads();
    const bool gpuTimestampActive = m_d3d12.beginGpuTimestamp(
        render::GpuTimestampRange::OpaqueWorld);
    if (drawCount != 0) {
        if (m_worldAssets->stats.claimFirstFrame(
                render::WorldFirstFrameDiagnostic::Prepared)) {
            TD_LOG_INFO(
                "[DX12Renderer] PreparedWorldFrame consumed: simulationFrame={} instances={} draws={}",
                frame.simulationFrame,
                m_worldAssets->frame.worldViewVisibleCount, drawCount);
        }
    }
    // The empty call intentionally resets per-frame static-mesh diagnostics;
    // otherwise a world frame with no ready packets would expose stale draw
    // counts from the prior frame.
    const container::Span<const render::DynamicPointLightRenderData>
        fxDynamicPointLights =
            m_worldAssets->fx.typed.advanceLights(frame.simulationFrame);
    container::Vector<render::DynamicPointLightRenderData>&
        combinedDynamicPointLights =
            m_worldAssets->frame.combinedDynamicPointLights;
    combinedDynamicPointLights.clear();
    combinedDynamicPointLights.insert(
        combinedDynamicPointLights.end(), fxDynamicPointLights.begin(),
        fxDynamicPointLights.end());
    constexpr size_t kMaximumSubmittedDynamicPointLights = 20u;
    if (m_worldAssets->frame.policeLightPresentationEpoch !=
            frame.presentationEpoch ||
        frame.simulationFrame < m_worldAssets->frame.policeLightSimulationFrame) {
        m_worldAssets->frame.policeLights.clear();
        m_worldAssets->frame.policeLightPresentationEpoch =
            frame.presentationEpoch;
    }
    m_worldAssets->frame.policeLightSimulationFrame = frame.simulationFrame;
    for (const render::PreparedRenderInstance& instance :
         frame.visibleInstances) {
        const render::RenderPoliceCarState& police =
            instance.visual.policeCar;
        if (!police.enabled) continue;
        m_worldAssets->frame.policeLights[instance.id] = {
            .light = {
                .position = instance.worldTransform.translation() +
                    math::vec3{0.0f, 0.0f, police.heightOffset},
                .color = police.diffuseColor,
                .ambientColor = police.ambientColor,
                .innerRadius = police.innerRadius,
                .outerRadius = police.outerRadius,
                .hasSeparateAmbientColor = true,
            },
            .lastSeenFrame = frame.simulationFrame,
        };
    }
    container::Vector<uint64_t>& policeLightKeys =
        m_worldAssets->frame.policeLightKeys;
    policeLightKeys.clear();
    policeLightKeys.reserve(m_worldAssets->frame.policeLights.size());
    for (auto iterator = m_worldAssets->frame.policeLights.begin();
         iterator != m_worldAssets->frame.policeLights.end();) {
        const float fade = render::policeCarLightFade(
            frame.simulationFrame, iterator->second.lastSeenFrame);
        if (fade <= 0.0f) {
            iterator = m_worldAssets->frame.policeLights.erase(iterator);
        } else {
            policeLightKeys.push_back(iterator->first);
            ++iterator;
        }
    }
    std::sort(policeLightKeys.begin(), policeLightKeys.end());
    for (const uint64_t key : policeLightKeys) {
        if (combinedDynamicPointLights.size() >=
            kMaximumSubmittedDynamicPointLights) {
            break;
        }
        const auto found = m_worldAssets->frame.policeLights.find(key);
        if (found == m_worldAssets->frame.policeLights.end()) continue;
        render::DynamicPointLightRenderData light = found->second.light;
        const float fade = render::policeCarLightFade(
            frame.simulationFrame, found->second.lastSeenFrame);
        light.color *= fade;
        light.ambientColor *= fade;
        light.innerRadius *= fade;
        light.outerRadius *= fade;
        if (light.color.length_sq() <= 0.0f &&
            light.ambientColor.length_sq() <= 0.0f) {
            continue;
        }
        combinedDynamicPointLights.push_back(light);
    }
    if (frame.localVisibility.hasPlayableBounds()) {
        std::erase_if(
            combinedDynamicPointLights,
            [&frame](const render::DynamicPointLightRenderData& light) {
                return !frame.localVisibility.isInsidePlayableBounds(
                    light.position);
            });
    }
    const container::Span<const render::DynamicPointLightRenderData>
        dynamicPointLights = combinedDynamicPointLights;
    const container::Span<const render::TerrainPointLightRenderData>
        scenePointLights = frame.terrain
            ? container::Span<const render::TerrainPointLightRenderData>(
                  frame.terrain->pointLights)
            : container::Span<const render::TerrainPointLightRenderData>{};
    if (frame.terrain && frame.terrain->waterMaterial &&
        frame.terrain->waterMaterial->waterType == 2 &&
        frame.terrain->waterMaterial->hasReflectionPlane) {
        m_worldAssets->frame.reflectionDrawPackets.clear();
        m_worldAssets->frame.reflectionDrawPackets.reserve(
            m_worldAssets->frame.drawPackets.size() +
            m_worldAssets->frame.bridgeDrawPackets.size());
        m_worldAssets->frame.reflectionDrawPackets.insert(
            m_worldAssets->frame.reflectionDrawPackets.end(),
            m_worldAssets->frame.drawPackets.begin(),
            m_worldAssets->frame.drawPackets.end());
        m_worldAssets->frame.reflectionDrawPackets.insert(
            m_worldAssets->frame.reflectionDrawPackets.end(),
            m_worldAssets->frame.bridgeDrawPackets.begin(),
            m_worldAssets->frame.bridgeDrawPackets.end());
        static_cast<void>(m_worldRenderer->renderWaterReflection(
            m_worldAssets->frame.reflectionDrawPackets, presentationCamera,
            frame.terrain->waterMaterial->reflectionPlaneZ,
            lightEnvironment, frame.localVisibility, visualTimeSeconds,
            dynamicPointLights, scenePointLights));
    }
    // RefCode closes the terrain object with bridges, terrain tracks and
    // W3DBibBuffer before traversing ordinary scene objects. Merge these
    // separately prepared packets only after reflection capture. Semantic
    // layers plus the projector split boundary restore the terrain-to-object
    // order without duplicating packet ownership.
    m_worldAssets->frame.drawPackets.reserve(
        m_worldAssets->frame.drawPackets.size() +
        m_worldAssets->frame.bridgeDrawPackets.size() +
        m_worldAssets->frame.overlayDrawPackets.size() +
        m_worldAssets->frame.bibDrawPackets.size());
    m_worldAssets->frame.drawPackets.insert(
        m_worldAssets->frame.drawPackets.end(),
        m_worldAssets->frame.bridgeDrawPackets.begin(),
        m_worldAssets->frame.bridgeDrawPackets.end());
    m_worldAssets->frame.drawPackets.insert(
        m_worldAssets->frame.drawPackets.end(),
        m_worldAssets->frame.overlayDrawPackets.begin(),
        m_worldAssets->frame.overlayDrawPackets.end());
    m_worldAssets->frame.drawPackets.insert(
        m_worldAssets->frame.drawPackets.end(),
        m_worldAssets->frame.bibDrawPackets.begin(),
        m_worldAssets->frame.bibDrawPackets.end());
    size_t projectedShadowCount = 0;
    if (m_worldAssets->frame.groundProjectors.empty()) {
        // render(empty) is an intentional no-command call that resets the
        // renderer's per-frame projector statistics before publication.
        projectedShadowCount =
            m_worldAssets->groundProjectorRenderer.render(
                m_worldAssets->frame.groundProjectors, presentationCamera,
                frame.simulationFrame);
        m_worldRenderer->renderStaticMeshes(
            m_worldAssets->frame.drawPackets, presentationCamera,
            lightEnvironment, frame.localVisibility,
            visualTimeSeconds, dynamicPointLights, scenePointLights);
    } else {
        // RefCode emits terrain projectors/scorches after roads and before
        // bridge/track/Bib/object traversal. The pre phase performs the one
        // complete shadow/visibility setup, the dedicated renderer records
        // projectors, then the post phase restores the static-world state.
        m_worldRenderer->renderStaticMeshes(
            m_worldAssets->frame.drawPackets, presentationCamera,
            lightEnvironment, frame.localVisibility,
            visualTimeSeconds, dynamicPointLights, scenePointLights,
            render::StaticMeshPassExecution::FullWorldBeforeProjectors);
        projectedShadowCount =
            m_worldAssets->groundProjectorRenderer.render(
                m_worldAssets->frame.groundProjectors, presentationCamera,
                frame.simulationFrame,
                m_worldRenderer->localVisibilityGpuBinding(
                    frame.localVisibility));
        m_worldRenderer->renderStaticMeshes(
            m_worldAssets->frame.drawPackets, presentationCamera,
            lightEnvironment, frame.localVisibility,
            visualTimeSeconds, dynamicPointLights, scenePointLights,
            render::StaticMeshPassExecution::FullWorldAfterProjectors);
    }
    if (!dynamicPointLights.empty() &&
        m_worldAssets->stats.claimFirstFrame(
            render::WorldFirstFrameDiagnostic::DynamicLight)) {
        const render::DynamicPointLightRuntimeStats& lightStats =
            m_worldAssets->fx.typed.lightStats();
        TD_LOG_INFO(
            "[DX12Renderer] First dynamic-light frame: active={} permanent={} highWater={} rejected={}",
            lightStats.activeLights, lightStats.permanentLights,
            lightStats.highWaterLights, lightStats.budgetRejectedCommands);
    }
    const render::StaticMeshRenderStats& staticMeshStats =
        m_worldRenderer->lastStaticMeshStats();
    if (staticMeshStats.shadowValid &&
        m_worldAssets->stats.claimFirstFrame(
            render::WorldFirstFrameDiagnostic::DirectionalShadow)) {
        TD_LOG_INFO(
            "[DX12Renderer] First directional shadow frame: casters={} draws={} triangles={} "
            "rejected(policy={},invalid={})",
            staticMeshStats.shadowCasterPackets,
            staticMeshStats.shadowDrawCalls,
            staticMeshStats.shadowTriangles,
            staticMeshStats.shadowPolicyRejectedPackets,
            staticMeshStats.shadowInvalidPackets);
    }
    if (gpuTimestampActive) {
        static_cast<void>(m_d3d12.endGpuTimestamp(
            render::GpuTimestampRange::OpaqueWorld));
    }
    return projectedShadowCount;
}

DX12Renderer::WorldEffectsPassResult
DX12Renderer::renderWorldEffectsAndOverlayPass(
    const render::PreparedWorldFrame& frame,
    const render::RenderCameraSnapshot& presentationCamera,
    render::ParticleRenderDrawList& particleDrawList,
    float renderDeltaSeconds,
    size_t modelRayPacketCount) {
    if (!m_worldAssets || !m_worldRenderer) return {};
    const size_t projectileTrailSegmentCount =
        m_worldAssets->projectileTrailRenderer.render(
            m_worldAssets->frame.interpolatedProjectiles,
            presentationCamera,
            m_worldAssets->frame.displayedSimulationFrame,
            renderDeltaSeconds, frame.localVisibility);
    if (projectileTrailSegmentCount != 0 &&
        m_worldAssets->stats.claimFirstFrame(
            render::WorldFirstFrameDiagnostic::ProjectileTrail)) {
        const render::ProjectileTrailRenderStats& trailStats =
            m_worldAssets->projectileTrailRenderer.stats();
        TD_LOG_INFO(
            "[DX12Renderer] First projectile trail frame: trails={} points={} segments={} highWater={}",
            trailStats.activeTrails, trailStats.activePoints,
            trailStats.renderedSegments, trailStats.trailHighWater);
    }
    const size_t typedFxTriangleCount =
        m_worldAssets->fx.typed.worldRenderer().render(
            presentationCamera, renderDeltaSeconds, frame.terrain.get(),
            m_worldAssets->fx.runtime.get(),
            m_worldAssets->frame.displayedSimulationFrame,
            frame.localVisibility);
    if ((typedFxTriangleCount != 0 || modelRayPacketCount != 0) &&
        m_worldAssets->stats.claimFirstFrame(
            render::WorldFirstFrameDiagnostic::TypedFx)) {
        const auto& typedStats =
            m_worldAssets->fx.typed.worldRenderer().stats();
        TD_LOG_INFO(
            "[DX12Renderer] First typed FX world frame: beams={} modelRays={} triangles={} modelPackets={} highWater={}",
            typedStats.activeBeams, typedStats.activeModelRays,
            typedStats.renderedTriangles, typedStats.renderedModelPackets,
            typedStats.highWaterEffects);
    }
    if (frame.presentationEpoch != 0 &&
        frame.presentationEpoch !=
            m_worldAssets->quality.gpuParticleAuthorityEpoch) {
        m_worldAssets->particleRenderer.requestGpuSimulationReset(
            frame.presentationEpoch);
        m_worldAssets->quality.gpuParticleAuthorityEpoch = frame.presentationEpoch;
    }
    const size_t particleDrawCount = m_worldAssets->particleRenderer.render(
        particleDrawList, presentationCamera, frame.localVisibility);
    if (particleDrawCount != 0 &&
        m_worldAssets->stats.claimFirstFrame(
            render::WorldFirstFrameDiagnostic::Particle)) {
        TD_LOG_INFO("[DX12Renderer] First particle frame: instances={} batches={} active={}",
                    particleDrawCount, particleDrawList.batches.size(),
                    m_worldAssets->fx.runtime
                        ? m_worldAssets->fx.runtime->particles().particleCount() : 0);
    }
    return {
        .projectileTrailSegmentCount = projectileTrailSegmentCount,
        .typedFxTriangleCount = typedFxTriangleCount,
        .particleDrawCount = particleDrawCount,
    };
}

DX12Renderer::WorldPostPassResult
DX12Renderer::renderWorldPostAndClientOverlayPass(
    const render::PreparedWorldFrame& frame,
    const render::RenderCameraSnapshot& presentationCamera,
    render::ParticleRenderDrawList& particleDrawList,
    const render::ClientOptionsRenderState& clientOptions,
    const render::WeatherRenderState& weather,
    TextureManager* objectIconTextures) {
    if (!m_worldAssets || !m_worldRenderer) return {};
    // W3DSnow follows authored particles in the transparent world pass, before
    // tactical-view filters and game UI. Use the existing D3D12 quad batch for
    // this bounded procedural fallback; SHOW_WEATHER gates real draw calls
    // here rather than ending as a detached bool with no consumer.
    // Resolve world MSAA now. Procedural snow and every later tactical
    // filter/UI pass are single-sample presentation work, exactly once per
    // output pixel.
    m_d3d12.resolveWorldRenderPass();
    const bool heatEffectsEnabled = frame.renderFeatureQuality
        ? frame.renderFeatureQuality->requested.useHeatEffects
        : RenderFeatureQualitySettings{}.useHeatEffects;
    const size_t smudgeDrawCount =
        m_worldAssets->particleRenderer.renderSmudges(
            particleDrawList, presentationCamera, heatEffectsEnabled,
            frame.localVisibility);
    if (smudgeDrawCount != 0 &&
        m_worldAssets->stats.claimFirstFrame(
            render::WorldFirstFrameDiagnostic::Smudge)) {
        TD_LOG_INFO(
            "[DX12Renderer] First SMUDGE distortion frame: instances={} heatEffects={}",
            smudgeDrawCount, heatEffectsEnabled);
    }
    container::Array<render::WeatherSnowflake, 192> snowflakes;
    const container::Span<render::WeatherSnowflake> snowflakeOutput{
        snowflakes.data(), snowflakes.size()};
    const float tacticalVirtualHeight = static_cast<float>(m_virtualH) *
        std::clamp(presentationCamera.tacticalViewportHeightScale, 0.1f, 1.0f);
    const size_t snowflakeCount = render::buildWeatherSnowflakes(
        weather, presentationCamera, frame.simulationFrame,
        static_cast<float>(m_virtualW), tacticalVirtualHeight,
        snowflakeOutput);
    for (size_t index = 0; index < snowflakeCount; ++index)
    {
        const render::WeatherSnowflake& flake = snowflakes[index];
        const float halfSize = flake.size * 0.5f;
        m_d3d12.drawSolidQuad(
            flake.x - halfSize, flake.y - halfSize, flake.size, flake.size, 1.0f, 1.0f, 1.0f, flake.opacity);
    }
    // WorldRenderer's post-process copies the current render target directly;
    // flush the device batch first so snow is included in BW/motion blur but
    // remains beneath later ControlBar/UI submissions.
    if (snowflakeCount != 0)
        m_d3d12.flushBatch();
    // The active BW or motion-blur filter captures this completed world pass
    // before screen fades and GUI. It is renderer-local, never a logic camera
    // state or an ECS/UI alpha overlay.
    m_worldRenderer->renderScriptViewFilters();
    const bool fxaaRendered = m_worldRenderer->renderFxaa(
        presentationCamera.tacticalViewportHeightScale);
    // Object icon UI is a genuine client overlay, not a model packet: draw
    // it after tactical-world filters but before CAMERA_FADE_* and normal
    // InGameUI. TextureManager remains caller-owned, avoiding an old global
    // asset collection inside the renderer.
    if (objectIconTextures) {
        static_cast<void>(m_worldAssets->objectIconOverlay.render(
            frame.objectIcons, presentationCamera,
            m_worldAssets->view.current.viewport,
            frame.simulationFrame, clientOptions, *this,
            *objectIconTextures));
        static_cast<void>(m_worldAssets->objectIconOverlay.renderWorldFeedback(
            frame.worldFeedback, presentationCamera,
            m_worldAssets->view.current.viewport,
            frame.simulationFrame, clientOptions, *this,
            *objectIconTextures));
        static_cast<void>(m_worldAssets->objectUiOverlay.render(
            frame.objectUi, presentationCamera,
            m_worldAssets->view.current.viewport,
            frame.simulationFrame,
            m_worldAssets->view.current.interpolationAlpha,
            clientOptions, *this, *objectIconTextures));
        static_cast<void>(m_worldAssets->tacticalRadar.render(
            frame.tacticalRadar, frame.objectUi, frame.terrain,
            frame.localVisibility, m_worldAssets->frame.bridgeRadarGeometry,
            presentationCamera, m_worldAssets->view.current.viewport,
            {
                .left = m_tacticalRadarPanel.left,
                .top = m_tacticalRadarPanel.top,
                .width = m_tacticalRadarPanel.width,
                .height = m_tacticalRadarPanel.height,
            },
            m_tacticalRadarPanel.visible,
            frame.simulationFrame,
            *this, *objectIconTextures));
    }
    // CAMERA_FADE_* is recorded after the sealed tactical-world frame and
    // before the application's InGameGuiSubsystem submits ControlBar/UI.
    // It has its own fixed-function-equivalent PSOs; do not route it through
    // drawQuad(), whose ordinary SRC_ALPHA blend would change RefCode's ADD,
    // REVSUBTRACT, SATURATE, and MULTIPLY semantics.
    m_worldRenderer->renderScreenFade(frame.screenFade, frame.simulationFrame);

    return {
        .smudgeDrawCount = smudgeDrawCount,
        .snowflakeCount = snowflakeCount,
        .fxaaRendered = fxaaRendered,
    };
}

} // namespace engine
