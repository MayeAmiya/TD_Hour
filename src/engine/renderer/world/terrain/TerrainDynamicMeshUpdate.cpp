#include "engine/renderer/world/terrain/TerrainDynamicMeshUpdate.h"

#include "engine/renderer/world/model/W3dStaticModel.h"
#include "engine/renderer/world/terrain/TerrainBridgeBibMeshBuilder.h"
#include "engine/renderer/world/terrain/TerrainSceneFuture.h"
#include "engine/renderer/world/terrain/TerrainWaterMeshBuilder.h"

#include <algorithm>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <utility>

namespace engine::render::detail {
namespace {

bool uploadRoad(
    TerrainGpuScene& target,
    const TerrainRoadRenderSegment& source,
    size_t roadIndex,
    TerrainRoadMeshCpu& cpu,
    TerrainGpuRoadChunk& output,
    container::String* error) {
    output.sourceRoadIndex = roadIndex;
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
    output.boundsCenter = (minimum + maximum) * 0.5f;
    output.boundsRadius = (maximum - output.boundsCenter).length();
    output.materialPass = cpu.materialPass;
    return target.materials.acquireRoad(
               source, output.geometry.materialIndex, error) &&
        target.geometry.upload(
            cpu.vertices, cpu.indices, output.geometry, error);
}

} // namespace

bool replaceTerrainRoads(
    TerrainGpuScene& target,
    const TerrainRenderSnapshot& terrain,
    container::String* error) {
    const auto roadPlan = findOrBuildCachedTerrainRoadMeshPlan(terrain);
    container::Vector<std::optional<TerrainRoadMeshCpu>> cpuProducts;
    if (!buildIndexedTerrainCpuProducts<std::optional<TerrainRoadMeshCpu>>(
            terrain.roads.size(), "terrain-road-cpu",
            [&terrain, &roadPlan](size_t roadIndex) {
                return buildTerrainRoadMesh(terrain, *roadPlan, roadIndex);
            }, cpuProducts, error)) {
        return false;
    }
    container::Vector<TerrainGpuRoadChunk> next(terrain.roads.size());
    for (size_t roadIndex = 0; roadIndex < terrain.roads.size(); ++roadIndex) {
        if (!cpuProducts[roadIndex]) continue;
        if (!uploadRoad(
                target, terrain.roads[roadIndex], roadIndex,
                *cpuProducts[roadIndex], next[roadIndex], error)) {
            target.geometry.retire(next);
            return false;
        }
    }
    target.geometry.retire(target.roads);
    target.roads = std::move(next);
    target.roadPlan = roadPlan;
    return true;
}

bool replaceTerrainRoadsInDirtyRegion(
    TerrainGpuScene& target,
    const TerrainRenderSnapshot& terrain,
    const TerrainRenderDirtyRegion& dirty,
    container::String* error) {
    if (!target.roadPlan || target.roads.size() != terrain.roads.size()) {
        return replaceTerrainRoads(target, terrain, error);
    }
    const math::vec3 dirtyMinimum = terrain.worldPosition(
        dirty.minX, dirty.minY);
    const math::vec3 dirtyMaximum = terrain.worldPosition(
        dirty.maxX, dirty.maxY);
    float maximumInfluence = terrain.cellWorldSize * 2.0f;
    for (const TerrainRoadRenderSegment& road : terrain.roads) {
        maximumInfluence = std::max(
            maximumInfluence,
            std::max(0.0f, road.width) *
                std::clamp(road.widthInTexture, 0.01f, 32.0f) * 2.0f);
    }
    const float minimumX = std::min(
        dirtyMinimum.x(), dirtyMaximum.x()) - maximumInfluence;
    const float maximumX = std::max(
        dirtyMinimum.x(), dirtyMaximum.x()) + maximumInfluence;
    const float minimumY = std::min(
        dirtyMinimum.y(), dirtyMaximum.y()) - maximumInfluence;
    const float maximumY = std::max(
        dirtyMinimum.y(), dirtyMaximum.y()) + maximumInfluence;

    container::Vector<size_t> affectedRoads;
    affectedRoads.reserve(terrain.roads.size() / 8u + 1u);
    for (size_t roadIndex = 0; roadIndex < terrain.roads.size(); ++roadIndex) {
        const TerrainRoadRenderSegment& road = terrain.roads[roadIndex];
        const float halfWidth = std::max(0.0f, road.width) *
            std::clamp(road.widthInTexture, 0.01f, 32.0f) * 0.5f;
        const float roadMinimumX =
            std::min(road.start.x(), road.end.x()) - halfWidth;
        const float roadMaximumX =
            std::max(road.start.x(), road.end.x()) + halfWidth;
        const float roadMinimumY =
            std::min(road.start.y(), road.end.y()) - halfWidth;
        const float roadMaximumY =
            std::max(road.start.y(), road.end.y()) + halfWidth;
        if (roadMaximumX < minimumX || roadMinimumX > maximumX ||
            roadMaximumY < minimumY || roadMinimumY > maximumY) {
            continue;
        }
        affectedRoads.push_back(roadIndex);
    }
    if (affectedRoads.empty()) return true;

    const auto roadPlan = target.roadPlan;
    container::Vector<std::optional<TerrainRoadMeshCpu>> cpuProducts;
    if (!buildIndexedTerrainCpuProducts<std::optional<TerrainRoadMeshCpu>>(
            affectedRoads.size(), "terrain-road-dirty-cpu",
            [&terrain, &affectedRoads, &roadPlan](size_t affectedIndex) {
                return buildTerrainRoadMesh(
                    terrain, *roadPlan, affectedRoads[affectedIndex]);
            }, cpuProducts, error)) {
        return false;
    }

    container::Vector<TerrainGpuRoadChunk> replacements(
        affectedRoads.size());
    for (size_t affectedIndex = 0;
         affectedIndex < affectedRoads.size(); ++affectedIndex) {
        const size_t roadIndex = affectedRoads[affectedIndex];
        if (!cpuProducts[affectedIndex]) continue;
        if (!uploadRoad(
                target, terrain.roads[roadIndex], roadIndex,
                *cpuProducts[affectedIndex], replacements[affectedIndex],
                error)) {
            target.geometry.retire(replacements);
            return false;
        }
    }
    for (size_t affectedIndex = 0;
         affectedIndex < affectedRoads.size(); ++affectedIndex) {
        TerrainGpuRoadChunk& previous =
            target.roads[affectedRoads[affectedIndex]];
        target.geometry.retire(previous.geometry);
        previous = std::move(replacements[affectedIndex]);
    }
    return true;
}

bool replaceTerrainBridges(
    TerrainGpuScene& target,
    const TerrainRenderSnapshot& terrain,
    container::String* error) {
    container::Vector<std::optional<TerrainBridgeMeshCpu>> cpuProducts;
    if (!buildIndexedTerrainCpuProducts<std::optional<TerrainBridgeMeshCpu>>(
            terrain.bridges.size(), "terrain-bridge-cpu",
            [&terrain](size_t bridgeIndex)
                -> std::optional<TerrainBridgeMeshCpu> {
                const TerrainBridgeRenderData& source =
                    terrain.bridges[bridgeIndex];
                const size_t damageSlot = std::min<size_t>(
                    static_cast<size_t>(source.damageState), 3u);
                if (!source.modelNames[damageSlot].empty()) return {};
                TerrainBridgeMeshCpu cpu;
                return buildTerrainBridgeMesh(source, cpu)
                    ? std::optional<TerrainBridgeMeshCpu>{std::move(cpu)}
                    : std::nullopt;
            }, cpuProducts, error)) {
        return false;
    }
    container::Vector<TerrainGpuBridgeChunk> next;
    next.reserve(terrain.bridges.size());
    for (size_t bridgeIndex = 0;
         bridgeIndex < terrain.bridges.size(); ++bridgeIndex) {
        const TerrainBridgeRenderData& source = terrain.bridges[bridgeIndex];
        const size_t damageSlot = std::min<size_t>(
            static_cast<size_t>(source.damageState), 3u);
        if (!source.modelNames[damageSlot].empty() ||
            !cpuProducts[bridgeIndex]) {
            continue;
        }
        TerrainBridgeMeshCpu& cpu = *cpuProducts[bridgeIndex];
        TerrainGpuBridgeChunk bridge;
        bridge.boundsCenter = source.boundsCenter;
        bridge.boundsRadius = source.boundsExtents.length();
        if (!target.materials.acquireBridge(
                source, bridge.geometry.materialIndex, error) ||
            !target.geometry.upload(
                cpu.vertices, cpu.indices, bridge.geometry, error)) {
            target.geometry.retire(next);
            return false;
        }
        next.push_back(std::move(bridge));
    }
    target.geometry.retire(target.bridges);
    target.bridges = std::move(next);
    target.bridgeRevision = terrain.bridgeRevision;
    return true;
}

bool replaceTerrainWater(
    TerrainGpuScene& target,
    const TerrainRenderSnapshot& terrain,
    container::String* error) {
    const TerrainWaterMaterialRenderData material =
        terrain.waterMaterial.value_or(TerrainWaterMaterialRenderData{});
    TerrainWaterMaterialRenderData normalizedMaterial = material;
    normalizedMaterial.skyTexelsPerUnit =
        target.textures.normalizedSkyTexelsPerUnit();
    const bool hasRiver = std::any_of(
        terrain.waterAreas.begin(), terrain.waterAreas.end(),
        [](const TerrainWaterRenderArea& area) { return area.river; });
    const bool buildVertexWater = terrain.vertexWater &&
        terrain.vertexWater->isValid() && !hasRiver;
    const size_t productCount = terrain.waterAreas.size() +
        (buildVertexWater ? 1u : 0u);
    container::Vector<std::optional<TerrainWaterMeshCpu>> cpuProducts;
    if (!buildIndexedTerrainCpuProducts<std::optional<TerrainWaterMeshCpu>>(
            productCount, "terrain-water-cpu",
            [&terrain, normalizedMaterial,
             areaCount = terrain.waterAreas.size()](size_t productIndex)
                -> std::optional<TerrainWaterMeshCpu> {
                TerrainWaterMeshCpu cpu;
                const bool built = productIndex < areaCount
                    ? buildTerrainWaterMesh(
                          terrain, terrain.waterAreas[productIndex],
                          normalizedMaterial, cpu)
                    : buildTerrainVertexWaterMesh(
                          *terrain.vertexWater, normalizedMaterial, cpu);
                return built
                    ? std::optional<TerrainWaterMeshCpu>{std::move(cpu)}
                    : std::nullopt;
            }, cpuProducts, error)) {
        return false;
    }

    container::Vector<TerrainGpuWaterChunk> next;
    next.reserve(productCount);
    for (size_t areaIndex = 0;
         areaIndex < terrain.waterAreas.size(); ++areaIndex) {
        if (!cpuProducts[areaIndex]) continue;
        TerrainWaterMeshCpu& cpu = *cpuProducts[areaIndex];
        TerrainGpuWaterChunk water;
        water.worldTransform = cpu.worldTransform;
        water.river = terrain.waterAreas[areaIndex].river;
        if (!target.geometry.upload(
                cpu.vertices, cpu.indices, water.geometry, error)) {
            target.geometry.retire(next);
            return false;
        }
        next.push_back(std::move(water));
    }
    if (buildVertexWater) {
        const size_t productIndex = terrain.waterAreas.size();
        if (cpuProducts[productIndex]) {
            TerrainWaterMeshCpu& cpu = *cpuProducts[productIndex];
            TerrainGpuWaterChunk water;
            water.worldTransform = cpu.worldTransform;
            water.vertexWater = terrain.vertexWater;
            if (!target.geometry.upload(
                    cpu.vertices, cpu.indices, water.geometry, error)) {
                target.geometry.retire(next);
                return false;
            }
            next.push_back(std::move(water));
        }
    }
    target.geometry.retire(target.waters);
    target.waters = std::move(next);
    target.textures.setWaterMaterial(material);
    target.waterRevision = terrain.waterRevision;
    return true;
}

bool updateTerrainDirtyGeometry(
    TerrainGpuScene& target,
    const TerrainRenderSnapshot& terrain,
    container::String* error) {
    const auto setError = [error](container::String message) {
        if (error) *error = std::move(message);
    };
    if (!terrain.isValid()) {
        setError("Terrain update snapshot is invalid");
        return false;
    }
    const bool terrainGeometryChanged =
        terrain.revision != target.terrainRevision;
    const bool borderPresentationChanged =
        terrain.borderShroudRevision != target.borderShroudRevision ||
        terrain.borderShroudEnabled != target.borderShroudEnabled ||
        terrain.playableMinimum.x() != target.playableMinimum.x() ||
        terrain.playableMinimum.y() != target.playableMinimum.y() ||
        terrain.playableMinimum.z() != target.playableMinimum.z() ||
        terrain.playableMaximum.x() != target.playableMaximum.x() ||
        terrain.playableMaximum.y() != target.playableMaximum.y() ||
        terrain.playableMaximum.z() != target.playableMaximum.z();
    if (!terrainGeometryChanged && !borderPresentationChanged) return true;
    if (!terrainGeometryChanged) {
        // Border shroud is now a draw-time presentation policy. Keep the
        // renderer-owned scene identity current without rebuilding any CPU
        // terrain vertices or replacing GPU buffers.
        target.borderShroudRevision = terrain.borderShroudRevision;
        target.borderShroudEnabled = terrain.borderShroudEnabled;
        target.playableMinimum = terrain.playableMinimum;
        target.playableMaximum = terrain.playableMaximum;
        return true;
    }

    const TerrainMaterialRenderData* materialData =
        terrain.materials &&
            terrain.materials->isValidFor(terrain.heights.size())
        ? &*terrain.materials
        : nullptr;
    if (terrain.layoutRevision != target.layoutRevision ||
        terrain.width < 2 || terrain.height < 2 ||
        terrain.borderSize != target.borderSize ||
        terrain.cellWorldSize != target.cellWorldSize ||
        terrain.heightWorldScale != target.heightWorldScale ||
        terrain.adjustCliffTextures != target.adjustCliffTextures ||
        (materialData != nullptr) != target.hasMaterialData) {
        setError(
            "Terrain layout/material source changed; full GPU upload required");
        return false;
    }

    int32_t highestCellX = -1;
    int32_t highestCellY = -1;
    for (const TerrainGpuChunk& chunk : target.chunks) {
        highestCellX = std::max(
            highestCellX, chunk.x0 + chunk.cellsX - 1);
        highestCellY = std::max(
            highestCellY, chunk.y0 + chunk.cellsY - 1);
    }
    if (highestCellX != terrain.width - 2 ||
        highestCellY != terrain.height - 2) {
        setError("Terrain dimensions changed; full GPU upload required");
        return false;
    }

    container::Vector<size_t> affectedChunks;
    affectedChunks.reserve(target.chunks.size());
    TerrainRenderDirtyRegion terrainDirty;
    if (!terrain.dirtyRegionSince(target.terrainRevision, terrainDirty)) {
        setError(
            "Terrain dirty revision history cannot cover current GPU revision");
        return false;
    }
    if (terrainDirty.minX < 0 || terrainDirty.minY < 0 ||
        terrainDirty.maxX >= terrain.width ||
        terrainDirty.maxY >= terrain.height) {
        setError(
            "Terrain dirty revision history is outside snapshot bounds");
        return false;
    }
    const int32_t minCellX = terrainDirty.minX > 1
        ? terrainDirty.minX - 2
        : 0;
    const int32_t minCellY = terrainDirty.minY > 1
        ? terrainDirty.minY - 2
        : 0;
    const int32_t maxCellX = terrainDirty.maxX >= terrain.width - 2
        ? terrain.width - 2
        : terrainDirty.maxX + 1;
    const int32_t maxCellY = terrainDirty.maxY >= terrain.height - 2
        ? terrain.height - 2
        : terrainDirty.maxY + 1;
    for (size_t index = 0; index < target.chunks.size(); ++index) {
        const TerrainGpuChunk& chunk = target.chunks[index];
        const int32_t chunkMaxX = chunk.x0 + chunk.cellsX - 1;
        const int32_t chunkMaxY = chunk.y0 + chunk.cellsY - 1;
        if (chunkMaxX < minCellX || chunk.x0 > maxCellX ||
            chunkMaxY < minCellY || chunk.y0 > maxCellY) {
            continue;
        }
        affectedChunks.push_back(index);
    }
    if (affectedChunks.empty()) {
        setError("Terrain dirty region does not overlap GPU chunks");
        return false;
    }

    struct ChunkSpec final {
        int32_t x0 = 0;
        int32_t y0 = 0;
        int32_t cellsX = 0;
        int32_t cellsY = 0;
    };
    container::Vector<ChunkSpec> specs;
    specs.reserve(affectedChunks.size());
    for (const size_t index : affectedChunks) {
        const TerrainGpuChunk& chunk = target.chunks[index];
        specs.push_back({chunk.x0, chunk.y0, chunk.cellsX, chunk.cellsY});
    }

    const auto cpuMaterialLayout =
        std::make_shared<const TerrainTileMaterialLayout>(
            target.materials.cpuLayout());
    container::Vector<std::future<TerrainTileMeshBuildResult>> tasks;
    tasks.reserve(specs.size());
    try {
        for (const ChunkSpec spec : specs) {
            tasks.push_back(submitTerrainSceneFuture<TerrainTileMeshBuildResult>(
                "terrain-partial-chunk-cpu",
                (static_cast<uint64_t>(static_cast<uint32_t>(spec.y0)) <<
                 32u) |
                    static_cast<uint32_t>(spec.x0),
                2ull * 1024ull * 1024ull,
                [&terrain, materialData, cpuMaterialLayout, spec]() {
                    TerrainTileMeshBuildResult result;
                    result.success = buildTerrainTileMeshChunk(
                        terrain, materialData, *cpuMaterialLayout,
                        spec.x0, spec.y0, spec.cellsX, spec.cellsY, false,
                        result.chunk, &result.error);
                    return result;
                }));
        }
    } catch (const std::exception& exception) {
        for (auto& task : tasks) {
            if (!task.valid()) continue;
            try { static_cast<void>(task.get()); } catch (...) {}
        }
        setError("Could not submit terrain partial CPU task: " +
            container::String(exception.what()));
        return false;
    } catch (...) {
        for (auto& task : tasks) {
            if (!task.valid()) continue;
            try { static_cast<void>(task.get()); } catch (...) {}
        }
        setError("Could not submit terrain partial CPU task");
        return false;
    }

    container::Vector<TerrainTileMeshBuildResult> cpuChunks(specs.size());
    bool cpuBuildFailed = false;
    for (size_t index = 0; index < tasks.size(); ++index) {
        try {
            cpuChunks[index] = tasks[index].get();
            if (!cpuChunks[index].success && !cpuBuildFailed) {
                setError(cpuChunks[index].error.empty()
                    ? "Terrain partial chunk CPU preparation failed"
                    : cpuChunks[index].error);
                cpuBuildFailed = true;
            }
        } catch (const std::exception& exception) {
            if (!cpuBuildFailed) {
                setError("Terrain partial chunk CPU task failed: " +
                    container::String(exception.what()));
                cpuBuildFailed = true;
            }
        } catch (...) {
            if (!cpuBuildFailed) {
                setError("Terrain partial chunk CPU task failed");
                cpuBuildFailed = true;
            }
        }
    }
    if (cpuBuildFailed) return false;

    container::Vector<TerrainGpuChunk> replacements;
    replacements.reserve(affectedChunks.size());
    for (TerrainTileMeshBuildResult& result : cpuChunks) {
        TerrainGpuChunk replacement;
        if (!target.geometry.uploadChunk(
                result.chunk, target.materials, replacement, error)) {
            for (TerrainGpuChunk& pending : replacements) {
                target.geometry.retire(pending);
            }
            return false;
        }
        replacements.push_back(std::move(replacement));
    }
    if (!replaceTerrainRoadsInDirtyRegion(
            target, terrain, terrainDirty, error) ||
         !replaceTerrainBridges(target, terrain, error) ||
         !replaceTerrainWater(target, terrain, error)) {
        for (TerrainGpuChunk& pending : replacements) {
            target.geometry.retire(pending);
        }
        return false;
    }
    for (size_t index = 0; index < affectedChunks.size(); ++index) {
        TerrainGpuChunk& previous = target.chunks[affectedChunks[index]];
        target.geometry.retire(previous);
        previous = std::move(replacements[index]);
    }
    target.terrainRevision = terrain.revision;
    target.borderShroudRevision = terrain.borderShroudRevision;
    target.borderShroudEnabled = terrain.borderShroudEnabled;
    target.playableMinimum = terrain.playableMinimum;
    target.playableMaximum = terrain.playableMaximum;
    return true;
}

} // namespace engine::render::detail
