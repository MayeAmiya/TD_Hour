#include "engine/renderer/world/particle/ParticleRenderer.h"

#include "engine/renderer/d3d12/runtime/D3D12Device.h"
#include "engine/renderer/world/resource/WorldTextureCache.h"
#include "debug/debug.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace engine::render {
namespace {

[[nodiscard]] D3D12_HEAP_PROPERTIES uploadHeapProperties() noexcept {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

[[nodiscard]] D3D12_HEAP_PROPERTIES defaultHeapProperties() noexcept {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

[[nodiscard]] D3D12_RESOURCE_DESC bufferDescription(uint64_t size) noexcept {
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

bool createUploadBuffer(
    ID3D12Device* device, const void* data, uint64_t size,
    Microsoft::WRL::ComPtr<ID3D12Resource>& output) {
    if (!device || !data || size == 0) return false;
    const D3D12_HEAP_PROPERTIES heap = uploadHeapProperties();
    const D3D12_RESOURCE_DESC description = bufferDescription(size);
    const HRESULT result = device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&output));
    if (FAILED(result)) return false;

    void* mapped = nullptr;
    if (FAILED(output->Map(0, nullptr, &mapped)) || !mapped) {
        output.Reset();
        return false;
    }
    std::memcpy(mapped, data, static_cast<size_t>(size));
    output->Unmap(0, nullptr);
    return true;
}

} // namespace

bool ParticleRenderer::ensureSmudgeSceneTargets(
    uint32_t width, uint32_t height) {
    if (!m_device || !m_device->getDevice() || width == 0 || height == 0) {
        return false;
    }
    const bool ready = m_smudgeSceneWidth == width &&
        m_smudgeSceneHeight == height &&
        std::all_of(m_smudgeSceneTargets.begin(),
                    m_smudgeSceneTargets.end(),
                    [](const auto& target) { return target != nullptr; }) &&
        std::all_of(m_smudgeSceneSrvs.begin(), m_smudgeSceneSrvs.end(),
                    [](uint32_t srv) { return srv != UINT32_MAX; });
    if (ready) return true;
    releaseSmudgeSceneTargets();

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = d3d12::D3D12Device::SWAP_FORMAT;
    description.SampleDesc.Count = 1;
    const D3D12_HEAP_PROPERTIES heap = defaultHeapProperties();
    for (size_t index = 0; index < m_smudgeSceneTargets.size(); ++index) {
        ++m_sceneColorStats.allocationAttempts;
        const D3D12_RESOURCE_ALLOCATION_INFO allocation =
            m_device->getDevice()->GetResourceAllocationInfo(
                0u, 1u, &description);
        if (allocation.SizeInBytes == 0 ||
            allocation.SizeInBytes == UINT64_MAX) {
            ++m_sceneColorStats.allocationFailures;
            releaseSmudgeSceneTargets();
            return false;
        }
        const HRESULT result =
            m_device->getDevice()->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&m_smudgeSceneTargets[index]));
        if (FAILED(result)) {
            ++m_sceneColorStats.allocationFailures;
            TD_LOG_ERROR(
                "[ParticleRenderer] smudge scene target {} failed: 0x{:08X}",
                index, static_cast<uint32_t>(result));
            releaseSmudgeSceneTargets();
            return false;
        }
        m_smudgeSceneTargetAllocationBytes[index] =
            allocation.SizeInBytes;
        m_sceneColorStats.residentAllocationBytes +=
            allocation.SizeInBytes;
        m_sceneColorStats.lifetimeResidentHighWaterBytes = std::max(
            m_sceneColorStats.lifetimeResidentHighWaterBytes,
            m_sceneColorStats.residentAllocationBytes);
        const uint32_t srv = m_device->allocateSrvDescriptor();
        if (srv == UINT32_MAX) {
            ++m_sceneColorStats.allocationFailures;
            releaseSmudgeSceneTargets();
            return false;
        }
        m_smudgeSceneSrvs[index] = srv;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDescription{};
        srvDescription.Format = d3d12::D3D12Device::SWAP_FORMAT;
        srvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDescription.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDescription.Texture2D.MipLevels = 1;
        m_device->getDevice()->CreateShaderResourceView(
            m_smudgeSceneTargets[index].Get(), &srvDescription,
            m_device->getSrvCpuHandle(srv));
        m_smudgeSceneStates[index] = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    m_smudgeSceneWidth = width;
    m_smudgeSceneHeight = height;
    return true;
}

void ParticleRenderer::releaseSmudgeSceneTargets() {
    for (size_t index = 0; index < m_smudgeSceneTargets.size(); ++index) {
        const uint64_t allocationBytes =
            m_smudgeSceneTargetAllocationBytes[index];
        if (m_smudgeSceneTargets[index]) {
            ++m_sceneColorStats.releaseCalls;
            m_sceneColorStats.releasedAllocationBytes += allocationBytes;
            if (m_device) {
                ++m_sceneColorStats.retirementRequests;
                m_sceneColorStats.retirementRequestedBytes +=
                    allocationBytes;
                m_device->retireResource(
                    std::move(m_smudgeSceneTargets[index]));
            } else {
                m_smudgeSceneTargets[index].Reset();
            }
        }
        m_sceneColorStats.residentAllocationBytes =
            m_sceneColorStats.residentAllocationBytes >= allocationBytes
            ? m_sceneColorStats.residentAllocationBytes - allocationBytes
            : 0;
        m_smudgeSceneTargetAllocationBytes[index] = 0;
        if (m_device && m_smudgeSceneSrvs[index] != UINT32_MAX) {
            m_device->freeSrvDescriptor(m_smudgeSceneSrvs[index]);
        }
        m_smudgeSceneSrvs[index] = UINT32_MAX;
        m_smudgeSceneStates[index] = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    m_smudgeSceneWidth = 0;
    m_smudgeSceneHeight = 0;
}

bool ParticleRenderer::createStaticQuad() {
    if (!m_device || !m_device->getDevice()) return false;
    constexpr QuadVertex vertices[] = {
        {{-0.5f, -0.5f}, {0.0f, 1.0f}},
        {{-0.5f,  0.5f}, {0.0f, 0.0f}},
        {{ 0.5f,  0.5f}, {1.0f, 0.0f}},
        {{ 0.5f, -0.5f}, {1.0f, 1.0f}},
    };
    constexpr uint16_t indices[] = {0, 1, 2, 0, 2, 3};
    if (!createUploadBuffer(m_device->getDevice(), vertices, sizeof(vertices),
                            m_quadVertexBuffer) ||
        !createUploadBuffer(m_device->getDevice(), indices, sizeof(indices),
                            m_quadIndexBuffer)) {
        TD_LOG_ERROR("[ParticleRenderer] static quad upload failed");
        return false;
    }
    m_quadVertexView.BufferLocation = m_quadVertexBuffer->GetGPUVirtualAddress();
    m_quadVertexView.SizeInBytes = sizeof(vertices);
    m_quadVertexView.StrideInBytes = sizeof(QuadVertex);
    m_quadIndexView.BufferLocation = m_quadIndexBuffer->GetGPUVirtualAddress();
    m_quadIndexView.SizeInBytes = sizeof(indices);
    m_quadIndexView.Format = DXGI_FORMAT_R16_UINT;
    return true;
}

uint32_t ParticleRenderer::textureSrv(container::StringView textureName) {
    if (!m_device || !m_textures || textureName.empty()) return 0;
    const container::String key(textureName);
    if (const auto found = m_textureSrvs.find(key); found != m_textureSrvs.end()) {
        return found->second;
    }
    const std::optional<uint32_t> acquired = m_textures->acquire(textureName);
    if (!acquired) return 0;
    m_textureSrvs.emplace(key, *acquired);
    m_textureCacheHighWater = std::max(m_textureCacheHighWater, m_textureSrvs.size());
#if TD_DEBUG_ENABLED
    TD_LOG_DEBUG(
        "[ParticleRenderer] particle texture ready: '{}' srv={}",
        textureName, *acquired);
#endif
    return *acquired;
}


} // namespace engine::render
