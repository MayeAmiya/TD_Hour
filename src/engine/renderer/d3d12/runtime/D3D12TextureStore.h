#pragma once

#include "core/container/container_types.h"
#include "D3D12FrameUploadArena.h"
#include "D3D12GpuRetirementQueue.h"
#include "D3D12SrvDescriptorHeap.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace engine::d3d12 {

struct TextureSubresourceUpload final {
    const uint8_t* data = nullptr;
    uint32_t rowPitch = 0;
    uint32_t slicePitch = 0;
};

class D3D12TextureStore final {
public:
    [[nodiscard]] bool initialize(
        ID3D12Device* device,
        D3D12SrvDescriptorHeap& descriptors,
        D3D12FrameUploadArena& frameUploads,
        D3D12GpuRetirementQueue& retirementQueue);
    void shutdown() noexcept;

    void recordFallbackUpload(ID3D12GraphicsCommandList* commandList) noexcept;
    void commitFrame() noexcept { m_fallbackRecordedThisFrame = false; }
    void abortFrame() noexcept;
    [[nodiscard]] uint32_t uploadRgba8(
        ID3D12GraphicsCommandList* commandList,
        uint32_t frameIndex,
        const void* pixels,
        uint32_t width,
        uint32_t height);
    [[nodiscard]] uint32_t upload2D(
        ID3D12GraphicsCommandList* commandList,
        uint32_t frameIndex,
        uint32_t width,
        uint32_t height,
        DXGI_FORMAT format,
        container::Span<const TextureSubresourceUpload> subresources);
    [[nodiscard]] bool retire(
        uint32_t descriptorIndex,
        uint64_t requestFrame,
        bool pending,
        uint64_t immediateFence);

private:
    [[nodiscard]] uint64_t resourceAllocationBytes(
        ID3D12Resource* resource) const noexcept;

    ID3D12Device* m_device = nullptr;
    D3D12SrvDescriptorHeap* m_descriptors = nullptr;
    D3D12FrameUploadArena* m_frameUploads = nullptr;
    D3D12GpuRetirementQueue* m_retirementQueue = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_fallbackTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_fallbackUpload;
    bool m_fallbackUploadPending = false;
    bool m_fallbackRecordedThisFrame = false;
    container::Array<Microsoft::WRL::ComPtr<ID3D12Resource>,
                     D3D12SrvDescriptorHeap::kCapacity> m_textures{};
};

} // namespace engine::d3d12
