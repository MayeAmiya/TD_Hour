#pragma once

#include "core/container/container_types.h"
#include "D3D12PerformanceSettings.h"
#include "engine/renderer/runtime/RendererStats.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace engine::d3d12 {

struct GpuRetirementIdentity final {
    uint64_t identityHash = 0;
    uint64_t generation = 0;
    uint64_t revision = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return identityHash != 0u;
    }
};

struct SrvDescriptorRetirementMetadata final {
    uint64_t lastUsedFrame = 0;
    uint64_t lastUsedFence = 0;
    GpuRetirementIdentity identity;
};

// Owns the shader-visible SRV heap, slot allocation state and submission-use
// tracking. Fence retirement records remain a separate queue owned by the
// submission layer; a slot returns here only after that queue completes.
class D3D12SrvDescriptorHeap final {
public:
    static constexpr uint32_t kCapacity =
        performance_limits::kSrvDescriptorCapacity;

    [[nodiscard]] bool initialize(ID3D12Device* device);
    void shutdown() noexcept;

    [[nodiscard]] uint32_t allocate();
    [[nodiscard]] bool canRetire(uint32_t index) const noexcept;
    [[nodiscard]] SrvDescriptorRetirementMetadata retirementMetadata(
        uint32_t index) const noexcept;
    void markRetiring(uint32_t index) noexcept;
    void releaseImmediately(uint32_t index) noexcept;
    void setRetirementIdentity(
        uint32_t index, GpuRetirementIdentity identity) noexcept;

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle(
        uint32_t index) const noexcept;
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle(
        uint32_t index, bool trackUse, uint64_t frameOrdinal) const noexcept;
    void bind(ID3D12GraphicsCommandList* commandList) const noexcept;

    void commitTouches(uint64_t fenceValue) noexcept;
    void discardTouches() noexcept;

    [[nodiscard]] uint32_t capacity() const noexcept { return m_count; }
    [[nodiscard]] ID3D12DescriptorHeap* heap() const noexcept {
        return m_heap.Get();
    }
    [[nodiscard]] uint64_t lastUsedFrame(uint32_t index) const noexcept;
    [[nodiscard]] uint64_t lastUsedFence(uint32_t index) const noexcept;
    [[nodiscard]] render::SrvDescriptorRenderStats stats() const noexcept;
    void appendLiveRetirementStats(
        render::GpuRetirementRenderStats& stats) const noexcept;

private:
    void reportPressure();

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    uint32_t m_descriptorSize = 0;
    uint32_t m_count = 0;
    container::Array<bool, kCapacity> m_allocated{};
    container::Array<bool, kCapacity> m_retiring{};
    mutable container::Array<uint64_t, kCapacity> m_lastUsedFrames{};
    mutable container::Array<uint64_t, kCapacity> m_lastUsedFences{};
    mutable container::Array<bool, kCapacity> m_touchedThisSubmission{};
    container::Array<GpuRetirementIdentity, kCapacity> m_identities{};
    mutable container::Array<uint32_t, kCapacity> m_touchedIndices{};
    mutable uint32_t m_touchedCount = 0;
    container::Vector<uint32_t> m_freeIndices;
    uint32_t m_allocatedCount = 0;
    uint32_t m_retiringCount = 0;
    uint32_t m_lifetimeHighWater = 0;
    uint32_t m_lastWarningHighWater = 0;
    uint64_t m_allocationFailures = 0;
    uint64_t m_pressureWarnings = 0;
};

} // namespace engine::d3d12
