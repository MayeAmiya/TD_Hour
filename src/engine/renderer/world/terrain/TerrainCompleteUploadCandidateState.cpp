#include "engine/renderer/world/terrain/TerrainCompleteUploadCandidateState.h"

#include "core/platform/runtime_threads.h"
#include "debug/debug.h"
#include "engine/renderer/world/terrain/TerrainDrawPacketBuilder.h"
#include "engine/renderer/world/terrain/TerrainSceneFuture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <utility>

namespace engine::render {
namespace {

constexpr int32_t kCellsPerChunk = kTerrainCellsPerChunk;

} // namespace

TerrainCompleteUploadCandidate::Impl::~Impl() {
    // Every task captures immutable source values. No task references this
    // owner or its renderer-owned visual, so cancellation can detach without
    // joining the shared resource executor on the render thread.
    cancelRequested->store(true, std::memory_order_release);
    if (!device) return;
    for (auto& road : refinedRoadChunks) {
        device->retireStaticBufferAllocation(
            std::move(road.geometry.vertexBuffer));
        device->retireStaticBufferAllocation(
            std::move(road.geometry.indexBuffer));
    }
}

bool TerrainCompleteUploadCandidate::Impl::readyToDestroy()
    const noexcept {
    return taskReady(roadGraphTask) && tasksReady(chunkTasks) &&
        tasksReady(roadTasks) && tasksReady(basicRoadTasks) &&
        tasksReady(waterTasks) && tasksReady(bridgeTasks);
}

void TerrainCompleteUploadCandidate::Impl::prioritizePendingChunks(
    const RenderCameraSnapshot& camera,
    float viewportAspectRatio) {
    if (nextChunkSpec >= chunkSpecs.size()) return;
    const auto priority = [this, &camera, viewportAspectRatio](
                              const ChunkSpec& spec) {
        const float guardBand = terrain &&
                std::isfinite(terrain->cellWorldSize)
            ? terrain->cellWorldSize * static_cast<float>(kCellsPerChunk)
            : 0.0f;
        const bool visible = detail::terrainChunkSphereVisible(
            camera, viewportAspectRatio, spec.boundsCenter,
            spec.boundsRadius + guardBand);
        const float distanceSquared =
            (spec.boundsCenter - camera.position).length_sq();
        return std::pair{
            visible ? 0u : 1u,
            std::isfinite(distanceSquared)
                ? distanceSquared
                : std::numeric_limits<float>::max()};
    };
    std::stable_sort(
        chunkSpecs.begin() + static_cast<std::ptrdiff_t>(nextChunkSpec),
        chunkSpecs.end(),
        [&priority](const ChunkSpec& left, const ChunkSpec& right) {
            return priority(left) < priority(right);
        });
}

void TerrainCompleteUploadCandidate::Impl::submitNextChunkWave(
    const RenderCameraSnapshot& camera,
    float viewportAspectRatio) {
    if (!chunkTasks.empty() || nextChunkSpec >= chunkSpecs.size()) return;
    prioritizePendingChunks(camera, viewportAspectRatio);
    const auto snapshot = terrain;
    const auto cancel = cancelRequested;
    const auto sharedMaterialLayout = materialLayout;
    const size_t end = std::min(
        chunkSpecs.size(), nextChunkSpec + chunkWaveSize);
    chunkTasks.reserve(end - nextChunkSpec);
    while (nextChunkSpec < end) {
        const ChunkSpec spec = chunkSpecs[nextChunkSpec];
        const uint64_t chunkVariant =
            (static_cast<uint64_t>(static_cast<uint32_t>(spec.y0)) << 32u) |
            static_cast<uint32_t>(spec.x0);
        chunkTasks.push_back(detail::submitTerrainSceneFuture<
            TerrainTileMeshBuildResult>(
                "terrain-complete-base-cpu", chunkVariant,
                2ull * 1024ull * 1024ull,
                [snapshot, cancel, sharedMaterialLayout, spec]() {
                    TerrainTileMeshBuildResult result;
                    if (cancel->load(std::memory_order_acquire)) {
                        result.error = "Complete terrain candidate cancelled";
                        return result;
                    }
#if TD_DEBUG_ENABLED
                    const auto started = std::chrono::steady_clock::now();
                    TD_LOG_INFO(
                        "[D3D12TerrainVisual] Complete terrain chunk started: origin={},{} cells={}x{}",
                        spec.x0, spec.y0, spec.cellsX, spec.cellsY);
#endif
                    const TerrainMaterialRenderData* materials =
                        snapshot->materials &&
                            snapshot->materials->isValidFor(
                                snapshot->heights.size())
                        ? &*snapshot->materials
                        : nullptr;
                    result.success = buildTerrainTileMeshChunk(
                        *snapshot, materials, *sharedMaterialLayout,
                        spec.x0, spec.y0, spec.cellsX, spec.cellsY,
                        false, result.chunk, &result.error);
                    if (cancel->load(std::memory_order_acquire)) {
                        result.success = false;
                        result.error = "Complete terrain candidate cancelled";
                    }
#if TD_DEBUG_ENABLED
                    const auto elapsed =
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - started);
                    TD_LOG_INFO(
                        "[D3D12TerrainVisual] Complete terrain chunk ready: origin={},{} success={} elapsed={}us",
                        spec.x0, spec.y0, result.success, elapsed.count());
#endif
                    return result;
                }));
        ++nextChunkSpec;
    }
}

bool TerrainCompleteUploadCandidate::Impl::
    prepareAndSubmitInitialCpuTasks(
        const RenderCameraSnapshot& camera,
        float viewportAspectRatio) {
    if (!terrain) {
        failure = "Complete terrain candidate has no source snapshot";
        failed = true;
        return false;
    }

    for (int32_t y0 = 0; y0 < terrain->height - 1;
         y0 += kCellsPerChunk) {
        const int32_t cellsY = std::min(
            kCellsPerChunk, terrain->height - 1 - y0);
        for (int32_t x0 = 0; x0 < terrain->width - 1;
             x0 += kCellsPerChunk) {
            ChunkSpec spec{
                .x0 = x0,
                .y0 = y0,
                .cellsX = std::min(
                    kCellsPerChunk, terrain->width - 1 - x0),
                .cellsY = cellsY,
            };
            math::vec3 minimum = terrain->worldPosition(spec.x0, spec.y0);
            math::vec3 maximum = terrain->worldPosition(
                spec.x0 + spec.cellsX, spec.y0 + spec.cellsY);
            if (minimum.x() > maximum.x()) std::swap(minimum[0], maximum[0]);
            if (minimum.y() > maximum.y()) std::swap(minimum[1], maximum[1]);
            for (int32_t sampleY = spec.y0;
                 sampleY <= spec.y0 + spec.cellsY; ++sampleY) {
                for (int32_t sampleX = spec.x0;
                     sampleX <= spec.x0 + spec.cellsX; ++sampleX) {
                    const float height =
                        terrain->heightWorld(sampleX, sampleY);
                    minimum[2] = std::min(minimum.z(), height);
                    maximum[2] = std::max(maximum.z(), height);
                }
            }
            spec.boundsCenter = (minimum + maximum) * 0.5f;
            spec.boundsRadius = (maximum - spec.boundsCenter).length();
            chunkSpecs.push_back(spec);
        }
    }
    chunks.reserve(chunkSpecs.size());

    const bool hasRiver = std::any_of(
        terrain->waterAreas.begin(), terrain->waterAreas.end(),
        [](const TerrainWaterRenderArea& area) { return area.river; });
    buildVertexWater = terrain->vertexWater &&
        terrain->vertexWater->isValid() && !hasRiver;
    const size_t waterProductCount = terrain->waterAreas.size() +
        (buildVertexWater ? 1u : 0u);
    TerrainWaterMaterialRenderData waterMaterial =
        terrain->waterMaterial.value_or(TerrainWaterMaterialRenderData{});
    waterMaterial.skyTexelsPerUnit = preparedSkyTexelsPerUnit;

    const auto snapshot = terrain;
    const auto cancel = cancelRequested;
    try {
        const size_t workerCount = std::max<size_t>(
            1u, platform::runtime::sceneResourceWorkerCount());
        // Keep at least half of the scene workers available for water, roads,
        // model decode and upload publication. A new chunk wave is submitted
        // only after the previous wave has been collected.
        chunkWaveSize = std::max<size_t>(1u, workerCount / 2u);
        const size_t targetRoadTaskCount = std::max<size_t>(
            1u, workerCount * 4u);
        const size_t basicRoadGrain = std::max<size_t>(
            16u, (snapshot->roads.size() + targetRoadTaskCount - 1u) /
                     targetRoadTaskCount);
        basicRoadTasks.reserve(
            (snapshot->roads.size() + basicRoadGrain - 1u) /
                basicRoadGrain);
        for (size_t roadBegin = 0; roadBegin < snapshot->roads.size();
             roadBegin += basicRoadGrain) {
            const size_t roadEnd = std::min(
                roadBegin + basicRoadGrain, snapshot->roads.size());
            basicRoadTasks.push_back(
                detail::submitTerrainSceneFuture<TerrainRoadMeshCpuBatch>(
                    "terrain-complete-basic-road-cpu", roadBegin,
                    2ull * 1024ull * 1024ull,
                    [snapshot, cancel, roadBegin, roadEnd]() {
                        TerrainRoadMeshCpuBatch batch;
                        batch.beginIndex = roadBegin;
                        batch.meshes.resize(roadEnd - roadBegin);
                        for (size_t roadIndex = roadBegin;
                             roadIndex < roadEnd; ++roadIndex) {
                            if (cancel->load(std::memory_order_acquire)) break;
                            batch.meshes[roadIndex - roadBegin] =
                                buildBasicTerrainRoadMesh(
                                    *snapshot, roadIndex);
                        }
                        return batch;
                    }));
        }

        waterTasks.reserve(waterProductCount);
        for (size_t index = 0; index < waterProductCount; ++index) {
            waterTasks.push_back(detail::submitTerrainSceneFuture<
                std::optional<TerrainWaterMeshCpu>>(
                    "terrain-complete-water-cpu", index,
                    1ull * 1024ull * 1024ull,
                    [snapshot, cancel, waterMaterial, index,
                     areaCount = snapshot->waterAreas.size()]()
                        -> std::optional<TerrainWaterMeshCpu> {
                        if (cancel->load(std::memory_order_acquire)) {
                            return std::nullopt;
                        }
#if TD_DEBUG_ENABLED
                        const auto started = std::chrono::steady_clock::now();
                        TD_LOG_INFO(
                            "[D3D12TerrainVisual] Complete water product started: index={} areas={}",
                            index, areaCount);
#endif
                        TerrainWaterMeshCpu cpu;
                        const bool built = index < areaCount
                            ? buildTerrainWaterMesh(
                                  *snapshot, snapshot->waterAreas[index],
                                  waterMaterial, cpu)
                            : buildTerrainVertexWaterMesh(
                                  *snapshot->vertexWater,
                                  waterMaterial, cpu);
                        if (!built ||
                            cancel->load(std::memory_order_acquire)) {
                            return std::nullopt;
                        }
#if TD_DEBUG_ENABLED
                        const auto elapsed =
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - started);
                        TD_LOG_INFO(
                            "[D3D12TerrainVisual] Complete water product ready: index={} elapsed={}us",
                            index, elapsed.count());
#endif
                        return cpu;
                    }));
        }

        bridgeTasks.reserve(terrain->bridges.size());
        for (size_t index = 0; index < terrain->bridges.size(); ++index) {
            bridgeTasks.push_back(detail::submitTerrainSceneFuture<
                std::optional<TerrainBridgeMeshCpu>>(
                    "terrain-complete-bridge-cpu", index,
                    256ull * 1024ull,
                    [snapshot, cancel, index]()
                        -> std::optional<TerrainBridgeMeshCpu> {
                        if (cancel->load(std::memory_order_acquire)) {
                            return std::nullopt;
                        }
                        const TerrainBridgeRenderData& source =
                            snapshot->bridges[index];
                        const size_t damageSlot = std::min<size_t>(
                            static_cast<size_t>(source.damageState), 3u);
                        if (!source.modelNames[damageSlot].empty()) {
                            return std::nullopt;
                        }
                        TerrainBridgeMeshCpu cpu;
                        if (!buildTerrainBridgeMesh(source, cpu) ||
                            cancel->load(std::memory_order_acquire)) {
                            return std::nullopt;
                        }
                        return cpu;
                    }));
        }

        submitNextChunkWave(camera, viewportAspectRatio);
        // Basic roads/water and one bounded chunk wave enter the queue before
        // the heavier refinement graph, which must not delay BaseReady.
        roadGraphTask = detail::submitTerrainSceneFuture<
            container::SharedPtr<const TerrainRoadMeshPlan>>(
                "terrain-complete-road-graph-cpu", 0u,
                8ull * 1024ull * 1024ull,
                [snapshot, cancel]()
                    -> container::SharedPtr<const TerrainRoadMeshPlan> {
                    if (cancel->load(std::memory_order_acquire)) return {};
#if TD_DEBUG_ENABLED
                    const auto started = std::chrono::steady_clock::now();
                    TD_LOG_INFO(
                        "[D3D12TerrainVisual] Complete road graph started: roads={}",
                        snapshot->roads.size());
#endif
                    auto plan = findOrBuildCachedTerrainRoadMeshPlan(*snapshot);
#if TD_DEBUG_ENABLED
                    const auto elapsed =
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - started);
                    TD_LOG_INFO(
                        "[D3D12TerrainVisual] Complete road graph ready: roads={} elapsed={}us",
                        snapshot->roads.size(), elapsed.count());
#endif
                    if (cancel->load(std::memory_order_acquire)) return {};
                    return plan;
                });
    } catch (const std::exception& exception) {
        cancelRequested->store(true, std::memory_order_release);
        failure = "Could not submit complete terrain CPU task: " +
            container::String(exception.what());
        failed = true;
        return false;
    } catch (...) {
        cancelRequested->store(true, std::memory_order_release);
        failure = "Could not submit complete terrain CPU task";
        failed = true;
        return false;
    }
    return true;
}

bool TerrainCompleteUploadCandidate::Impl::
    submitRefinedRoadTasks() {
    if (roadTasksSubmitted) return true;
    if (!roadGraphCollected || !terrain || !roadGraph) return false;

    const auto snapshot = terrain;
    const auto cancel = cancelRequested;
    const auto graph = roadGraph;
    try {
        const size_t workerCount = std::max<size_t>(
            1u, platform::runtime::sceneResourceWorkerCount());
        const size_t targetTaskCount = std::max<size_t>(
            1u, workerCount * 4u);
        const size_t roadGrain = std::max<size_t>(
            16u, (snapshot->roads.size() + targetTaskCount - 1u) /
                     targetTaskCount);
        roadTasks.reserve(
            (snapshot->roads.size() + roadGrain - 1u) / roadGrain);
        for (size_t roadBegin = 0; roadBegin < snapshot->roads.size();
             roadBegin += roadGrain) {
            const size_t roadEnd = std::min(
                roadBegin + roadGrain, snapshot->roads.size());
            roadTasks.push_back(
                detail::submitTerrainSceneFuture<TerrainRoadMeshCpuBatch>(
                    "terrain-complete-road-cpu", roadBegin,
                    2ull * 1024ull * 1024ull,
                    [snapshot, cancel, graph, roadBegin, roadEnd]()
                        -> TerrainRoadMeshCpuBatch {
#if TD_DEBUG_ENABLED
                        const auto started = std::chrono::steady_clock::now();
                        TD_LOG_INFO(
                            "[D3D12TerrainVisual] Complete road batch started: range=[{},{})",
                            roadBegin, roadEnd);
#endif
                        TerrainRoadMeshCpuBatch batch;
                        batch.beginIndex = roadBegin;
                        batch.meshes.resize(roadEnd - roadBegin);
                        for (size_t roadIndex = roadBegin;
                             roadIndex < roadEnd; ++roadIndex) {
                            if (cancel->load(std::memory_order_acquire)) break;
                            const TerrainRoadRenderSegment& source =
                                snapshot->roads[roadIndex];
#if TD_DEBUG_ENABLED
                            const auto roadStarted =
                                std::chrono::steady_clock::now();
#endif
                            std::optional<TerrainRoadMeshCpu> cpu =
                                buildTerrainRoadMesh(
                                    *snapshot, *graph, roadIndex);
#if TD_DEBUG_ENABLED
                            const auto roadElapsed =
                                std::chrono::duration_cast<
                                    std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() -
                                    roadStarted);
                            if (roadElapsed.count() >= 50000) {
                                const float dx =
                                    source.end.x() - source.start.x();
                                const float dy =
                                    source.end.y() - source.start.y();
                                TD_LOG_INFO(
                                    "[D3D12TerrainVisual] Slow road mesh: index={} built={} length={} width={} widthInTexture={} vertices={} elapsed={}us",
                                    roadIndex, cpu.has_value(),
                                    std::sqrt(dx * dx + dy * dy),
                                    source.width, source.widthInTexture,
                                    cpu ? cpu->vertices.size() : 0u,
                                    roadElapsed.count());
                            }
#endif
                            if (cpu) {
                                batch.meshes[roadIndex - roadBegin] =
                                    std::move(*cpu);
                            }
                        }
#if TD_DEBUG_ENABLED
                        const auto elapsed =
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - started);
                        TD_LOG_INFO(
                            "[D3D12TerrainVisual] Complete road batch ready: range=[{},{}) elapsed={}us",
                            roadBegin, roadEnd, elapsed.count());
#endif
                        return batch;
                    }));
        }
        roadTasksSubmitted = true;
        return true;
    } catch (const std::exception& exception) {
        failure = "Could not submit terrain road CPU task: " +
            container::String(exception.what());
    } catch (...) {
        failure = "Could not submit terrain road CPU task";
    }
    failed = true;
    return false;
}

TerrainCompleteUploadCandidate::Impl::CpuProgress
TerrainCompleteUploadCandidate::Impl::advanceCpuPreparation(
    const RenderCameraSnapshot& camera,
    float viewportAspectRatio) {
    if (failed) return CpuProgress::Failed;

    // Drain one completed chunk wave before admitting the next. Futures from
    // old waves are removed immediately, bounding scheduler pressure and the
    // amount of work that cancellation can leave in flight.
    if (tasksReady(chunkTasks)) {
        try {
            for (auto& task : chunkTasks) {
                chunks.push_back(task.get());
                if (!chunks.back().success && !failed) {
                    failed = true;
                    failure = chunks.back().error.empty()
                        ? "Terrain chunk CPU preparation failed"
                        : chunks.back().error;
                }
            }
            chunkTasks.clear();
            if (!failed && basePublished) {
                submitNextChunkWave(camera, viewportAspectRatio);
            }
        } catch (const std::exception& exception) {
            failed = true;
            failure = "Terrain chunk CPU task failed: " +
                container::String(exception.what());
        } catch (...) {
            failed = true;
            failure = "Terrain chunk CPU task failed";
        }
        if (failed) return CpuProgress::Failed;
    }

    if (!roadGraphCollected && taskReady(roadGraphTask)) {
        try {
            container::SharedPtr<const TerrainRoadMeshPlan> graph =
                roadGraphTask.get();
            if (!graph) {
                cancelRequested->store(true, std::memory_order_release);
                return CpuProgress::Stale;
            }
            roadGraph = std::move(graph);
            roadGraphCollected = true;
        } catch (const std::exception& exception) {
            failed = true;
            failure = "Terrain road graph CPU task failed: " +
                container::String(exception.what());
        } catch (...) {
            failed = true;
            failure = "Terrain road graph CPU task failed";
        }
        if (failed) return CpuProgress::Failed;
    }

    if (basePublished && roadGraphCollected && !roadTasksSubmitted &&
        !submitRefinedRoadTasks()) {
        return CpuProgress::Failed;
    }

    const bool baseAuxiliaryProductsReady =
        tasksReady(waterTasks) && tasksReady(bridgeTasks);
    if (!baseProductsCollected && baseAuxiliaryProductsReady) {
        try {
            waters.reserve(waterTasks.size());
            for (auto& task : waterTasks) waters.push_back(task.get());
            bridges.reserve(bridgeTasks.size());
            for (auto& task : bridgeTasks) bridges.push_back(task.get());
        } catch (const std::exception& exception) {
            failed = true;
            failure = "Complete terrain CPU task failed: " +
                container::String(exception.what());
        } catch (...) {
            failed = true;
            failure = "Complete terrain CPU task failed";
        }
        baseProductsCollected = true;
        if (failed) return CpuProgress::Failed;
    }

    // The first prioritized chunk wave is sufficient to publish the camera
    // neighbourhood. Remaining chunks continue through the same candidate
    // after BaseReady and are appended to the long-lived terrain visual.
    if (!basePublished) {
        return baseProductsCollected && !chunks.empty()
            ? CpuProgress::Ready : CpuProgress::Pending;
    }

    // Wake GPU commit whenever another CPU wave has been collected. If no
    // collected wave is pending, keep polling until every chunk exists; only
    // then may the optional refined-road phase finish the candidate.
    if (nextChunkUpload < chunks.size()) return CpuProgress::Ready;
    const bool allTerrainChunksPrepared =
        nextChunkSpec == chunkSpecs.size() && chunkTasks.empty();
    if (!allTerrainChunksPrepared) return CpuProgress::Pending;
    return CpuProgress::Ready;
}

TerrainCompleteUploadCandidate::Impl::CpuProgress
TerrainCompleteUploadCandidate::Impl::
    advanceRefinedRoadPreparation() {
    if (!roadTasksSubmitted || !tasksReady(roadTasks)) {
        return CpuProgress::Pending;
    }
    if (refinedProductsCollected) return CpuProgress::Ready;

    try {
        refinedRoads.resize(terrain->roads.size());
        for (auto& task : roadTasks) {
            TerrainRoadMeshCpuBatch batch = task.get();
            for (size_t offset = 0; offset < batch.meshes.size(); ++offset) {
                const size_t roadIndex = batch.beginIndex + offset;
                if (roadIndex < refinedRoads.size()) {
                    refinedRoads[roadIndex] =
                        std::move(batch.meshes[offset]);
                }
            }
        }
        refinedRoadChunks.resize(refinedRoads.size());
        refinedProductsCollected = true;
        return CpuProgress::Ready;
    } catch (const std::exception& exception) {
        failure = "Refined road CPU task failed: " +
            container::String(exception.what());
    } catch (...) {
        failure = "Refined road CPU task failed";
    }
    failed = true;
    return CpuProgress::Failed;
}

} // namespace engine::render
