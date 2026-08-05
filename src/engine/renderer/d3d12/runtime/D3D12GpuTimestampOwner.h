#pragma once

#include "core/container/container_types.h"
#include "engine/renderer/runtime/RendererStats.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace engine::d3d12 {

// Owns the asynchronous timestamp query ring independently of swap-chain and
// frame-submission policy. A frame slot is consumed only after its fence has
// completed, so collecting diagnostics never introduces an in-frame GPU wait.
class D3D12GpuTimestampOwner final {
public:
    static constexpr uint32_t kFrameSlotCount = 2u;

    [[nodiscard]] bool initialize(
        ID3D12Device* device, ID3D12CommandQueue* commandQueue);
    void shutdown() noexcept;

    // Called after the caller has established that this frame slot's previous
    // submission fence is complete. It publishes the old result and prepares
    // the slot for recording a new frame.
    void beginFrameSlot(uint32_t frameIndex) noexcept;

    [[nodiscard]] bool begin(
        ID3D12GraphicsCommandList* commandList,
        bool frameOpen,
        uint32_t frameIndex,
        render::GpuTimestampRange range) noexcept;
    [[nodiscard]] bool end(
        ID3D12GraphicsCommandList* commandList,
        bool frameOpen,
        uint32_t frameIndex,
        render::GpuTimestampRange range) noexcept;
    void resolve(
        ID3D12GraphicsCommandList* commandList,
        uint32_t frameIndex) noexcept;
    void sealSubmitted(uint32_t frameIndex, uint64_t frameOrdinal) noexcept;

    [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
    [[nodiscard]] const render::GpuTimestampRenderStats& stats() const noexcept {
        return m_lastStats;
    }

private:
    static constexpr uint32_t kQueriesPerRange = 2u;
    static constexpr uint32_t kQueriesPerFrame =
        render::kGpuTimestampRangeCount * kQueriesPerRange;
    static constexpr uint32_t kQueryCount =
        kFrameSlotCount * kQueriesPerFrame;
    static_assert(render::kGpuTimestampRangeCount <= 64u);

    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_queryHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_readback;
    uint64_t* m_mappedReadback = nullptr;
    uint64_t m_frequencyHz = 0;
    container::Array<uint64_t, kFrameSlotCount> m_submittedMasks{};
    container::Array<uint64_t, kFrameSlotCount> m_sourceFrameOrdinals{};
    uint64_t m_begunMask = 0;
    uint64_t m_completedMask = 0;
    render::GpuTimestampRenderStats m_lastStats;
    bool m_enabled = false;
};

} // namespace engine::d3d12
