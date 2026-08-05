#include "DX12RendererWorldAssetRuntime.h"

#include "core/debug/debug.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>

namespace engine {

namespace render {
namespace {

[[nodiscard]] bool groundDecalVisibleToLocalObserver(
    const GroundProjectorInstance& decal,
    const LocalVisibilityRenderSnapshot& localVisibility) noexcept {
    if (!localVisibility.isValid()) return true;
    const RenderVector center =
        (decal.corners[0] + decal.corners[1] + decal.corners[2] +
         decal.corners[3]) *
        0.25f;
    return localVisibility.worldState(center) ==
        LocalVisibilityRenderCellState::Visible;
}

[[nodiscard]] bool groundProjectorVisibleToCamera(
    const GroundProjectorInstance& projector,
    const RenderCameraSnapshot& camera,
    float viewportAspectRatio) noexcept {
    RenderVector center{};
    for (const RenderVector& corner : projector.corners) center += corner;
    center = center / static_cast<float>(projector.corners.size());
    float radiusSquared = 0.0f;
    for (const RenderVector& corner : projector.corners) {
        radiusSquared = std::max(
            radiusSquared, (corner - center).length_sq());
    }
    return D3D12TerrainVisual::chunkSphereVisible(
        camera, viewportAspectRatio, center,
        std::sqrt(std::max(radiusSquared, 0.0f)));
}

[[nodiscard]] std::optional<TerrainScorchRenderData> typedFxScorchData(
    const fx::FxTerrainScorchCommand& command) noexcept {
    const fx::ParticleVector3 source = command.anchor.world.position;
    if (!std::isfinite(source.x) || !std::isfinite(source.y) ||
        !std::isfinite(source.z) || !std::isfinite(command.radius) ||
        command.radius <= 0.0f) {
        return std::nullopt;
    }
    const int type = command.type == fx::FxTerrainScorch::Random
        ? static_cast<int>(command.identity.eventId % 4u)
        : static_cast<int>(command.type);
    return TerrainScorchRenderData{
        .position = {source.x, source.y, source.z},
        .radius = command.radius,
        .type = type,
    };
}

} // namespace
} // namespace render

size_t DX12Renderer::prepareWorldOverlayPackets(
    const render::PreparedWorldFrame& frame,
    float visualTimeSeconds) {
    if (!m_worldAssets) return 0;
    size_t drawableParticlePacketCount = 0;
    if (m_worldAssets->fx.runtime && m_worldAssets->fx.particleCatalog) {
        drawableParticlePacketCount =
            m_worldAssets->particleRenderer.appendDrawableParticleDrawPackets(
                m_worldAssets->fx.runtime->particles(),
                *m_worldAssets->fx.particleCatalog,
                m_worldAssets->residency.assets,
                m_worldAssets->frame.drawPackets,
                m_worldAssets->residency.restPalettes,
                visualTimeSeconds,
                m_worldAssets->frame.particleDrawList.interpolationAlpha,
                frame.localVisibility,
                &m_worldAssets->frame.modelGraphTraversalStats);
    }
    // W3DGameClient creates a normal Drawable for a model-only RayEffect.
    // Feed its midpoint instance through the shared W3D cache/packet stream
    // before the normal static-mesh pass, alongside other world geometry.
    const size_t modelRayPacketCount =
        m_worldAssets->fx.typed.worldRenderer().appendModelRayDrawPackets(
            m_worldAssets->residency.assets, m_worldAssets->frame.drawPackets,
            m_worldAssets->residency.restPalettes,
            visualTimeSeconds, frame.simulationFrame,
            &m_worldAssets->frame.modelGraphTraversalStats);

    // Track history consumes the dedicated, non-frustum-culled source stream
    // copied through WorldRenderPipeline. Camera culling must not break a
    // vehicle's strip merely because it drove off screen; hidden/FOW/airborne
    // objects are omitted at extraction and therefore cap the live anchor.
    container::Vector<render::TrackMarkRenderSource>& trackMarkSources =
        m_worldAssets->frame.trackMarkSources;
    render::TrackMarkRenderDrawList& trackMarkDrawList =
        m_worldAssets->frame.trackMarkDrawList;
    m_worldAssets->trackMarkRenderer.buildDrawListIntoRetained(
        trackMarkDrawList, trackMarkSources, frame.terrain.get(),
        m_worldAssets->frame.displayedSimulationFrame,
        frame.presentationEpoch);
    m_worldAssets->trackMarkRenderer.prepareTextureBindings(trackMarkDrawList);
    static_cast<void>(m_worldAssets->trackMarkRenderer.appendDrawPackets(
        trackMarkDrawList, m_worldAssets->frame.overlayDrawPackets));
    static_cast<void>(m_worldAssets->waypointRenderer.appendDrawPackets(
        frame.objectUi.waypointSegments, frame.camera,
        m_worldAssets->frame.overlayDrawPackets));
    if (trackMarkDrawList.segmentCount != 0 &&
        m_worldAssets->stats.claimFirstFrame(
            render::WorldFirstFrameDiagnostic::TrackMark)) {
        const render::TrackMarkRenderStats& trackStats =
            m_worldAssets->trackMarkRenderer.stats();
        TD_LOG_INFO(
            "[DX12Renderer] First track-mark frame: streams={} edges={} segments={} highWater={}/{}",
            trackStats.activeStreams, trackStats.activeEdges,
            trackStats.renderedSegments, trackStats.streamHighWater,
            trackStats.edgeHighWater);
    }

    return drawableParticlePacketCount + modelRayPacketCount;
}

void DX12Renderer::prepareWorldGroundProjectors(
    const render::PreparedWorldFrame& frame,
    const render::RenderCameraSnapshot& presentationCamera,
    const render::WorldLightEnvironment& lightEnvironment) {
    if (!m_worldAssets) return;
    using Clock = std::chrono::steady_clock;
    const auto preparationStart = Clock::now();
    const float viewportAspectRatio = m_d3d12.height() != 0u
        ? static_cast<float>(m_d3d12.width()) /
              static_cast<float>(m_d3d12.height())
        : 4.0f / 3.0f;
    m_worldAssets->frame.groundProjectors.clear();
    render::GroundProjectorRenderer::appendProjectedShadows(
        m_worldAssets->frame.groundProjectors, frame.visibleInstances,
        m_worldAssets->frame.interpolatedProjectiles,
        frame.terrain.get(), lightEnvironment);
    const size_t projectedShadowCount =
        m_worldAssets->frame.groundProjectors.size();
    const auto projectedShadowsEnd = Clock::now();
    size_t mapScorchBuilt = 0;
    size_t mapScorchVisible = 0;
    if (frame.terrain) {
        const size_t sourceCount = frame.terrain->scorches.size();
        if (m_worldAssets->frame.mapScorchTerrainRevision !=
                frame.terrain->revision ||
            m_worldAssets->frame.mapScorchSourceCount != sourceCount) {
            m_worldAssets->frame.mapScorchProjectors.clear();
            m_worldAssets->frame.mapScorchTerrainRevision =
                frame.terrain->revision;
            m_worldAssets->frame.mapScorchSourceCount = sourceCount;
            m_worldAssets->frame.mapScorchSourceCursor = 0;
        }

        // The original BaseHeightMap retains its scorch VB and rebuilds only
        // when dirty.  Keep the same lifetime property, but spread the initial
        // reconstruction over frames so Loading, uploads and window messages
        // cannot be starved by map-wide decoration work.
        constexpr size_t kMapScorchSourcesPerFrame = 8;
        const size_t remaining = sourceCount -
            m_worldAssets->frame.mapScorchSourceCursor;
        const size_t sourceBatch = std::min(
            remaining, kMapScorchSourcesPerFrame);
        if (sourceBatch != 0u) {
            const size_t initialProjectorCount =
                m_worldAssets->frame.mapScorchProjectors.size();
            const auto* first = frame.terrain->scorches.data() +
                m_worldAssets->frame.mapScorchSourceCursor;
            render::GroundProjectorRenderer::appendTerrainScorches(
                m_worldAssets->frame.mapScorchProjectors,
                container::Span<const render::TerrainScorchRenderData>{
                    first, sourceBatch},
                frame.terrain.get());
            mapScorchBuilt =
                m_worldAssets->frame.mapScorchProjectors.size() -
                initialProjectorCount;
            m_worldAssets->frame.mapScorchSourceCursor += sourceBatch;
        }

        for (const render::GroundProjectorInstance& projector :
             m_worldAssets->frame.mapScorchProjectors) {
            if (render::groundProjectorVisibleToCamera(
                    projector, presentationCamera, viewportAspectRatio)) {
                m_worldAssets->frame.groundProjectors.push_back(projector);
                ++mapScorchVisible;
            }
        }
    } else {
        m_worldAssets->frame.mapScorchProjectors.clear();
        m_worldAssets->frame.mapScorchTerrainRevision = 0;
        m_worldAssets->frame.mapScorchSourceCount = 0;
        m_worldAssets->frame.mapScorchSourceCursor = 0;
    }
    const auto mapScorchesEnd = Clock::now();

    size_t typedScorchBuilt = 0;
    size_t typedScorchVisible = 0;
    const container::Span<const fx::FxTerrainScorchCommand> typedScorches =
        m_worldAssets->fx.typed.terrainScorches();
    if (frame.terrain) {
        if (m_worldAssets->frame.typedScorchTerrainRevision !=
                frame.terrain->revision ||
            m_worldAssets->frame.typedScorchSourceCursor >
                typedScorches.size()) {
            m_worldAssets->frame.typedScorchProjectors.clear();
            m_worldAssets->frame.typedScorchTerrainRevision =
                frame.terrain->revision;
            m_worldAssets->frame.typedScorchSourceCursor = 0;
        }
        if (m_worldAssets->frame.typedScorchSourceCursor <
            typedScorches.size()) {
            m_worldAssets->frame.typedScorchBuildSources.clear();
            m_worldAssets->frame.typedScorchBuildSources.reserve(
                typedScorches.size() -
                m_worldAssets->frame.typedScorchSourceCursor);
            for (size_t index =
                     m_worldAssets->frame.typedScorchSourceCursor;
                 index < typedScorches.size(); ++index) {
                const std::optional<render::TerrainScorchRenderData> data =
                    render::typedFxScorchData(typedScorches[index]);
                if (data) {
                    m_worldAssets->frame.typedScorchBuildSources.push_back(
                        *data);
                }
            }
            const size_t initialProjectorCount =
                m_worldAssets->frame.typedScorchProjectors.size();
            render::GroundProjectorRenderer::appendTerrainScorches(
                m_worldAssets->frame.typedScorchProjectors,
                m_worldAssets->frame.typedScorchBuildSources,
                frame.terrain.get());
            typedScorchBuilt =
                m_worldAssets->frame.typedScorchProjectors.size() -
                initialProjectorCount;
            m_worldAssets->frame.typedScorchSourceCursor =
                typedScorches.size();
        }
        for (const render::GroundProjectorInstance& projector :
             m_worldAssets->frame.typedScorchProjectors) {
            if (render::groundProjectorVisibleToCamera(
                    projector, presentationCamera, viewportAspectRatio)) {
                m_worldAssets->frame.groundProjectors.push_back(projector);
                ++typedScorchVisible;
            }
        }
    } else {
        m_worldAssets->frame.typedScorchBuildSources.clear();
        m_worldAssets->frame.typedScorchProjectors.clear();
        m_worldAssets->frame.typedScorchTerrainRevision = 0;
        m_worldAssets->frame.typedScorchSourceCursor = 0;
    }
    const auto typedScorchesEnd = Clock::now();

    const size_t persistentDecalStart =
        m_worldAssets->frame.groundProjectors.size();
    m_worldAssets->groundDecalPresentation.appendProjectors(
        m_worldAssets->frame.groundProjectors, frame.terrain.get(),
        &presentationCamera, viewportAspectRatio);
    if (m_worldAssets->groundDecalPresentation.stats().emittedProjectors != 0 &&
        m_worldAssets->stats.claimFirstFrame(
            render::WorldFirstFrameDiagnostic::GroundDecal)) {
        const render::GroundDecalPresentationStats& decalStats =
            m_worldAssets->groundDecalPresentation.stats();
        TD_LOG_INFO(
            "[GroundDecal] First persistent frame: owners={} projectors={} highWater={} stale={} orphan={} budgetRejected={}",
            decalStats.activeOwners, decalStats.emittedProjectors,
            decalStats.highWaterOwners, decalStats.staleRejectedEvents,
            decalStats.orphanUpdateEvents,
            decalStats.budgetRejectedOwners);
    }
    const size_t persistentDecalCount =
        m_worldAssets->frame.groundProjectors.size() - persistentDecalStart;
    const auto persistentDecalsEnd = Clock::now();
    size_t generalDecalVisible = 0;
    if (frame.presentationEpoch != 0) {
        if (m_worldAssets->frame.generalGroundDecalEpoch == 0 ||
            frame.presentationEpoch >
                m_worldAssets->frame.generalGroundDecalEpoch) {
            m_worldAssets->frame.generalGroundDecals.clear();
            m_worldAssets->frame.generalGroundDecalEpoch = frame.presentationEpoch;
        }
        if (frame.presentationEpoch ==
            m_worldAssets->frame.generalGroundDecalEpoch) {
            std::erase_if(
                m_worldAssets->frame.generalGroundDecals,
                [&frame](const render::GroundProjectorInstance& decal) {
                    return !decal.visible ||
                        (decal.expireSimulationFrame != 0 &&
                         frame.simulationFrame >=
                             decal.expireSimulationFrame);
                });
            for (const render::GroundProjectorInstance& decal :
                 m_worldAssets->frame.generalGroundDecals) {
                // Local visibility gates only this frame's collection. Keep
                // shrouded decals resident so they can reappear before expiry.
                if (render::groundDecalVisibleToLocalObserver(
                        decal, frame.localVisibility)) {
                    m_worldAssets->frame.groundProjectors.push_back(decal);
                    ++generalDecalVisible;
                }
            }
        }
    }
    const auto preparationEnd = Clock::now();
    const auto micros = [](Clock::time_point begin,
                           Clock::time_point end) noexcept {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                end - begin).count());
    };
    const uint64_t totalMicroseconds =
        micros(preparationStart, preparationEnd);
    if (totalMicroseconds >= 10000u) {
        TD_LOG_INFO(
            "[GroundProjectorPrep] total={}us shadows={}/{}us map={}/{} cache={} {}us typed={}/{} cache={} sources={} {}us persistent={}/{}us general={} {}us",
            totalMicroseconds, projectedShadowCount,
            micros(preparationStart, projectedShadowsEnd),
            mapScorchBuilt, mapScorchVisible,
            m_worldAssets->frame.mapScorchProjectors.size(),
            micros(projectedShadowsEnd, mapScorchesEnd),
            typedScorchBuilt, typedScorchVisible,
            m_worldAssets->frame.typedScorchProjectors.size(),
            typedScorches.size(),
            micros(mapScorchesEnd, typedScorchesEnd),
            persistentDecalCount,
            micros(typedScorchesEnd, persistentDecalsEnd),
            generalDecalVisible,
            micros(persistentDecalsEnd, preparationEnd));
    }
}

} // namespace engine
