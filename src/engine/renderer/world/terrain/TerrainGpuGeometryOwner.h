#pragma once

#include "core/container/container_types.h"
#include "engine/renderer/world/terrain/D3D12TerrainGpuState.h"
#include "engine/renderer/world/terrain/TerrainTileMeshBuilder.h"

namespace engine::d3d12 { class D3D12Device; }

namespace engine::render::detail {

class TerrainGpuMaterialOwner;

class TerrainGpuGeometryOwner final {
public:
    TerrainGpuGeometryOwner() = default;
    explicit TerrainGpuGeometryOwner(d3d12::D3D12Device& device) noexcept;

    void setDevice(d3d12::D3D12Device& device) noexcept;
    [[nodiscard]] bool upload(
        const container::Vector<StaticMeshVertex>& vertices,
        const container::Vector<uint32_t>& indices,
        TerrainGpuGeometry& output,
        container::String* error);
    [[nodiscard]] bool uploadChunk(
        TerrainTileMeshChunk& cpuChunk,
        const TerrainGpuMaterialOwner& materials,
        TerrainGpuChunk& output,
        container::String* error);

    void retire(TerrainGpuGeometry& geometry) noexcept;
    void retire(TerrainGpuChunk& chunk) noexcept;
    void retire(container::Vector<TerrainGpuWaterChunk>& chunks) noexcept;
    void retire(container::Vector<TerrainGpuRoadChunk>& chunks) noexcept;
    void retire(container::Vector<TerrainGpuBridgeChunk>& chunks) noexcept;
    void retire(container::Vector<TerrainGpuBibChunk>& chunks) noexcept;

private:
    d3d12::D3D12Device* m_device = nullptr;
};

} // namespace engine::render::detail
