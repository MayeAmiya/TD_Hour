#include "DX12RendererWorldAssetRuntime.h"

#include "core/debug/debug.h"
#include "engine/renderer/world/effects/EnvironmentPresentationRender.h"
#include "engine/renderer/world/effects/SkyboxMaterialOverrides.h"
#include "engine/renderer/world/terrain/BridgeTowerPresentation.h"
#include "engine/renderer/world/terrain/BridgeW3dPresentation.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace engine {

void DX12Renderer::publishWorldTerrainResources(
    const render::PreparedWorldFrame& frame,
    const render::RenderCameraSnapshot& presentationCamera) {
    if (!m_worldAssets) return;
    const float viewportAspectRatio = m_d3d12.height() != 0u
        ? static_cast<float>(m_d3d12.width()) /
              static_cast<float>(m_d3d12.height())
        : 4.0f / 3.0f;
    if (m_worldAssets->terrainStartupFailure.active() &&
        !m_worldAssets->terrainStartupFailure.matches(frame)) {
        m_worldAssets->terrainStartupFailure.clear();
    }
    m_worldAssets->terrainUploads.reapRetired();
    if (!frame.terrain || !frame.terrain->isValid()) {
        m_worldAssets->terrainUploads.invalidateForTexturePolicy();
        m_worldAssets->terrain.reset();
        m_worldAssets->terrainStartupFailure.record(frame);
        return;
    }

    if (!m_worldAssets->residency.terrainTextureResolver) {
        m_worldAssets->residency.terrainTextureResolver =
            std::make_shared<const render::TerrainTextureResolver>();
    }

    const bool revisionChanged = m_worldAssets->terrain &&
        (m_worldAssets->terrain->revision() != frame.terrain->revision ||
         m_worldAssets->terrain->borderShroudRevision() !=
             frame.terrain->borderShroudRevision);
    bool updatedPartially = false;
    // A candidate already owns an immutable copy for this/newer revision.
    // Do not retry the same partial-update rejection for every full world
    // snapshot while that complete product is still preparing.
    if (revisionChanged &&
        !m_worldAssets->terrainUploads.fallbackActive() &&
        !m_worldAssets->terrainUploads.hasCurrent()) {
        container::String partialError;
        updatedPartially = m_worldAssets->terrain->updateTerrain(
            *frame.terrain, &partialError);
        if (updatedPartially) {
            TD_LOG_INFO(
                "[DX12Renderer] Terrain GPU chunk update: revision={} chunks={} samples={}x{}",
                frame.terrain->revision,
                m_worldAssets->terrain->chunkCount(),
                frame.terrain->width, frame.terrain->height);
        } else {
            TD_LOG_INFO(
                "[DX12Renderer] Terrain revision {} requires full GPU upload: {}",
                frame.terrain->revision, partialError);
        }
    }

    // A playable scene is published atomically. Do not expose the old
    // geometry-only/deep-green fallback and then upgrade it over several
    // frames: roads, water, bridges and authored materials are part of the
    // same required terrain product.
    if (!m_worldAssets->terrain ||
        (revisionChanged &&
         (m_worldAssets->terrainUploads.fallbackActive() ||
          !updatedPartially))) {
        m_worldAssets->terrainUploads.setFallbackActive(false);
        if (!m_worldAssets->terrainStartupFailure.matches(frame)) {
            m_worldAssets->terrainUploads.requestTextureUpgrade(
                static_cast<bool>(m_worldAssets->residency.textures));
        }
    }

    if (m_worldAssets->terrainUploads.textureUpgradePending() &&
        m_worldAssets->residency.textures) {
        container::String error;
        if (!m_worldAssets->terrainUploads.hasCurrent()) {
            m_worldAssets->terrainUploads.adopt(
                render::D3D12TerrainVisual::beginCompleteUpload(
                    m_d3d12, m_worldAssets->residency.textures,
                    m_worldAssets->residency.terrainTextureResolver, frame.terrain,
                    frame.presentationEpoch, frame.sessionRevision,
                    presentationCamera, viewportAspectRatio, &error));
            if (!m_worldAssets->terrainUploads.hasCurrent()) {
                TD_LOG_ERROR(
                    "[DX12Renderer] Could not begin complete terrain preparation at revision {}: {}; retaining the previous complete visual",
                    frame.terrain->revision, error);
                m_worldAssets->terrainUploads.clearTextureUpgrade();
                m_worldAssets->terrainStartupFailure.record(frame);
                return;
            }
        }

        container::UniquePtr<render::D3D12TerrainVisual> textured;
        const auto status = render::D3D12TerrainVisual::pollCompleteUpload(
            *m_worldAssets->terrainUploads.current(), frame.presentationEpoch,
            frame.sessionRevision, *frame.terrain,
            presentationCamera, viewportAspectRatio,
            textured, m_worldAssets->terrain.get(), &error);
        if (status ==
            render::D3D12TerrainVisual::CompleteUploadStatus::BaseReady) {
            TD_LOG_INFO(
                "[DX12Renderer] Terrain base GPU ready: epoch={} session={} terrain={} chunks={} basicRoads={} water={} bridges={}",
                frame.presentationEpoch, frame.sessionRevision,
                textured->revision(), textured->chunkCount(),
                textured->roadChunkCount(), textured->waterChunkCount(),
                textured->bridgeChunkCount());
            m_worldAssets->terrain = std::move(textured);
            m_worldAssets->terrainUploads.setFallbackActive(false);
            m_worldAssets->terrainStartupFailure.clear();
        } else if (status ==
                   render::D3D12TerrainVisual::CompleteUploadStatus::Ready) {
            const render::D3D12TerrainVisual& refined =
                *m_worldAssets->terrain;
            TD_LOG_INFO(
                "[DX12Renderer] Terrain refinement GPU ready: epoch={} session={} terrain={} layout={} border={} waterRevision={} bridge={} chunks={} materialGeometry={} roads={} water={} mapLights={} mapScorches={} samples={}x{}",
                frame.presentationEpoch, frame.sessionRevision,
                refined.revision(), refined.layoutRevision(),
                refined.borderShroudRevision(), refined.waterRevision(),
                refined.bridgeRevision(), refined.chunkCount(),
                refined.materialGeometryCount(),
                refined.roadChunkCount(), refined.waterChunkCount(),
                frame.terrain->pointLights.size(),
                frame.terrain->scorches.size(), frame.terrain->width,
                frame.terrain->height);
            m_worldAssets->terrainUploads.completeCurrent();
            m_worldAssets->terrainStartupFailure.clear();
        } else if (status ==
                   render::D3D12TerrainVisual::CompleteUploadStatus::Failed) {
            const bool baseMatches = m_worldAssets->terrain &&
                m_worldAssets->terrain->revision() == frame.terrain->revision &&
                m_worldAssets->terrain->layoutRevision() ==
                    frame.terrain->layoutRevision;
            if (baseMatches) {
                TD_LOG_WARN(
                    "[DX12Renderer] Terrain road refinement failed at revision {}: {}; retaining the published base roads",
                    frame.terrain->revision, error);
            } else {
                TD_LOG_ERROR(
                    "[DX12Renderer] Complete terrain preparation/upload failed at revision {}: {}; retaining the previous complete visual",
                    frame.terrain->revision, error);
            }
            m_worldAssets->terrainUploads.failCurrent();
            if (baseMatches) m_worldAssets->terrainStartupFailure.clear();
            else m_worldAssets->terrainStartupFailure.record(frame);
        } else if (status ==
                   render::D3D12TerrainVisual::CompleteUploadStatus::Stale) {
            m_worldAssets->terrainUploads.retireCurrent();
        }
    }
    if (m_worldAssets->terrain &&
        !m_worldAssets->terrainUploads.fallbackActive() &&
        m_worldAssets->terrain->revision() == frame.terrain->revision &&
        m_worldAssets->terrain->waterRevision() !=
            frame.terrain->waterRevision) {
        container::String error;
        if (!m_worldAssets->terrain->updateWater(*frame.terrain, &error)) {
            TD_LOG_ERROR(
                "[DX12Renderer] Terrain water GPU update failed at revision {}: {}",
                frame.terrain->waterRevision, error);
            m_worldAssets->terrainStartupFailure.record(frame);
        }
    }
    if (m_worldAssets->terrain &&
        !m_worldAssets->terrainUploads.fallbackActive() &&
        m_worldAssets->terrain->revision() == frame.terrain->revision &&
        m_worldAssets->terrain->bridgeRevision() !=
            frame.terrain->bridgeRevision) {
        container::String error;
        if (!m_worldAssets->terrain->updateBridges(*frame.terrain, &error)) {
            TD_LOG_ERROR(
                "[DX12Renderer] Terrain bridge GPU update failed at revision {}: {}",
                frame.terrain->bridgeRevision, error);
            m_worldAssets->terrainStartupFailure.record(frame);
        }
    }
}

void DX12Renderer::prepareWorldTerrainPackets(
    const render::PreparedWorldFrame& frame,
    const render::RenderCameraSnapshot& presentationCamera,
    float visualTimeSeconds) {
    if (!m_worldAssets || !m_worldAssets->terrain) return;

    container::String bibError;
    if (!m_worldAssets->terrain->updateBibs(frame.terrainBibs, &bibError)) {
        if (m_worldAssets->residency.reportedAssetFailures.insert(
                "terrain:bibs").second) {
            TD_LOG_ERROR(
                "[DX12Renderer] Terrain bib upload failed: {}", bibError);
        }
    } else {
        m_worldAssets->residency.reportedAssetFailures.erase("terrain:bibs");
    }
    const float terrainAspect = m_d3d12.height() != 0
        ? static_cast<float>(m_d3d12.width()) /
            static_cast<float>(m_d3d12.height())
        : 4.0f / 3.0f;
    m_worldAssets->terrain->appendDrawPackets(
        m_worldAssets->frame.drawPackets, &presentationCamera, terrainAspect,
        visualTimeSeconds, m_worldAssets->quality.useCloudMap,
        m_worldAssets->quality.useLightMap);

    const auto extractTerrainLayer = [this](
        render::StaticMeshWorldLayer layer,
        container::Vector<render::StaticMeshDrawPacket>& destination) {
        container::Vector<render::StaticMeshDrawPacket>& source =
            m_worldAssets->frame.drawPackets;
        size_t retained = 0;
        for (size_t index = 0; index < source.size(); ++index) {
            if (source[index].worldLayer == layer) {
                destination.push_back(std::move(source[index]));
            } else {
                if (retained != index) {
                    source[retained] = std::move(source[index]);
                }
                ++retained;
            }
        }
        source.resize(retained);
    };
    extractTerrainLayer(
        render::StaticMeshWorldLayer::Bridges,
        m_worldAssets->frame.bridgeDrawPackets);
    extractTerrainLayer(
        render::StaticMeshWorldLayer::Bibs,
        m_worldAssets->frame.bibDrawPackets);
}

void DX12Renderer::prepareWorldSkyboxPass(
    const render::SkyboxRenderState& skybox,
    const render::RenderCameraSnapshot& presentationCamera,
    float visualTimeSeconds) {
    if (!m_worldAssets || !skybox.enabled) return;
    constexpr container::StringView kLegacySkyboxPrototype = "new_skybox";
    const render::W3dModelHandle handle =
        m_worldAssets->residency.assets.request(kLegacySkyboxPrototype, false);
    if (!handle) return;

    const auto state = m_worldAssets->residency.assets.state(handle);
    if (state == render::W3dAssetState::Failed ||
        state == render::W3dAssetState::GpuUploadFailed) {
        if (m_worldAssets->residency.reportedAssetFailures.insert(
                container::String(kLegacySkyboxPrototype)).second) {
            TD_LOG_ERROR(
                "[DX12Renderer] Script skybox '{}' failed: {}",
                kLegacySkyboxPrototype,
                m_worldAssets->residency.assets.error(handle));
        }
        if (state == render::W3dAssetState::GpuUploadFailed) {
            m_worldAssets->residency.assets.queueGpuUpload(
                handle, render::W3dGpuUploadPriority::Visible);
        }
        return;
    }
    if (state == render::W3dAssetState::CpuReady) {
        m_worldAssets->residency.assets.queueGpuUpload(
            handle, render::W3dGpuUploadPriority::Visible);
        return;
    }
    if (state != render::W3dAssetState::GpuReady) return;

    const auto model = std::dynamic_pointer_cast<
        const render::D3D12W3dModel>(m_worldAssets->residency.assets.gpuModel(handle));
    if (!model || model->retired()) return;

    container::String overrideError;
    const bool overridesReady =
        m_worldAssets->residency.skyboxTextureOverrides.update(
            *m_worldAssets->residency.textures, skybox.textureNames, &overrideError);
    if (!overridesReady &&
        m_worldAssets->residency.reportedAssetFailures.insert(
            "new_skybox:WaterTransparency").second) {
        TD_LOG_ERROR(
            "[DX12Renderer] Script skybox texture override failed: {}",
            overrideError);
    }
    auto& textureOverrides =
        m_worldAssets->frame.materialTextureOverrideScratch;
    textureOverrides.clear();
    if (overridesReady) {
        textureOverrides = render::makeSkyboxMaterialTextureOverrides(
            model->materialTextureBindings(),
            m_worldAssets->residency.skyboxTextureOverrides.textureSrvs);
    }
    const float scale = std::isfinite(skybox.scale) &&
            std::abs(skybox.scale) > math::EPSILON
        ? skybox.scale : 4.5f;
    const float positionZ = std::isfinite(skybox.positionZ)
        ? skybox.positionZ : 0.0f;
    const math::transform skyboxWorld = math::transform::from_trs(
        {scale, scale, scale}, math::quat::identity(),
        {presentationCamera.position.x(), presentationCamera.position.y(),
         positionZ});
    const size_t skyboxPacketStart = m_worldAssets->frame.drawPackets.size();
    static_cast<void>(render::appendW3dModelGraphDrawPackets(
        m_worldAssets->residency.assets, handle, skyboxWorld, {}, {},
        m_worldAssets->frame.drawPackets,
        {.visualTimeSeconds = visualTimeSeconds,
         .materialTextureOverrides = textureOverrides,
         .restPalettes = &m_worldAssets->residency.restPalettes},
        &m_worldAssets->frame.modelGraphTraversalStats));
    for (size_t packetIndex = skyboxPacketStart;
         packetIndex < m_worldAssets->frame.drawPackets.size(); ++packetIndex) {
        render::StaticMeshDrawPacket& packet =
            m_worldAssets->frame.drawPackets[packetIndex];
        packet.castsShadow = false;
        packet.receivesShadow = false;
        packet.receivesDynamicLights = false;
        packet.receivesScenePointLights = false;
        packet.worldLayer = render::StaticMeshWorldLayer::Background;
    }
}

void DX12Renderer::prepareWorldTerrainBridgePackets(
    const render::PreparedWorldFrame& frame,
    float visualTimeSeconds) {
    if (!m_worldAssets || !frame.terrain) return;
    for (const render::TerrainBridgeRenderData& bridge :
         frame.terrain->bridges) {
        const size_t damageSlot = std::min<size_t>(
            static_cast<size_t>(bridge.damageState), 3u);
        const container::String& modelAsset = bridge.modelNames[damageSlot];
        if (modelAsset.empty()) continue;

        const render::W3dModelHandle handle =
            m_worldAssets->residency.assets.requestAsync(
                modelAsset, false,
                render::RenderAssetPriority::Visible);
        if (!handle) continue;
        const std::optional<render::W3dAssetState> state =
            m_worldAssets->residency.assets.state(handle);
        if (state == render::W3dAssetState::Failed ||
            state == render::W3dAssetState::GpuUploadFailed) {
            const container::String failureKey =
                modelAsset + ":TerrainBridge";
            if (m_worldAssets->residency.reportedAssetFailures.insert(
                    failureKey).second) {
                TD_LOG_ERROR(
                    "[DX12Renderer] Terrain bridge W3D '{}' failed: {}",
                    modelAsset, m_worldAssets->residency.assets.error(handle));
            }
            if (state == render::W3dAssetState::GpuUploadFailed) {
                m_worldAssets->residency.assets.queueGpuUpload(
                    handle, render::W3dGpuUploadPriority::Visible);
            }
            continue;
        }
        if (state == render::W3dAssetState::CpuReady) {
            m_worldAssets->residency.assets.queueGpuUpload(
                handle, render::W3dGpuUploadPriority::Visible);
            continue;
        }
        if (state != render::W3dAssetState::GpuReady) continue;

        const container::SharedPtr<const render::CpuStaticModel> cpu =
            m_worldAssets->residency.assets.cpuModel(handle);
        const auto model = std::dynamic_pointer_cast<
            const render::D3D12W3dModel>(
                m_worldAssets->residency.assets.gpuModel(handle));
        if (!cpu || !model || model->retired()) continue;
        const std::optional<render::BridgeW3dPresentationPlan> plan =
            render::buildBridgeW3dPresentationPlan(*cpu, bridge);
        if (!plan) {
            const container::String failureKey =
                modelAsset + ":TerrainBridgeSections";
            if (m_worldAssets->residency.reportedAssetFailures.insert(
                    failureKey).second) {
                TD_LOG_ERROR(
                    "[DX12Renderer] Terrain bridge W3D '{}' has no usable BRIDGE_LEFT/SPAN/RIGHT layout",
                    modelAsset);
            }
            continue;
        }
        m_worldAssets->frame.bridgeRadarGeometry.push_back({
            .sourceRecordIndex = bridge.sourceRecordIndex,
            .bridgeWidth = plan->bridgeWidth,
        });

        auto& textureOverrides =
            m_worldAssets->frame.materialTextureOverrideScratch;
        textureOverrides.clear();
        const container::String& textureName =
            bridge.textureNames[damageSlot];
        if (!textureName.empty()) {
            const std::optional<uint32_t> textureSrv =
                m_worldAssets->residency.treeTextureOverrides.acquire(
                    *m_worldAssets->residency.textures, textureName,
                    frame.presentationEpoch);
            if (!textureSrv) {
                const container::String failureKey =
                    textureName + ":TerrainBridgeTexture";
                if (m_worldAssets->residency.reportedAssetFailures.insert(
                        failureKey).second) {
                    TD_LOG_ERROR(
                        "[DX12Renderer] Terrain bridge texture '{}' upload failed",
                        textureName);
                }
                continue;
            }
            const container::Span<const render::W3dMaterialTextureBinding>
                bindings = model->materialTextureBindings();
            textureOverrides.reserve(bindings.size());
            for (const render::W3dMaterialTextureBinding& binding :
                 bindings) {
                textureOverrides.push_back({
                    .materialIndex = binding.materialIndex,
                    .textureSrvIndex = *textureSrv,
                    .samplerMode = 0,
                    .overridesDetailTexture = true,
                    .detailTextureSrvIndex = 0,
                });
            }
        }

        for (const render::BridgeW3dSectionInstance& section :
             plan->sections) {
            const size_t packetStart =
                m_worldAssets->frame.bridgeDrawPackets.size();
            static_cast<void>(render::appendW3dModelGraphDrawPackets(
                m_worldAssets->residency.assets, handle, section.worldTransform,
                {}, {}, m_worldAssets->frame.bridgeDrawPackets,
                {.visualTimeSeconds = visualTimeSeconds,
                 .materialTextureOverrides = textureOverrides,
                 .receivesDynamicLights = false,
                 .subObjectVisibility = section.subObjectVisibility,
                 .restPalettes = &m_worldAssets->residency.restPalettes},
                &m_worldAssets->frame.modelGraphTraversalStats));
            for (size_t packetIndex = packetStart;
                 packetIndex < m_worldAssets->frame.bridgeDrawPackets.size();
                 ++packetIndex) {
                render::StaticMeshDrawPacket& packet =
                    m_worldAssets->frame.bridgeDrawPackets[packetIndex];
                packet.worldLayer = render::StaticMeshWorldLayer::Bridges;
                packet.receivesVisibility = true;
                packet.receivesMapBorder = true;
                packet.receivesDynamicLights = false;
                packet.receivesScenePointLights = true;
                packet.dynamicLightReceiver =
                    render::StaticMeshDynamicLightReceiver::Object;
            }
        }

        const render::BridgeTowerPresentationPlan towerPlan =
            render::buildBridgeTowerPresentationPlan(
                bridge, *frame.terrain,
                plan->sourceMinimumY, plan->sourceMaximumY);
        for (const render::BridgeTowerPresentationInstance& tower :
             towerPlan.instances) {
            const render::W3dModelHandle towerHandle =
                m_worldAssets->residency.assets.requestAsync(
                    tower.modelAsset, false,
                    render::RenderAssetPriority::Visible);
            if (!towerHandle) continue;
            const std::optional<render::W3dAssetState> towerState =
                m_worldAssets->residency.assets.state(towerHandle);
            if (towerState == render::W3dAssetState::Failed ||
                towerState == render::W3dAssetState::GpuUploadFailed) {
                const container::String failureKey =
                    tower.modelAsset + ":TerrainBridgeTower";
                if (m_worldAssets->residency.reportedAssetFailures.insert(
                        failureKey).second) {
                    TD_LOG_ERROR(
                        "[DX12Renderer] Terrain bridge tower W3D '{}' failed: {}",
                        tower.modelAsset,
                        m_worldAssets->residency.assets.error(towerHandle));
                }
                if (towerState == render::W3dAssetState::GpuUploadFailed) {
                    m_worldAssets->residency.assets.queueGpuUpload(
                        towerHandle, render::W3dGpuUploadPriority::Visible);
                }
                continue;
            }
            if (towerState == render::W3dAssetState::CpuReady) {
                m_worldAssets->residency.assets.queueGpuUpload(
                    towerHandle, render::W3dGpuUploadPriority::Visible);
                continue;
            }
            if (towerState != render::W3dAssetState::GpuReady) continue;
            const auto towerModel = std::dynamic_pointer_cast<
                const render::D3D12W3dModel>(
                    m_worldAssets->residency.assets.gpuModel(towerHandle));
            if (!towerModel || towerModel->retired()) continue;

            const size_t packetStart =
                m_worldAssets->frame.bridgeDrawPackets.size();
            static_cast<void>(render::appendW3dModelGraphDrawPackets(
                m_worldAssets->residency.assets, towerHandle, tower.worldTransform,
                {}, {}, m_worldAssets->frame.bridgeDrawPackets,
                {.visualTimeSeconds = visualTimeSeconds,
                 .receivesDynamicLights = false,
                 .restPalettes = &m_worldAssets->residency.restPalettes},
                &m_worldAssets->frame.modelGraphTraversalStats));
            for (size_t packetIndex = packetStart;
                 packetIndex < m_worldAssets->frame.bridgeDrawPackets.size();
                 ++packetIndex) {
                render::StaticMeshDrawPacket& packet =
                    m_worldAssets->frame.bridgeDrawPackets[packetIndex];
                packet.worldLayer = render::StaticMeshWorldLayer::Bridges;
                packet.receivesVisibility = true;
                packet.receivesMapBorder = true;
                packet.receivesDynamicLights = false;
                packet.receivesScenePointLights = true;
                packet.dynamicLightReceiver =
                    render::StaticMeshDynamicLightReceiver::Object;
            }
        }
    }
}

} // namespace engine
