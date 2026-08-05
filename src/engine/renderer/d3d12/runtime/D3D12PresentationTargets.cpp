#include "D3D12PresentationTargets.h"

#include "D3D12QualitySettings.h"
#include "debug/debug.h"

namespace engine::d3d12 {
namespace {

[[nodiscard]] D3D12_RESOURCE_BARRIER transition(
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

bool D3D12PresentationTargets::initialize(
    ID3D12Device* device, IDXGISwapChain3* swapChain,
    uint32_t width, uint32_t height,
    DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat) {
    shutdown();
    if (!device || !swapChain || width == 0u || height == 0u) return false;
    m_device = device;
    m_swapChain = swapChain;
    m_colorFormat = colorFormat;
    m_depthFormat = depthFormat;
    return recreate(width, height);
}

void D3D12PresentationTargets::shutdown() noexcept {
    releaseForResize();
    m_rtvHeap.Reset();
    m_dsvHeap.Reset();
    m_device = nullptr;
    m_swapChain = nullptr;
    m_colorFormat = DXGI_FORMAT_UNKNOWN;
    m_depthFormat = DXGI_FORMAT_UNKNOWN;
    m_width = m_height = 0u;
    m_sampleCount = m_requestedSampleCount = 1u;
}

void D3D12PresentationTargets::releaseForResize() noexcept {
    for (auto& value : m_backBuffers) value.Reset();
    for (auto& value : m_depthTargets) value.Reset();
    for (auto& value : m_multisampleColors) value.Reset();
    for (auto& value : m_multisampleDepths) value.Reset();
    for (auto& value : m_captureTargets) value.Reset();
    m_readyCaptureIndex = UINT32_MAX;
    m_captureRequested = false;
    m_multisamplePassActive = false;
    m_valid = false;
}

bool D3D12PresentationTargets::recreate(uint32_t width, uint32_t height) {
    if (!m_device || !m_swapChain || width == 0u || height == 0u) return false;
    releaseForResize();
    m_width = width;
    m_height = height;
    m_valid = createBackBuffersAndCapture() && createDepthTargets() &&
        createMultisampleTargets();
    return m_valid;
}

bool D3D12PresentationTargets::createBackBuffersAndCapture() {
    if (!m_rtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.NumDescriptors = kFrameCount * 2u;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(m_device->CreateDescriptorHeap(
                &heap, IID_PPV_ARGS(&m_rtvHeap)))) return false;
        m_rtvStride = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    for (uint32_t index = 0; index < kFrameCount; ++index) {
        if (FAILED(m_swapChain->GetBuffer(
                index, IID_PPV_ARGS(&m_backBuffers[index])))) return false;
        m_device->CreateRenderTargetView(m_backBuffers[index].Get(), nullptr, handle);
        handle.ptr += m_rtvStride;
        const D3D12_RESOURCE_DESC description = m_backBuffers[index]->GetDesc();
        if (FAILED(m_device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&m_captureTargets[index])))) return false;
    }
    return true;
}

bool D3D12PresentationTargets::createDepthTargets() {
    if (!m_dsvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.NumDescriptors = kFrameCount * 2u;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(m_device->CreateDescriptorHeap(
                &heap, IID_PPV_ARGS(&m_dsvHeap)))) return false;
        m_dsvStride = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    }
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = m_width;
    description.Height = m_height;
    description.DepthOrArraySize = 1u;
    description.MipLevels = 1u;
    description.Format = m_depthFormat;
    description.SampleDesc.Count = 1u;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{.Format = m_depthFormat};
    clear.DepthStencil.Depth = 1.0f;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t index = 0; index < kFrameCount; ++index) {
        if (FAILED(m_device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                IID_PPV_ARGS(&m_depthTargets[index])))) return false;
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = m_depthFormat;
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_device->CreateDepthStencilView(m_depthTargets[index].Get(), &view, handle);
        handle.ptr += m_dsvStride;
    }
    return true;
}

bool D3D12PresentationTargets::createMultisampleTargets() {
    if (m_sampleCount <= 1u) return true;
    D3D12_RESOURCE_DESC color{};
    color.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    color.Width = m_width;
    color.Height = m_height;
    color.DepthOrArraySize = 1u;
    color.MipLevels = 1u;
    color.Format = m_colorFormat;
    color.SampleDesc.Count = m_sampleCount;
    color.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_RESOURCE_DESC depth = color;
    depth.Format = m_depthFormat;
    depth.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE depthClear{.Format = m_depthFormat};
    depthClear.DepthStencil.Depth = 1.0f;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += kFrameCount * m_rtvStride;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv =
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    dsv.ptr += kFrameCount * m_dsvStride;
    for (uint32_t index = 0; index < kFrameCount; ++index) {
        if (FAILED(m_device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &color,
                D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                IID_PPV_ARGS(&m_multisampleColors[index])))) return false;
        m_device->CreateRenderTargetView(m_multisampleColors[index].Get(), nullptr, rtv);
        rtv.ptr += m_rtvStride;
        if (FAILED(m_device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &depth,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
                IID_PPV_ARGS(&m_multisampleDepths[index])))) return false;
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = m_depthFormat;
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
        m_device->CreateDepthStencilView(m_multisampleDepths[index].Get(), &view, dsv);
        dsv.ptr += m_dsvStride;
    }
    return true;
}

uint32_t D3D12PresentationTargets::configureMultisampling(uint32_t requested) {
    requested = requestedMultisampleCount(requested);
    if (!m_device) return 1u;
    if (requested == m_requestedSampleCount) return m_sampleCount;
    uint32_t selected = 1u;
    for (uint32_t candidate = requested; candidate >= 2u; candidate /= 2u) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS color{
            .Format = m_colorFormat, .SampleCount = candidate};
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS depth{
            .Format = m_depthFormat, .SampleCount = candidate};
        if (SUCCEEDED(m_device->CheckFeatureSupport(
                D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &color, sizeof(color))) &&
            SUCCEEDED(m_device->CheckFeatureSupport(
                D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &depth, sizeof(depth))) &&
            color.NumQualityLevels && depth.NumQualityLevels) {
            selected = candidate;
            break;
        }
    }
    m_requestedSampleCount = requested;
    if (selected == m_sampleCount) return selected;
    m_sampleCount = selected;
    for (auto& value : m_multisampleColors) value.Reset();
    for (auto& value : m_multisampleDepths) value.Reset();
    if (!createMultisampleTargets()) m_sampleCount = 1u;
    return m_sampleCount;
}

ID3D12Resource* D3D12PresentationTargets::backBuffer(uint32_t index) const noexcept {
    return index < kFrameCount ? m_backBuffers[index].Get() : nullptr;
}

ID3D12Resource* D3D12PresentationTargets::currentColorTarget(
    uint32_t index) const noexcept {
    if (index >= kFrameCount) return nullptr;
    return m_multisamplePassActive
        ? m_multisampleColors[index].Get() : m_backBuffers[index].Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12PresentationTargets::backBufferRtv(
    uint32_t index) const noexcept {
    auto handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += index * m_rtvStride;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12PresentationTargets::backBufferDsv(
    uint32_t index) const noexcept {
    auto handle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += index * m_dsvStride;
    return handle;
}

void D3D12PresentationTargets::bind(
    ID3D12GraphicsCommandList* list, uint32_t index) const noexcept {
    if (!list || index >= kFrameCount) return;
    const uint32_t targetIndex = m_multisamplePassActive
        ? kFrameCount + index : index;
    auto rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += targetIndex * m_rtvStride;
    auto dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    dsv.ptr += targetIndex * m_dsvStride;
    list->OMSetRenderTargets(1u, &rtv, FALSE, &dsv);
}

bool D3D12PresentationTargets::beginWorldPass(
    ID3D12GraphicsCommandList* list, uint32_t index,
    const container::Array<float, 4>& clearColor,
    render::WorldResourceStateRenderStats& stats) {
    ++stats.beginCalls;
    if (!list || index >= kFrameCount) {
        ++stats.beginFailures;
        return false;
    }
    if (m_sampleCount <= 1u) {
        m_multisamplePassActive = false;
        stats.worldPassBegan = true;
        stats.multisamplePassActive = false;
        stats.presentationTargetRestored = true;
        bind(list, index);
        return true;
    }
    if (!m_multisampleColors[index] || !m_multisampleDepths[index]) {
        ++stats.beginFailures;
        return false;
    }
    m_multisamplePassActive = true;
    stats.worldPassBegan = true;
    stats.multisamplePassActive = true;
    stats.presentationTargetRestored = false;
    auto rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (kFrameCount + index) * m_rtvStride;
    auto dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    dsv.ptr += (kFrameCount + index) * m_dsvStride;
    list->ClearRenderTargetView(rtv, clearColor.data(), 0u, nullptr);
    list->ClearDepthStencilView(
        dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0u, 0u, nullptr);
    list->OMSetRenderTargets(1u, &rtv, FALSE, &dsv);
    return true;
}

void D3D12PresentationTargets::resolveWorldPass(
    ID3D12GraphicsCommandList* list, uint32_t index,
    render::WorldResourceStateRenderStats& stats) {
    ++stats.resolveCalls;
    if (m_sampleCount <= 1u) {
        ++stats.resolveNoopSingleSample;
        stats.presentationTargetRestored = true;
        return;
    }
    if (!m_multisamplePassActive) {
        ++stats.resolveNoopInactive;
        return;
    }
    ID3D12Resource* multisample = m_multisampleColors[index].Get();
    ID3D12Resource* back = m_backBuffers[index].Get();
    if (!list || !multisample || !back) {
        ++stats.resolveFailures;
        return;
    }
    container::Array<D3D12_RESOURCE_BARRIER, 2> barriers{
        transition(multisample, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RESOLVE_SOURCE),
        transition(back, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RESOLVE_DEST),
    };
    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    stats.transitionBarriers += static_cast<uint32_t>(barriers.size());
    list->ResolveSubresource(back, 0u, multisample, 0u, m_colorFormat);
    barriers = {
        transition(multisample, D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET),
        transition(back, D3D12_RESOURCE_STATE_RESOLVE_DEST,
            D3D12_RESOURCE_STATE_RENDER_TARGET),
    };
    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    stats.transitionBarriers += static_cast<uint32_t>(barriers.size());
    ++stats.resolveExecutions;
    m_multisamplePassActive = false;
    stats.multisamplePassActive = false;
    stats.presentationTargetRestored = true;
    bind(list, index);
}

bool D3D12PresentationTargets::requestCapture() noexcept {
    if (!m_valid || m_captureRequested || m_readyCaptureIndex < kFrameCount) return false;
    m_captureRequested = true;
    return true;
}

ID3D12Resource* D3D12PresentationTargets::captureTarget(uint32_t index) const noexcept {
    return index < kFrameCount ? m_captureTargets[index].Get() : nullptr;
}

void D3D12PresentationTargets::markCaptureReady(uint32_t index) noexcept {
    m_readyCaptureIndex = index;
    m_captureRequested = false;
}

ID3D12Resource* D3D12PresentationTargets::consumeReadyCapture() noexcept {
    if (m_readyCaptureIndex >= kFrameCount) return nullptr;
    ID3D12Resource* result = m_captureTargets[m_readyCaptureIndex].Get();
    m_readyCaptureIndex = UINT32_MAX;
    return result;
}

} // namespace engine::d3d12
