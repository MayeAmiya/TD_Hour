#pragma once

#include "core/container/container_types.h"
#include "D3D12PerformanceSettings.h"
#include "engine/renderer/runtime/RendererStats.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace engine::d3d12 {

struct FrameUploadAllocation {
    void* cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = 0;
    ID3D12Resource* resource = nullptr;
    uint64_t resourceOffset = 0;
    uint32_t size = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return cpu != nullptr && gpuAddress != 0;
    }
};

using ConstantBufferAllocation = FrameUploadAllocation;

// Owns the persistently mapped per-frame upload ring and its fence-slot-local
// spill pages. The caller may reuse a slot only after its frame fence completes.
class D3D12FrameUploadArena final {
public:
    static constexpr uint32_t kFrameSlotCount = 2u;
    static constexpr uint32_t kPrimaryBytes =
        performance_limits::kFrameUploadPrimaryBytes;
    static constexpr uint32_t kSpillPageBytes =
        performance_limits::kFrameUploadSpillPageBytes;
    static constexpr uint32_t kMaximumSpillBytes =
        performance_limits::kMaximumFrameUploadSpillBytes;

    [[nodiscard]] bool initialize(ID3D12Device* device);
    void shutdown() noexcept;
    void beginFrameSlot(uint32_t frameIndex, uint64_t frameOrdinal) noexcept;
    void finishFrame() noexcept { m_lastStats = m_currentStats; }

    [[nodiscard]] FrameUploadAllocation allocate(
        uint32_t frameIndex, const void* data, uint32_t size,
        uint32_t alignment);
    [[nodiscard]] FrameUploadAllocation allocateUninitialized(
        uint32_t frameIndex, uint32_t size, uint32_t alignment);

    [[nodiscard]] const render::FrameUploadRenderStats& currentStats() const noexcept {
        return m_currentStats;
    }
    [[nodiscard]] const render::FrameUploadRenderStats& lastStats() const noexcept {
        return m_lastStats;
    }
    [[nodiscard]] uint64_t remainingBytes() const noexcept;

private:
    struct SpillPage final {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        uint8_t* mapped = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS gpuBase = 0;
        uint32_t capacity = 0;
        uint32_t offset = 0;
    };

    [[nodiscard]] FrameUploadAllocation allocateSpill(
        uint32_t frameIndex, uint32_t size, uint32_t alignment);
    [[nodiscard]] bool createSpillPage(
        uint32_t frameIndex, uint32_t minimumCapacity);

    ID3D12Device* m_device = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_primaryBuffer;
    uint8_t* m_mappedPrimary = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS m_primaryGpuBase = 0;
    uint32_t m_primaryOffset = 0;
    container::Array<container::Vector<SpillPage>, kFrameSlotCount> m_spillPages;
    render::FrameUploadRenderStats m_currentStats;
    render::FrameUploadRenderStats m_lastStats;
    uint32_t m_primaryLifetimeHighWater = 0;
    uint32_t m_spillLifetimeHighWater = 0;
};

} // namespace engine::d3d12
