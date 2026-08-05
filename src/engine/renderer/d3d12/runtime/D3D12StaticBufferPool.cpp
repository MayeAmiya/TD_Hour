#include "D3D12StaticBufferPool.h"

#include "debug/debug.h"

#include <algorithm>
#include <limits>

namespace engine::d3d12 {
namespace {

[[nodiscard]] D3D12_RESOURCE_DESC makeBufferDesc(uint64_t size) noexcept {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

[[nodiscard]] D3D12_RESOURCE_BARRIER transition(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

} // namespace

bool D3D12StaticBufferPool::initialize(ID3D12Device* device) noexcept {
    shutdown();
    m_device = device;
    return m_device != nullptr;
}

void D3D12StaticBufferPool::shutdown() noexcept {
    m_pendingUploads.clear();
    m_allocations.clear();
    m_pages.clear();
    m_nextPageToken = 0u;
    m_nextAllocationToken = 0u;
    m_device = nullptr;
}

StaticBufferAllocation D3D12StaticBufferPool::allocate(uint64_t byteSize) {
    if (!m_device || byteSize == 0u ||
        byteSize > std::numeric_limits<uint64_t>::max() -
            (kSliceAlignment - 1u)) {
        return {};
    }
    const uint64_t reservedSize =
        (byteSize + kSliceAlignment - 1u) & ~(kSliceAlignment - 1u);
    const bool oversized = reservedSize > kPageBytes;
    size_t bestPage = std::numeric_limits<size_t>::max();
    size_t bestRange = std::numeric_limits<size_t>::max();
    uint64_t bestWaste = std::numeric_limits<uint64_t>::max();
    const auto selectBestFit = [&]() {
        for (size_t pageIndex = 0; pageIndex < m_pages.size(); ++pageIndex) {
            const Page& page = m_pages[pageIndex];
            if (page.oversized != oversized) continue;
            for (size_t rangeIndex = 0;
                 rangeIndex < page.freeRanges.size(); ++rangeIndex) {
                const FreeRange& range = page.freeRanges[rangeIndex];
                if (range.size < reservedSize) continue;
                const uint64_t waste = range.size - reservedSize;
                if (waste >= bestWaste) continue;
                bestPage = pageIndex;
                bestRange = rangeIndex;
                bestWaste = waste;
            }
        }
    };
    selectBestFit();
    if (bestPage == std::numeric_limits<size_t>::max()) {
        if (!createPage(reservedSize)) return {};
        selectBestFit();
        if (bestPage == std::numeric_limits<size_t>::max()) return {};
    }

    Page& page = m_pages[bestPage];
    const uint64_t offset = page.freeRanges[bestRange].offset;
    uint64_t token = ++m_nextAllocationToken;
    if (token == 0u) token = ++m_nextAllocationToken;
    try {
        m_allocations.emplace(token, Record{
            .pageToken = page.token,
            .offset = offset,
            .size = byteSize,
            .reservedSize = reservedSize,
        });
    } catch (...) {
        TD_LOG_ERROR("[D3D12StaticBufferPool] Slice registration failed");
        return {};
    }
    FreeRange& selected = page.freeRanges[bestRange];
    selected.offset += reservedSize;
    selected.size -= reservedSize;
    if (selected.size == 0u) {
        page.freeRanges.erase(page.freeRanges.begin() + bestRange);
    }
    ++page.liveAllocationCount;
    StaticBufferAllocation allocation;
    allocation.token = token;
    allocation.resource = page.resource.Get();
    allocation.resourceOffset = offset;
    allocation.size = byteSize;
    allocation.gpuAddress = page.resource->GetGPUVirtualAddress() + offset;
    return allocation;
}

bool D3D12StaticBufferPool::createPage(uint64_t minimumCapacity) {
    constexpr uint64_t kCommittedAlignment = 64u * 1024u;
    if (!m_device || minimumCapacity == 0u ||
        minimumCapacity > std::numeric_limits<uint64_t>::max() -
            (kCommittedAlignment - 1u)) {
        return false;
    }
    const bool oversized = minimumCapacity > kPageBytes;
    const uint64_t capacity = oversized
        ? (minimumCapacity + kCommittedAlignment - 1u) &
            ~(kCommittedAlignment - 1u)
        : kPageBytes;
    Page page;
    page.token = ++m_nextPageToken;
    if (page.token == 0u) page.token = ++m_nextPageToken;
    page.capacity = capacity;
    page.oversized = oversized;
    page.freeRanges.push_back({.offset = 0u, .size = capacity});
    const D3D12_RESOURCE_DESC description = makeBufferDesc(capacity);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const HRESULT result = m_device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&page.resource));
    if (FAILED(result)) {
        TD_LOG_ERROR(
            "[D3D12StaticBufferPool] DEFAULT page allocation failed ({} bytes): 0x{:08X}",
            capacity, result);
        return false;
    }
    try {
        m_pages.push_back(std::move(page));
    } catch (...) {
        TD_LOG_ERROR("[D3D12StaticBufferPool] Page registration failed");
        return false;
    }
    TD_LOG_INFO(
        "[D3D12StaticBufferPool] Page added: token={} size={}KB oversized={}",
        m_pages.back().token, capacity / 1024u, oversized);
    return true;
}

bool D3D12StaticBufferPool::queueUpload(
    const StaticBufferAllocation& allocation,
    const FrameUploadAllocation& upload) {
    const auto found = m_allocations.find(allocation.token);
    if (found == m_allocations.end() || !upload) return false;
    try {
        m_pendingUploads.push_back({
            .pageToken = found->second.pageToken,
            .destination = allocation.resource,
            .destinationOffset = allocation.resourceOffset,
            .source = upload.resource,
            .sourceOffset = upload.resourceOffset,
            .size = allocation.size,
        });
    } catch (...) {
        TD_LOG_ERROR("[D3D12StaticBufferPool] Pending copy registration failed");
        return false;
    }
    return true;
}

void D3D12StaticBufferPool::flush(ID3D12GraphicsCommandList* commandList) {
    if (!commandList || m_pendingUploads.empty()) return;
    for (Page& page : m_pages) {
        const bool touched = std::any_of(
            m_pendingUploads.begin(), m_pendingUploads.end(),
            [&page](const PendingUpload& upload) {
                return upload.pageToken == page.token;
            });
        if (!touched || page.state == D3D12_RESOURCE_STATE_COPY_DEST) continue;
        const D3D12_RESOURCE_BARRIER barrier = transition(
            page.resource.Get(), page.state, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->ResourceBarrier(1u, &barrier);
        page.state = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    for (const PendingUpload& upload : m_pendingUploads) {
        commandList->CopyBufferRegion(
            upload.destination, upload.destinationOffset,
            upload.source, upload.sourceOffset, upload.size);
    }
    for (Page& page : m_pages) {
        const bool touched = std::any_of(
            m_pendingUploads.begin(), m_pendingUploads.end(),
            [&page](const PendingUpload& upload) {
                return upload.pageToken == page.token;
            });
        if (!touched) continue;
        const D3D12_RESOURCE_BARRIER barrier = transition(
            page.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_GENERIC_READ);
        commandList->ResourceBarrier(1u, &barrier);
        page.state = D3D12_RESOURCE_STATE_GENERIC_READ;
    }
    m_pendingUploads.clear();
}

bool D3D12StaticBufferPool::beginRetirement(
    uint64_t allocationToken,
    uint64_t requestFrame,
    uint64_t lastUsedFrame,
    uint64_t lastUsedFence,
    GpuRetirementIdentity identity,
    StaticBufferRetirement& retirement) noexcept {
    const auto found = m_allocations.find(allocationToken);
    if (found == m_allocations.end()) return false;
    const Record record = found->second;
    retirement = {
        .allocationToken = allocationToken,
        .pageToken = record.pageToken,
        .offset = record.offset,
        .size = record.size,
        .reservedSize = record.reservedSize,
        .requestFrame = requestFrame,
        .lastUsedFrame = lastUsedFrame,
        .lastUsedFence = lastUsedFence,
        .identity = identity,
    };
    return true;
}

void D3D12StaticBufferPool::commitRetirement(
    uint64_t allocationToken) noexcept {
    m_allocations.erase(allocationToken);
}

bool D3D12StaticBufferPool::release(
    const StaticBufferRetirement& retirement) noexcept {
    const auto pageIterator = std::find_if(
        m_pages.begin(), m_pages.end(),
        [&retirement](const Page& page) {
            return page.token == retirement.pageToken;
        });
    if (pageIterator == m_pages.end() || retirement.reservedSize == 0u) {
        return false;
    }
    Page& page = *pageIterator;
    try {
        const auto insertionPoint = std::lower_bound(
            page.freeRanges.begin(), page.freeRanges.end(), retirement.offset,
            [](const FreeRange& range, uint64_t value) {
                return range.offset < value;
            });
        page.freeRanges.insert(insertionPoint, {
            .offset = retirement.offset,
            .size = retirement.reservedSize,
        });
    } catch (...) {
        TD_LOG_ERROR("[D3D12StaticBufferPool] Free-range registration failed");
        return false;
    }
    if (page.liveAllocationCount != 0u) --page.liveAllocationCount;
    for (size_t index = 1; index < page.freeRanges.size();) {
        FreeRange& previous = page.freeRanges[index - 1u];
        const FreeRange& current = page.freeRanges[index];
        const uint64_t previousEnd = previous.offset + previous.size;
        if (previousEnd < current.offset) {
            ++index;
            continue;
        }
        previous.size = std::max(previousEnd, current.offset + current.size) -
            previous.offset;
        page.freeRanges.erase(page.freeRanges.begin() + index);
    }
    if (page.liveAllocationCount == 0u) {
        const bool keepWarmPage = !page.oversized && m_pages.size() <= 1u;
        if (!keepWarmPage) m_pages.erase(pageIterator);
    }
    return true;
}

StaticBufferRenderStats D3D12StaticBufferPool::stats(
    uint32_t retiringCount, uint64_t retiringBytes) const noexcept {
    StaticBufferRenderStats result;
    result.pageCount = static_cast<uint32_t>(std::min<size_t>(
        m_pages.size(), std::numeric_limits<uint32_t>::max()));
    for (const Page& page : m_pages) {
        result.pageCapacityBytes += page.capacity;
        if (page.oversized) ++result.oversizedPageCount;
    }
    result.activeSliceCount = static_cast<uint32_t>(std::min<size_t>(
        m_allocations.size(), std::numeric_limits<uint32_t>::max()));
    for (const auto& allocation : m_allocations) {
        result.liveLogicalBytes += allocation.second.size;
    }
    result.retiringSliceCount = retiringCount;
    result.liveLogicalBytes += retiringBytes;
    const uint64_t liveCount =
        static_cast<uint64_t>(m_allocations.size()) + retiringCount;
    result.liveSliceCount = static_cast<uint32_t>(std::min<uint64_t>(
        liveCount, std::numeric_limits<uint32_t>::max()));
    result.pendingCopyCount = static_cast<uint32_t>(std::min<size_t>(
        m_pendingUploads.size(), std::numeric_limits<uint32_t>::max()));
    for (const PendingUpload& upload : m_pendingUploads) {
        result.pendingCopyBytes += upload.size;
    }
    return result;
}

} // namespace engine::d3d12
