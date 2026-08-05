#pragma once

#include "core/container/container_types.h"
#include "engine/renderer/d3d12/runtime/D3D12Device.h"
#include "engine/renderer/runtime/RendererStats.h"
#include "engine/renderer/world/pipeline/WorldRendererGpuLayout.h"

#include <cstddef>

namespace engine::render {
struct StaticMeshDrawPacket;
}

namespace engine::render::world_renderer_detail {

struct alignas(16) SkinPaletteGpuJoint final {
    float previous[16];
    float current[16];
};
static_assert(sizeof(SkinPaletteGpuJoint) == 128);

struct SkinPaletteUploadEntry final {
    const StaticMeshDrawPacket* packet = nullptr;
    d3d12::ConstantBufferAllocation allocation;
};

struct StaticMeshRecordedDraw final {
    ID3D12PipelineState* pipeline = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS worldConstants = 0;
    D3D12_GPU_VIRTUAL_ADDRESS skinConstants = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE texture{};
    D3D12_GPU_DESCRIPTOR_HANDLE detailTexture{};
    D3D12_GPU_DESCRIPTOR_HANDLE terrainCloudTexture{};
    D3D12_GPU_DESCRIPTOR_HANDLE terrainMacroTexture{};
    D3D12_VERTEX_BUFFER_VIEW vertexView{};
    D3D12_VERTEX_BUFFER_VIEW instanceView{};
    D3D12_INDEX_BUFFER_VIEW indexView{};
    uint32_t indexCount = 0;
    uint32_t instanceCount = 0;
    uint32_t firstIndex = 0;
    int32_t baseVertex = 0;
};

class WorldRendererUploadOwner final {
public:
    class PaletteSession final {
    public:
        [[nodiscard]] d3d12::ConstantBufferAllocation resolve(
            const StaticMeshDrawPacket& draw);

    private:
        friend class WorldRendererUploadOwner;

        PaletteSession(
            d3d12::D3D12Device& device,
            container::Vector<SkinPaletteUploadEntry>& entries,
            container::Vector<SkinPaletteGpuJoint>& pairScratch,
            StaticMeshRenderStats& stats) noexcept;

        d3d12::D3D12Device& m_device;
        container::Vector<SkinPaletteUploadEntry>& m_entries;
        container::Vector<SkinPaletteGpuJoint>& m_pairScratch;
        StaticMeshRenderStats& m_stats;
        d3d12::ConstantBufferAllocation m_identityAllocation;
    };

    [[nodiscard]] PaletteSession beginPaletteUploads(
        d3d12::D3D12Device& device, size_t packetCapacity,
        StaticMeshRenderStats& stats);

    void resetPaletteEntries() noexcept;
    void projectCapacities(StaticMeshRenderStats& stats) const noexcept;
    void noteInstanceReserve(
        size_t capacityBefore, StaticMeshRenderStats& stats) noexcept;
    void clearFramePointers() noexcept;

    [[nodiscard]] container::Vector<StaticMeshGpuInstance>& instances() noexcept {
        return m_instances;
    }
    [[nodiscard]] container::Vector<StaticMeshRecordedDraw>& recordedDraws() noexcept {
        return m_recordedDraws;
    }

private:
    container::Vector<StaticMeshGpuInstance> m_instances;
    container::Vector<SkinPaletteUploadEntry> m_skinPaletteEntries;
    container::Vector<SkinPaletteGpuJoint> m_skinPalettePairs;
    container::Vector<StaticMeshRecordedDraw> m_recordedDraws;
    uint32_t m_instanceCapacityHighWater = 0;
    uint32_t m_skinPaletteCapacityHighWater = 0;
};

} // namespace engine::render::world_renderer_detail
