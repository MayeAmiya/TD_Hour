#include "D3D12GpuTimestampOwner.h"

#include "debug/debug.h"

namespace engine::d3d12 {
namespace {

[[nodiscard]] D3D12_HEAP_PROPERTIES makeReadbackHeap() noexcept {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_READBACK;
    return properties;
}

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

} // namespace

bool D3D12GpuTimestampOwner::initialize(
    ID3D12Device* device, ID3D12CommandQueue* commandQueue) {
    if (m_enabled && m_queryHeap && m_readback && m_mappedReadback) {
        return true;
    }
    shutdown();

    UINT64 frequency = 0;
    HRESULT result = commandQueue
        ? commandQueue->GetTimestampFrequency(&frequency) : E_POINTER;
    if (!device || FAILED(result) || frequency == 0u) {
        TD_LOG_WARN(
            "[D3D12GpuTimestampOwner] GPU timestamps disabled: frequency query failed (0x{:08X})",
            static_cast<uint32_t>(result));
        return false;
    }

    D3D12_QUERY_HEAP_DESC queryDescription{};
    queryDescription.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queryDescription.Count = kQueryCount;
    result = device->CreateQueryHeap(
        &queryDescription, IID_PPV_ARGS(&m_queryHeap));
    if (FAILED(result)) {
        TD_LOG_WARN(
            "[D3D12GpuTimestampOwner] GPU timestamps disabled: query heap creation failed (0x{:08X})",
            static_cast<uint32_t>(result));
        shutdown();
        return false;
    }

    const uint64_t readbackBytes =
        static_cast<uint64_t>(kQueryCount) * sizeof(uint64_t);
    const D3D12_HEAP_PROPERTIES readbackHeap = makeReadbackHeap();
    const D3D12_RESOURCE_DESC readbackDescription =
        makeBufferDesc(readbackBytes);
    result = device->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDescription,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_readback));
    if (FAILED(result)) {
        TD_LOG_WARN(
            "[D3D12GpuTimestampOwner] GPU timestamps disabled: readback creation failed (0x{:08X})",
            static_cast<uint32_t>(result));
        shutdown();
        return false;
    }

    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(readbackBytes)};
    result = m_readback->Map(
        0, &readRange, reinterpret_cast<void**>(&m_mappedReadback));
    if (FAILED(result) || !m_mappedReadback) {
        TD_LOG_WARN(
            "[D3D12GpuTimestampOwner] GPU timestamps disabled: readback map failed (0x{:08X})",
            static_cast<uint32_t>(result));
        shutdown();
        return false;
    }

    m_frequencyHz = frequency;
    m_submittedMasks.fill(0u);
    m_sourceFrameOrdinals.fill(0u);
    m_begunMask = 0u;
    m_completedMask = 0u;
    m_lastStats = {
        .timestampFrequencyHz = frequency,
        .infrastructureReady = true,
    };
    m_enabled = true;
    TD_LOG_INFO(
        "[D3D12GpuTimestampOwner] asynchronous GPU timestamps enabled: {} Hz, {} ranges, {} frame slots",
        frequency, render::kGpuTimestampRangeCount, kFrameSlotCount);
    return true;
}

void D3D12GpuTimestampOwner::shutdown() noexcept {
    m_enabled = false;
    if (m_mappedReadback && m_readback) {
        const D3D12_RANGE writtenRange{0, 0};
        m_readback->Unmap(0, &writtenRange);
    }
    m_mappedReadback = nullptr;
    m_readback.Reset();
    m_queryHeap.Reset();
    m_frequencyHz = 0u;
    m_submittedMasks.fill(0u);
    m_sourceFrameOrdinals.fill(0u);
    m_begunMask = 0u;
    m_completedMask = 0u;
    m_lastStats = {};
}

void D3D12GpuTimestampOwner::beginFrameSlot(uint32_t frameIndex) noexcept {
    if (!m_enabled || !m_mappedReadback || m_frequencyHz == 0u ||
        frameIndex >= kFrameSlotCount) {
        return;
    }

    const uint64_t submittedMask = m_submittedMasks[frameIndex];
    const uint64_t sourceFrame = m_sourceFrameOrdinals[frameIndex];
    if (submittedMask != 0u && sourceFrame != 0u) {
        render::GpuTimestampRenderStats result{};
        result.sourceFrameOrdinal = sourceFrame;
        result.timestampFrequencyHz = m_frequencyHz;
        result.infrastructureReady = true;
        const uint32_t slotQueryBase = frameIndex * kQueriesPerFrame;
        for (uint32_t rangeIndex = 0;
             rangeIndex < render::kGpuTimestampRangeCount; ++rangeIndex) {
            const uint64_t rangeBit = uint64_t{1} << rangeIndex;
            if ((submittedMask & rangeBit) == 0u) continue;
            const uint32_t queryIndex =
                slotQueryBase + rangeIndex * kQueriesPerRange;
            const uint64_t beginTick = m_mappedReadback[queryIndex];
            const uint64_t endTick = m_mappedReadback[queryIndex + 1u];
            if (endTick < beginTick) continue;
            const uint64_t deltaTicks = endTick - beginTick;
            const long double microseconds =
                static_cast<long double>(deltaTicks) * 1000000.0L /
                static_cast<long double>(m_frequencyHz);
            result.rangeMicroseconds[rangeIndex] =
                static_cast<uint64_t>(microseconds);
            result.validRangeMask |= rangeBit;
        }
        m_lastStats = result;
    }

    m_submittedMasks[frameIndex] = 0u;
    m_sourceFrameOrdinals[frameIndex] = 0u;
    m_begunMask = 0u;
    m_completedMask = 0u;
}

bool D3D12GpuTimestampOwner::begin(
    ID3D12GraphicsCommandList* commandList,
    bool frameOpen,
    uint32_t frameIndex,
    render::GpuTimestampRange range) noexcept {
    const uint32_t rangeIndex = static_cast<uint32_t>(range);
    if (!m_enabled || !frameOpen || !commandList || !m_queryHeap ||
        frameIndex >= kFrameSlotCount ||
        rangeIndex >= render::kGpuTimestampRangeCount) {
        return false;
    }
    const uint64_t rangeBit = uint64_t{1} << rangeIndex;
    if ((m_begunMask & rangeBit) != 0u) return false;

    const uint32_t queryIndex = frameIndex * kQueriesPerFrame +
        rangeIndex * kQueriesPerRange;
    commandList->EndQuery(
        m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryIndex);
    m_begunMask |= rangeBit;
    return true;
}

bool D3D12GpuTimestampOwner::end(
    ID3D12GraphicsCommandList* commandList,
    bool frameOpen,
    uint32_t frameIndex,
    render::GpuTimestampRange range) noexcept {
    const uint32_t rangeIndex = static_cast<uint32_t>(range);
    if (!m_enabled || !frameOpen || !commandList || !m_queryHeap ||
        frameIndex >= kFrameSlotCount ||
        rangeIndex >= render::kGpuTimestampRangeCount) {
        return false;
    }
    const uint64_t rangeBit = uint64_t{1} << rangeIndex;
    if ((m_begunMask & rangeBit) == 0u ||
        (m_completedMask & rangeBit) != 0u) {
        return false;
    }

    const uint32_t queryIndex = frameIndex * kQueriesPerFrame +
        rangeIndex * kQueriesPerRange + 1u;
    commandList->EndQuery(
        m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryIndex);
    m_completedMask |= rangeBit;
    return true;
}

void D3D12GpuTimestampOwner::resolve(
    ID3D12GraphicsCommandList* commandList,
    uint32_t frameIndex) noexcept {
    if (!m_enabled || !commandList || !m_queryHeap || !m_readback ||
        frameIndex >= kFrameSlotCount) {
        return;
    }
    for (uint32_t rangeIndex = 0;
         rangeIndex < render::kGpuTimestampRangeCount; ++rangeIndex) {
        const uint64_t rangeBit = uint64_t{1} << rangeIndex;
        if ((m_completedMask & rangeBit) == 0u) continue;
        const uint32_t queryIndex = frameIndex * kQueriesPerFrame +
            rangeIndex * kQueriesPerRange;
        const uint64_t destinationOffset =
            static_cast<uint64_t>(queryIndex) * sizeof(uint64_t);
        commandList->ResolveQueryData(
            m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
            queryIndex, kQueriesPerRange, m_readback.Get(), destinationOffset);
    }
}

void D3D12GpuTimestampOwner::sealSubmitted(
    uint32_t frameIndex, uint64_t frameOrdinal) noexcept {
    if (!m_enabled || frameIndex >= kFrameSlotCount ||
        m_completedMask == 0u) {
        return;
    }
    m_submittedMasks[frameIndex] = m_completedMask;
    m_sourceFrameOrdinals[frameIndex] = frameOrdinal;
}

} // namespace engine::d3d12
