#include "D3D12FenceTimeline.h"

#include "debug/debug.h"

namespace engine::d3d12 {

bool D3D12FenceTimeline::initialize(
    ID3D12Device* device, ID3D12CommandQueue* queue) {
    shutdown();
    if (!device || !queue) return false;
    const HRESULT result = device->CreateFence(
        0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(result)) {
        TD_LOG_ERROR("[D3D12FenceTimeline] Fence creation failed: 0x{:08X}", result);
        return false;
    }
    m_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_event) {
        TD_LOG_ERROR("[D3D12FenceTimeline] Event creation failed");
        shutdown();
        return false;
    }
    m_queue = queue;
    return true;
}

void D3D12FenceTimeline::shutdown() noexcept {
    if (m_event) CloseHandle(m_event);
    m_event = nullptr;
    m_nextValue = 0u;
    m_fence.Reset();
    m_queue = nullptr;
}

uint64_t D3D12FenceTimeline::signal() noexcept {
    if (!valid()) return 0u;
    const uint64_t value = ++m_nextValue;
    const HRESULT result = m_queue->Signal(m_fence.Get(), value);
    if (FAILED(result)) {
        TD_LOG_ERROR("[D3D12FenceTimeline] Signal failed: 0x{:08X}", result);
        return 0u;
    }
    return value;
}

bool D3D12FenceTimeline::wait(uint64_t fenceValue) noexcept {
    if (fenceValue == 0u) return true;
    if (!valid()) return false;
    const uint64_t completed = m_fence->GetCompletedValue();
    if (completed == UINT64_MAX) {
        TD_LOG_ERROR("[D3D12FenceTimeline] Fence reports a removed device");
        return false;
    }
    if (completed >= fenceValue) return true;
    const HRESULT result = m_fence->SetEventOnCompletion(fenceValue, m_event);
    if (FAILED(result)) {
        TD_LOG_ERROR("[D3D12FenceTimeline] SetEventOnCompletion failed: 0x{:08X}", result);
        return false;
    }
    const DWORD waitResult = WaitForSingleObject(m_event, INFINITE);
    if (waitResult != WAIT_OBJECT_0) {
        TD_LOG_ERROR("[D3D12FenceTimeline] Wait failed: 0x{:08X}", waitResult);
        return false;
    }
    return true;
}

uint64_t D3D12FenceTimeline::completedValue() const noexcept {
    return m_fence ? m_fence->GetCompletedValue() : 0u;
}

} // namespace engine::d3d12
