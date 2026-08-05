#include "D3D12TextureStore.h"

#include "core/constants/Colors.h"
#include "debug/debug.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

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

[[nodiscard]] D3D12_RESOURCE_BARRIER transition(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

} // namespace

bool D3D12TextureStore::initialize(
    ID3D12Device* device,
    D3D12SrvDescriptorHeap& descriptors,
    D3D12FrameUploadArena& frameUploads,
    D3D12GpuRetirementQueue& retirementQueue) {
    shutdown();
    if (!device || descriptors.capacity() == 0u) return false;
    m_device = device;
    m_descriptors = &descriptors;
    m_frameUploads = &frameUploads;
    m_retirementQueue = &retirementQueue;

    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = 1u;
    texture.Height = 1u;
    texture.DepthOrArraySize = 1u;
    texture.MipLevels = 1u;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.SampleDesc.Count = 1u;
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    HRESULT result = device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &texture,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_fallbackTexture));
    if (FAILED(result)) {
        TD_LOG_ERROR("[D3D12TextureStore] Fallback texture allocation failed: 0x{:08X}", result);
        shutdown();
        return false;
    }
    static_cast<void>(m_fallbackTexture->SetName(
        L"D3D12 Fallback White Texture SRV 0"));

    const D3D12_RESOURCE_DESC upload = bufferDesc(
        D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    result = device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &upload,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_fallbackUpload));
    if (FAILED(result)) {
        TD_LOG_ERROR("[D3D12TextureStore] Fallback upload allocation failed: 0x{:08X}", result);
        shutdown();
        return false;
    }
    void* mapped = nullptr;
    result = m_fallbackUpload->Map(0, nullptr, &mapped);
    if (FAILED(result) || !mapped) {
        TD_LOG_ERROR("[D3D12TextureStore] Fallback upload map failed: 0x{:08X}", result);
        shutdown();
        return false;
    }
    const uint32_t white = COLOR_WHITE;
    std::memcpy(mapped, &white, sizeof(white));
    m_fallbackUpload->Unmap(0, nullptr);

    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1u;
    device->CreateShaderResourceView(
        m_fallbackTexture.Get(), &view, descriptors.cpuHandle(0u));
    m_fallbackUploadPending = true;
    return true;
}

void D3D12TextureStore::shutdown() noexcept {
    for (auto& texture : m_textures) texture.Reset();
    m_fallbackUpload.Reset();
    m_fallbackTexture.Reset();
    m_fallbackUploadPending = false;
    m_fallbackRecordedThisFrame = false;
    m_retirementQueue = nullptr;
    m_frameUploads = nullptr;
    m_descriptors = nullptr;
    m_device = nullptr;
}

void D3D12TextureStore::recordFallbackUpload(
    ID3D12GraphicsCommandList* commandList) noexcept {
    if (!m_fallbackUploadPending || !commandList || !m_fallbackTexture ||
        !m_fallbackUpload) return;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = m_fallbackUpload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    source.PlacedFootprint.Footprint.Width = 1u;
    source.PlacedFootprint.Footprint.Height = 1u;
    source.PlacedFootprint.Footprint.Depth = 1u;
    source.PlacedFootprint.Footprint.RowPitch =
        D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = m_fallbackTexture.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    const D3D12_RESOURCE_BARRIER barrier = transition(
        m_fallbackTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1u, &barrier);
    m_fallbackUploadPending = false;
    m_fallbackRecordedThisFrame = true;
}

void D3D12TextureStore::abortFrame() noexcept {
    if (!m_fallbackRecordedThisFrame) return;
    m_fallbackUploadPending = true;
    m_fallbackRecordedThisFrame = false;
}

uint32_t D3D12TextureStore::uploadRgba8(
    ID3D12GraphicsCommandList* commandList,
    uint32_t frameIndex,
    const void* pixels,
    uint32_t width,
    uint32_t height) {
    if (!pixels || width == 0u || height == 0u) return UINT32_MAX;
    const uint64_t rowPitch = static_cast<uint64_t>(width) * 4u;
    const uint64_t slicePitch = rowPitch * height;
    if (rowPitch > std::numeric_limits<uint32_t>::max() ||
        slicePitch > std::numeric_limits<uint32_t>::max()) return UINT32_MAX;
    const TextureSubresourceUpload subresource{
        .data = static_cast<const uint8_t*>(pixels),
        .rowPitch = static_cast<uint32_t>(rowPitch),
        .slicePitch = static_cast<uint32_t>(slicePitch),
    };
    return upload2D(commandList, frameIndex, width, height,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        container::Span<const TextureSubresourceUpload>(&subresource, 1u));
}

uint32_t D3D12TextureStore::upload2D(
    ID3D12GraphicsCommandList* commandList,
    uint32_t frameIndex,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    container::Span<const TextureSubresourceUpload> subresources) {
    if (!m_device || !m_descriptors || !m_frameUploads || !commandList ||
        width == 0u || height == 0u || format == DXGI_FORMAT_UNKNOWN ||
        subresources.empty() || subresources.size() >
            static_cast<size_t>(std::numeric_limits<uint16_t>::max())) {
        return UINT32_MAX;
    }
    uint32_t maximumMipLevels = 1u;
    for (uint32_t extent = std::max(width, height); extent > 1u; extent >>= 1u) {
        ++maximumMipLevels;
    }
    if (subresources.size() > maximumMipLevels) return UINT32_MAX;
    const uint32_t descriptorIndex = m_descriptors->allocate();
    if (descriptorIndex == UINT32_MAX) return UINT32_MAX;

    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = width;
    texture.Height = height;
    texture.DepthOrArraySize = 1u;
    texture.MipLevels = static_cast<uint16_t>(subresources.size());
    texture.Format = format;
    texture.SampleDesc.Count = 1u;
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    HRESULT result = m_device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &texture,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource));
    if (FAILED(result)) {
        m_descriptors->releaseImmediately(descriptorIndex);
        TD_LOG_ERROR("[D3D12TextureStore] Texture allocation failed: 0x{:08X}", result);
        return UINT32_MAX;
    }
    container::Vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subresources.size());
    container::Vector<UINT> rowCounts(subresources.size());
    container::Vector<UINT64> rowSizes(subresources.size());
    UINT64 uploadSize = 0u;
    m_device->GetCopyableFootprints(&texture, 0u,
        static_cast<UINT>(subresources.size()), 0u, layouts.data(),
        rowCounts.data(), rowSizes.data(), &uploadSize);
    if (uploadSize == 0u || uploadSize > std::numeric_limits<uint32_t>::max()) {
        m_descriptors->releaseImmediately(descriptorIndex);
        return UINT32_MAX;
    }
    FrameUploadAllocation upload = m_frameUploads->allocateUninitialized(
        frameIndex, static_cast<uint32_t>(uploadSize),
        D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (!upload) {
        m_descriptors->releaseImmediately(descriptorIndex);
        return UINT32_MAX;
    }
    for (size_t index = 0; index < subresources.size(); ++index) {
        const TextureSubresourceUpload& source = subresources[index];
        const uint64_t requiredSlice =
            static_cast<uint64_t>(source.rowPitch) * rowCounts[index];
        if (!source.data || source.rowPitch < rowSizes[index] ||
            source.slicePitch < requiredSlice) {
            m_descriptors->releaseImmediately(descriptorIndex);
            return UINT32_MAX;
        }
        uint8_t* destination = static_cast<uint8_t*>(upload.cpu) +
            layouts[index].Offset;
        for (UINT row = 0; row < rowCounts[index]; ++row) {
            std::memcpy(destination + static_cast<size_t>(row) *
                    layouts[index].Footprint.RowPitch,
                source.data + static_cast<size_t>(row) * source.rowPitch,
                static_cast<size_t>(rowSizes[index]));
        }
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = static_cast<UINT>(subresources.size());
    m_device->CreateShaderResourceView(
        resource.Get(), &view, m_descriptors->cpuHandle(descriptorIndex));
    m_textures[descriptorIndex] = resource;
    for (UINT index = 0; index < static_cast<UINT>(subresources.size()); ++index) {
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = upload.resource;
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = layouts[index];
        source.PlacedFootprint.Offset += upload.resourceOffset;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = resource.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = index;
        commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    const D3D12_RESOURCE_BARRIER barrier = transition(
        resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1u, &barrier);
    return descriptorIndex;
}

bool D3D12TextureStore::retire(
    uint32_t descriptorIndex,
    uint64_t requestFrame,
    bool pending,
    uint64_t immediateFence) {
    if (!m_descriptors || !m_retirementQueue ||
        descriptorIndex >= m_textures.size() || !m_textures[descriptorIndex] ||
        !m_descriptors->canRetire(descriptorIndex)) {
        if (m_retirementQueue) m_retirementQueue->reject();
        return false;
    }
    const SrvDescriptorRetirementMetadata metadata =
        m_descriptors->retirementMetadata(descriptorIndex);
    const uint64_t bytes = resourceAllocationBytes(
        m_textures[descriptorIndex].Get());
    m_retirementQueue->retireResourceAndDescriptor(
        std::move(m_textures[descriptorIndex]), bytes, descriptorIndex,
        requestFrame, metadata, pending, immediateFence);
    m_descriptors->markRetiring(descriptorIndex);
    return true;
}

uint64_t D3D12TextureStore::resourceAllocationBytes(
    ID3D12Resource* resource) const noexcept {
    if (!resource || !m_device) return 0u;
    const D3D12_RESOURCE_DESC description = resource->GetDesc();
    return m_device->GetResourceAllocationInfo(
        0u, 1u, &description).SizeInBytes;
}

} // namespace engine::d3d12
