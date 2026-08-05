#include "engine/renderer/world/pipeline/WorldRenderer.h"

#include "debug/debug.h"
#include "engine/renderer/d3d12/runtime/D3D12Device.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace engine::render {
namespace {

[[nodiscard]] D3D12_HEAP_PROPERTIES defaultHeapProperties() noexcept {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    return properties;
}

[[nodiscard]] D3D12_RESOURCE_BARRIER transitionBarrier(
    ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
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

bool WorldRenderer::createLocalVisibilityFallbackSrv() {
    if (!m_device || !m_device->getDevice()) return false;
    if (m_localVisibilityFallbackSrv != UINT32_MAX) return true;
    m_localVisibilityFallbackSrv = m_device->allocateSrvDescriptor();
    if (m_localVisibilityFallbackSrv == UINT32_MAX) {
        TD_LOG_ERROR("[WorldRenderer] Visibility fallback SRV allocation failed");
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC description{};
    description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    description.Format = DXGI_FORMAT_R8_UNORM;
    description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    description.Texture2D.MipLevels = 1;
    m_device->getDevice()->CreateShaderResourceView(
        nullptr, &description,
        m_device->getSrvCpuHandle(m_localVisibilityFallbackSrv));
    return true;
}

bool WorldRenderer::updateLocalVisibilityTexture(
    const LocalVisibilityRenderSnapshot& visibility) {
    m_localVisibilityEnabled = false;
    if (!visibility.isValid()) return true;
    if (!m_device || !m_device->getDevice() || !m_device->commandList()) return false;

    const uint32_t width = static_cast<uint32_t>(visibility.width);
    const uint32_t height = static_cast<uint32_t>(visibility.height);
    const bool recreate = !m_localVisibilityTexture ||
        m_localVisibilityWidth != width || m_localVisibilityHeight != height ||
        m_localVisibilityLayoutRevision != visibility.terrainLayoutRevision;
    const bool sameProjection =
        m_localVisibilityPresentationEpoch == visibility.presentationEpoch &&
        m_localVisibilityObserverPlayer == visibility.observerPlayer &&
        m_localVisibilityPolicyRevision == visibility.policyRevision;
    if (!recreate && sameProjection &&
        m_localVisibilityRevision == visibility.revision) {
        m_localVisibilityEnabled = true;
        return true;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> replacementTexture;
    uint32_t replacementSrv = UINT32_MAX;
    if (recreate) {
        D3D12_RESOURCE_DESC textureDescription{};
        textureDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDescription.Width = width;
        textureDescription.Height = height;
        textureDescription.DepthOrArraySize = 1;
        textureDescription.MipLevels = 1;
        textureDescription.Format = DXGI_FORMAT_R8_UNORM;
        textureDescription.SampleDesc.Count = 1;
        textureDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        const D3D12_HEAP_PROPERTIES heap = defaultHeapProperties();
        const HRESULT textureResult = m_device->getDevice()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &textureDescription,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&replacementTexture));
        if (FAILED(textureResult)) {
            TD_LOG_ERROR("[WorldRenderer] Visibility texture allocation failed: 0x{:08X}",
                         static_cast<uint32_t>(textureResult));
            return false;
        }
        replacementSrv = m_device->allocateSrvDescriptor();
        if (replacementSrv == UINT32_MAX) return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDescription{};
        srvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDescription.Format = DXGI_FORMAT_R8_UNORM;
        srvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDescription.Texture2D.MipLevels = 1;
        m_device->getDevice()->CreateShaderResourceView(
            replacementTexture.Get(), &srvDescription,
            m_device->getSrvCpuHandle(replacementSrv));
    }

    int32_t minX = 0;
    int32_t minY = 0;
    int32_t maxX = visibility.width - 1;
    int32_t maxY = visibility.height - 1;
    // Newest-only frame delivery may skip one or more authority revisions.
    // A dirty rectangle describes only its own revision, so partial upload is
    // safe solely for the immediately consecutive revision of the identical
    // observer/alliance projection. Every other change uploads the full grid.
    const bool consecutiveRevision = m_localVisibilityRevision != 0 &&
        visibility.revision == m_localVisibilityRevision + 1u;
    if (!recreate && sameProjection && consecutiveRevision &&
        visibility.dirtyRegion.isValid()) {
        const int32_t clippedMinX = std::max(visibility.dirtyRegion.minX, 0);
        const int32_t clippedMinY = std::max(visibility.dirtyRegion.minY, 0);
        const int32_t clippedMaxX = std::min(
            visibility.dirtyRegion.maxX, visibility.width - 1);
        const int32_t clippedMaxY = std::min(
            visibility.dirtyRegion.maxY, visibility.height - 1);
        if (clippedMinX <= clippedMaxX && clippedMinY <= clippedMaxY) {
            minX = clippedMinX;
            minY = clippedMinY;
            maxX = clippedMaxX;
            maxY = clippedMaxY;
        }
    }
    const uint32_t regionWidth = static_cast<uint32_t>(maxX - minX + 1);
    const uint32_t regionHeight = static_cast<uint32_t>(maxY - minY + 1);
    const uint64_t alignedRowPitch =
        (static_cast<uint64_t>(regionWidth) + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
        ~static_cast<uint64_t>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    if (alignedRowPitch > std::numeric_limits<uint32_t>::max()) {
        TD_LOG_ERROR("[WorldRenderer] Visibility upload row pitch is too large");
        if (replacementSrv != UINT32_MAX) m_device->freeSrvDescriptor(replacementSrv);
        return false;
    }
    const uint32_t rowPitch = static_cast<uint32_t>(alignedRowPitch);
    const uint64_t uploadSize = static_cast<uint64_t>(rowPitch) * regionHeight;
    if (uploadSize > std::numeric_limits<uint32_t>::max()) {
        TD_LOG_ERROR("[WorldRenderer] Visibility upload is too large");
        if (replacementSrv != UINT32_MAX) m_device->freeSrvDescriptor(replacementSrv);
        return false;
    }
    const d3d12::FrameUploadAllocation upload =
        m_device->allocateFrameUploadUninitialized(
            static_cast<uint32_t>(uploadSize),
            D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (!upload || !upload.resource) {
        TD_LOG_ERROR("[WorldRenderer] Visibility frame-upload allocation failed");
        if (replacementSrv != UINT32_MAX) m_device->freeSrvDescriptor(replacementSrv);
        return false;
    }
    auto* mapped = static_cast<uint8_t*>(upload.cpu);
    std::memset(mapped, 0, static_cast<size_t>(uploadSize));
    constexpr container::Array<uint8_t, 3> kEncodedStates{{0u, 127u, 255u}};
    const container::Span<const uint8_t> cells = visibility.cellValues();
    const container::Span<const uint8_t> visualLevels =
        visibility.visualLevelValues();
    const bool hasVisualLevels = visualLevels.size() == cells.size();
    for (uint32_t row = 0; row < regionHeight; ++row) {
        uint8_t* destination = mapped + static_cast<size_t>(row) * rowPitch;
        const size_t sourceOffset =
            static_cast<size_t>(minY + static_cast<int32_t>(row)) * width +
            static_cast<size_t>(minX);
        for (uint32_t column = 0; column < regionWidth; ++column) {
            destination[column] = hasVisualLevels
                ? visualLevels[sourceOffset + column]
                : kEncodedStates[std::min<size_t>(
                    cells[sourceOffset + column],
                    kEncodedStates.size() - 1u)];
        }
    }
    ID3D12GraphicsCommandList* commandList = m_device->commandList();
    ID3D12Resource* destinationTexture = recreate
        ? replacementTexture.Get() : m_localVisibilityTexture.Get();
    D3D12_RESOURCE_STATES destinationState = recreate
        ? D3D12_RESOURCE_STATE_COPY_DEST : m_localVisibilityState;
    if (destinationState != D3D12_RESOURCE_STATE_COPY_DEST) {
        const D3D12_RESOURCE_BARRIER barrier = transitionBarrier(
            destinationTexture, destinationState,
            D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->ResourceBarrier(1, &barrier);
    }
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = destinationTexture;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload.resource;
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = upload.resourceOffset;
    source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
    source.PlacedFootprint.Footprint.Width = regionWidth;
    source.PlacedFootprint.Footprint.Height = regionHeight;
    source.PlacedFootprint.Footprint.Depth = 1;
    source.PlacedFootprint.Footprint.RowPitch = rowPitch;
    commandList->CopyTextureRegion(
        &destination, static_cast<UINT>(minX), static_cast<UINT>(minY), 0,
        &source, nullptr);
    const D3D12_RESOURCE_BARRIER finish = transitionBarrier(
        destinationTexture, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &finish);
    m_localVisibilityState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    if (recreate) {
        if (m_localVisibilitySrv != UINT32_MAX) {
            m_device->freeSrvDescriptor(m_localVisibilitySrv);
        }
        if (m_localVisibilityTexture) {
            m_device->retireResource(std::move(m_localVisibilityTexture));
        }
        m_localVisibilityTexture = std::move(replacementTexture);
        m_localVisibilitySrv = replacementSrv;
    }

    m_localVisibilityWidth = width;
    m_localVisibilityHeight = height;
    m_localVisibilityPresentationEpoch = visibility.presentationEpoch;
    m_localVisibilityRevision = visibility.revision;
    m_localVisibilityPolicyRevision = visibility.policyRevision;
    m_localVisibilityLayoutRevision = visibility.terrainLayoutRevision;
    m_localVisibilityObserverPlayer = visibility.observerPlayer;
    m_localVisibilityEnabled = true;
    m_lastStaticMeshStats.visibilityUploadedCells =
        static_cast<uint32_t>(std::min<uint64_t>(
            static_cast<uint64_t>(regionWidth) * regionHeight,
            std::numeric_limits<uint32_t>::max()));
    m_lastStaticMeshStats.visibilityUploadedBytes = uploadSize;
    return true;
}

void WorldRenderer::releaseLocalVisibilityResources() noexcept {
    m_localVisibilityEnabled = false;
    if (m_device && m_device->getDevice()) {
        if (m_localVisibilitySrv != UINT32_MAX) {
            m_device->freeSrvDescriptor(m_localVisibilitySrv);
        }
        if (m_localVisibilityFallbackSrv != UINT32_MAX) {
            m_device->freeSrvDescriptor(m_localVisibilityFallbackSrv);
        }
    }
    m_localVisibilitySrv = UINT32_MAX;
    m_localVisibilityFallbackSrv = UINT32_MAX;
    m_localVisibilityTexture.Reset();
    m_localVisibilityState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_localVisibilityWidth = 0;
    m_localVisibilityHeight = 0;
    m_localVisibilityPresentationEpoch = 0;
    m_localVisibilityRevision = 0;
    m_localVisibilityPolicyRevision = 0;
    m_localVisibilityLayoutRevision = 0;
    m_localVisibilityObserverPlayer = UINT8_MAX;
}

WorldLocalVisibilityGpuBinding WorldRenderer::localVisibilityGpuBinding(
    const LocalVisibilityRenderSnapshot& localVisibility) const noexcept {
    WorldLocalVisibilityGpuBinding binding;
    if (!m_device) return binding;
    if (localVisibility.hasPlayableBounds()) {
        binding.playableMinimum = {
            localVisibility.playableMinimum.x(),
            localVisibility.playableMinimum.y(),
        };
        binding.playableMaximum = {
            localVisibility.playableMaximum.x(),
            localVisibility.playableMaximum.y(),
        };
        binding.playableBoundsEnabled = true;
    }
    if (!localVisibility.isValid()) return binding;
    const uint32_t srvIndex =
        m_localVisibilityEnabled && m_localVisibilitySrv != UINT32_MAX
            ? m_localVisibilitySrv
            : m_localVisibilityFallbackSrv;
    if (srvIndex == UINT32_MAX) return binding;
    binding.textureSrv = m_device->getSrvGpuHandle(srvIndex);
    binding.origin = {localVisibility.originX, localVisibility.originY};
    binding.textureSize = {
        static_cast<float>(localVisibility.width),
        static_cast<float>(localVisibility.height),
    };
    binding.inverseCellSize = 1.0f / localVisibility.cellWorldSize;
    binding.enabled = true;
    return binding;
}

} // namespace engine::render
