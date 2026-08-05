#include "engine/renderer/world/terrain/TerrainDrawPacketBuilder.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::render::detail {

bool terrainChunkSphereVisible(
    const RenderCameraSnapshot& camera,
    float viewportAspectRatio,
    math::vec3 center,
    float radius) noexcept {
    radius = std::isfinite(radius) ? std::max(0.0f, radius) : 0.0f;
    const math::vec3 delta = center - camera.position;
    if (!std::isfinite(delta.length_sq())) return true;

    math::vec3 forward = camera.target - camera.position;
    math::vec3 cameraUp = camera.up;
    const float forwardLength = forward.length();
    const float upLength = cameraUp.length();
    if (!std::isfinite(forwardLength) || !std::isfinite(upLength) ||
        forwardLength <= math::EPSILON || upLength <= math::EPSILON) {
        return true;
    }
    forward = forward / forwardLength;
    cameraUp = cameraUp / upLength;
    math::vec3 right = forward.cross(cameraUp);
    const float rightLength = right.length();
    if (!std::isfinite(rightLength) || rightLength <= math::EPSILON) return true;
    right = right / rightLength;
    cameraUp = right.cross(forward).normalized();

    const float depth = delta.dot(forward);
    const float nearClip = std::isfinite(camera.nearClip)
        ? std::max(0.0f, camera.nearClip)
        : 0.0f;
    const float farClip = std::isfinite(camera.farClip) &&
            camera.farClip > nearClip
        ? camera.farClip
        : std::numeric_limits<float>::max();
    if (depth + radius < nearClip || depth - radius > farClip) return false;

    const float verticalFov = renderCameraVerticalFovRadians(
        camera, viewportAspectRatio);
    const float tangentVertical = std::tan(verticalFov * 0.5f);
    const float safeAspect = renderCameraEffectiveAspectRatio(
        camera, viewportAspectRatio);
    const float tangentHorizontal = tangentVertical * safeAspect;
    const float horizontalLimit = depth * tangentHorizontal +
        radius * std::sqrt(1.0f + tangentHorizontal * tangentHorizontal);
    const float verticalLimit = depth * tangentVertical +
        radius * std::sqrt(1.0f + tangentVertical * tangentVertical);
    return std::abs(delta.dot(right)) <= horizontalLimit &&
        std::abs(delta.dot(cameraUp)) <= verticalLimit;
}

TerrainDrawPacketStats appendTerrainDrawPackets(
    const TerrainDrawPacketSource& source,
    container::Vector<StaticMeshDrawPacket>& output,
    const RenderCameraSnapshot* camera,
    float viewportAspectRatio,
    float visualTimeSeconds,
    bool useCloudMap,
    bool useLightMap) {
    TerrainDrawPacketStats stats;
    if (!source.device || !source.materials || !source.chunks ||
        !source.roads || !source.waters || !source.bridges || !source.bibs ||
        !source.waterMaterial) {
        return stats;
    }

    const auto& materials = *source.materials;
    const float deterministicVisualTime =
        std::isfinite(visualTimeSeconds) && visualTimeSeconds >= 0.0f
            ? visualTimeSeconds
            : 0.0f;
    const auto applyTerrainMacroLayers = [&](StaticMeshDrawPacket& packet,
                                              bool allowLightMap) {
        const bool cloudEnabled =
            useCloudMap && source.cloudAllowedByTimeOfDay;
        const bool macroEnabled = useLightMap && allowLightMap;
        packet.terrainMacroFlags = static_cast<uint8_t>(
            (cloudEnabled ? 1u : 0u) | (macroEnabled ? 2u : 0u));
        packet.terrainCloudTextureSrv = source.device->getSrvGpuHandle(
            source.terrainCloudTextureSrvIndex);
        packet.terrainMacroTextureSrv = source.device->getSrvGpuHandle(
            source.terrainMacroTextureSrvIndex);
        packet.visualTimeSeconds = deterministicVisualTime;
    };

    size_t terrainGeometryCount = 0;
    for (const TerrainGpuChunk& chunk : *source.chunks) {
        terrainGeometryCount += chunk.geometries.size();
    }
    output.reserve(output.size() + terrainGeometryCount +
                   source.roads->size() + source.waters->size() +
                   source.bridges->size());

    for (const TerrainGpuChunk& chunk : *source.chunks) {
        if (camera && !terrainChunkSphereVisible(
                *camera, viewportAspectRatio, chunk.boundsCenter,
                chunk.boundsRadius)) {
            ++stats.culledChunks;
            continue;
        }
        ++stats.visibleChunks;
        for (const TerrainGpuGeometry& geometry : chunk.geometries) {
            if (geometry.materialIndex >= materials.size()) continue;
            const bool hasDetailMaterial =
                geometry.detailMaterialIndex < materials.size();
            if (geometry.terrainEdgePhase ==
                    StaticMeshTerrainEdgePhase::BlendSource &&
                !hasDetailMaterial) {
                continue;
            }
            const TerrainGpuMaterial& material =
                materials[geometry.materialIndex];
            StaticMeshDrawPacket packet;
            packet.vertexBuffer = geometry.vertexView;
            packet.indexBuffer = geometry.indexView;
            packet.textureSrv = source.device->getSrvGpuHandle(
                material.textureSrvIndex);
            if (hasDetailMaterial) {
                const TerrainGpuMaterial& detail =
                    materials[geometry.detailMaterialIndex];
                packet.detailTextureSrv = source.device->getSrvGpuHandle(
                    detail.textureSrvIndex);
                packet.hasDetailTexture = true;
            }
            packet.diffuse = material.diffuse;
            packet.lightingEnabled = false;
            packet.castsShadow = false;
            packet.receivesShadow = true;
            packet.receivesVisibility = true;
            packet.receivesMapBorder = true;
            packet.fadesMapBorder = true;
            packet.receivesDynamicLights = true;
            packet.dynamicLightReceiver =
                StaticMeshDynamicLightReceiver::Terrain;
            packet.twoSided = geometry.twoSided;
            packet.samplerMode = geometry.samplerMode;
            packet.detailSamplerMode = geometry.detailSamplerMode;
            packet.terrainEdgePhase = geometry.terrainEdgePhase;
            packet.fogFunc = geometry.terrainEdgePhase ==
                    StaticMeshTerrainEdgePhase::Disabled
                ? 1
                : 0;
            packet.depthWrite = !geometry.alphaBlend;
            packet.depthCompare = StaticMeshDepthCompare::LessEqual;
            packet.blendMode = geometry.alphaBlend
                ? StaticMeshBlendMode::Alpha
                : StaticMeshBlendMode::Opaque;
            packet.worldLayer = geometry.terrainEdgePhase !=
                    StaticMeshTerrainEdgePhase::Disabled ||
                    geometry.materialPass >= 2u
                ? StaticMeshWorldLayer::TerrainExtra
                : geometry.alphaBlend
                    ? StaticMeshWorldLayer::TerrainBlend
                    : StaticMeshWorldLayer::TerrainBase;
            packet.materialPass = geometry.materialPass;
            packet.indexCount = geometry.indexCount;
            applyTerrainMacroLayers(packet, true);
            output.push_back(packet);
        }
    }

    for (const TerrainGpuRoadChunk& road : *source.roads) {
        if (road.geometry.indexCount == 0u) continue;
        if (camera && !terrainChunkSphereVisible(
                *camera, viewportAspectRatio, road.boundsCenter,
                road.boundsRadius)) {
            ++stats.culledChunks;
            continue;
        }
        if (road.geometry.materialIndex >= materials.size()) continue;
        const TerrainGpuMaterial& material =
            materials[road.geometry.materialIndex];
        StaticMeshDrawPacket packet;
        packet.vertexBuffer = road.geometry.vertexView;
        packet.indexBuffer = road.geometry.indexView;
        packet.textureSrv = source.device->getSrvGpuHandle(
            material.textureSrvIndex);
        packet.diffuse = material.diffuse;
        packet.sortCenter = road.boundsCenter;
        packet.hasExplicitSortCenter = true;
        packet.lightingEnabled = false;
        packet.castsShadow = false;
        packet.receivesShadow = true;
        packet.receivesVisibility = true;
        packet.receivesMapBorder = true;
        packet.fadesMapBorder = true;
        packet.receivesDynamicLights = false;
        packet.dynamicLightReceiver = StaticMeshDynamicLightReceiver::None;
        packet.twoSided = true;
        packet.fogFunc = 1;
        packet.depthWrite = false;
        packet.depthCompare = StaticMeshDepthCompare::LessEqual;
        packet.blendMode = StaticMeshBlendMode::Alpha;
        packet.worldLayer = StaticMeshWorldLayer::Roads;
        packet.materialPass = road.materialPass;
        packet.indexCount = road.geometry.indexCount;
        applyTerrainMacroLayers(packet, true);
        output.push_back(packet);
    }

    for (const TerrainGpuWaterChunk& water : *source.waters) {
        StaticMeshDrawPacket packet;
        packet.vertexBuffer = water.geometry.vertexView;
        packet.indexBuffer = water.geometry.indexView;
        // RefCode's renderWater()/drawRiverWater() paths both bind
        // WaterTransparency::StandingWaterTexture for authored map polygons.
        // WaterSet::WaterTexture belongs to the separate deforming/sea grid.
        // Selecting it for ordinary non-river trigger polygons is especially
        // harmful for TSWater: its alpha channel is not the standing-water
        // opacity contract and can make the complete surface disappear.
        packet.textureSrv = source.device->getSrvGpuHandle(
            water.vertexWater ? source.waterTextureSrvIndex
                              : source.standingWaterTextureSrvIndex);
        if (source.skyWaterTextureSrvIndex != 0u &&
            !source.skyWaterTextureName.empty()) {
            packet.detailTextureSrv = source.device->getSrvGpuHandle(
                source.skyWaterTextureSrvIndex);
            packet.hasDetailTexture = true;
            packet.detailColorFunc = 3;
        }
        packet.worldTransform = water.worldTransform;
        packet.diffuse = {
            source.waterMaterial->diffuseColor.x() *
                source.waterMaterial->standingWaterColor.x(),
            source.waterMaterial->diffuseColor.y() *
                source.waterMaterial->standingWaterColor.y(),
            source.waterMaterial->diffuseColor.z() *
                source.waterMaterial->standingWaterColor.z(),
            std::clamp(std::max(
                source.waterMaterial->transparentDiffuseColor.w(),
                source.waterMaterial->minimumOpacity), 0.0f, 1.0f),
        };
        packet.emissive = {};
        packet.textureMappers[0].type = StaticTextureMapperType::LinearOffset;
        packet.textureMappers[0].uPerSecond =
            -source.waterMaterial->uScrollPerSecond;
        packet.textureMappers[0].vPerSecond =
            -source.waterMaterial->vScrollPerSecond;
        packet.visualTimeSeconds = deterministicVisualTime;
        packet.lightingEnabled = false;
        packet.castsShadow = false;
        packet.receivesShadow = false;
        packet.receivesVisibility = true;
        packet.receivesMapBorder = true;
        packet.fadesMapBorder = true;
        packet.twoSided = true;
        packet.fogFunc = 1;
        packet.depthWrite = false;
        packet.depthCompare = StaticMeshDepthCompare::LessEqual;
        packet.blendMode = source.waterMaterial->additiveBlending
            ? StaticMeshBlendMode::Additive
            : StaticMeshBlendMode::Alpha;
        packet.waterSurface = true;
        packet.worldLayer = StaticMeshWorldLayer::Water;
        packet.indexCount = water.geometry.indexCount;
        output.push_back(packet);
    }

    for (const TerrainGpuBridgeChunk& bridge : *source.bridges) {
        if (camera && !terrainChunkSphereVisible(
                *camera, viewportAspectRatio, bridge.boundsCenter,
                bridge.boundsRadius)) {
            ++stats.culledChunks;
            continue;
        }
        if (bridge.geometry.materialIndex >= materials.size()) continue;
        const TerrainGpuMaterial& material =
            materials[bridge.geometry.materialIndex];
        StaticMeshDrawPacket packet;
        packet.vertexBuffer = bridge.geometry.vertexView;
        packet.indexBuffer = bridge.geometry.indexView;
        packet.textureSrv = source.device->getSrvGpuHandle(
            material.textureSrvIndex);
        packet.sortCenter = bridge.boundsCenter;
        packet.hasExplicitSortCenter = true;
        packet.diffuse = material.diffuse;
        packet.ambient = {0.25f, 0.25f, 0.25f};
        packet.lightingEnabled = true;
        packet.castsShadow = true;
        packet.receivesShadow = true;
        packet.receivesVisibility = true;
        packet.receivesMapBorder = true;
        packet.receivesDynamicLights = false;
        packet.receivesScenePointLights = true;
        packet.dynamicLightReceiver = StaticMeshDynamicLightReceiver::Object;
        packet.twoSided = true;
        packet.fogFunc = 1;
        packet.depthWrite = true;
        packet.depthCompare = StaticMeshDepthCompare::LessEqual;
        packet.blendMode = StaticMeshBlendMode::Opaque;
        packet.worldLayer = StaticMeshWorldLayer::Bridges;
        packet.indexCount = bridge.geometry.indexCount;
        applyTerrainMacroLayers(packet, false);
        output.push_back(packet);
    }

    for (const TerrainGpuBibChunk& bib : *source.bibs) {
        if (camera && !terrainChunkSphereVisible(
                *camera, viewportAspectRatio, bib.boundsCenter,
                bib.boundsRadius)) {
            continue;
        }
        if (bib.geometry.materialIndex >= materials.size()) continue;
        const TerrainGpuMaterial& material =
            materials[bib.geometry.materialIndex];
        StaticMeshDrawPacket packet;
        packet.vertexBuffer = bib.geometry.vertexView;
        packet.indexBuffer = bib.geometry.indexView;
        packet.textureSrv = source.device->getSrvGpuHandle(
            material.textureSrvIndex);
        packet.sortCenter = bib.boundsCenter;
        packet.hasExplicitSortCenter = true;
        packet.diffuse = material.diffuse;
        packet.lightingEnabled = false;
        packet.castsShadow = false;
        packet.receivesShadow = false;
        packet.receivesVisibility = bib.receivesVisibility;
        packet.receivesMapBorder = true;
        packet.twoSided = true;
        packet.fogFunc = 0;
        packet.depthWrite = false;
        packet.depthCompare = StaticMeshDepthCompare::Always;
        packet.blendMode = StaticMeshBlendMode::Alpha;
        packet.worldLayer = StaticMeshWorldLayer::Bibs;
        packet.indexCount = bib.geometry.indexCount;
        output.push_back(packet);
    }
    return stats;
}

} // namespace engine::render::detail
