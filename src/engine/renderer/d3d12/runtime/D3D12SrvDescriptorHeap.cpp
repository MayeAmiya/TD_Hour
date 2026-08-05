#include "D3D12SrvDescriptorHeap.h"

#include "debug/debug.h"

#include <algorithm>

namespace engine::d3d12 {

bool D3D12SrvDescriptorHeap::initialize(ID3D12Device* device) {
    shutdown();
    if (!device) return false;
    D3D12_DESCRIPTOR_HEAP_DESC description{};
    description.NumDescriptors = kCapacity;
    description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    const HRESULT result = device->CreateDescriptorHeap(
        &description, IID_PPV_ARGS(&m_heap));
    if (FAILED(result)) {
        TD_LOG_ERROR("[D3D12SrvDescriptorHeap] Heap creation failed: 0x{:08X}", result);
        return false;
    }
    m_descriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_count = kCapacity;
    m_freeIndices.reserve(m_count - 1u);
    for (uint32_t index = m_count; index-- > 1u;) {
        m_freeIndices.push_back(index);
    }
    // Slot zero is the device-owned fallback white texture and is never
    // admitted to ordinary allocation or retirement.
    m_allocated[0] = true;
    m_allocatedCount = 1u;
    m_lifetimeHighWater = 1u;
    return true;
}

void D3D12SrvDescriptorHeap::shutdown() noexcept {
    m_heap.Reset();
    m_descriptorSize = 0u;
    m_count = 0u;
    m_allocated.fill(false);
    m_retiring.fill(false);
    m_lastUsedFrames.fill(0u);
    m_lastUsedFences.fill(0u);
    m_touchedThisSubmission.fill(false);
    m_identities.fill(GpuRetirementIdentity{});
    m_touchedCount = 0u;
    m_freeIndices.clear();
    m_allocatedCount = 0u;
    m_retiringCount = 0u;
    m_lifetimeHighWater = 0u;
    m_lastWarningHighWater = 0u;
    m_allocationFailures = 0u;
    m_pressureWarnings = 0u;
}

uint32_t D3D12SrvDescriptorHeap::allocate() {
    if (m_freeIndices.empty()) {
        ++m_allocationFailures;
        TD_LOG_ERROR("[D3D12SrvDescriptorHeap] Heap exhausted");
        return UINT32_MAX;
    }
    const uint32_t index = m_freeIndices.back();
    m_freeIndices.pop_back();
    m_allocated[index] = true;
    m_retiring[index] = false;
    m_lastUsedFrames[index] = 0u;
    m_lastUsedFences[index] = 0u;
    m_touchedThisSubmission[index] = false;
    m_identities[index] = {};
    ++m_allocatedCount;
    m_lifetimeHighWater = std::max(m_lifetimeHighWater, m_allocatedCount);
    reportPressure();
    return index;
}

bool D3D12SrvDescriptorHeap::canRetire(uint32_t index) const noexcept {
    return index != 0u && index < m_count &&
        m_allocated[index] && !m_retiring[index];
}

SrvDescriptorRetirementMetadata
D3D12SrvDescriptorHeap::retirementMetadata(uint32_t index) const noexcept {
    if (index >= m_count) return {};
    return {
        .lastUsedFrame = m_lastUsedFrames[index],
        .lastUsedFence = m_lastUsedFences[index],
        .identity = m_identities[index],
    };
}

void D3D12SrvDescriptorHeap::markRetiring(uint32_t index) noexcept {
    if (!canRetire(index)) return;
    m_retiring[index] = true;
    ++m_retiringCount;
}

void D3D12SrvDescriptorHeap::releaseImmediately(uint32_t index) noexcept {
    if (index == 0u || index >= m_count || !m_allocated[index]) return;
    if (m_retiring[index] && m_retiringCount != 0u) --m_retiringCount;
    if (m_allocatedCount != 0u) --m_allocatedCount;
    m_allocated[index] = false;
    m_retiring[index] = false;
    m_lastUsedFrames[index] = 0u;
    m_lastUsedFences[index] = 0u;
    m_touchedThisSubmission[index] = false;
    m_identities[index] = {};
    m_freeIndices.push_back(index);
}

void D3D12SrvDescriptorHeap::setRetirementIdentity(
    uint32_t index, GpuRetirementIdentity identity) noexcept {
    if (!canRetire(index)) return;
    m_identities[index] = identity;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12SrvDescriptorHeap::cpuHandle(
    uint32_t index) const noexcept {
    if (!m_heap) return {};
    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        m_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * m_descriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12SrvDescriptorHeap::gpuHandle(
    uint32_t index, bool trackUse, uint64_t frameOrdinal) const noexcept {
    if (!m_heap) return {};
    if (trackUse && index < m_count && m_allocated[index]) {
        m_lastUsedFrames[index] = frameOrdinal;
        if (!m_touchedThisSubmission[index] &&
            m_touchedCount < m_touchedIndices.size()) {
            m_touchedThisSubmission[index] = true;
            m_touchedIndices[m_touchedCount++] = index;
        }
    }
    D3D12_GPU_DESCRIPTOR_HANDLE handle =
        m_heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<uint64_t>(index) * m_descriptorSize;
    return handle;
}

void D3D12SrvDescriptorHeap::bind(
    ID3D12GraphicsCommandList* commandList) const noexcept {
    if (!commandList || !m_heap) return;
    ID3D12DescriptorHeap* heaps[] = {m_heap.Get()};
    commandList->SetDescriptorHeaps(1u, heaps);
}

void D3D12SrvDescriptorHeap::commitTouches(uint64_t fenceValue) noexcept {
    if (fenceValue == 0u) return;
    for (uint32_t touched = 0; touched < m_touchedCount; ++touched) {
        const uint32_t index = m_touchedIndices[touched];
        if (index >= m_count) continue;
        m_lastUsedFences[index] = std::max(
            m_lastUsedFences[index], fenceValue);
        m_touchedThisSubmission[index] = false;
    }
    m_touchedCount = 0u;
}

void D3D12SrvDescriptorHeap::discardTouches() noexcept {
    for (uint32_t touched = 0; touched < m_touchedCount; ++touched) {
        const uint32_t index = m_touchedIndices[touched];
        if (index < m_count) m_touchedThisSubmission[index] = false;
    }
    m_touchedCount = 0u;
}

uint64_t D3D12SrvDescriptorHeap::lastUsedFrame(uint32_t index) const noexcept {
    return index < m_count ? m_lastUsedFrames[index] : 0u;
}

uint64_t D3D12SrvDescriptorHeap::lastUsedFence(uint32_t index) const noexcept {
    return index < m_count ? m_lastUsedFences[index] : 0u;
}

render::SrvDescriptorRenderStats D3D12SrvDescriptorHeap::stats() const noexcept {
    return {
        .capacity = m_count,
        .allocated = m_allocatedCount,
        .retiring = m_retiringCount,
        .available = m_count >= m_allocatedCount
            ? m_count - m_allocatedCount : 0u,
        .lifetimeHighWater = m_lifetimeHighWater,
        .allocationFailures = m_allocationFailures,
        .pressureWarnings = m_pressureWarnings,
    };
}

void D3D12SrvDescriptorHeap::appendLiveRetirementStats(
    render::GpuRetirementRenderStats& stats) const noexcept {
    for (uint32_t index = 0; index < m_count; ++index) {
        if (!m_allocated[index]) continue;
        stats.latestSrvUseFrame = std::max(
            stats.latestSrvUseFrame, m_lastUsedFrames[index]);
        stats.latestSrvUseFence = std::max(
            stats.latestSrvUseFence, m_lastUsedFences[index]);
    }
}

void D3D12SrvDescriptorHeap::reportPressure() {
    const uint32_t usagePercent = performance_limits::percentageOf(
        m_allocatedCount, m_count);
    if (usagePercent < performance_limits::kSrvPressureWarningPercent) return;
    const bool critical = usagePercent >=
        performance_limits::kSrvPressureCriticalPercent;
    if (m_lastWarningHighWater != 0u &&
        m_lifetimeHighWater < m_lastWarningHighWater +
            performance_limits::kSrvWarningHighWaterStep) {
        return;
    }
    m_lastWarningHighWater = m_lifetimeHighWater;
    ++m_pressureWarnings;
    TD_LOG_WARN(
        "[D3D12SrvDescriptorHeap] pressure: used={}/{} ({}%) retiring={} highWater={} failures={} level={}",
        m_allocatedCount, m_count, usagePercent, m_retiringCount,
        m_lifetimeHighWater, m_allocationFailures,
        critical ? "critical" : "warning");
}

} // namespace engine::d3d12
