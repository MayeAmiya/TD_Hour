#pragma once

#include "core/container/container_types.h"

#include "presentation/render/TerrainRenderSnapshot.h"
#include "presentation/render/RenderViewSnapshot.h"
#include "engine/renderer/world/pipeline/WorldRenderer.h"
#include "engine/renderer/world/terrain/TerrainCompleteUploadCandidate.h"
#include <cstdint>
namespace engine::d3d12 { class D3D12Device; }

namespace engine::render {

class TerrainTextureResolver;

class WorldTextureCache;

// Renderer-owned fixed-size terrain chunks.  This layer consumes only the
// immutable terrain snapshot; it neither reads .map files nor observes live
// terrain logic state.  Chunking is for bounded GPU resources and future
// dirty-region updates, not dynamic LOD.
class D3D12TerrainVisual final {
public:
    enum class UploadMode : uint8_t {
        Complete,
        TexturedSurface,
        HeightfieldFallback,
    };

    enum class CompleteUploadStatus : uint8_t {
        Pending,
        BaseReady,
        Ready,
        Failed,
        Stale,
    };

    using CompleteUploadCandidate = TerrainCompleteUploadCandidate;

    ~D3D12TerrainVisual();

    D3D12TerrainVisual(const D3D12TerrainVisual&) = delete;
    D3D12TerrainVisual& operator=(const D3D12TerrainVisual&) = delete;

    [[nodiscard]] static container::UniquePtr<D3D12TerrainVisual> upload(
        d3d12::D3D12Device& device,
        container::SharedPtr<WorldTextureCache> textures,
        container::SharedPtr<const TerrainTextureResolver> textureResolver,
        const TerrainRenderSnapshot& terrain,
        container::String* error = nullptr,
        bool* deferred = nullptr,
        UploadMode mode = UploadMode::Complete);

    // Complete terrain construction is split across render frames. The
    // candidate owns its immutable source snapshot and all CPU products;
    // workers never observe renderer caches or D3D12 state. GPU material
    // acquisition and upload occur only when pollCompleteUpload reports
    // Ready, allowing the renderer to replace the published visual atomically.
    [[nodiscard]] static container::UniquePtr<CompleteUploadCandidate>
    beginCompleteUpload(
        d3d12::D3D12Device& device,
        container::SharedPtr<WorldTextureCache> textures,
        container::SharedPtr<const TerrainTextureResolver> textureResolver,
        container::SharedPtr<const TerrainRenderSnapshot> terrain,
        uint64_t presentationEpoch,
        uint64_t sessionRevision,
        const RenderCameraSnapshot& camera,
        float viewportAspectRatio,
        container::String* error = nullptr);

    [[nodiscard]] static CompleteUploadStatus pollCompleteUpload(
        CompleteUploadCandidate& candidate,
        uint64_t presentationEpoch,
        uint64_t sessionRevision,
        const TerrainRenderSnapshot& currentTerrain,
        const RenderCameraSnapshot& camera,
        float viewportAspectRatio,
        container::UniquePtr<D3D12TerrainVisual>& output,
        D3D12TerrainVisual* publishedVisual = nullptr,
        container::String* error = nullptr);

    // Compatibility/fallback entry point for tools without a render-thread
    // texture cache. It renders material selectors with deterministic colours
    // and the device white texture, never with a live map/loader reference.
    [[nodiscard]] static container::UniquePtr<D3D12TerrainVisual> upload(
        d3d12::D3D12Device& device,
        const TerrainRenderSnapshot& terrain,
        container::String* error = nullptr);

    [[nodiscard]] static bool chunkSphereVisible(
        const RenderCameraSnapshot& camera,
        float viewportAspectRatio,
        math::vec3 center,
        float radius) noexcept;
    void appendDrawPackets(container::Vector<StaticMeshDrawPacket>& output,
                           const RenderCameraSnapshot* camera = nullptr,
                           float viewportAspectRatio = 4.0f / 3.0f,
                           float visualTimeSeconds = 0.0f,
                           bool useCloudMap = true,
                           bool useLightMap = true) const;
    // Water can animate every simulation tick while terrain height chunks are
    // unchanged. Replace only the small water buffers instead of rebuilding
    // the heightfield GPU mesh on every flood step.
    // Height deformation is handled similarly: when the retained dirty
    // revision journal covers this visual's GPU revision, only overlapping
    // chunks are replaced. A false return leaves the current visual intact so
    // the caller can safely choose a full upload fallback.
    bool updateTerrain(const TerrainRenderSnapshot& terrain, container::String* error = nullptr);
    bool updateWater(const TerrainRenderSnapshot& terrain, container::String* error = nullptr);
    bool updateBridges(const TerrainRenderSnapshot& terrain,
                       container::String* error = nullptr);
    bool updateBibs(container::Span<const TerrainBibRenderData> bibs,
                    container::String* error = nullptr);
    // True only after the newest bib content observed by updateBibs() has
    // completed CPU preparation and its GPU buffers have been published.
    [[nodiscard]] bool bibsReady() const noexcept;
    void retire() noexcept;

    [[nodiscard]] uint64_t revision() const noexcept;
    [[nodiscard]] uint64_t layoutRevision() const noexcept;
    [[nodiscard]] uint64_t borderShroudRevision() const noexcept;
    [[nodiscard]] uint64_t waterRevision() const noexcept;
    [[nodiscard]] uint64_t bridgeRevision() const noexcept;
    [[nodiscard]] size_t chunkCount() const noexcept;
    [[nodiscard]] size_t materialGeometryCount() const noexcept;
    [[nodiscard]] size_t roadChunkCount() const noexcept;
    [[nodiscard]] size_t bridgeChunkCount() const noexcept;
    [[nodiscard]] size_t bibChunkCount() const noexcept;
    [[nodiscard]] size_t waterChunkCount() const noexcept;
    [[nodiscard]] size_t lastVisibleChunkCount() const noexcept;
    [[nodiscard]] size_t lastCulledChunkCount() const noexcept;

private:
    struct Impl;
    explicit D3D12TerrainVisual(container::UniquePtr<Impl> impl) noexcept;

    container::UniquePtr<Impl> m_impl;
};

} // namespace engine::render
