#pragma once

#include "core/container/container_types.h"
#include "D3D12FrameUploadArena.h"
#include "D3D12GpuRetirementQueue.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <utility>

namespace engine::d3d12 {

struct StaticBufferAllocation final {
    uint64_t token = 0;
    ID3D12Resource* resource = nullptr;
    uint64_t resourceOffset = 0;
    uint64_t size = 0;
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = 0;

    StaticBufferAllocation() = default;
    StaticBufferAllocation(const StaticBufferAllocation&) = delete;
    StaticBufferAllocation& operator=(const StaticBufferAllocation&) = delete;
    StaticBufferAllocation(StaticBufferAllocation&& other) noexcept
        : token(std::exchange(other.token, 0u)),
          resource(std::exchange(other.resource, nullptr)),
          resourceOffset(std::exchange(other.resourceOffset, 0u)),
          size(std::exchange(other.size, 0u)),
          gpuAddress(std::exchange(other.gpuAddress, 0u)) {}
    StaticBufferAllocation& operator=(StaticBufferAllocation&& other) noexcept {
        if (this == &other) return *this;
        token = std::exchange(other.token, 0u);
        resource = std::exchange(other.resource, nullptr);
        resourceOffset = std::exchange(other.resourceOffset, 0u);
        size = std::exchange(other.size, 0u);
        gpuAddress = std::exchange(other.gpuAddress, 0u);
        return *this;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return token != 0u && resource != nullptr && size != 0u &&
            gpuAddress != 0u;
    }
};

struct StaticBufferRenderStats final {
    uint32_t pageCount = 0;
    uint32_t oversizedPageCount = 0;
    uint32_t activeSliceCount = 0;
    uint32_t retiringSliceCount = 0;
    uint32_t liveSliceCount = 0;
    uint32_t pendingCopyCount = 0;
    uint64_t pageCapacityBytes = 0;
    uint64_t liveLogicalBytes = 0;
    uint64_t pendingCopyBytes = 0;
};

class D3D12StaticBufferPool final {
public:
    [[nodiscard]] bool initialize(ID3D12Device* device) noexcept;
    void shutdown() noexcept;

    [[nodiscard]] StaticBufferAllocation allocate(uint64_t byteSize);
    [[nodiscard]] bool queueUpload(
        const StaticBufferAllocation& allocation,
        const FrameUploadAllocation& upload);
    void flush(ID3D12GraphicsCommandList* commandList);
    [[nodiscard]] bool beginRetirement(
        uint64_t allocationToken,
        uint64_t requestFrame,
        uint64_t lastUsedFrame,
        uint64_t lastUsedFence,
        GpuRetirementIdentity identity,
        StaticBufferRetirement& retirement) noexcept;
    void commitRetirement(uint64_t allocationToken) noexcept;
    [[nodiscard]] bool release(
        const StaticBufferRetirement& retirement) noexcept;
    [[nodiscard]] StaticBufferRenderStats stats(
        uint32_t retiringCount, uint64_t retiringBytes) const noexcept;

private:
    static constexpr uint64_t kPageBytes = 32u * 1024u * 1024u;
    static constexpr uint64_t kSliceAlignment = 16u;

    struct FreeRange final { uint64_t offset = 0; uint64_t size = 0; };
    struct Page final {
        uint64_t token = 0;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        uint64_t capacity = 0;
        uint64_t liveAllocationCount = 0;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        bool oversized = false;
        container::Vector<FreeRange> freeRanges;
    };
    struct Record final {
        uint64_t pageToken = 0;
        uint64_t offset = 0;
        uint64_t size = 0;
        uint64_t reservedSize = 0;
    };
    struct PendingUpload final {
        uint64_t pageToken = 0;
        ID3D12Resource* destination = nullptr;
        uint64_t destinationOffset = 0;
        ID3D12Resource* source = nullptr;
        uint64_t sourceOffset = 0;
        uint64_t size = 0;
    };

    [[nodiscard]] bool createPage(uint64_t minimumCapacity);

    ID3D12Device* m_device = nullptr;
    container::Vector<Page> m_pages;
    container::TreeMap<uint64_t, Record> m_allocations;
    container::Vector<PendingUpload> m_pendingUploads;
    uint64_t m_nextPageToken = 0;
    uint64_t m_nextAllocationToken = 0;
};

} // namespace engine::d3d12
