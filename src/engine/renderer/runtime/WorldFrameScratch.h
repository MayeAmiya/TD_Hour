#pragma once

#include "core/container/hash_containers.h"
#include "engine/fx/runtime/FxRuntime.h"
#include "engine/renderer/world/effects/DynamicPointLightRuntime.h"
#include "engine/renderer/world/effects/TrackMarkRenderer.h"
#include "engine/renderer/world/model/D3D12W3dModel.h"
#include "engine/renderer/world/particle/ParticleRenderer.h"
#include "engine/renderer/world/pipeline/WorldRenderer.h"
#include "engine/renderer/world/radar/TacticalRadarPresentation.h"
#include "engine/renderer/world/terrain/GroundProjectorRenderer.h"

#include <taskflow/taskflow.hpp>

#include <cstddef>
#include <cstdint>

namespace engine::render {

// Reusable CPU presentation buffers and their local incremental cursors.
// They contain no authoritative game state and may be discarded/rebuilt
// without changing simulation or long-lived GPU asset ownership.
struct WorldFrameScratch final {
    ParticleRenderDrawList particleDrawList;
    container::Vector<PreparedProjectileRenderSnapshot>
        interpolatedProjectiles;
    container::Vector<TrackMarkRenderSource> trackMarkSources;
    TrackMarkRenderDrawList trackMarkDrawList;
    container::Vector<fx::FxBonePoseDemand> fxBonePoseDemands;
    container::Vector<fx::FxPresentationBonePose> fxPresentationBonePoses;
    container::Vector<fx::FxModelParticleEmitterPose>
        fxModelParticleEmitterPoses;
    container::Vector<fx::FxPresentationInvocation> fxAdmittedInvocations;
    // Admitted confirmed FX must wait until the current prepared pose has
    // published every demanded bone. Executing the invocation first makes a
    // newly requested muzzle/tracer fall back to the object root for one
    // shot, because its bone demand is only discovered from that invocation.
    container::Vector<fx::FxPresentationSnapshot>
        fxDeferredExecutionSnapshots;

    container::Vector<StaticMeshDrawPacket> drawPackets;
    container::Vector<uint8_t> worldViewVisibility;
    container::Vector<size_t> worldViewTaskVisibleCounts;
    container::Vector<size_t> worldViewTaskDistanceCulledCounts;
    container::Vector<size_t> worldViewTaskFrustumCulledCounts;
    tf::Taskflow worldViewCullingTaskflow;
    size_t worldViewVisibleCount = 0;
    size_t worldViewDistanceCulledCount = 0;
    size_t worldViewFrustumCulledCount = 0;
    uint32_t poseBindingGenerationRejects = 0;
    // Highest confirmed frame actually visible on the displayed side of the
    // A/B timeline. During A->B interpolation this remains A until alpha=1.
    uint64_t displayedSimulationFrame = 0;
    W3dModelGraphTraversalStats modelGraphTraversalStats;
    container::Vector<StaticMeshDrawPacket> bridgeDrawPackets;
    container::Vector<StaticMeshDrawPacket> reflectionDrawPackets;
    container::Vector<TerrainBridgeRadarGeometry> bridgeRadarGeometry;
    container::Vector<StaticMeshDrawPacket> overlayDrawPackets;
    container::Vector<StaticMeshDrawPacket> bibDrawPackets;
    container::Vector<W3dMaterialTextureOverride>
        materialTextureOverrideScratch;
    container::Vector<GroundProjectorInstance> groundProjectors;
    container::Vector<GroundProjectorInstance> mapScorchProjectors;
    uint64_t mapScorchTerrainRevision = 0;
    size_t mapScorchSourceCount = 0;
    size_t mapScorchSourceCursor = 0;
    // Confirmed FX scorch commands are durable presentation state. Keep their
    // terrain-conforming geometry just as the original projected-shadow
    // manager kept long-lived decal objects; a render frame must not
    // retessellate every historical scorch.
    container::Vector<TerrainScorchRenderData> typedScorchBuildSources;
    container::Vector<GroundProjectorInstance> typedScorchProjectors;
    uint64_t typedScorchTerrainRevision = 0;
    size_t typedScorchSourceCursor = 0;
    container::Vector<GroundProjectorInstance> generalGroundDecals;
    uint64_t generalGroundDecalEpoch = 0;

    struct PoliceLightPresentation final {
        DynamicPointLightRenderData light;
        uint64_t lastSeenFrame = 0;
    };
    container::HashMap<uint64_t, PoliceLightPresentation> policeLights;
    container::Vector<uint64_t> policeLightKeys;
    container::Vector<DynamicPointLightRenderData> combinedDynamicPointLights;
    uint64_t policeLightPresentationEpoch = 0;
    uint64_t policeLightSimulationFrame = 0;
};

} // namespace engine::render
