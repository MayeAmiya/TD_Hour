#include "D3D12FrameUploadArena.h"

#include "debug/debug.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>

namespace engine::d3d12 {
namespace {

class ScopedNanosecondAccumulator final {
public:
    explicit ScopedNanosecondAccumulator(uint64_t& destination) noexcept
        : m_destination(destination), m_started(Clock::now()) {}
    ~ScopedNanosecondAccumulator() {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - m_started).count();
        if (elapsed > 0) m_destination += static_cast<uint64_t>(elapsed);
    }
private:
    using Clock = std::chrono::steady_clock;
    uint64_t& m_destination;
    Clock::time_point m_started;
};

[[nodiscard]] D3D12_RESOURCE_DESC makeBufferDesc(uint64_t size) noexcept {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return description;
}

[[nodiscard]] D3D12_HEAP_PROPERTIES makeUploadHeap() noexcept {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    return properties;
}

} // namespace

bool D3D12FrameUploadArena::initialize(ID3D12Device* device) {
    shutdown();
    if (!device) return false;
    m_device = device;
    const uint64_t totalBytes =
        static_cast<uint64_t>(kPrimaryBytes) * kFrameSlotCount;
    const D3D12_RESOURCE_DESC description = makeBufferDesc(totalBytes);
    const D3D12_HEAP_PROPERTIES uploadHeap = makeUploadHeap();
    HRESULT result = device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_primaryBuffer));
    if (FAILED(result)) {
        TD_LOG_ERROR(
            "[D3D12FrameUploadArena] Primary buffer allocation failed: 0x{:08X}",
            result);
        shutdown();
        return false;
    }
    const D3D12_RANGE noCpuReads{0, 0};
    result = m_primaryBuffer->Map(
        0, &noCpuReads, reinterpret_cast<void**>(&m_mappedPrimary));
    if (FAILED(result) || !m_mappedPrimary) {
        TD_LOG_ERROR(
            "[D3D12FrameUploadArena] Primary buffer map failed: 0x{:08X}",
            result);
        shutdown();
        return false;
    }
    m_primaryGpuBase = m_primaryBuffer->GetGPUVirtualAddress();
    return true;
}

void D3D12FrameUploadArena::shutdown() noexcept {
    for (auto& framePages : m_spillPages) {
        for (SpillPage& page : framePages) {
            if (page.mapped && page.resource) page.resource->Unmap(0, nullptr);
            page = {};
        }
        framePages.clear();
    }
    if (m_mappedPrimary && m_primaryBuffer) {
        m_primaryBuffer->Unmap(0, nullptr);
    }
    m_mappedPrimary = nullptr;
    m_primaryBuffer.Reset();
    m_primaryGpuBase = 0;
    m_primaryOffset = 0;
    m_currentStats = {};
    m_lastStats = {};
    m_primaryLifetimeHighWater = 0;
    m_spillLifetimeHighWater = 0;
    m_device = nullptr;
}

void D3D12FrameUploadArena::beginFrameSlot(
    uint32_t frameIndex, uint64_t frameOrdinal) noexcept {
    if (frameIndex >= kFrameSlotCount) return;
    m_primaryOffset = 0;
    uint64_t spillCapacity = 0;
    for (SpillPage& page : m_spillPages[frameIndex]) {
        page.offset = 0;
        spillCapacity += page.capacity;
    }
    m_currentStats = {
        .frameOrdinal = frameOrdinal,
        .primaryCapacityBytes = kPrimaryBytes,
        .spillCapacityBytes = static_cast<uint32_t>(std::min<uint64_t>(
            spillCapacity, std::numeric_limits<uint32_t>::max())),
        .spillPageCount = static_cast<uint32_t>(m_spillPages[frameIndex].size()),
        .lifetimePrimaryHighWaterBytes = m_primaryLifetimeHighWater,
        .lifetimeSpillHighWaterBytes = m_spillLifetimeHighWater,
    };
}

FrameUploadAllocation D3D12FrameUploadArena::allocate(
    uint32_t frameIndex, const void* data, uint32_t size,
    uint32_t alignment) {
    if (!data) {
        m_currentStats.requestedBytes += size;
        m_currentStats.rejectedBytes += size;
        ++m_currentStats.failedAllocationCount;
        return {};
    }
    FrameUploadAllocation allocation =
        allocateUninitialized(frameIndex, size, alignment);
    if (allocation) {
        ScopedNanosecondAccumulator copyTimer(
            m_currentStats.copyCpuNanoseconds);
        std::memcpy(allocation.cpu, data, size);
    }
    return allocation;
}

FrameUploadAllocation D3D12FrameUploadArena::allocateUninitialized(
    uint32_t frameIndex, uint32_t size, uint32_t alignment) {
    ScopedNanosecondAccumulator allocationTimer(
        m_currentStats.allocationCpuNanoseconds);
    m_currentStats.requestedBytes += size;
    if (frameIndex >= kFrameSlotCount || size == 0 || !m_mappedPrimary) {
        m_currentStats.rejectedBytes += size;
        ++m_currentStats.failedAllocationCount;
        return {};
    }
    if (alignment == 0 || (alignment & (alignment - 1u)) != 0u) {
        TD_LOG_ERROR(
            "[D3D12FrameUploadArena] Invalid allocation alignment {}", alignment);
        m_currentStats.rejectedBytes += size;
        ++m_currentStats.failedAllocationCount;
        return {};
    }

    const uint64_t alignedOffset64 =
        (static_cast<uint64_t>(m_primaryOffset) + alignment - 1u) &
        ~static_cast<uint64_t>(alignment - 1u);
    if (alignedOffset64 > kPrimaryBytes ||
        size > kPrimaryBytes - static_cast<uint32_t>(alignedOffset64)) {
        FrameUploadAllocation spill = allocateSpill(
            frameIndex, size, alignment);
        if (!spill) {
            m_currentStats.rejectedBytes += size;
            ++m_currentStats.failedAllocationCount;
            TD_LOG_ERROR(
                "[D3D12FrameUploadArena] Spill pool exhausted (requested {} bytes, alignment {}, limit={}MB)",
                size, alignment, kMaximumSpillBytes / (1024u * 1024u));
        } else {
            m_currentStats.uploadedBytes += size;
        }
        return spill;
    }

    const uint32_t alignedOffset = static_cast<uint32_t>(alignedOffset64);
    const uint64_t frameOffset =
        static_cast<uint64_t>(frameIndex) * kPrimaryBytes;
    const uint64_t allocationOffset = frameOffset + alignedOffset;
    FrameUploadAllocation allocation{
        .cpu = m_mappedPrimary + allocationOffset,
        .gpuAddress = m_primaryGpuBase + allocationOffset,
        .resource = m_primaryBuffer.Get(),
        .resourceOffset = allocationOffset,
        .size = size,
    };
    m_primaryOffset = alignedOffset + size;
    ++m_currentStats.allocationCount;
    m_currentStats.primaryBytesUsed = m_primaryOffset;
    m_primaryLifetimeHighWater = std::max(
        m_primaryLifetimeHighWater, m_primaryOffset);
    m_currentStats.lifetimePrimaryHighWaterBytes = m_primaryLifetimeHighWater;
    m_currentStats.uploadedBytes += size;
    return allocation;
}

FrameUploadAllocation D3D12FrameUploadArena::allocateSpill(
    uint32_t frameIndex, uint32_t size, uint32_t alignment) {
    auto tryAllocate = [size, alignment](SpillPage& page) {
        FrameUploadAllocation allocation;
        const uint64_t alignedOffset64 =
            (static_cast<uint64_t>(page.offset) + alignment - 1u) &
            ~static_cast<uint64_t>(alignment - 1u);
        if (alignedOffset64 > page.capacity ||
            size > page.capacity - static_cast<uint32_t>(alignedOffset64)) {
            return allocation;
        }
        const uint32_t alignedOffset = static_cast<uint32_t>(alignedOffset64);
        allocation = {
            .cpu = page.mapped + alignedOffset,
            .gpuAddress = page.gpuBase + alignedOffset,
            .resource = page.resource.Get(),
            .resourceOffset = alignedOffset,
            .size = size,
        };
        page.offset = alignedOffset + size;
        return allocation;
    };

    auto& pages = m_spillPages[frameIndex];
    for (SpillPage& page : pages) {
        const uint32_t previousOffset = page.offset;
        FrameUploadAllocation allocation = tryAllocate(page);
        if (!allocation) continue;
        m_currentStats.spillBytesUsed += page.offset - previousOffset;
        ++m_currentStats.allocationCount;
        ++m_currentStats.spillAllocationCount;
        ++m_currentStats.spillPageReuseAllocationCount;
        m_spillLifetimeHighWater = std::max(
            m_spillLifetimeHighWater, m_currentStats.spillBytesUsed);
        m_currentStats.lifetimeSpillHighWaterBytes = m_spillLifetimeHighWater;
        return allocation;
    }

    const uint64_t minimumCapacity =
        static_cast<uint64_t>(size) + alignment - 1u;
    if (minimumCapacity > std::numeric_limits<uint32_t>::max() ||
        !createSpillPage(frameIndex, static_cast<uint32_t>(minimumCapacity))) {
        return {};
    }
    SpillPage& page = pages.back();
    const uint32_t previousOffset = page.offset;
    FrameUploadAllocation allocation = tryAllocate(page);
    if (!allocation) return {};
    m_currentStats.spillBytesUsed += page.offset - previousOffset;
    ++m_currentStats.allocationCount;
    ++m_currentStats.spillAllocationCount;
    m_spillLifetimeHighWater = std::max(
        m_spillLifetimeHighWater, m_currentStats.spillBytesUsed);
    m_currentStats.lifetimeSpillHighWaterBytes = m_spillLifetimeHighWater;
    return allocation;
}

bool D3D12FrameUploadArena::createSpillPage(
    uint32_t frameIndex, uint32_t minimumCapacity) {
    constexpr uint32_t kPageAlignment = 64u * 1024u;
    const uint64_t alignedCapacity =
        (static_cast<uint64_t>(minimumCapacity) + kPageAlignment - 1u) &
        ~static_cast<uint64_t>(kPageAlignment - 1u);
    auto& pages = m_spillPages[frameIndex];
    uint64_t retainedCapacity = 0;
    for (const SpillPage& page : pages) retainedCapacity += page.capacity;
    const uint64_t geometricCapacity = pages.empty()
        ? kSpillPageBytes
        : std::min<uint64_t>(
            static_cast<uint64_t>(pages.back().capacity) * 2u,
            kMaximumSpillBytes);
    uint64_t requestedCapacity = std::max(alignedCapacity, geometricCapacity);
    const uint64_t remainingCapacity = kMaximumSpillBytes - retainedCapacity;
    if (requestedCapacity > remainingCapacity) {
        requestedCapacity = remainingCapacity;
    }
    if (requestedCapacity < alignedCapacity || requestedCapacity == 0u) {
        return false;
    }

    SpillPage page;
    page.capacity = static_cast<uint32_t>(requestedCapacity);
    const D3D12_RESOURCE_DESC description = makeBufferDesc(page.capacity);
    const D3D12_HEAP_PROPERTIES uploadHeap = makeUploadHeap();
    HRESULT result = m_device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&page.resource));
    if (FAILED(result)) {
        TD_LOG_ERROR(
            "[D3D12FrameUploadArena] Spill page allocation failed: 0x{:08X}",
            result);
        return false;
    }
    const D3D12_RANGE noCpuReads{0, 0};
    result = page.resource->Map(
        0, &noCpuReads, reinterpret_cast<void**>(&page.mapped));
    if (FAILED(result) || !page.mapped) {
        TD_LOG_ERROR(
            "[D3D12FrameUploadArena] Spill page map failed: 0x{:08X}", result);
        return false;
    }
    page.gpuBase = page.resource->GetGPUVirtualAddress();
    try {
        pages.push_back(std::move(page));
    } catch (...) {
        if (page.mapped && page.resource) page.resource->Unmap(0, nullptr);
        TD_LOG_ERROR("[D3D12FrameUploadArena] Spill page registration failed");
        return false;
    }
    m_currentStats.spillCapacityBytes = static_cast<uint32_t>(
        retainedCapacity + requestedCapacity);
    m_currentStats.spillPageCount = static_cast<uint32_t>(pages.size());
    ++m_currentStats.spillPageGrowthCount;
    TD_LOG_INFO(
        "[D3D12FrameUploadArena] Spill page added: frame={} page={}KB retained={}KB",
        frameIndex, requestedCapacity / 1024u,
        (retainedCapacity + requestedCapacity) / 1024u);
    return true;
}

uint64_t D3D12FrameUploadArena::remainingBytes() const noexcept {
    const uint64_t primaryRemaining = kPrimaryBytes >
            m_currentStats.primaryBytesUsed
        ? kPrimaryBytes - m_currentStats.primaryBytesUsed : 0u;
    const uint64_t spillRemaining = kMaximumSpillBytes >
            m_currentStats.spillBytesUsed
        ? kMaximumSpillBytes - m_currentStats.spillBytesUsed : 0u;
    return primaryRemaining + spillRemaining;
}

} // namespace engine::d3d12
