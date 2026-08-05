#pragma once

#include "engine/renderer/world/terrain/D3D12TerrainGpuState.h"
#include "engine/renderer/world/terrain/D3D12TerrainVisual.h"
#include "engine/renderer/world/terrain/TerrainBridgeBibMeshBuilder.h"
#include "engine/renderer/world/terrain/TerrainRoadMeshBuilder.h"
#include "engine/renderer/world/terrain/TerrainTileMeshBuilder.h"
#include "engine/renderer/world/terrain/TerrainWaterMeshBuilder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>

namespace engine::render {

namespace detail { struct TerrainGpuScene; }

struct TerrainCompleteUploadCandidate::Impl final {
    enum class CpuProgress : uint8_t {
        Pending,
        Ready,
        Stale,
        Failed,
    };
    enum class GpuProgress : uint8_t {
        Pending,
        BaseReady,
        Ready,
        Failed,
    };

    struct ChunkSpec final {
        int32_t x0 = 0;
        int32_t y0 = 0;
        int32_t cellsX = 0;
        int32_t cellsY = 0;
        math::vec3 boundsCenter{};
        float boundsRadius = 0.0f;
    };

    container::UniquePtr<D3D12TerrainVisual> visual;
    d3d12::D3D12Device* device = nullptr;
    container::SharedPtr<const TerrainRenderSnapshot> terrain;
    container::SharedPtr<std::atomic_bool> cancelRequested =
        std::make_shared<std::atomic_bool>(false);
    container::SharedPtr<TerrainTileMaterialLayout> materialLayout =
        std::make_shared<TerrainTileMaterialLayout>();
    uint64_t presentationEpoch = 0;
    uint64_t sessionRevision = 0;
    uint64_t terrainRevision = 0;
    uint64_t layoutRevision = 0;
    uint64_t borderShroudRevision = 0;
    uint64_t waterRevision = 0;
    uint64_t bridgeRevision = 0;
    bool buildVertexWater = false;
    float preparedSkyTexelsPerUnit = 0.0f;
    bool roadGraphCollected = false;
    bool roadTasksSubmitted = false;
    bool baseProductsCollected = false;
    bool refinedProductsCollected = false;
    bool basePublished = false;
    bool gpuMaterialStatePrepared = false;
    bool failed = false;
    container::String failure;
    container::SharedPtr<const TerrainRoadMeshPlan> roadGraph;
    std::future<container::SharedPtr<const TerrainRoadMeshPlan>> roadGraphTask;
    container::Vector<std::future<TerrainTileMeshBuildResult>> chunkTasks;
    container::Vector<ChunkSpec> chunkSpecs;
    size_t nextChunkSpec = 0;
    size_t chunkWaveSize = 1;
    container::Vector<std::future<TerrainRoadMeshCpuBatch>> roadTasks;
    container::Vector<std::future<TerrainRoadMeshCpuBatch>> basicRoadTasks;
    container::Vector<std::future<std::optional<TerrainWaterMeshCpu>>>
        waterTasks;
    container::Vector<std::future<std::optional<TerrainBridgeMeshCpu>>>
        bridgeTasks;
    container::Vector<TerrainTileMeshBuildResult> chunks;
    container::Vector<std::optional<TerrainRoadMeshCpu>> roads;
    container::Vector<std::optional<TerrainRoadMeshCpu>> refinedRoads;
    container::Vector<detail::TerrainGpuRoadChunk> refinedRoadChunks;
    container::Vector<std::optional<TerrainWaterMeshCpu>> waters;
    container::Vector<std::optional<TerrainBridgeMeshCpu>> bridges;
    size_t nextChunkUpload = 0;
    size_t nextRoadUpload = 0;
    size_t nextRefinedRoadUpload = 0;
    size_t nextBridgeUpload = 0;
    size_t nextWaterUpload = 0;
    D3D12TerrainVisual* publishedVisual = nullptr;

    template <typename Product>
    [[nodiscard]] static bool taskReady(
        const std::future<Product>& task) noexcept {
        return !task.valid() ||
            task.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready;
    }

    template <typename Product>
    [[nodiscard]] static bool tasksReady(
        const container::Vector<std::future<Product>>& tasks) noexcept {
        return std::all_of(
            tasks.begin(), tasks.end(),
            [](const std::future<Product>& task) { return taskReady(task); });
    }

    ~Impl();

    [[nodiscard]] bool readyToDestroy() const noexcept;
    void prioritizePendingChunks(
        const RenderCameraSnapshot& camera,
        float viewportAspectRatio);
    void submitNextChunkWave(
        const RenderCameraSnapshot& camera,
        float viewportAspectRatio);
    [[nodiscard]] bool prepareAndSubmitInitialCpuTasks(
        const RenderCameraSnapshot& camera,
        float viewportAspectRatio);
    [[nodiscard]] bool submitRefinedRoadTasks();
    [[nodiscard]] CpuProgress advanceCpuPreparation(
        const RenderCameraSnapshot& camera,
        float viewportAspectRatio);
    [[nodiscard]] CpuProgress advanceRefinedRoadPreparation();
    [[nodiscard]] GpuProgress advanceGpuCommit(
        detail::TerrainGpuScene& target,
        container::UniquePtr<D3D12TerrainVisual>& output,
        container::String* error);
};

} // namespace engine::render
