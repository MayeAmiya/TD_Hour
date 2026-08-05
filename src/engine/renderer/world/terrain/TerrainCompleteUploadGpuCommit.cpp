#include "engine/renderer/world/terrain/TerrainGpuScene.h"

#include "engine/renderer/world/model/W3dStaticModel.h"
#include "engine/renderer/world/terrain/TerrainCompleteUploadCandidateState.h"
#include "engine/renderer/world/terrain/TerrainGpuCommitBudget.h"

#include <algorithm>
#include <utility>

namespace engine::render {

TerrainCompleteUploadCandidate::Impl::GpuProgress
TerrainCompleteUploadCandidate::Impl::advanceGpuCommit(
    detail::TerrainGpuScene& target,
    container::UniquePtr<D3D12TerrainVisual>& output,
    container::String* error) {
    if (!gpuMaterialStatePrepared) {
        if (!target.textures.prepare(*terrain, false))
            return GpuProgress::Pending;
        if (!target.textures.acquire(error)) return GpuProgress::Failed;

        const TerrainMaterialRenderData* materialData =
            terrain->materials &&
                terrain->materials->isValidFor(terrain->heights.size())
            ? &*terrain->materials
            : nullptr;
        if (!target.materials.establishLayout(*materialLayout, error)) {
            return GpuProgress::Failed;
        }
        if (!target.materials.matchesLayout(*materialLayout)) {
            if (error) {
                *error = "Terrain material layout changed before GPU commit";
            }
            return GpuProgress::Failed;
        }
        target.chunks.reserve(chunkSpecs.size());
        target.roads.resize(roads.size());
        target.bridges.reserve(bridges.size());
        target.waters.reserve(waters.size());
        gpuMaterialStatePrepared = true;
    }

    detail::TerrainGpuCommitBudget budget(*target.device);
    if (!budget.available()) return GpuProgress::Pending;

    while (nextChunkUpload < chunks.size()) {
        TerrainTileMeshBuildResult& result = chunks[nextChunkUpload];
        const TerrainMaterialRenderData* materialData =
            terrain->materials &&
                terrain->materials->isValidFor(terrain->heights.size())
            ? &*terrain->materials : nullptr;
        if (!target.materials.prepareChunkMaterials(
                materialData, result.chunk)) {
            return GpuProgress::Pending;
        }
        if (!target.materials.acquireChunkMaterials(
                materialData, result.chunk, error)) {
            return GpuProgress::Failed;
        }
        size_t vertexCount = 0;
        size_t indexCount = 0;
        for (const TerrainTileMeshGeometry& geometry :
             result.chunk.geometries) {
            vertexCount += geometry.vertices.size();
            indexCount += geometry.indices.size();
        }
        if (!budget.admitMesh(vertexCount, indexCount)) {
            return GpuProgress::Pending;
        }
        detail::TerrainGpuChunk chunk;
        if (!target.geometry.uploadChunk(
                result.chunk, target.materials, chunk, error)) {
            return GpuProgress::Failed;
        }
        target.chunks.push_back(std::move(chunk));
        ++nextChunkUpload;
    }

    if (basePublished) {
        const bool allTerrainChunksPublished =
            nextChunkSpec == chunkSpecs.size() && chunkTasks.empty() &&
            nextChunkUpload == chunks.size();
        if (!allTerrainChunksPublished) return GpuProgress::Pending;

        const CpuProgress roadProgress = advanceRefinedRoadPreparation();
        if (roadProgress == CpuProgress::Pending) return GpuProgress::Pending;
        if (roadProgress != CpuProgress::Ready) return GpuProgress::Failed;

        while (nextRefinedRoadUpload < refinedRoads.size()) {
            const size_t roadIndex = nextRefinedRoadUpload;
            if (!refinedRoads[roadIndex]) {
                ++nextRefinedRoadUpload;
                continue;
            }
            TerrainRoadMeshCpu& cpu = *refinedRoads[roadIndex];
            if (!target.materials.prepareRoad(
                    terrain->roads[roadIndex])) {
                return GpuProgress::Pending;
            }
            if (!budget.admitMesh(
                    cpu.vertices.size(), cpu.indices.size())) {
                return GpuProgress::Pending;
            }
            detail::TerrainGpuRoadChunk road;
            road.sourceRoadIndex = roadIndex;
            math::vec3 minimum = cpu.vertices.front().position;
            math::vec3 maximum = minimum;
            for (const StaticMeshVertex& vertex : cpu.vertices) {
                minimum = {
                    std::min(minimum.x(), vertex.position.x()),
                    std::min(minimum.y(), vertex.position.y()),
                    std::min(minimum.z(), vertex.position.z()),
                };
                maximum = {
                    std::max(maximum.x(), vertex.position.x()),
                    std::max(maximum.y(), vertex.position.y()),
                    std::max(maximum.z(), vertex.position.z()),
                };
            }
            road.boundsCenter = (minimum + maximum) * 0.5f;
            road.boundsRadius = (maximum - road.boundsCenter).length();
            road.materialPass = cpu.materialPass;
            if (!target.materials.acquireRoad(
                    terrain->roads[roadIndex],
                    road.geometry.materialIndex, error) ||
                !target.geometry.upload(
                    cpu.vertices, cpu.indices, road.geometry, error)) {
                return GpuProgress::Failed;
            }
            refinedRoadChunks[roadIndex] = std::move(road);
            ++nextRefinedRoadUpload;
        }
        target.geometry.retire(target.roads);
        target.roads = std::move(refinedRoadChunks);
        target.roadPlan = roadGraph;
        return GpuProgress::Ready;
    }
    if (target.chunks.empty()) {
        if (error) *error = "Complete terrain candidate produced no chunks";
        return GpuProgress::Failed;
    }

    while (nextRoadUpload < roads.size()) {
        const size_t roadIndex = nextRoadUpload;
        if (!roads[roadIndex]) {
            ++nextRoadUpload;
            continue;
        }
        TerrainRoadMeshCpu& cpu = *roads[roadIndex];
        if (!budget.admitMesh(cpu.vertices.size(), cpu.indices.size())) {
            return GpuProgress::Pending;
        }
        const TerrainRoadRenderSegment& source = terrain->roads[roadIndex];
        detail::TerrainGpuRoadChunk road;
        road.sourceRoadIndex = roadIndex;
        math::vec3 minimum = cpu.vertices.front().position;
        math::vec3 maximum = minimum;
        for (const StaticMeshVertex& vertex : cpu.vertices) {
            minimum = {
                std::min(minimum.x(), vertex.position.x()),
                std::min(minimum.y(), vertex.position.y()),
                std::min(minimum.z(), vertex.position.z()),
            };
            maximum = {
                std::max(maximum.x(), vertex.position.x()),
                std::max(maximum.y(), vertex.position.y()),
                std::max(maximum.z(), vertex.position.z()),
            };
        }
        road.boundsCenter = (minimum + maximum) * 0.5f;
        road.boundsRadius = (maximum - road.boundsCenter).length();
        road.materialPass = cpu.materialPass;
        if (!target.materials.acquireRoad(
                source, road.geometry.materialIndex, error) ||
            !target.geometry.upload(
                cpu.vertices, cpu.indices, road.geometry, error)) {
            return GpuProgress::Failed;
        }
        target.roads[roadIndex] = std::move(road);
        ++nextRoadUpload;
    }

    while (nextBridgeUpload < bridges.size()) {
        const size_t bridgeIndex = nextBridgeUpload;
        const TerrainBridgeRenderData& source = terrain->bridges[bridgeIndex];
        const size_t damageSlot = std::min<size_t>(
            static_cast<size_t>(source.damageState), 3u);
        if (!bridges[bridgeIndex] ||
            !source.modelNames[damageSlot].empty()) {
            ++nextBridgeUpload;
            continue;
        }
        TerrainBridgeMeshCpu& cpu = *bridges[bridgeIndex];
        if (!budget.admitMesh(cpu.vertices.size(), cpu.indices.size())) {
            return GpuProgress::Pending;
        }
        detail::TerrainGpuBridgeChunk bridge;
        bridge.boundsCenter = source.boundsCenter;
        bridge.boundsRadius = source.boundsExtents.length();
        if (!target.materials.acquireBridge(
                source, bridge.geometry.materialIndex, error) ||
            !target.geometry.upload(
                cpu.vertices, cpu.indices, bridge.geometry, error)) {
            return GpuProgress::Failed;
        }
        target.bridges.push_back(std::move(bridge));
        ++nextBridgeUpload;
    }

    while (nextWaterUpload < waters.size()) {
        const size_t waterIndex = nextWaterUpload;
        if (!waters[waterIndex]) {
            ++nextWaterUpload;
            continue;
        }
        TerrainWaterMeshCpu& cpu = *waters[waterIndex];
        if (!budget.admitMesh(cpu.vertices.size(), cpu.indices.size())) {
            return GpuProgress::Pending;
        }
        detail::TerrainGpuWaterChunk water;
        water.worldTransform = cpu.worldTransform;
        if (waterIndex < terrain->waterAreas.size()) {
            water.river = terrain->waterAreas[waterIndex].river;
        } else if (buildVertexWater) {
            water.vertexWater = terrain->vertexWater;
        }
        if (!target.geometry.upload(
                cpu.vertices, cpu.indices, water.geometry, error)) {
            return GpuProgress::Failed;
        }
        target.waters.push_back(std::move(water));
        ++nextWaterUpload;
    }

    target.bridgeRevision = terrain->bridgeRevision;
    target.textures.setWaterMaterial(
        terrain->waterMaterial.value_or(TerrainWaterMaterialRenderData{}));
    target.waterRevision = terrain->waterRevision;
    target.roadPlan = roadGraph;

    output = std::move(visual);
    publishedVisual = output.get();
    basePublished = true;
    return GpuProgress::BaseReady;
}

} // namespace engine::render
