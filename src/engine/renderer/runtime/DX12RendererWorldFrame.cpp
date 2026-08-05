#include "DX12RendererWorldAssetRuntime.h"
#include "core/debug/debug.h"
#include "engine/renderer/world/effects/EnvironmentPresentationRender.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace engine {

namespace {

[[nodiscard]] render::RenderVector interpolateDirection(
    const render::RenderVector& previous,
    const render::RenderVector& current,
    float alpha) noexcept {
    const render::RenderVector blended =
        previous + (current - previous) * alpha;
    return blended.length_sq() > math::EPSILON * math::EPSILON
        ? blended.normalized() : current;
}

void appendTrackSource(
    container::Vector<render::TrackMarkRenderSource>& output,
    const render::TrackMarkRenderInput& input,
    const render::RenderVector& position,
    const render::RenderVector& forward) {
    output.push_back({
        .objectId = input.objectId,
        .position = position,
        .forward = forward,
        .textureName = input.textureName,
        .trackWidth = input.trackWidth,
        .edgeSpacing = input.edgeSpacing,
        .maximumEdges = input.maximumEdges,
        .opaqueEdges = input.opaqueEdges,
        .fadeLifetimeFrames = input.fadeLifetimeFrames,
        .moving = input.moving,
        .visible = true,
    });
}

} // namespace

void DX12Renderer::prepareWorldTransientStreams(
    const render::PreparedWorldFrame& frame) {
    if (!m_worldAssets) return;
    auto& projectiles = m_worldAssets->frame.interpolatedProjectiles;
    auto& tracks = m_worldAssets->frame.trackMarkSources;
    projectiles.clear();
    tracks.clear();

    const render::PreparedWorldFrame* previous =
        m_worldAssets->pipeline.previousFrame();
    const float alpha = std::clamp(
        m_worldAssets->view.current.interpolationAlpha, 0.0f, 1.0f);
    const bool interpolate = alpha < 1.0f && previous &&
        m_worldAssets->view.current.worldARevision ==
            previous->stamp.worldRevision &&
        m_worldAssets->view.current.worldBRevision ==
            frame.stamp.worldRevision;
    if (!interpolate) {
        projectiles = frame.projectiles;
        tracks.reserve(frame.trackMarks.size());
        for (const render::TrackMarkRenderInput& input : frame.trackMarks) {
            appendTrackSource(tracks, input, input.position, input.forward);
        }
        return;
    }

    // Both source columns are ObjectId-sorted by extraction. Merge A/B once
    // per present frame: common objects receive smooth endpoints, A-only
    // objects survive until alpha=1, and B-only objects appear at B.
    projectiles.reserve(
        previous->projectiles.size() + frame.projectiles.size());
    size_t previousProjectile = 0;
    size_t currentProjectile = 0;
    while (previousProjectile < previous->projectiles.size() ||
           currentProjectile < frame.projectiles.size()) {
        const uint64_t previousId =
            previousProjectile < previous->projectiles.size()
            ? previous->projectiles[previousProjectile].projectile.objectId
            : UINT64_MAX;
        const uint64_t currentId =
            currentProjectile < frame.projectiles.size()
            ? frame.projectiles[currentProjectile].projectile.objectId
            : UINT64_MAX;
        if (previousId < currentId) {
            projectiles.push_back(previous->projectiles[previousProjectile++]);
            continue;
        }
        if (currentId < previousId) {
            // B-only projectiles were born after A. They have no earlier
            // endpoint to blend from, so retain their authoritative B pose
            // for this present instead of dropping the launch frame.
            projectiles.push_back(frame.projectiles[currentProjectile++]);
            continue;
        }
        render::PreparedProjectileRenderSnapshot blended =
            frame.projectiles[currentProjectile++];
        const render::ProjectileRenderSnapshot& previousValue =
            previous->projectiles[previousProjectile++].projectile;
        blended.projectile.position = previousValue.position +
            (blended.projectile.position - previousValue.position) * alpha;
        blended.projectile.forward = interpolateDirection(
            previousValue.forward, blended.projectile.forward, alpha);
        projectiles.push_back(std::move(blended));
    }

    tracks.reserve(previous->trackMarks.size() + frame.trackMarks.size());
    size_t previousTrack = 0;
    size_t currentTrack = 0;
    while (previousTrack < previous->trackMarks.size() ||
           currentTrack < frame.trackMarks.size()) {
        const uint64_t previousId = previousTrack < previous->trackMarks.size()
            ? previous->trackMarks[previousTrack].objectId : UINT64_MAX;
        const uint64_t currentId = currentTrack < frame.trackMarks.size()
            ? frame.trackMarks[currentTrack].objectId : UINT64_MAX;
        if (previousId < currentId) {
            const render::TrackMarkRenderInput& value =
                previous->trackMarks[previousTrack++];
            appendTrackSource(tracks, value, value.position, value.forward);
            continue;
        }
        if (currentId < previousId) {
            // A fresh track source likewise has no A endpoint. Keep B so a
            // newly moving vehicle cannot lose its first visible trail mark.
            const render::TrackMarkRenderInput& value =
                frame.trackMarks[currentTrack++];
            appendTrackSource(tracks, value, value.position, value.forward);
            continue;
        }
        const render::TrackMarkRenderInput& previousValue =
            previous->trackMarks[previousTrack++];
        const render::TrackMarkRenderInput& currentValue =
            frame.trackMarks[currentTrack++];
        appendTrackSource(
            tracks, currentValue,
            previousValue.position +
                (currentValue.position - previousValue.position) * alpha,
            interpolateDirection(
                previousValue.forward, currentValue.forward, alpha));
    }
}

size_t DX12Renderer::renderWorldFrame(const render::PreparedWorldFrame& frame,
                                      TextureManager* objectIconTextures) {
    if (!m_worldRenderer || !m_worldAssets) return 0;
    if (!m_d3d12.beginWorldRenderPass()) {
        TD_LOG_ERROR("[DX12Renderer] failed to begin the world render pass");
        return 0;
    }
    m_worldAssets->lifetime.started = true;
    constexpr float kLogicFrameSeconds = 1.0f / 30.0f;
    const float visualTimeSeconds =
        static_cast<float>(frame.simulationFrame) * kLogicFrameSeconds;

    const auto presentationNow = std::chrono::steady_clock::now();
    const render::GpuTimestampRenderStats& gpuTimestamps =
        m_d3d12.gpuTimestampStats();
    const uint32_t gpuFrameRange = static_cast<uint32_t>(
        render::GpuTimestampRange::Frame);
    const uint64_t gpuFrameMicroseconds =
        (gpuTimestamps.validRangeMask & (uint64_t{1} << gpuFrameRange)) != 0u
        ? gpuTimestamps.rangeMicroseconds[gpuFrameRange]
        : 0u;
    const float measuredRenderDeltaSeconds =
        m_worldAssets->stats.beginPresentationFrame(
            presentationNow, gpuFrameMicroseconds);
    const float renderDeltaSeconds = measuredRenderDeltaSeconds > 0.0f
        ? measuredRenderDeltaSeconds
        : 1.0f / 60.0f;
    const render::ClientOptionsRenderState& clientOptions =
        m_worldAssets->clientOptionsPresentation.consume(frame.clientOptions, frame.simulationFrame);
    prepareWorldTransientStreams(frame);
    render::ParticleRenderDrawList& particleDrawList =
        m_worldAssets->frame.particleDrawList;
    const auto particleUpdatePassStart = std::chrono::steady_clock::now();
    updateWorldParticlePass(
        frame, renderDeltaSeconds, particleDrawList);
    const uint64_t particleUpdatePassMicroseconds =
        static_cast<uint64_t>(std::chrono::duration_cast<
            std::chrono::microseconds>(
                std::chrono::steady_clock::now() -
                particleUpdatePassStart).count());

    // The command-recording pass receives only the sealed terrain snapshot,
    // never a live map/global-data object.  A missing GlobalLighting chunk
    // keeps WorldRenderer's stable diagnostic fallback environment.
    const render::WorldLightEnvironment lightEnvironment =
        frame.terrain && frame.terrain->globalLighting
        ? render::WorldLightEnvironment::fromTerrainGlobalLighting(*frame.terrain->globalLighting)
        : render::WorldLightEnvironment{};

    // SCREEN_SHAKE is consumed only at the renderer boundary. The returned
    // camera is a stack value used for this world pass; GameCameraDirector,
    // input state, audio listener positioning, and the sealed frame itself
    // all remain untouched. Skybox follows this same visual camera in XY,
    // matching W3DWater's camera-centered render object.
    const render::RenderCameraSnapshot& renderViewCamera =
        m_worldAssets->view.current.camera;
    const render::CameraSlavePresentationCamera slaveCamera =
        m_worldRenderer->applyScriptCameraSlave(
            renderViewCamera, frame.cameraSlave,
            frame.cameraSlaveTargetPresent,
            frame.cameraSlaveBoneWorldTransform);
    m_worldAssets->durablePresentation.consumeCameraSlaveListener(
        frame.cameraSlave, slaveCamera, frame.simulationFrame);
    // Advance screen-shake presentation even while the slave transform wins
    // for this world pass. W3D updates its shaker before it overwrites the
    // camera matrix, so disabling slave later must not resurrect a stale
    // undamped shake.
    const render::RenderCameraSnapshot scriptShakenCamera =
        m_worldRenderer->applyScriptScreenShake(
            renderViewCamera, frame.simulationFrame, frame.screenShake);
    const render::RenderCameraSnapshot shakenCamera =
        m_worldAssets->fx.typed.applyViewShake(
            scriptShakenCamera, frame.simulationFrame);
    const render::RenderCameraSnapshot cameraBeforeViewFilters =
        slaveCamera.applied ? slaveCamera.camera : shakenCamera;
    // BW and CAMERA_MOTION_BLUR* share RefCode's single tactical-view filter
    // slot. Resolve their stamped replacement order before world packets are
    // recorded so JUMP can affect only this renderer-local presentation view.
    const render::RenderCameraSnapshot presentationCamera = m_worldRenderer->prepareScriptViewFilters(
        cameraBeforeViewFilters, frame.simulationFrame, frame.blackAndWhite, frame.motionBlur);
    // This is the camera that the user actually sees. Publish it through the
    // same RenderViewState consumed by picking and use it for culling; keeping
    // the pre-shake/pre-slave endpoint camera here makes visible geometry,
    // overlays and input describe three different views of one frame.
    m_worldAssets->view.current.camera = presentationCamera;
    prepareWorldViewVisibility(frame, presentationCamera);
    const auto particlePreparePassStart = std::chrono::steady_clock::now();
    prepareWorldParticleDrawPass(
        frame, presentationCamera, particleDrawList);
    const uint64_t particlePreparePassMicroseconds =
        static_cast<uint64_t>(std::chrono::duration_cast<
            std::chrono::microseconds>(
                std::chrono::steady_clock::now() -
                particlePreparePassStart).count());

    m_worldAssets->frame.drawPackets.clear();
    m_worldAssets->residency.restPalettes.clear();
    m_worldAssets->frame.poseBindingGenerationRejects = 0;
    m_worldAssets->frame.modelGraphTraversalStats = {};
    m_worldAssets->frame.bridgeDrawPackets.clear();
    m_worldAssets->frame.bridgeRadarGeometry.clear();
    m_worldAssets->frame.overlayDrawPackets.clear();
    m_worldAssets->frame.bibDrawPackets.clear();
    m_worldAssets->residency.treeTextureOverrides.beginEpoch(
        *m_worldAssets->residency.textures, frame.presentationEpoch);
    const RenderObjectFeedbackGameData defaultObjectFeedback;
    const RenderObjectFeedbackGameData& objectFeedback =
        frame.renderGameDataSettings
        ? frame.renderGameDataSettings->visual.objectFeedback
        : defaultObjectFeedback;
    m_worldAssets->selectionFlash.consume(
        frame.objectUi, frame.simulationFrame, objectFeedback);
    const auto terrainResourcePassStart =
        std::chrono::steady_clock::now();
    publishWorldTerrainResources(frame, presentationCamera);
    const uint64_t terrainResourcePassMicroseconds =
        static_cast<uint64_t>(std::chrono::duration_cast<
            std::chrono::microseconds>(
                std::chrono::steady_clock::now() -
                terrainResourcePassStart).count());
    const bool terrainResourcePassSkipped =
        !frame.terrain || !frame.terrain->isValid();
    const render::SkyboxRenderState& skybox =
        m_worldAssets->durablePresentation.consumeSkybox(
            frame.skybox, frame.simulationFrame);
    const render::TreeSwayRenderState& treeSway =
        m_worldAssets->durablePresentation.consumeTreeSway(
            frame.treeSway, frame.simulationFrame);
    const render::WeatherRenderState& weather =
        m_worldAssets->durablePresentation.consumeWeather(
            frame.weather, frame.simulationFrame);
    const auto skyboxPassStart = std::chrono::steady_clock::now();
    prepareWorldSkyboxPass(
        skybox, presentationCamera, visualTimeSeconds);
    const uint64_t skyboxPassMicroseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - skyboxPassStart).count());
    const bool skyboxPassSkipped = !skybox.enabled;
    const auto terrainPacketPassStart = std::chrono::steady_clock::now();
    prepareWorldTerrainPackets(
        frame, presentationCamera, visualTimeSeconds);
    const uint64_t terrainPacketPassMicroseconds =
        static_cast<uint64_t>(std::chrono::duration_cast<
            std::chrono::microseconds>(
                std::chrono::steady_clock::now() -
                terrainPacketPassStart).count());
    const bool terrainPacketPassSkipped = !m_worldAssets->terrain;
    const auto terrainBridgePassStart = std::chrono::steady_clock::now();
    prepareWorldTerrainBridgePackets(frame, visualTimeSeconds);
    const uint64_t terrainBridgePassMicroseconds =
        static_cast<uint64_t>(std::chrono::duration_cast<
            std::chrono::microseconds>(
                std::chrono::steady_clock::now() -
                terrainBridgePassStart).count());
    const bool terrainBridgePassSkipped =
        !frame.terrain || frame.terrain->bridges.empty();
    const auto objectPacketPassStart = std::chrono::steady_clock::now();
    prepareWorldObjectPackets(frame, treeSway, visualTimeSeconds);
    const uint64_t objectPacketPassMicroseconds =
        static_cast<uint64_t>(std::chrono::duration_cast<
            std::chrono::microseconds>(
                std::chrono::steady_clock::now() -
                objectPacketPassStart).count());
    const bool objectPacketPassSkipped =
        m_worldAssets->frame.worldViewVisibleCount == 0u;
    const auto worldOverlayPacketPassStart =
        std::chrono::steady_clock::now();
    const size_t modelRayPacketCount =
        prepareWorldOverlayPackets(frame, visualTimeSeconds);
    const uint64_t worldOverlayPacketPassMicroseconds =
        static_cast<uint64_t>(std::chrono::duration_cast<
            std::chrono::microseconds>(
                std::chrono::steady_clock::now() -
                worldOverlayPacketPassStart).count());
    const bool worldOverlayPacketPassSkipped =
        m_worldAssets->frame.trackMarkSources.empty() &&
        frame.objectUi.waypointSegments.empty() &&
        modelRayPacketCount == 0;
    const size_t drawCount = m_worldAssets->frame.drawPackets.size() +
        m_worldAssets->frame.bridgeDrawPackets.size() +
        m_worldAssets->frame.overlayDrawPackets.size() +
        m_worldAssets->frame.bibDrawPackets.size();
    const auto projectorPreparePassStart =
        std::chrono::steady_clock::now();
    prepareWorldGroundProjectors(
        frame, presentationCamera, lightEnvironment);
    const uint64_t projectorPreparePassMicroseconds =
        static_cast<uint64_t>(std::chrono::duration_cast<
            std::chrono::microseconds>(
                std::chrono::steady_clock::now() -
                projectorPreparePassStart).count());
    const bool projectorPreparePassSkipped =
        m_worldAssets->frame.groundProjectors.empty();
    const auto opaqueWorldPassStart = std::chrono::steady_clock::now();
    const size_t projectedShadowCount = renderWorldOpaqueGeometryPass(
        frame, presentationCamera, lightEnvironment, visualTimeSeconds,
        drawCount);
    const uint64_t opaqueWorldPassMicroseconds =
        static_cast<uint64_t>(std::chrono::duration_cast<
            std::chrono::microseconds>(
                std::chrono::steady_clock::now() -
                opaqueWorldPassStart).count());
    const bool opaqueWorldPassSkipped =
        drawCount == 0 && projectedShadowCount == 0;
    const auto worldEffectsPassStart = std::chrono::steady_clock::now();
    WorldEffectsPassResult worldEffects =
        renderWorldEffectsAndOverlayPass(
            frame, presentationCamera, particleDrawList,
            renderDeltaSeconds, modelRayPacketCount);
    worldEffects.projectedShadowCount = projectedShadowCount;
    worldEffects.projectorPrepareMicroseconds =
        projectorPreparePassMicroseconds;
    worldEffects.projectorPrepareSkipped =
        projectorPreparePassSkipped;
    const uint64_t worldEffectsPassMicroseconds =
        static_cast<uint64_t>(std::chrono::duration_cast<
            std::chrono::microseconds>(
                std::chrono::steady_clock::now() -
                worldEffectsPassStart).count());
    const bool worldEffectsPassSkipped =
        worldEffects.projectileTrailSegmentCount == 0 &&
        worldEffects.typedFxTriangleCount == 0 &&
        worldEffects.particleDrawCount == 0;
    const auto worldPostPassStart = std::chrono::steady_clock::now();
    const WorldPostPassResult worldPost =
        renderWorldPostAndClientOverlayPass(
            frame, presentationCamera, particleDrawList, clientOptions,
            weather, objectIconTextures);
    const uint64_t worldPostPassMicroseconds =
        static_cast<uint64_t>(std::chrono::duration_cast<
            std::chrono::microseconds>(
                std::chrono::steady_clock::now() -
                worldPostPassStart).count());
    const bool worldPostPassSkipped = false;
    const render::WorldPassTimingRenderStats passTimings{
        .particleUpdateMicroseconds = particleUpdatePassMicroseconds,
        .particlePrepareMicroseconds = particlePreparePassMicroseconds,
        .terrainResourceMicroseconds = terrainResourcePassMicroseconds,
        .terrainResourceSkipped = terrainResourcePassSkipped,
        .skyboxMicroseconds = skyboxPassMicroseconds,
        .skyboxSkipped = skyboxPassSkipped,
        .terrainPacketMicroseconds = terrainPacketPassMicroseconds,
        .terrainPacketSkipped = terrainPacketPassSkipped,
        .terrainBridgeMicroseconds = terrainBridgePassMicroseconds,
        .terrainBridgeSkipped = terrainBridgePassSkipped,
        .objectPacketMicroseconds = objectPacketPassMicroseconds,
        .objectPacketSkipped = objectPacketPassSkipped,
        .worldOverlayPacketMicroseconds =
            worldOverlayPacketPassMicroseconds,
        .worldOverlayPacketSkipped = worldOverlayPacketPassSkipped,
        .opaqueWorldMicroseconds = opaqueWorldPassMicroseconds,
        .opaqueWorldSkipped = opaqueWorldPassSkipped,
        .projectorPrepareMicroseconds =
            worldEffects.projectorPrepareMicroseconds,
        .projectorPrepareSkipped =
            worldEffects.projectorPrepareSkipped,
        .worldEffectsMicroseconds = worldEffectsPassMicroseconds,
        .worldEffectsSkipped = worldEffectsPassSkipped,
        .worldPostMicroseconds = worldPostPassMicroseconds,
        .worldPostSkipped = worldPostPassSkipped,
    };
    const size_t finalizedDrawCount = finalizeWorldFrameStats(
        frame, particleDrawList,
        {
            .passTimings = passTimings,
            .preparedDrawCount = drawCount,
            .effects = worldEffects,
            .post = worldPost,
        });
    // StaticMeshDrawPacket stores a non-owning palette pointer. All command
    // recording and statistics are complete here, so retire every packet
    // list before the frame-owned pose arena can be recycled.
    m_worldAssets->frame.drawPackets.clear();
    m_worldAssets->frame.bridgeDrawPackets.clear();
    m_worldAssets->frame.reflectionDrawPackets.clear();
    m_worldAssets->frame.overlayDrawPackets.clear();
    m_worldAssets->frame.bibDrawPackets.clear();
    m_worldAssets->residency.restPalettes.clear();
    return finalizedDrawCount;
}

} // namespace engine
