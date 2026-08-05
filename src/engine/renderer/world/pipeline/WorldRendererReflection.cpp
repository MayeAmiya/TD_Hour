#include "engine/renderer/world/pipeline/WorldRenderer.h"

#include "engine/renderer/d3d12/runtime/D3D12Device.h"

#include <cmath>
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

bool WorldRenderer::ensureWaterReflectionResources() {
    // The renderer is converging on a single-sample world contract. Do not
    // grow a second MSAA/resolve lifecycle solely for this legacy 256 target.
    // Until the old option is removed globally, fail closed when its PSO is
    // incompatible with this fixed single-sample pass.
    if (!m_device || !m_device->getDevice() || m_sampleCount != 1u) return false;
    if (m_waterReflectionTarget && m_waterReflectionTexture &&
        m_waterReflectionDepth && m_waterReflectionRtvHeap &&
        m_waterReflectionDsvHeap && m_waterReflectionSrv != UINT32_MAX) {
        return true;
    }
    releaseWaterReflectionResources();

    ID3D12Device* device = m_device->getDevice();
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDescription{};
    rtvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDescription.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(
            &rtvHeapDescription,
            IID_PPV_ARGS(&m_waterReflectionRtvHeap)))) {
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDescription{};
    dsvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDescription.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(
            &dsvHeapDescription,
            IID_PPV_ARGS(&m_waterReflectionDsvHeap)))) {
        releaseWaterReflectionResources();
        return false;
    }

    const D3D12_HEAP_PROPERTIES heap = defaultHeapProperties();
    D3D12_RESOURCE_DESC colorDescription{};
    colorDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    colorDescription.Width = kWaterReflectionSize;
    colorDescription.Height = kWaterReflectionSize;
    colorDescription.DepthOrArraySize = 1;
    colorDescription.MipLevels = 1;
    colorDescription.Format = d3d12::D3D12Device::SWAP_FORMAT;
    colorDescription.SampleDesc.Count = 1;
    colorDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    colorDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE colorClear{};
    colorClear.Format = d3d12::D3D12Device::SWAP_FORMAT;
    colorClear.Color[3] = 1.0f;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &colorDescription,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &colorClear,
            IID_PPV_ARGS(&m_waterReflectionTarget)))) {
        releaseWaterReflectionResources();
        return false;
    }
    device->CreateRenderTargetView(
        m_waterReflectionTarget.Get(), nullptr,
        m_waterReflectionRtvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_RESOURCE_DESC sampledDescription = colorDescription;
    sampledDescription.SampleDesc.Count = 1;
    sampledDescription.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &sampledDescription,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&m_waterReflectionTexture)))) {
        releaseWaterReflectionResources();
        return false;
    }
    m_waterReflectionSrv = m_device->allocateSrvDescriptor();
    if (m_waterReflectionSrv == UINT32_MAX) {
        releaseWaterReflectionResources();
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDescription{};
    srvDescription.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDescription.Format = d3d12::D3D12Device::SWAP_FORMAT;
    srvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDescription.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(
        m_waterReflectionTexture.Get(), &srvDescription,
        m_device->getSrvCpuHandle(m_waterReflectionSrv));

    D3D12_RESOURCE_DESC depthDescription = colorDescription;
    depthDescription.Format = d3d12::D3D12Device::DEPTH_FORMAT;
    depthDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format = d3d12::D3D12Device::DEPTH_FORMAT;
    depthClear.DepthStencil.Depth = 1.0f;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &depthDescription,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
            IID_PPV_ARGS(&m_waterReflectionDepth)))) {
        releaseWaterReflectionResources();
        return false;
    }
    device->CreateDepthStencilView(
        m_waterReflectionDepth.Get(), nullptr,
        m_waterReflectionDsvHeap->GetCPUDescriptorHandleForHeapStart());
    m_waterReflectionTargetState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_waterReflectionTextureState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    return true;
}

void WorldRenderer::releaseWaterReflectionResources() noexcept {
    m_waterReflectionValid = false;
    if (m_waterReflectionSrv != UINT32_MAX && m_device &&
        m_device->getDevice()) {
        m_device->freeSrvDescriptor(m_waterReflectionSrv);
    }
    m_waterReflectionSrv = UINT32_MAX;
    if (m_device) {
        if (m_waterReflectionTarget) {
            m_device->retireResource(std::move(m_waterReflectionTarget));
        }
        if (m_waterReflectionTexture) {
            m_device->retireResource(std::move(m_waterReflectionTexture));
        }
        if (m_waterReflectionDepth) {
            m_device->retireResource(std::move(m_waterReflectionDepth));
        }
    } else {
        m_waterReflectionTarget.Reset();
        m_waterReflectionTexture.Reset();
        m_waterReflectionDepth.Reset();
    }
    m_waterReflectionRtvHeap.Reset();
    m_waterReflectionDsvHeap.Reset();
    m_waterReflectionTargetState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_waterReflectionTextureState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_waterReflectionViewProjection = {};
}

bool WorldRenderer::renderWaterReflection(
    container::Span<const StaticMeshDrawPacket> drawPackets,
    const RenderCameraSnapshot& cameraSnapshot, float planeZ,
    const WorldLightEnvironment& lightEnvironment,
    const LocalVisibilityRenderSnapshot& localVisibility,
    float worldTimeSeconds,
    container::Span<const DynamicPointLightRenderData> dynamicPointLights,
    container::Span<const TerrainPointLightRenderData> scenePointLights) {
    m_waterReflectionValid = false;
    if (!m_initialized || !m_device || !std::isfinite(planeZ) ||
        !ensureWaterReflectionResources()) {
        return false;
    }
    ID3D12GraphicsCommandList* commandList = m_device->commandList();
    if (!commandList) return false;

    m_reflectionDrawScratch.clear();
    m_reflectionDrawScratch.reserve(drawPackets.size());
    for (const StaticMeshDrawPacket& source : drawPackets) {
        if (source.waterSurface) continue;
        StaticMeshDrawPacket caster = source;
        caster.castsShadow = false;
        caster.receivesShadow = false;
        m_reflectionDrawScratch.push_back(std::move(caster));
    }
    if (m_reflectionDrawScratch.empty()) return false;

    RenderCameraSnapshot mirrored = cameraSnapshot;
    mirrored.position = {
        cameraSnapshot.position.x(), cameraSnapshot.position.y(),
        planeZ * 2.0f - cameraSnapshot.position.z()};
    mirrored.target = {
        cameraSnapshot.target.x(), cameraSnapshot.target.y(),
        planeZ * 2.0f - cameraSnapshot.target.z()};
    mirrored.up = {
        cameraSnapshot.up.x(), cameraSnapshot.up.y(),
        -cameraSnapshot.up.z()};
    mirrored.tacticalViewportHeightScale = 1.0f;
    const WorldCamera mirrorCamera = WorldCamera::fromSnapshot(mirrored);
    m_waterReflectionViewProjection = mirrorCamera.viewProjectionMatrix(1.0f);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        m_waterReflectionRtvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv =
        m_waterReflectionDsvHeap->GetCPUDescriptorHandleForHeapStart();
    constexpr float clearColor[4]{0.0f, 0.0f, 0.0f, 1.0f};
    commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    commandList->ClearDepthStencilView(
        dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    const StaticMeshRenderStats savedStats = m_lastStaticMeshStats;
    renderStaticMeshes(
        m_reflectionDrawScratch, mirrored, lightEnvironment,
        localVisibility, worldTimeSeconds, dynamicPointLights,
        scenePointLights, StaticMeshPassExecution::Reflection);
    m_lastStaticMeshStats = savedStats;

    constexpr D3D12_RESOURCE_STATES transferSource =
        D3D12_RESOURCE_STATE_COPY_SOURCE;
    constexpr D3D12_RESOURCE_STATES transferDestination =
        D3D12_RESOURCE_STATE_COPY_DEST;
    container::Array<D3D12_RESOURCE_BARRIER, 2> barriers{{
        transitionBarrier(m_waterReflectionTarget.Get(),
                          m_waterReflectionTargetState, transferSource),
        transitionBarrier(m_waterReflectionTexture.Get(),
                          m_waterReflectionTextureState,
                          transferDestination),
    }};
    commandList->ResourceBarrier(
        static_cast<UINT>(barriers.size()), barriers.data());
    commandList->CopyResource(
        m_waterReflectionTexture.Get(), m_waterReflectionTarget.Get());
    barriers = {{
        transitionBarrier(m_waterReflectionTarget.Get(), transferSource,
                          D3D12_RESOURCE_STATE_RENDER_TARGET),
        transitionBarrier(m_waterReflectionTexture.Get(), transferDestination,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
    }};
    commandList->ResourceBarrier(
        static_cast<UINT>(barriers.size()), barriers.data());
    m_waterReflectionTargetState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_waterReflectionTextureState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_device->bindMainRenderTargets();
    m_waterReflectionValid = true;
    m_reflectionDrawScratch.clear();
    return true;
}

} // namespace engine::render
