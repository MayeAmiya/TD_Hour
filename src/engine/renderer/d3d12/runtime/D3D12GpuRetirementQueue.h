#pragma once

#include "core/container/container_types.h"
#include "D3D12SrvDescriptorHeap.h"
#include "engine/renderer/runtime/RendererStats.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace engine::d3d12 {

struct StaticBufferRetirement final {
    uint64_t allocationToken = 0;
    uint64_t pageToken = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t reservedSize = 0;
    uint64_t requestFrame = 0;
    uint64_t lastUsedFrame = 0;
    uint64_t lastUsedFence = 0;
    GpuRetirementIdentity identity;
};

struct RetirementFenceHistory final {
    container::Span<const uint64_t> frameOrdinals;
    container::Span<const uint64_t> fenceValues;
    uint64_t completedFence = 0;
    uint64_t currentFrameOrdinal = 0;
};

// Owns the pending -> fenced -> reclaimed lifecycle for GPU resources and
// typed pool tokens. It does not know how descriptor slots or static-buffer
// ranges are allocated; completed tokens are returned to those owners.
class D3D12GpuRetirementQueue final {
public:
    void clear() noexcept;
    void reject() noexcept { ++m_rejectedRetirements; }

    void retireResource(
        Microsoft::WRL::ComPtr<ID3D12Resource> resource,
        uint64_t byteSize,
        uint64_t requestFrame,
        uint64_t lastUsedFrame,
        uint64_t lastUsedFence,
        GpuRetirementIdentity identity,
        bool pending,
        uint64_t immediateFence);
    void retireDescriptor(
        uint32_t index,
        uint64_t requestFrame,
        SrvDescriptorRetirementMetadata metadata,
        bool pending,
        uint64_t immediateFence);
    void retireResourceAndDescriptor(
        Microsoft::WRL::ComPtr<ID3D12Resource> resource,
        uint64_t byteSize,
        uint32_t descriptorIndex,
        uint64_t requestFrame,
        SrvDescriptorRetirementMetadata metadata,
        bool pending,
        uint64_t immediateFence);
    void retireStaticBuffer(
        StaticBufferRetirement retirement,
        bool pending,
        uint64_t immediateFence);

    void sealPending(uint64_t fenceValue);
    void reclaim(
        uint64_t completedFence,
        container::Vector<uint32_t>& completedDescriptors,
        container::Vector<StaticBufferRetirement>& completedStaticBuffers);

    [[nodiscard]] render::GpuRetirementRenderStats stats(
        uint64_t frameOrdinal,
        const RetirementFenceHistory& history) const noexcept;
    [[nodiscard]] uint32_t retiringStaticBufferCount() const noexcept;
    [[nodiscard]] uint64_t retiringStaticBufferBytes() const noexcept;

private:
    struct ResourceRecord final {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        uint64_t byteSize = 0;
        uint64_t requestFrame = 0;
        uint64_t lastUsedFrame = 0;
        uint64_t lastUsedFence = 0;
        GpuRetirementIdentity identity;
        uint64_t retireFence = 0;
    };
    struct DescriptorRecord final {
        uint32_t index = UINT32_MAX;
        uint64_t requestFrame = 0;
        uint64_t lastUsedFrame = 0;
        uint64_t lastUsedFence = 0;
        GpuRetirementIdentity identity;
        uint64_t retireFence = 0;
    };
    struct StaticBufferRecord final {
        StaticBufferRetirement retirement;
        uint64_t retireFence = 0;
    };

    container::Vector<ResourceRecord> m_pendingResources;
    container::Vector<ResourceRecord> m_fencedResources;
    container::Vector<DescriptorRecord> m_pendingDescriptors;
    container::Vector<DescriptorRecord> m_fencedDescriptors;
    container::Vector<StaticBufferRecord> m_pendingStaticBuffers;
    container::Vector<StaticBufferRecord> m_fencedStaticBuffers;
    uint64_t m_reclaimedResources = 0;
    uint64_t m_reclaimedResourceBytes = 0;
    uint64_t m_reclaimedDescriptors = 0;
    uint64_t m_rejectedRetirements = 0;
};

} // namespace engine::d3d12
