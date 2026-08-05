#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace engine::d3d12 {

class D3D12FenceTimeline final {
public:
    [[nodiscard]] bool initialize(
        ID3D12Device* device, ID3D12CommandQueue* queue);
    void shutdown() noexcept;

    [[nodiscard]] uint64_t signal() noexcept;
    [[nodiscard]] bool wait(uint64_t fenceValue) noexcept;
    [[nodiscard]] uint64_t completedValue() const noexcept;
    [[nodiscard]] uint64_t lastIssuedValue() const noexcept {
        return m_nextValue;
    }
    [[nodiscard]] bool valid() const noexcept {
        return m_fence && m_event != nullptr && m_queue != nullptr;
    }

private:
    ID3D12CommandQueue* m_queue = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    uint64_t m_nextValue = 0;
    HANDLE m_event = nullptr;
};

} // namespace engine::d3d12
