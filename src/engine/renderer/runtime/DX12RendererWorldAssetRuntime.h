#pragma once

#include "DX12Renderer.h"
#include "DX12OutputLifecycle.h"
#include "engine/renderer/runtime/WorldAssetResidency.h"
#include "engine/renderer/runtime/WorldFrameScratch.h"
#include "engine/renderer/runtime/WorldFxPresentationRuntime.h"
#include "engine/renderer/runtime/WorldRenderQualityState.h"
#include "engine/renderer/world/effects/ProjectileTrailRenderer.h"
#include "engine/renderer/world/effects/TrackMarkRenderer.h"
#include "engine/renderer/world/overlay/ClientOptionsPresentation.h"
#include "engine/renderer/world/overlay/ObjectIconOverlayPresentation.h"
#include "engine/renderer/world/overlay/ObjectUiOverlayPresentation.h"
#include "engine/renderer/world/overlay/SelectionFlashPresentation.h"
#include "engine/renderer/world/overlay/WaypointRenderer.h"
#include "engine/renderer/world/particle/ParticleRenderer.h"
#include "engine/renderer/world/pipeline/DebugWorldSceneState.h"
#include "engine/renderer/world/pipeline/WorldDurablePresentationState.h"
#include "engine/renderer/world/pipeline/WorldRenderPipeline.h"
#include "engine/renderer/world/pipeline/WorldRenderStatsOwner.h"
#include "engine/renderer/world/radar/TacticalRadarPresentation.h"
#include "engine/renderer/world/resource/TerrainStartupFailureState.h"
#include "engine/renderer/world/resource/TerrainUploadCandidateLifecycle.h"
#include "engine/renderer/runtime/WorldPresentationLifetime.h"
#include "engine/renderer/runtime/WorldViewRuntime.h"
#include "engine/renderer/world/terrain/D3D12TerrainVisual.h"
#include "engine/renderer/world/terrain/GroundDecalPresentation.h"
#include "engine/renderer/world/terrain/GroundProjectorRenderer.h"
#include "presentation/render/SupportDrawPresentation.h"

namespace engine {

// Private renderer composition state shared only by DX12Renderer translation
// units. Keeping the complete type out of DX12Renderer.h preserves the public
// backend boundary while allowing pass implementations to be split from the
// former monolithic runtime source.
struct DX12Renderer::WorldAssetRuntime {
    explicit WorldAssetRuntime(d3d12::D3D12Device& device)
        : residency(device),
          particleRenderer(device, residency.textures),
          projectileTrailRenderer(device, residency.textures),
          trackMarkRenderer(device, residency.textures),
          waypointRenderer(device, residency.textures),
          groundProjectorRenderer(device, residency.textures),
          fx(device, residency.textures) {}

    [[nodiscard]] uint64_t retainedScratchCapacityBytes() const noexcept;

    render::WorldAssetResidency residency;
    render::ParticleRenderer particleRenderer;
    render::ProjectileTrailRenderer projectileTrailRenderer;
    render::TrackMarkRenderer trackMarkRenderer;
    render::WaypointRenderer waypointRenderer;
    render::GroundProjectorRenderer groundProjectorRenderer;
    render::GroundDecalPresentation groundDecalPresentation;
    render::WorldFxPresentationRuntime fx;
    render::WorldRenderQualityState quality;
    render::WorldPresentationLifetime lifetime;
#if TD_DEBUG_ENABLED
    container::HashMap<render::RenderEntityId, size_t>
        debugVisualTraceHashes;
    container::HashMap<render::RenderEntityId, size_t>
        debugVisualPreparedHashes;
    uint64_t debugVisualTraceObjectId = 0;
#endif
    render::DX12OutputLifecycle output;
    render::WorldRenderPipeline pipeline;
    render::ObjectIconOverlayPresentation objectIconOverlay;
    render::ObjectUiOverlayPresentation objectUiOverlay;
    render::SelectionFlashPresentation selectionFlash;
    render::TacticalRadarPresentation tacticalRadar;
    render::DebugWorldSceneState debugWorld;
    container::UniquePtr<render::D3D12TerrainVisual> terrain;
    render::TerrainUploadCandidateLifecycle terrainUploads;
    render::TerrainStartupFailureState terrainStartupFailure;
    render::WorldViewRuntime view;
    render::WorldFrameScratch frame;
    render::WorldDurablePresentationState durablePresentation;
    render::ClientOptionsPresentationConsumer clientOptionsPresentation;
    render::WorldRenderStatsOwner stats;
    bool submittedPreparationPending = false;
};

} // namespace engine
