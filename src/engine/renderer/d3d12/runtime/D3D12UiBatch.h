#pragma once

#include "D3D12SrvDescriptorHeap.h"
#include "D3D12UiPipeline.h"
#include "engine/renderer/runtime/RendererStats.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace engine::d3d12 {

struct UiVertex final {
    float x, y;
    float u, v;
    float r, g, b, a;
};

struct UiBatchContext final {
    ID3D12GraphicsCommandList* commandList = nullptr;
    uint64_t frameOrdinal = 0;
    render::RenderBindingStats* bindingStats = nullptr;
};

class D3D12UiBatch final {
public:
    static constexpr uint32_t kFrameCount = 2u;
    static constexpr uint32_t kMaximumVertices = 65536u;
    static constexpr uint32_t kMaximumIndices = kMaximumVertices * 6u / 4u;

    [[nodiscard]] bool initialize(
        ID3D12Device* device,
        D3D12UiPipeline& pipeline,
        D3D12SrvDescriptorHeap& descriptors);
    void shutdown() noexcept;
    void beginFrame(uint32_t frameIndex);
    void discard() noexcept;
    void setVirtualResolution(float width, float height) noexcept;
    void setSamplerMode(uint32_t mode) noexcept { m_samplerMode = mode; }
    [[nodiscard]] uint32_t samplerMode() const noexcept { return m_samplerMode; }

    void drawSolidQuad(const UiBatchContext& context,
        float x, float y, float width, float height,
        float r, float g, float b, float a);
    void drawSolidGradientLine(const UiBatchContext& context,
        float startX, float startY, float endX, float endY, float width,
        float startR, float startG, float startB, float startA,
        float endR, float endG, float endB, float endA);
    void drawTexturedQuad(const UiBatchContext& context,
        float x, float y, float width, float height,
        float u0, float v0, float u1, float v1,
        float r, float g, float b, float a,
        D3D12_GPU_DESCRIPTOR_HANDLE texture);
    void flush(const UiBatchContext& context);

private:
    enum class BatchKind : uint8_t { None, Solid, Textured };
    struct Constants final {
        float resolutionX = 800.0f;
        float resolutionY = 600.0f;
        float padding[62]{};
    };

    D3D12UiPipeline* m_pipeline = nullptr;
    D3D12SrvDescriptorHeap* m_descriptors = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_constantBuffer;
    UiVertex* m_mappedVertexBuffer = nullptr;
    UiVertex* m_vertices = nullptr;
    uint8_t* m_mappedConstants = nullptr;
    D3D12_VERTEX_BUFFER_VIEW m_vertexView{};
    D3D12_INDEX_BUFFER_VIEW m_indexView{};
    D3D12_GPU_VIRTUAL_ADDRESS m_constantGpuAddress = 0;
    uint64_t m_constantFrameSize = 0;
    Constants m_constants;
    uint32_t m_vertexCount = 0;
    uint32_t m_batchStart = 0;
    uint32_t m_samplerMode = 0;
    BatchKind m_batchKind = BatchKind::None;
    D3D12_GPU_DESCRIPTOR_HANDLE m_batchTexture{};
};

} // namespace engine::d3d12
