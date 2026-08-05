#include "D3D12UiBatch.h"

#include "debug/debug.h"

#include <cmath>
#include <cstring>

namespace engine::d3d12 {
namespace {

[[nodiscard]] D3D12_RESOURCE_DESC bufferDesc(uint64_t size) noexcept {
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

} // namespace

bool D3D12UiBatch::initialize(
    ID3D12Device* device,
    D3D12UiPipeline& pipeline,
    D3D12SrvDescriptorHeap& descriptors) {
    shutdown();
    if (!device) return false;
    m_pipeline = &pipeline;
    m_descriptors = &descriptors;
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    const uint64_t vertexFrameBytes =
        static_cast<uint64_t>(kMaximumVertices) * sizeof(UiVertex);
    D3D12_RESOURCE_DESC description = bufferDesc(
        vertexFrameBytes * kFrameCount);
    HRESULT result = device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_vertexBuffer));
    if (FAILED(result)) return false;
    result = m_vertexBuffer->Map(
        0, nullptr, reinterpret_cast<void**>(&m_mappedVertexBuffer));
    if (FAILED(result) || !m_mappedVertexBuffer) return false;
    m_vertexView.StrideInBytes = sizeof(UiVertex);
    m_vertexView.SizeInBytes = static_cast<UINT>(vertexFrameBytes);

    container::Vector<uint16_t> indices;
    indices.reserve(kMaximumIndices);
    for (uint32_t vertex = 0; vertex < kMaximumVertices; vertex += 4u) {
        indices.push_back(static_cast<uint16_t>(vertex));
        indices.push_back(static_cast<uint16_t>(vertex + 1u));
        indices.push_back(static_cast<uint16_t>(vertex + 2u));
        indices.push_back(static_cast<uint16_t>(vertex + 2u));
        indices.push_back(static_cast<uint16_t>(vertex + 3u));
        indices.push_back(static_cast<uint16_t>(vertex));
    }
    description = bufferDesc(indices.size() * sizeof(uint16_t));
    result = device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_indexBuffer));
    if (FAILED(result)) return false;
    void* mappedIndices = nullptr;
    result = m_indexBuffer->Map(0, nullptr, &mappedIndices);
    if (FAILED(result) || !mappedIndices) return false;
    std::memcpy(mappedIndices, indices.data(),
        indices.size() * sizeof(uint16_t));
    m_indexBuffer->Unmap(0, nullptr);
    m_indexView = {
        .BufferLocation = m_indexBuffer->GetGPUVirtualAddress(),
        .SizeInBytes = static_cast<UINT>(indices.size() * sizeof(uint16_t)),
        .Format = DXGI_FORMAT_R16_UINT,
    };

    constexpr uint64_t alignment =
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    m_constantFrameSize = (sizeof(Constants) + alignment - 1u) &
        ~(alignment - 1u);
    description = bufferDesc(m_constantFrameSize * kFrameCount);
    result = device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_constantBuffer));
    if (FAILED(result)) return false;
    result = m_constantBuffer->Map(
        0, nullptr, reinterpret_cast<void**>(&m_mappedConstants));
    return SUCCEEDED(result) && m_mappedConstants;
}

void D3D12UiBatch::shutdown() noexcept {
    if (m_mappedVertexBuffer && m_vertexBuffer) m_vertexBuffer->Unmap(0, nullptr);
    if (m_mappedConstants && m_constantBuffer) m_constantBuffer->Unmap(0, nullptr);
    m_mappedVertexBuffer = nullptr;
    m_vertices = nullptr;
    m_mappedConstants = nullptr;
    m_constantBuffer.Reset();
    m_indexBuffer.Reset();
    m_vertexBuffer.Reset();
    m_pipeline = nullptr;
    m_descriptors = nullptr;
    m_vertexView = {};
    m_indexView = {};
    m_constantGpuAddress = 0;
    m_vertexCount = 0;
    m_batchStart = 0;
    m_batchKind = BatchKind::None;
    m_batchTexture = {};
}

void D3D12UiBatch::beginFrame(uint32_t frameIndex) {
    if (frameIndex >= kFrameCount || !m_mappedVertexBuffer ||
        !m_mappedConstants) return;
    const uint64_t vertexFrameBytes =
        static_cast<uint64_t>(kMaximumVertices) * sizeof(UiVertex);
    m_vertices = m_mappedVertexBuffer +
        static_cast<size_t>(frameIndex) * kMaximumVertices;
    m_vertexView.BufferLocation =
        m_vertexBuffer->GetGPUVirtualAddress() + frameIndex * vertexFrameBytes;
    const uint64_t constantOffset = frameIndex * m_constantFrameSize;
    std::memcpy(m_mappedConstants + constantOffset,
        &m_constants, sizeof(m_constants));
    m_constantGpuAddress =
        m_constantBuffer->GetGPUVirtualAddress() + constantOffset;
    m_vertexCount = 0u;
    m_batchStart = 0u;
    m_batchKind = BatchKind::None;
    m_batchTexture = {};
}

void D3D12UiBatch::discard() noexcept {
    m_vertexCount = 0u;
    m_batchStart = 0u;
    m_batchKind = BatchKind::None;
    m_batchTexture = {};
}

void D3D12UiBatch::setVirtualResolution(
    float width, float height) noexcept {
    if (std::isfinite(width) && width > 0.0f) m_constants.resolutionX = width;
    if (std::isfinite(height) && height > 0.0f) m_constants.resolutionY = height;
}

void D3D12UiBatch::drawSolidQuad(const UiBatchContext& context,
    float x, float y, float width, float height,
    float r, float g, float b, float a) {
    if (m_batchKind == BatchKind::Textured) flush(context);
    if (!m_vertices || m_vertexCount + 4u > kMaximumVertices) {
        TD_LOG_ERROR("[D3D12UiBatch] Vertex buffer exhausted");
        return;
    }
    const uint32_t base = m_vertexCount;
    m_vertices[base + 0u] = {x, y, 0, 0, r, g, b, a};
    m_vertices[base + 1u] = {x + width, y, 1, 0, r, g, b, a};
    m_vertices[base + 2u] = {x + width, y + height, 1, 1, r, g, b, a};
    m_vertices[base + 3u] = {x, y + height, 0, 1, r, g, b, a};
    m_vertexCount += 4u;
    m_batchKind = BatchKind::Solid;
}

void D3D12UiBatch::drawSolidGradientLine(const UiBatchContext& context,
    float startX, float startY, float endX, float endY, float width,
    float startR, float startG, float startB, float startA,
    float endR, float endG, float endB, float endA) {
    if (m_batchKind == BatchKind::Textured) flush(context);
    if (!m_vertices || m_vertexCount + 4u > kMaximumVertices) {
        TD_LOG_ERROR("[D3D12UiBatch] Vertex buffer exhausted");
        return;
    }
    const float dx = endX - startX;
    const float dy = endY - startY;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (!std::isfinite(length) || length <= 0.0f ||
        !std::isfinite(width) || width <= 0.0f) return;
    const float scale = width * 0.5f / length;
    const float nx = -dy * scale;
    const float ny = dx * scale;
    const uint32_t base = m_vertexCount;
    m_vertices[base + 0u] = {startX - nx, startY - ny, 0, 0,
        startR, startG, startB, startA};
    m_vertices[base + 1u] = {endX - nx, endY - ny, 1, 0,
        endR, endG, endB, endA};
    m_vertices[base + 2u] = {endX + nx, endY + ny, 1, 1,
        endR, endG, endB, endA};
    m_vertices[base + 3u] = {startX + nx, startY + ny, 0, 1,
        startR, startG, startB, startA};
    m_vertexCount += 4u;
    m_batchKind = BatchKind::Solid;
}

void D3D12UiBatch::drawTexturedQuad(const UiBatchContext& context,
    float x, float y, float width, float height,
    float u0, float v0, float u1, float v1,
    float r, float g, float b, float a,
    D3D12_GPU_DESCRIPTOR_HANDLE texture) {
    if (m_batchKind == BatchKind::Solid ||
        (m_batchKind == BatchKind::Textured &&
         m_batchTexture.ptr != texture.ptr)) flush(context);
    if (!m_vertices || m_vertexCount + 4u > kMaximumVertices) {
        TD_LOG_ERROR("[D3D12UiBatch] Vertex buffer exhausted");
        return;
    }
    const uint32_t base = m_vertexCount;
    m_vertices[base + 0u] = {x, y, u0, v0, r, g, b, a};
    m_vertices[base + 1u] = {x + width, y, u1, v0, r, g, b, a};
    m_vertices[base + 2u] = {x + width, y + height, u1, v1, r, g, b, a};
    m_vertices[base + 3u] = {x, y + height, u0, v1, r, g, b, a};
    m_vertexCount += 4u;
    m_batchKind = BatchKind::Textured;
    m_batchTexture = texture;
}

void D3D12UiBatch::flush(const UiBatchContext& context) {
    if (!context.commandList || !m_pipeline || !m_descriptors ||
        m_batchKind == BatchKind::None || m_vertexCount <= m_batchStart) return;
    const bool pointSampled =
        m_batchKind == BatchKind::Textured && m_samplerMode == 1u;
    context.commandList->SetGraphicsRootSignature(
        m_pipeline->rootSignature(pointSampled));
    context.commandList->SetPipelineState(m_pipeline->pipeline(
        m_batchKind == BatchKind::Solid ? UiPipelineKind::Solid
        : pointSampled ? UiPipelineKind::TexturedPoint
                       : UiPipelineKind::TexturedLinear));
    context.commandList->SetGraphicsRootConstantBufferView(
        0u, m_constantGpuAddress);
    m_descriptors->bind(context.commandList);
    context.commandList->SetGraphicsRootDescriptorTable(1u,
        m_batchKind == BatchKind::Solid
            ? m_descriptors->gpuHandle(0u, true, context.frameOrdinal)
            : m_batchTexture);
    context.commandList->IASetVertexBuffers(0u, 1u, &m_vertexView);
    context.commandList->IASetIndexBuffer(&m_indexView);
    context.commandList->IASetPrimitiveTopology(
        D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const uint32_t vertexCount = m_vertexCount - m_batchStart;
    const uint32_t indexOffset = (m_batchStart / 4u) * 6u;
    const uint32_t indexCount = (vertexCount / 4u) * 6u;
    context.commandList->DrawIndexedInstanced(
        indexCount, 1u, indexOffset, 0, 0u);
    if (context.bindingStats) {
        ++context.bindingStats->graphicsRootSignatureCalls;
        ++context.bindingStats->pipelineStateCalls;
        ++context.bindingStats->graphicsDescriptorTableCalls;
        ++context.bindingStats->vertexBufferCalls;
        ++context.bindingStats->indexBufferCalls;
        ++context.bindingStats->drawCalls;
    }
    m_batchStart = m_vertexCount;
    m_batchKind = BatchKind::None;
    m_batchTexture = {};
}

} // namespace engine::d3d12
