#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace engine::d3d12 {

enum class UiPipelineKind : uint8_t {
    Solid,
    TexturedLinear,
    TexturedPoint,
};

class D3D12UiPipeline final {
public:
    [[nodiscard]] bool initialize(
        ID3D12Device* device,
        DXGI_FORMAT renderTargetFormat,
        DXGI_FORMAT depthFormat);
    void shutdown() noexcept;

    [[nodiscard]] ID3D12RootSignature* rootSignature(
        bool pointSampled) const noexcept {
        return pointSampled ? m_pointRoot.Get() : m_linearRoot.Get();
    }
    [[nodiscard]] ID3D12PipelineState* pipeline(
        UiPipelineKind kind) const noexcept;

private:
    [[nodiscard]] bool createRootSignature(
        ID3D12Device* device,
        D3D12_FILTER filter,
        Microsoft::WRL::ComPtr<ID3D12RootSignature>& output);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_linearRoot;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_pointRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_solid;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_texturedLinear;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_texturedPoint;
};

} // namespace engine::d3d12
