#pragma once

#include "core/container/container_types.h"
#include "engine/renderer/runtime/RendererStats.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdint>

namespace engine::d3d12 {

class D3D12PresentationTargets final {
public:
    static constexpr uint32_t kFrameCount = 2u;

    [[nodiscard]] bool initialize(
        ID3D12Device* device,
        IDXGISwapChain3* swapChain,
        uint32_t width,
        uint32_t height,
        DXGI_FORMAT colorFormat,
        DXGI_FORMAT depthFormat);
    void shutdown() noexcept;
    void releaseForResize() noexcept;
    [[nodiscard]] bool recreate(uint32_t width, uint32_t height);

    [[nodiscard]] uint32_t configureMultisampling(uint32_t requestedCount);
    [[nodiscard]] uint32_t sampleCount() const noexcept { return m_sampleCount; }
    [[nodiscard]] bool multisamplePassActive() const noexcept {
        return m_multisamplePassActive;
    }
    [[nodiscard]] bool valid() const noexcept { return m_valid; }
    [[nodiscard]] uint32_t width() const noexcept { return m_width; }
    [[nodiscard]] uint32_t height() const noexcept { return m_height; }

    [[nodiscard]] ID3D12Resource* backBuffer(uint32_t frameIndex) const noexcept;
    [[nodiscard]] ID3D12Resource* currentColorTarget(
        uint32_t frameIndex) const noexcept;
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv(
        uint32_t frameIndex) const noexcept;
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE backBufferDsv(
        uint32_t frameIndex) const noexcept;
    void bind(ID3D12GraphicsCommandList* commandList, uint32_t frameIndex) const noexcept;
    [[nodiscard]] bool beginWorldPass(
        ID3D12GraphicsCommandList* commandList,
        uint32_t frameIndex,
        const container::Array<float, 4>& clearColor,
        render::WorldResourceStateRenderStats& stats);
    void resolveWorldPass(
        ID3D12GraphicsCommandList* commandList,
        uint32_t frameIndex,
        render::WorldResourceStateRenderStats& stats);
    void abortFrame() noexcept { m_multisamplePassActive = false; }

    [[nodiscard]] bool requestCapture() noexcept;
    [[nodiscard]] bool captureRequested() const noexcept {
        return m_captureRequested;
    }
    [[nodiscard]] bool captureReady() const noexcept {
        return m_readyCaptureIndex < kFrameCount;
    }
    [[nodiscard]] ID3D12Resource* captureTarget(uint32_t frameIndex) const noexcept;
    void markCaptureReady(uint32_t frameIndex) noexcept;
    [[nodiscard]] ID3D12Resource* consumeReadyCapture() noexcept;

private:
    [[nodiscard]] bool createBackBuffersAndCapture();
    [[nodiscard]] bool createDepthTargets();
    [[nodiscard]] bool createMultisampleTargets();

    ID3D12Device* m_device = nullptr;
    IDXGISwapChain3* m_swapChain = nullptr;
    DXGI_FORMAT m_colorFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT m_depthFormat = DXGI_FORMAT_UNKNOWN;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    container::Array<Microsoft::WRL::ComPtr<ID3D12Resource>, kFrameCount>
        m_backBuffers;
    container::Array<Microsoft::WRL::ComPtr<ID3D12Resource>, kFrameCount>
        m_depthTargets;
    container::Array<Microsoft::WRL::ComPtr<ID3D12Resource>, kFrameCount>
        m_multisampleColors;
    container::Array<Microsoft::WRL::ComPtr<ID3D12Resource>, kFrameCount>
        m_multisampleDepths;
    container::Array<Microsoft::WRL::ComPtr<ID3D12Resource>, kFrameCount>
        m_captureTargets;
    uint32_t m_rtvStride = 0;
    uint32_t m_dsvStride = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_sampleCount = 1;
    uint32_t m_requestedSampleCount = 1;
    uint32_t m_readyCaptureIndex = UINT32_MAX;
    bool m_captureRequested = false;
    bool m_multisamplePassActive = false;
    bool m_valid = false;
};

} // namespace engine::d3d12
