#pragma once

#include "core/container/container_types.h"
#include "engine/renderer/world/terrain/D3D12TerrainGpuState.h"
#include "engine/renderer/world/terrain/TerrainBridgeBibMeshBuilder.h"
#include "presentation/render/TerrainRenderSnapshot.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <optional>

namespace engine::render {

class WorldTextureCache;

namespace detail {

class TerrainGpuGeometryOwner;
class TerrainGpuMaterialOwner;

class TerrainBibUpdateOwner final {
public:
    TerrainBibUpdateOwner() = default;
    ~TerrainBibUpdateOwner();

    TerrainBibUpdateOwner(const TerrainBibUpdateOwner&) = delete;
    TerrainBibUpdateOwner& operator=(const TerrainBibUpdateOwner&) = delete;

    [[nodiscard]] bool update(
        container::Span<const TerrainBibRenderData> bibs,
        WorldTextureCache* textures,
        TerrainGpuMaterialOwner& materials,
        TerrainGpuGeometryOwner& geometry,
        container::String* error);
    [[nodiscard]] bool ready() const noexcept;
    void requestCancel() noexcept;
    void retire(TerrainGpuGeometryOwner& geometry) noexcept;

    [[nodiscard]] const container::Vector<TerrainGpuBibChunk>& chunks()
        const noexcept;
    [[nodiscard]] size_t chunkCount() const noexcept;

private:
    struct CpuCandidate final {
        uint64_t contentHash = 0;
        container::Vector<TerrainBibRenderData> sources;
        container::SharedPtr<std::atomic_bool> cancelRequested =
            std::make_shared<std::atomic_bool>(false);
        container::Vector<std::future<std::optional<TerrainBibMeshCpu>>> tasks;

        [[nodiscard]] bool ready() const noexcept;
        void requestCancel() noexcept;
    };

    uint64_t m_contentHash = 0;
    uint64_t m_requestedContentHash = 0;
    std::optional<CpuCandidate> m_candidate;
    container::Vector<TerrainGpuBibChunk> m_chunks;
};

} // namespace detail
} // namespace engine::render
