#include "D3D12GpuRetirementQueue.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace engine::d3d12 {
namespace {

[[nodiscard]] uint64_t resolvedFence(
    uint64_t frameOrdinal,
    uint64_t explicitFence,
    const RetirementFenceHistory& history,
    bool& completedWithoutExactFence) noexcept {
    if (explicitFence != 0u) return explicitFence;
    for (size_t index = 0;
         index < history.frameOrdinals.size() &&
         index < history.fenceValues.size(); ++index) {
        if (history.frameOrdinals[index] != frameOrdinal) continue;
        const uint64_t fence = history.fenceValues[index];
        completedWithoutExactFence = frameOrdinal != 0u && fence == 0u &&
            frameOrdinal < history.currentFrameOrdinal;
        return fence;
    }
    completedWithoutExactFence = frameOrdinal != 0u &&
        frameOrdinal < history.currentFrameOrdinal;
    return 0u;
}

} // namespace

void D3D12GpuRetirementQueue::clear() noexcept {
    m_pendingResources.clear();
    m_fencedResources.clear();
    m_pendingDescriptors.clear();
    m_fencedDescriptors.clear();
    m_pendingStaticBuffers.clear();
    m_fencedStaticBuffers.clear();
    m_reclaimedResources = 0u;
    m_reclaimedResourceBytes = 0u;
    m_reclaimedDescriptors = 0u;
    m_rejectedRetirements = 0u;
}

void D3D12GpuRetirementQueue::retireResource(
    Microsoft::WRL::ComPtr<ID3D12Resource> resource,
    uint64_t byteSize,
    uint64_t requestFrame,
    uint64_t lastUsedFrame,
    uint64_t lastUsedFence,
    GpuRetirementIdentity identity,
    bool pending,
    uint64_t immediateFence) {
    if (!resource) return;
    ResourceRecord record{
        .resource = std::move(resource),
        .byteSize = byteSize,
        .requestFrame = requestFrame,
        .lastUsedFrame = lastUsedFrame,
        .lastUsedFence = lastUsedFence,
        .identity = identity,
        .retireFence = pending ? 0u : immediateFence,
    };
    (pending ? m_pendingResources : m_fencedResources)
        .push_back(std::move(record));
}

void D3D12GpuRetirementQueue::retireDescriptor(
    uint32_t index,
    uint64_t requestFrame,
    SrvDescriptorRetirementMetadata metadata,
    bool pending,
    uint64_t immediateFence) {
    DescriptorRecord record{
        .index = index,
        .requestFrame = requestFrame,
        .lastUsedFrame = metadata.lastUsedFrame,
        .lastUsedFence = metadata.lastUsedFence,
        .identity = metadata.identity,
        .retireFence = pending ? 0u : immediateFence,
    };
    (pending ? m_pendingDescriptors : m_fencedDescriptors).push_back(record);
}

void D3D12GpuRetirementQueue::retireResourceAndDescriptor(
    Microsoft::WRL::ComPtr<ID3D12Resource> resource,
    uint64_t byteSize,
    uint32_t descriptorIndex,
    uint64_t requestFrame,
    SrvDescriptorRetirementMetadata metadata,
    bool pending,
    uint64_t immediateFence) {
    auto& resources = pending ? m_pendingResources : m_fencedResources;
    auto& descriptors = pending ? m_pendingDescriptors : m_fencedDescriptors;
    resources.reserve(resources.size() + 1u);
    descriptors.reserve(descriptors.size() + 1u);
    retireResource(std::move(resource), byteSize, requestFrame,
        metadata.lastUsedFrame, metadata.lastUsedFence, metadata.identity,
        pending, immediateFence);
    retireDescriptor(descriptorIndex, requestFrame, metadata,
        pending, immediateFence);
}

void D3D12GpuRetirementQueue::retireStaticBuffer(
    StaticBufferRetirement retirement,
    bool pending,
    uint64_t immediateFence) {
    StaticBufferRecord record{
        .retirement = retirement,
        .retireFence = pending ? 0u : immediateFence,
    };
    (pending ? m_pendingStaticBuffers : m_fencedStaticBuffers)
        .push_back(record);
}

void D3D12GpuRetirementQueue::sealPending(uint64_t fenceValue) {
    if (fenceValue == 0u) return;
    for (ResourceRecord& record : m_pendingResources) {
        record.retireFence = fenceValue;
        m_fencedResources.push_back(std::move(record));
    }
    m_pendingResources.clear();
    for (DescriptorRecord record : m_pendingDescriptors) {
        record.retireFence = fenceValue;
        m_fencedDescriptors.push_back(record);
    }
    m_pendingDescriptors.clear();
    for (StaticBufferRecord record : m_pendingStaticBuffers) {
        record.retireFence = fenceValue;
        m_fencedStaticBuffers.push_back(record);
    }
    m_pendingStaticBuffers.clear();
}

void D3D12GpuRetirementQueue::reclaim(
    uint64_t completedFence,
    container::Vector<uint32_t>& completedDescriptors,
    container::Vector<StaticBufferRetirement>& completedStaticBuffers) {
    completedDescriptors.clear();
    completedStaticBuffers.clear();
    for (const DescriptorRecord& record : m_fencedDescriptors) {
        if (record.retireFence <= completedFence) {
            completedDescriptors.push_back(record.index);
        }
    }
    for (const StaticBufferRecord& record : m_fencedStaticBuffers) {
        if (record.retireFence <= completedFence) {
            completedStaticBuffers.push_back(record.retirement);
        }
    }
    const auto descriptorsEnd = std::remove_if(
        m_fencedDescriptors.begin(), m_fencedDescriptors.end(),
        [this, completedFence](const DescriptorRecord& record) {
            if (record.retireFence > completedFence) return false;
            ++m_reclaimedDescriptors;
            return true;
        });
    m_fencedDescriptors.erase(descriptorsEnd, m_fencedDescriptors.end());
    const auto resourcesEnd = std::remove_if(
        m_fencedResources.begin(), m_fencedResources.end(),
        [this, completedFence](const ResourceRecord& record) {
            if (record.retireFence > completedFence) return false;
            ++m_reclaimedResources;
            m_reclaimedResourceBytes += record.byteSize;
            return true;
        });
    m_fencedResources.erase(resourcesEnd, m_fencedResources.end());
    const auto staticEnd = std::remove_if(
        m_fencedStaticBuffers.begin(), m_fencedStaticBuffers.end(),
        [this, completedFence](const StaticBufferRecord& record) {
            if (record.retireFence > completedFence) return false;
            ++m_reclaimedResources;
            m_reclaimedResourceBytes += record.retirement.size;
            return true;
        });
    m_fencedStaticBuffers.erase(staticEnd, m_fencedStaticBuffers.end());
}

render::GpuRetirementRenderStats D3D12GpuRetirementQueue::stats(
    uint64_t frameOrdinal,
    const RetirementFenceHistory& history) const noexcept {
    render::GpuRetirementRenderStats result{
        .frameOrdinal = frameOrdinal,
        .pendingResources = static_cast<uint32_t>(std::min<size_t>(
            m_pendingResources.size() + m_pendingStaticBuffers.size(),
            std::numeric_limits<uint32_t>::max())),
        .fencedResources = static_cast<uint32_t>(std::min<size_t>(
            m_fencedResources.size() + m_fencedStaticBuffers.size(),
            std::numeric_limits<uint32_t>::max())),
        .pendingSrvDescriptors = static_cast<uint32_t>(
            m_pendingDescriptors.size()),
        .fencedSrvDescriptors = static_cast<uint32_t>(
            m_fencedDescriptors.size()),
        .completedFence = history.completedFence,
        .reclaimedResources = m_reclaimedResources,
        .reclaimedResourceBytes = m_reclaimedResourceBytes,
        .reclaimedSrvDescriptors = m_reclaimedDescriptors,
        .rejectedRetirements = m_rejectedRetirements,
    };
    result.oldestRetireRequestFrame = std::numeric_limits<uint64_t>::max();
    const auto observeResource = [&result, &history](
        uint64_t bytes, uint64_t requestFrame, uint64_t lastUsedFrame,
        uint64_t lastUsedFence, GpuRetirementIdentity identity,
        uint64_t retireFence, bool pending) {
        (pending ? result.pendingResourceBytes : result.fencedResourceBytes) += bytes;
        if (identity) ++result.attributedResources;
        result.latestResourceUseFrame = std::max(
            result.latestResourceUseFrame, lastUsedFrame);
        bool completedWithoutExactFence = false;
        const uint64_t useFence = resolvedFence(
            lastUsedFrame, lastUsedFence, history, completedWithoutExactFence);
        result.latestResourceUseFence = std::max(
            result.latestResourceUseFence, useFence);
        if (completedWithoutExactFence) {
            ++result.completedResourceUsesWithoutExactFence;
        }
        result.lastSealedRetirementFence = std::max(
            result.lastSealedRetirementFence, retireFence);
        result.oldestRetireRequestFrame = std::min(
            result.oldestRetireRequestFrame, requestFrame);
    };
    for (const ResourceRecord& record : m_pendingResources) {
        observeResource(record.byteSize, record.requestFrame,
            record.lastUsedFrame, record.lastUsedFence, record.identity,
            record.retireFence, true);
    }
    for (const ResourceRecord& record : m_fencedResources) {
        observeResource(record.byteSize, record.requestFrame,
            record.lastUsedFrame, record.lastUsedFence, record.identity,
            record.retireFence, false);
    }
    for (const StaticBufferRecord& record : m_pendingStaticBuffers) {
        const auto& value = record.retirement;
        observeResource(value.size, value.requestFrame, value.lastUsedFrame,
            value.lastUsedFence, value.identity, record.retireFence, true);
    }
    for (const StaticBufferRecord& record : m_fencedStaticBuffers) {
        const auto& value = record.retirement;
        observeResource(value.size, value.requestFrame, value.lastUsedFrame,
            value.lastUsedFence, value.identity, record.retireFence, false);
    }
    const auto observeDescriptor = [&result](const DescriptorRecord& record) {
        if (record.identity) ++result.attributedSrvDescriptors;
        result.lastSealedRetirementFence = std::max(
            result.lastSealedRetirementFence, record.retireFence);
        result.oldestRetireRequestFrame = std::min(
            result.oldestRetireRequestFrame, record.requestFrame);
    };
    for (const DescriptorRecord& record : m_pendingDescriptors) {
        observeDescriptor(record);
    }
    for (const DescriptorRecord& record : m_fencedDescriptors) {
        observeDescriptor(record);
    }
    if (result.oldestRetireRequestFrame ==
        std::numeric_limits<uint64_t>::max()) {
        result.oldestRetireRequestFrame = 0u;
    }
    return result;
}

uint32_t D3D12GpuRetirementQueue::retiringStaticBufferCount() const noexcept {
    return static_cast<uint32_t>(std::min<size_t>(
        m_pendingStaticBuffers.size() + m_fencedStaticBuffers.size(),
        std::numeric_limits<uint32_t>::max()));
}

uint64_t D3D12GpuRetirementQueue::retiringStaticBufferBytes() const noexcept {
    uint64_t bytes = 0u;
    for (const StaticBufferRecord& record : m_pendingStaticBuffers) {
        bytes += record.retirement.size;
    }
    for (const StaticBufferRecord& record : m_fencedStaticBuffers) {
        bytes += record.retirement.size;
    }
    return bytes;
}

} // namespace engine::d3d12
