#include "core/container/hash_containers.h"
#include "engine/renderer/world/terrain/D3D12TerrainVisual.h"
#include "engine/renderer/world/terrain/TerrainBibUpdateOwner.h"
#include "engine/renderer/world/terrain/TerrainRoadMeshBuilder.h"
#include "engine/renderer/world/terrain/TerrainTileMeshBuilder.h"
#include "engine/renderer/world/terrain/TerrainWaterMeshBuilder.h"
#include "engine/renderer/world/terrain/TerrainDrawPacketBuilder.h"
#include "engine/renderer/world/terrain/TerrainDynamicMeshUpdate.h"
#include "engine/renderer/world/terrain/TerrainCompleteUploadCandidateState.h"
#include "engine/renderer/world/terrain/TerrainGpuScene.h"
#include "engine/renderer/world/terrain/TerrainSceneFuture.h"

#include "engine/renderer/world/model/W3dStaticModel.h"
#include "engine/renderer/world/terrain/TerrainTextureResolver.h"
#include "engine/renderer/world/resource/WorldTextureCache.h"
#include "engine/renderer/d3d12/runtime/D3D12Device.h"
#include "debug/debug.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <optional>
#include <utility>

namespace engine::render {
namespace {

constexpr int32_t kCellsPerChunk = kTerrainCellsPerChunk;

void setError(container::String* error, container::String message) {
    if (error) *error = std::move(message);
}

} // namespace

struct D3D12TerrainVisual::Impl {
    using Chunk = detail::TerrainGpuChunk;

    using CpuGeometry = TerrainTileMeshGeometry;
    using CpuChunk = TerrainTileMeshChunk;
    using CpuChunkResult = TerrainTileMeshBuildResult;
    using CpuMaterialLayout = TerrainTileMaterialLayout;
    using RoadChunk = detail::TerrainGpuRoadChunk;

    detail::TerrainGpuScene scene;
    container::SharedPtr<WorldTextureCache> textures;
    container::SharedPtr<const TerrainTextureResolver> terrainTextureResolver;
    detail::TerrainBibUpdateOwner bibOwner;
    bool retired = false;
    mutable size_t lastVisibleChunks = 0;
    mutable size_t lastCulledChunks = 0;

    bool acquireMaterial(const TerrainMaterialRenderData* source, int32_t textureClassIndex,
                         uint32_t& output, container::String* error) {
        return scene.materials.acquireTerrain(
            source, textureClassIndex, output, error);
    }

    // The first playable frame only needs a depth-correct surface on which
    // objects can be seen.  Keep this path independent from terrain material
    // classes and point lights: one shared vertex per height sample avoids the
    // complete renderer's per-cell/per-blend vertex expansion while textures
    // and the authored road/water geometry are prepared in later frames.
    bool buildHeightfieldFallbackChunk(const TerrainRenderSnapshot& terrain,
                                       int32_t x0, int32_t y0,
                                       int32_t cellsX, int32_t cellsY,
                                       Chunk& output,
                                       container::String* error) {
        uint32_t materialIndex = 0;
        if (!acquireMaterial(nullptr, -1, materialIndex, error)) return false;
        TerrainTileMeshChunk cpu;
        if (!buildTerrainHeightfieldFallbackMeshChunk(
                terrain, materialIndex, x0, y0, cellsX, cellsY,
                cpu, error)) {
            return false;
        }
        return uploadCpuChunk(cpu, output, error);
    }
    bool buildChunk(const TerrainRenderSnapshot& terrain,
                    const TerrainMaterialRenderData* materialData,
                    int32_t x0, int32_t y0, int32_t cellsX, int32_t cellsY,
                    bool allowMaterialCreation, bool simplifiedSurface,
                    Chunk& output, container::String* error) {
        static_cast<void>(allowMaterialCreation);
        CpuMaterialLayout materialLayout = scene.materials.cpuLayout();
        CpuChunk cpu;
        if (!buildTerrainTileMeshChunk(
                terrain, materialData, materialLayout,
                x0, y0, cellsX, cellsY, simplifiedSurface,
                cpu, error)) {
            return false;
        }
        return uploadCpuChunk(cpu, output, error);
    }

    bool prepareChunkMaterialLayout(
        const TerrainRenderSnapshot& terrain,
        const TerrainMaterialRenderData* materialData,
        bool simplifiedSurface, CpuMaterialLayout* cpuLayout,
        container::String* error) {
        return scene.materials.prepareLayout(
            terrain, materialData, simplifiedSurface, cpuLayout, error);
    }

    bool uploadCpuChunk(CpuChunk& cpuChunk, Chunk& output,
                        container::String* error) {
        return scene.geometry.uploadChunk(
            cpuChunk, scene.materials, output, error);
    }
};

D3D12TerrainVisual::D3D12TerrainVisual(container::UniquePtr<Impl> impl) noexcept
    : m_impl(std::move(impl)) {}

TerrainCompleteUploadCandidate::TerrainCompleteUploadCandidate(
    container::UniquePtr<Impl> impl) noexcept
    : m_impl(std::move(impl)) {}

TerrainCompleteUploadCandidate::~TerrainCompleteUploadCandidate() = default;

void TerrainCompleteUploadCandidate::requestCancel() noexcept {
    if (m_impl) {
        m_impl->cancelRequested->store(true, std::memory_order_release);
    }
}

bool TerrainCompleteUploadCandidate::readyToDestroy() const noexcept {
    return !m_impl || m_impl->readyToDestroy();
}

container::UniquePtr<TerrainCompleteUploadCandidate>
D3D12TerrainVisual::beginCompleteUpload(
    d3d12::D3D12Device& device,
    container::SharedPtr<WorldTextureCache> textures,
    container::SharedPtr<const TerrainTextureResolver> textureResolver,
    container::SharedPtr<const TerrainRenderSnapshot> terrain,
    uint64_t presentationEpoch,
    uint64_t sessionRevision,
    const RenderCameraSnapshot& camera,
    float viewportAspectRatio,
    container::String* error) {
    if (error) error->clear();
    if (!terrain || !terrain->isValid() || terrain->width < 2 ||
        terrain->height < 2 || presentationEpoch == 0u ||
        sessionRevision == 0u) {
        setError(error, "Complete terrain candidate source is invalid");
        return nullptr;
    }

    auto visualImpl = std::make_unique<D3D12TerrainVisual::Impl>();
    visualImpl->scene.device = &device;
    visualImpl->scene.geometry.setDevice(device);
    visualImpl->textures = std::move(textures);
    visualImpl->terrainTextureResolver = textureResolver
        ? std::move(textureResolver)
        : std::make_shared<const TerrainTextureResolver>();
    visualImpl->scene.textures.setSources(
        visualImpl->textures, visualImpl->terrainTextureResolver);
    visualImpl->scene.materials.setSources(
        visualImpl->textures, visualImpl->terrainTextureResolver);
    visualImpl->scene.textures.configure(
        *terrain, detail::TerrainTexturePreparationScope::Complete);
    visualImpl->scene.terrainRevision = terrain->revision;
    visualImpl->scene.borderShroudRevision = terrain->borderShroudRevision;
    visualImpl->scene.layoutRevision = terrain->layoutRevision;
    visualImpl->scene.borderSize = terrain->borderSize;
    visualImpl->scene.playableMinimum = terrain->playableMinimum;
    visualImpl->scene.playableMaximum = terrain->playableMaximum;
    visualImpl->scene.borderShroudEnabled = terrain->borderShroudEnabled;
    visualImpl->scene.cellWorldSize = terrain->cellWorldSize;
    visualImpl->scene.heightWorldScale = terrain->heightWorldScale;
    visualImpl->scene.adjustCliffTextures = terrain->adjustCliffTextures;
    const TerrainMaterialRenderData* materialData =
        terrain->materials &&
            terrain->materials->isValidFor(terrain->heights.size())
        ? &*terrain->materials : nullptr;
    visualImpl->scene.hasMaterialData = materialData != nullptr;

    auto candidateImpl =
        std::make_unique<CompleteUploadCandidate::Impl>();
    candidateImpl->visual = container::UniquePtr<D3D12TerrainVisual>(
        new D3D12TerrainVisual(std::move(visualImpl)));
    candidateImpl->device = &device;
    candidateImpl->terrain = std::move(terrain);
    candidateImpl->presentationEpoch = presentationEpoch;
    candidateImpl->sessionRevision = sessionRevision;
    candidateImpl->terrainRevision = candidateImpl->terrain->revision;
    candidateImpl->layoutRevision = candidateImpl->terrain->layoutRevision;
    candidateImpl->borderShroudRevision =
        candidateImpl->terrain->borderShroudRevision;
    candidateImpl->waterRevision = candidateImpl->terrain->waterRevision;
    candidateImpl->bridgeRevision = candidateImpl->terrain->bridgeRevision;
    candidateImpl->preparedSkyTexelsPerUnit =
        candidateImpl->visual->m_impl->scene.textures.waterMaterial()
            .skyTexelsPerUnit;
    TD_LOG_INFO(
        "[D3D12TerrainVisual] Complete candidate begin: epoch={} session={} terrain={} layout={} border={} water={} bridge={}",
        candidateImpl->presentationEpoch, candidateImpl->sessionRevision,
        candidateImpl->terrainRevision, candidateImpl->layoutRevision,
        candidateImpl->borderShroudRevision, candidateImpl->waterRevision,
        candidateImpl->bridgeRevision);

    // This pass only assigns stable logical material indices. It never
    // acquires a texture or touches the renderer's mutable GPU material maps.
    if (!candidateImpl->visual->m_impl->prepareChunkMaterialLayout(
            *candidateImpl->terrain, materialData, false,
            candidateImpl->materialLayout.get(), error)) {
        return nullptr;
    }
    static_cast<void>(candidateImpl->visual->m_impl->scene.textures.prepare(
        *candidateImpl->terrain));
    candidateImpl->preparedSkyTexelsPerUnit =
        candidateImpl->visual->m_impl->scene.textures
            .normalizedSkyTexelsPerUnit();

    if (!candidateImpl->prepareAndSubmitInitialCpuTasks(
            camera, viewportAspectRatio)) {
        setError(error, candidateImpl->failure);
        return nullptr;
    }
    return container::UniquePtr<CompleteUploadCandidate>(
        new CompleteUploadCandidate(std::move(candidateImpl)));
}

D3D12TerrainVisual::CompleteUploadStatus
D3D12TerrainVisual::pollCompleteUpload(
    CompleteUploadCandidate& candidate,
    uint64_t presentationEpoch,
    uint64_t sessionRevision,
    const TerrainRenderSnapshot& currentTerrain,
    const RenderCameraSnapshot& camera,
    float viewportAspectRatio,
    container::UniquePtr<D3D12TerrainVisual>& output,
    D3D12TerrainVisual* publishedVisual,
    container::String* error) {
    if (error) error->clear();
    output.reset();
    CompleteUploadCandidate::Impl* state = candidate.m_impl.get();
    if (!state || !state->terrain ||
        (!state->visual &&
         (!state->basePublished || !publishedVisual ||
          state->publishedVisual != publishedVisual))) {
        setError(error, "Complete terrain candidate is invalid");
        return CompleteUploadStatus::Failed;
    }
    const bool cancelled =
        state->cancelRequested->load(std::memory_order_acquire);
    const bool identityChanged =
        state->presentationEpoch != presentationEpoch ||
        state->sessionRevision != sessionRevision ||
        state->terrainRevision != currentTerrain.revision ||
        state->layoutRevision != currentTerrain.layoutRevision ||
        state->borderShroudRevision != currentTerrain.borderShroudRevision ||
        state->waterRevision != currentTerrain.waterRevision ||
        state->bridgeRevision != currentTerrain.bridgeRevision;
    if (cancelled || identityChanged) {
        TD_LOG_INFO(
            "[D3D12TerrainVisual] Complete candidate stale: cancelled={} old=[epoch={} session={} terrain={} layout={} border={} water={} bridge={}] current=[epoch={} session={} terrain={} layout={} border={} water={} bridge={}]",
            cancelled, state->presentationEpoch, state->sessionRevision,
            state->terrainRevision, state->layoutRevision,
            state->borderShroudRevision, state->waterRevision,
            state->bridgeRevision, presentationEpoch, sessionRevision,
            currentTerrain.revision, currentTerrain.layoutRevision,
            currentTerrain.borderShroudRevision, currentTerrain.waterRevision,
            currentTerrain.bridgeRevision);
        state->cancelRequested->store(true, std::memory_order_release);
        return CompleteUploadStatus::Stale;
    }
    if (state->failed) {
        setError(error, state->failure);
        return CompleteUploadStatus::Failed;
    }

    switch (state->advanceCpuPreparation(camera, viewportAspectRatio)) {
    case CompleteUploadCandidate::Impl::CpuProgress::Pending:
        return CompleteUploadStatus::Pending;
    case CompleteUploadCandidate::Impl::CpuProgress::Stale:
        return CompleteUploadStatus::Stale;
    case CompleteUploadCandidate::Impl::CpuProgress::Failed:
        setError(error, state->failure);
        return CompleteUploadStatus::Failed;
    case CompleteUploadCandidate::Impl::CpuProgress::Ready:
        break;
    }
    D3D12TerrainVisual::Impl& visual = state->basePublished
        ? *publishedVisual->m_impl : *state->visual->m_impl;
    switch (state->advanceGpuCommit(visual.scene, output, error)) {
    case CompleteUploadCandidate::Impl::GpuProgress::Pending:
        return CompleteUploadStatus::Pending;
    case CompleteUploadCandidate::Impl::GpuProgress::BaseReady:
        return CompleteUploadStatus::BaseReady;
    case CompleteUploadCandidate::Impl::GpuProgress::Ready:
        return CompleteUploadStatus::Ready;
    case CompleteUploadCandidate::Impl::GpuProgress::Failed:
        return CompleteUploadStatus::Failed;
    }
    return CompleteUploadStatus::Failed;
}

bool D3D12TerrainVisual::chunkSphereVisible(
    const RenderCameraSnapshot& camera,
    float viewportAspectRatio,
    math::vec3 center,
    float radius) noexcept {
    return detail::terrainChunkSphereVisible(
        camera, viewportAspectRatio, center, radius);
}

D3D12TerrainVisual::~D3D12TerrainVisual() {
    retire();
}

container::UniquePtr<D3D12TerrainVisual> D3D12TerrainVisual::upload(
    d3d12::D3D12Device& device,
    const TerrainRenderSnapshot& terrain,
    container::String* error) {
    return upload(device, {}, {}, terrain, error);
}

container::UniquePtr<D3D12TerrainVisual> D3D12TerrainVisual::upload(
    d3d12::D3D12Device& device,
    container::SharedPtr<WorldTextureCache> textures,
    container::SharedPtr<const TerrainTextureResolver> textureResolver,
    const TerrainRenderSnapshot& terrain,
    container::String* error,
    bool* deferred,
    UploadMode mode) {
    if (error) error->clear();
    if (deferred) *deferred = false;
    if (!terrain.isValid() || terrain.width < 2 || terrain.height < 2) {
        setError(error, "Terrain snapshot has no renderable heightfield");
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    impl->scene.device = &device;
    impl->scene.geometry.setDevice(device);
    impl->textures = std::move(textures);
    impl->terrainTextureResolver = textureResolver
        ? std::move(textureResolver)
        : std::make_shared<const TerrainTextureResolver>();
    impl->scene.textures.setSources(
        impl->textures, impl->terrainTextureResolver);
    impl->scene.materials.setSources(
        impl->textures, impl->terrainTextureResolver);
    impl->scene.terrainRevision = terrain.revision;
    impl->scene.borderShroudRevision = terrain.borderShroudRevision;
    impl->scene.layoutRevision = terrain.layoutRevision;
    impl->scene.borderSize = terrain.borderSize;
    impl->scene.playableMinimum = terrain.playableMinimum;
    impl->scene.playableMaximum = terrain.playableMaximum;
    impl->scene.borderShroudEnabled = terrain.borderShroudEnabled;
    impl->scene.cellWorldSize = terrain.cellWorldSize;
    impl->scene.heightWorldScale = terrain.heightWorldScale;
    impl->scene.adjustCliffTextures = terrain.adjustCliffTextures;
    auto visual = container::UniquePtr<D3D12TerrainVisual>(new D3D12TerrainVisual(std::move(impl)));

    const bool heightfieldFallback = mode == UploadMode::HeightfieldFallback;
    const bool texturedSurfaceOnly = mode == UploadMode::TexturedSurface;
    visual->m_impl->scene.textures.configure(
        terrain,
        heightfieldFallback
            ? detail::TerrainTexturePreparationScope::HeightfieldFallback
            : texturedSurfaceOnly
                ? detail::TerrainTexturePreparationScope::TexturedSurface
                : detail::TerrainTexturePreparationScope::Complete);
    const TerrainMaterialRenderData* materialData = nullptr;
    if (!heightfieldFallback && terrain.materials &&
        terrain.materials->isValidFor(terrain.heights.size())) {
        materialData = &*terrain.materials;
    }

    visual->m_impl->scene.hasMaterialData = materialData != nullptr;

    if (!visual->m_impl->scene.textures.prepare(terrain)) {
        if (deferred) *deferred = true;
        setError(error, "terrain texture CPU preparation pending");
        return nullptr;
    }
    if (!visual->m_impl->scene.textures.acquire(error)) return nullptr;
    struct ChunkSpec final {
        int32_t x0 = 0;
        int32_t y0 = 0;
        int32_t cellsX = 0;
        int32_t cellsY = 0;
    };
    container::Vector<ChunkSpec> chunkSpecs;
    for (int32_t y0 = 0; y0 < terrain.height - 1; y0 += kCellsPerChunk) {
        const int32_t cellsY = std::min(kCellsPerChunk, terrain.height - 1 - y0);
        for (int32_t x0 = 0; x0 < terrain.width - 1; x0 += kCellsPerChunk) {
            chunkSpecs.push_back({
                .x0 = x0,
                .y0 = y0,
                .cellsX = std::min(kCellsPerChunk, terrain.width - 1 - x0),
                .cellsY = cellsY,
            });
        }
    }

    if (heightfieldFallback) {
        // This emergency first-frame path still combines fallback material
        // acquisition and D3D12 upload. Keep it on the render thread until it
        // has the same detached CpuChunk representation as the full surface.
        for (const ChunkSpec& spec : chunkSpecs) {
            Impl::Chunk chunk;
            if (!visual->m_impl->buildHeightfieldFallbackChunk(
                    terrain, spec.x0, spec.y0, spec.cellsX, spec.cellsY,
                    chunk, error)) {
                return nullptr;
            }
            visual->m_impl->scene.chunks.push_back(std::move(chunk));
        }
    } else {
        // Material creation reaches texture residency and mutates renderer
        // maps, so establish the actually referenced immutable lookup table
        // before any resource-executor task starts.
        if (!visual->m_impl->prepareChunkMaterialLayout(
                terrain, materialData, texturedSurfaceOnly, nullptr,
                error)) {
            return nullptr;
        }

        container::Vector<std::future<Impl::CpuChunkResult>> tasks;
        tasks.reserve(chunkSpecs.size());
        const Impl::CpuMaterialLayout cpuMaterialLayout =
            visual->m_impl->scene.materials.cpuLayout();
        try {
            for (const ChunkSpec spec : chunkSpecs) {
                tasks.push_back(detail::submitTerrainSceneFuture<Impl::CpuChunkResult>(
                    "terrain-base-chunk-cpu",
                    (static_cast<uint64_t>(static_cast<uint32_t>(spec.y0)) << 32u) |
                        static_cast<uint32_t>(spec.x0),
                    2ull * 1024ull * 1024ull,
                    [&terrain, materialData, cpuMaterialLayout, spec,
                     texturedSurfaceOnly]() {
                        Impl::CpuChunkResult result;
                        result.success = buildTerrainTileMeshChunk(
                            terrain, materialData, cpuMaterialLayout,
                            spec.x0, spec.y0, spec.cellsX, spec.cellsY,
                            texturedSurfaceOnly, result.chunk,
                            &result.error);
                        return result;
                    }));
            }
        } catch (const std::exception& exception) {
            for (std::future<Impl::CpuChunkResult>& task : tasks) {
                if (!task.valid()) continue;
                try { static_cast<void>(task.get()); } catch (...) {}
            }
            setError(error, "Could not submit terrain chunk CPU task: " +
                            container::String(exception.what()));
            return nullptr;
        } catch (...) {
            for (std::future<Impl::CpuChunkResult>& task : tasks) {
                if (!task.valid()) continue;
                try { static_cast<void>(task.get()); } catch (...) {}
            }
            setError(error, "Could not submit terrain chunk CPU task");
            return nullptr;
        }

        container::Vector<Impl::CpuChunkResult> cpuChunks(chunkSpecs.size());
        bool cpuBuildFailed = false;
        for (size_t index = 0; index < tasks.size(); ++index) {
            try {
                cpuChunks[index] = tasks[index].get();
                if (!cpuChunks[index].success && !cpuBuildFailed) {
                    setError(error, cpuChunks[index].error.empty()
                        ? "Terrain chunk CPU preparation failed"
                        : cpuChunks[index].error);
                    cpuBuildFailed = true;
                }
            } catch (const std::exception& exception) {
                if (!cpuBuildFailed) {
                    setError(error, "Terrain chunk CPU task failed: " +
                                    container::String(exception.what()));
                    cpuBuildFailed = true;
                }
            } catch (...) {
                if (!cpuBuildFailed) {
                    setError(error, "Terrain chunk CPU task failed");
                    cpuBuildFailed = true;
                }
            }
        }
        if (cpuBuildFailed) return nullptr;

        // Futures and results retain row-major ChunkSpec order regardless of
        // worker completion order. D3D12 upload and COM ownership stay here.
        visual->m_impl->scene.chunks.reserve(cpuChunks.size());
        for (Impl::CpuChunkResult& result : cpuChunks) {
            Impl::Chunk chunk;
            if (!visual->m_impl->uploadCpuChunk(
                    result.chunk, chunk, error)) {
                return nullptr;
            }
            visual->m_impl->scene.chunks.push_back(std::move(chunk));
        }
    }
    if (visual->m_impl->scene.chunks.empty()) {
        setError(error, "Terrain visual produced no chunks");
        return nullptr;
    }
    if (heightfieldFallback || texturedSurfaceOnly) return visual;
    if (!detail::replaceTerrainRoads(
            visual->m_impl->scene, terrain, error)) {
        return nullptr;
    }
    if (!detail::replaceTerrainBridges(
            visual->m_impl->scene, terrain, error)) {
        return nullptr;
    }
    if (!detail::replaceTerrainWater(
            visual->m_impl->scene, terrain, error)) {
        return nullptr;
    }
    return visual;
}

void D3D12TerrainVisual::appendDrawPackets(
    container::Vector<StaticMeshDrawPacket>& output,
    const RenderCameraSnapshot* camera,
    float viewportAspectRatio,
                           float visualTimeSeconds,
                           bool useCloudMap,
                           bool useLightMap) const {
    if (!m_impl || m_impl->retired || !m_impl->scene.device) return;
    const detail::TerrainDrawPacketSource source{
        .device = m_impl->scene.device,
        .materials = &m_impl->scene.materials.materials(),
        .chunks = &m_impl->scene.chunks,
        .roads = &m_impl->scene.roads,
        .waters = &m_impl->scene.waters,
        .bridges = &m_impl->scene.bridges,
        .bibs = &m_impl->bibOwner.chunks(),
        .waterMaterial = &m_impl->scene.textures.waterMaterial(),
        .skyWaterTextureName = m_impl->scene.textures.skyWaterTextureName(),
        .waterTextureSrvIndex =
            m_impl->scene.textures.waterTextureSrvIndex(),
        .standingWaterTextureSrvIndex =
            m_impl->scene.textures.standingWaterTextureSrvIndex(),
        .skyWaterTextureSrvIndex =
            m_impl->scene.textures.skyWaterTextureSrvIndex(),
        .terrainCloudTextureSrvIndex =
            m_impl->scene.textures.terrainCloudTextureSrvIndex(),
        .terrainMacroTextureSrvIndex =
            m_impl->scene.textures.terrainMacroTextureSrvIndex(),
        .cloudAllowedByTimeOfDay =
            m_impl->scene.textures.cloudAllowedByTimeOfDay(),
    };
    const detail::TerrainDrawPacketStats stats =
        detail::appendTerrainDrawPackets(
            source, output, camera, viewportAspectRatio, visualTimeSeconds,
            useCloudMap, useLightMap);
    m_impl->lastVisibleChunks = stats.visibleChunks;
    m_impl->lastCulledChunks = stats.culledChunks;
}

bool D3D12TerrainVisual::updateTerrain(const TerrainRenderSnapshot& terrain, container::String* error) {
    if (error) error->clear();
    if (!m_impl || m_impl->retired || !terrain.isValid()) {
        setError(error, "Terrain visual or update snapshot is invalid");
        return false;
    }
    return detail::updateTerrainDirtyGeometry(
        m_impl->scene, terrain, error);
}

bool D3D12TerrainVisual::updateWater(const TerrainRenderSnapshot& terrain, container::String* error) {
    if (error) error->clear();
    if (!m_impl || m_impl->retired ||
        terrain.revision != m_impl->scene.terrainRevision) {
        setError(error, "Terrain visual revision does not match water update");
        return false;
    }
    if (terrain.waterRevision == m_impl->scene.waterRevision) return true;
    return detail::replaceTerrainWater(
        m_impl->scene, terrain, error);
}

bool D3D12TerrainVisual::updateBridges(
    const TerrainRenderSnapshot& terrain, container::String* error) {
    if (error) error->clear();
    if (!m_impl || m_impl->retired ||
        terrain.revision != m_impl->scene.terrainRevision) {
        setError(error, "Terrain visual revision does not match bridge update");
        return false;
    }
    if (terrain.bridgeRevision == m_impl->scene.bridgeRevision) return true;
    return detail::replaceTerrainBridges(
        m_impl->scene, terrain, error);
}

bool D3D12TerrainVisual::updateBibs(
    container::Span<const TerrainBibRenderData> bibs,
    container::String* error) {
    if (error) error->clear();
    if (!m_impl || m_impl->retired) {
        setError(error, "Terrain visual is unavailable for bib update");
        return false;
    }
    return m_impl->bibOwner.update(
        bibs, m_impl->textures.get(), m_impl->scene.materials,
        m_impl->scene.geometry, error);
}

bool D3D12TerrainVisual::bibsReady() const noexcept {
    return m_impl && !m_impl->retired && m_impl->bibOwner.ready();
}

void D3D12TerrainVisual::retire() noexcept {
    if (!m_impl || m_impl->retired) return;
    m_impl->retired = true;
    m_impl->bibOwner.requestCancel();
    m_impl->bibOwner.retire(m_impl->scene.geometry);
    m_impl->scene.retire();
}

uint64_t D3D12TerrainVisual::revision() const noexcept {
    return m_impl ? m_impl->scene.terrainRevision : 0;
}

uint64_t D3D12TerrainVisual::layoutRevision() const noexcept {
    return m_impl ? m_impl->scene.layoutRevision : 0;
}

uint64_t D3D12TerrainVisual::borderShroudRevision() const noexcept {
    return m_impl ? m_impl->scene.borderShroudRevision : 0;
}

uint64_t D3D12TerrainVisual::waterRevision() const noexcept {
    return m_impl ? m_impl->scene.waterRevision : 0;
}

uint64_t D3D12TerrainVisual::bridgeRevision() const noexcept {
    return m_impl ? m_impl->scene.bridgeRevision : 0;
}

size_t D3D12TerrainVisual::chunkCount() const noexcept {
    return m_impl ? m_impl->scene.chunks.size() : 0;
}

size_t D3D12TerrainVisual::materialGeometryCount() const noexcept {
    if (!m_impl) return 0;
    size_t count = 0;
    for (const Impl::Chunk& chunk : m_impl->scene.chunks) {
        count += chunk.geometries.size();
    }
    return count;
}

size_t D3D12TerrainVisual::waterChunkCount() const noexcept {
    return m_impl ? m_impl->scene.waters.size() : 0;
}

size_t D3D12TerrainVisual::roadChunkCount() const noexcept {
    if (!m_impl) return 0;
    return static_cast<size_t>(std::count_if(
        m_impl->scene.roads.begin(), m_impl->scene.roads.end(),
        [](const Impl::RoadChunk& road) {
            return road.geometry.indexCount != 0u;
        }));
}

size_t D3D12TerrainVisual::bridgeChunkCount() const noexcept {
    return m_impl ? m_impl->scene.bridges.size() : 0;
}

size_t D3D12TerrainVisual::bibChunkCount() const noexcept {
    return m_impl ? m_impl->bibOwner.chunkCount() : 0;
}

size_t D3D12TerrainVisual::lastVisibleChunkCount() const noexcept {
    return m_impl ? m_impl->lastVisibleChunks : 0;
}

size_t D3D12TerrainVisual::lastCulledChunkCount() const noexcept {
    return m_impl ? m_impl->lastCulledChunks : 0;
}

} // namespace engine::render
