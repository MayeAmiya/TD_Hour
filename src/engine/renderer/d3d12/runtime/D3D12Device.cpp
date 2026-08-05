#include "core/container/container_types.h"
#include "D3D12Device.h"
#include "core/platform/runtime_threads.h"
#include <dxgi1_4.h>
#include <d3d12sdklayers.h>

#include <cstring>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <future>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

// Helper: create a buffer resource descriptor
static D3D12_RESOURCE_DESC makeBufferDesc(UINT64 size) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

// Helper: create upload heap properties
static D3D12_HEAP_PROPERTIES makeUploadHeap() {
    D3D12_HEAP_PROPERTIES props = {};
    props.Type = D3D12_HEAP_TYPE_UPLOAD;
    return props;
}

static D3D12_HEAP_PROPERTIES makeReadbackHeap() {
    D3D12_HEAP_PROPERTIES props = {};
    props.Type = D3D12_HEAP_TYPE_READBACK;
    return props;
}

// Helper: create a transition barrier
static D3D12_RESOURCE_BARRIER makeTransition(ID3D12Resource* res,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = res;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

namespace engine::d3d12 {

D3D12Device::~D3D12Device() {
    shutdown();
}

bool D3D12Device::init(void* hwnd, uint32_t width, uint32_t height, bool fullscreen) {
    TD_LOG_INFO("[D3D12Device] Initializing...");

    if (m_device || m_commandQueue || m_swapChain) shutdown();
    const auto fail = [this]() {
        shutdown();
        return false;
    };

    if (!createDevice()) return false;
    if (!m_staticBufferPool.initialize(m_device.Get())) return fail();
    if (!createCommandQueue()) return fail();
    if (!createSwapChain(hwnd, width, height, fullscreen)) return fail();
    if (!m_presentationTargets.initialize(
            m_device.Get(), m_swapChain.Get(), width, height,
            SWAP_FORMAT, DEPTH_FORMAT)) return fail();
    if (!m_fenceTimeline.initialize(m_device.Get(), m_commandQueue.Get())) {
        return fail();
    }
    for (auto& value : m_fenceValues) value = 0u;
    m_frameOrdinalsByContext.fill(0u);
    m_unfencedSubmissions.fill(false);
    // Timestamp diagnostics are optional: unsupported or failed measurement
    // infrastructure must never prevent the renderer from starting.
    static_cast<void>(m_gpuTimestampOwner.initialize(
        m_device.Get(), m_commandQueue.Get()));
    if (!m_uiPipeline.initialize(m_device.Get(), SWAP_FORMAT, DEPTH_FORMAT)) {
        return fail();
    }
    if (!createBuffers()) return fail();
    if (!createSrvHeap()) return fail();

    TD_LOG_INFO("[D3D12Device] Init complete: {}x{}", width, height);
    return true;
}

void D3D12Device::shutdown() {
    if (!m_device && !m_commandQueue && !m_swapChain) return;
    TD_LOG_INFO("[D3D12Device] Shutting down...");
    if (m_displayGammaApplied) {
        static_cast<void>(configureDisplayGamma(1.0f));
    }
    // Close any unsubmitted lists before their allocators are released. Only
    // previously submitted queue work is covered by the following wait.
    abortOpenFrame();
    if (!waitForGpu()) {
        TD_LOG_WARN(
            "[D3D12Device] GPU wait failed; releasing resources in degraded teardown");
    }
    // Resource destruction can surface lifetime/state diagnostics after the
    // final Present. Drain once more before releasing the InfoQueue so an
    // orderly shutdown is part of log-based validation coverage.
    drainInfoQueue();

    m_fenceTimeline.shutdown();
    m_uiBatch.shutdown();
    m_frameUploadArena.shutdown();
    m_gpuTimestampOwner.shutdown();

    for (auto& uploads : m_inFlightUploads) uploads.clear();
    m_staticBufferPool.shutdown();
    m_retirementQueue.clear();
    m_completedRetiredDescriptors.clear();
    m_completedRetiredStaticBuffers.clear();
    m_textureStore.shutdown();
    m_srvDescriptors.shutdown();
    m_currentWorldResourceStateStats = {};
    m_frameOrdinal = 0;
    m_uiPipeline.shutdown();
    m_presentationTargets.shutdown();
    m_commandList.Reset();
    for (auto& list : m_primaryCommandLists) list.Reset();
    for (auto& list : m_parallelCommandLists) list.Reset();
    m_temporaryCommandList.Reset();
    for (FrameCommandContext& context : m_frameCommandContexts) {
        for (auto& allocator : context.primaryAllocators) allocator.Reset();
        for (auto& allocator : context.workerAllocators) allocator.Reset();
    }
    m_temporaryCommandAllocator.Reset();
    m_swapChain.Reset();
    m_commandQueue.Reset();
    // Capture diagnostics emitted by the resource/descriptor destruction
    // above before the debug interfaces themselves are released.
    drainInfoQueue();
    m_infoQueue.Reset();
    m_device.Reset();
    m_unfencedSubmissions.fill(false);
    m_frameSubmissionLists.fill(nullptr);
    m_frameSubmissionListCount = 0;
    m_primaryCommandSegment = 0;
    m_parallelRecordingUsed = false;
    m_frameOpen = false;

    TD_LOG_INFO("[D3D12Device] Shutdown complete");
}

bool D3D12Device::createDevice() {
    ComPtr<IDXGIFactory4> factory;
    UINT flags = 0;
#if TD_DEBUG_ENABLED
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            ComPtr<ID3D12Debug1> debugController1;
            if (SUCCEEDED(debugController.As(&debugController1))) {
                debugController1->SetEnableGPUBasedValidation(TRUE);
                TD_LOG_INFO("[D3D12Device] GPU-based validation enabled");
            } else {
                TD_LOG_WARN("[D3D12Device] GPU-based validation interface unavailable");
            }
            flags = DXGI_CREATE_FACTORY_DEBUG;
            TD_LOG_INFO("[D3D12Device] Debug layer enabled");
        }
    }
#endif

    HRESULT hr = CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        TD_LOG_ERROR("[D3D12Device] CreateDXGIFactory2 failed: 0x{:08X}", hr);
        return false;
    }

    ComPtr<IDXGIAdapter1> bestAdapter;
    SIZE_T bestMemory = 0;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr);
        if (SUCCEEDED(hr) && desc.DedicatedVideoMemory > bestMemory) {
            bestMemory = desc.DedicatedVideoMemory;
            bestAdapter = adapter;
            char name[128] = {};
            for (size_t j = 0; j + 1 < std::size(name) && desc.Description[j]; ++j)
                name[j] = static_cast<char>(desc.Description[j]);
            TD_LOG_INFO("[D3D12Device] Adapter {}: {} ({}MB)", i, name, desc.DedicatedVideoMemory / (1024*1024));
        }
    }

    if (!bestAdapter) {
        TD_LOG_WARN("[D3D12Device] No hardware adapter, using WARP");
        factory->EnumWarpAdapter(IID_PPV_ARGS(&bestAdapter));
    }

    hr = D3D12CreateDevice(bestAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
    if (FAILED(hr)) {
        TD_LOG_ERROR("[D3D12Device] D3D12CreateDevice failed: 0x{:08X}", hr);
        return false;
    }
    TD_LOG_INFO("[D3D12Device] Device created");

#if TD_DEBUG_ENABLED
    {
        m_device.As(&m_infoQueue);
        if (m_infoQueue) {
            TD_LOG_INFO("[D3D12Device] InfoQueue configured, will drain each frame");
        }
    }
#endif
    return true;
}

bool D3D12Device::createCommandQueue() {
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HRESULT hr = m_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_commandQueue));
    if (FAILED(hr)) { TD_LOG_ERROR("[D3D12Device] CreateCommandQueue failed: 0x{:08X}", hr); return false; }

    m_parallelWorkerCount = std::clamp(
        platform::runtime::renderWorkerCount(), 1u,
        MAX_PARALLEL_GRAPHICS_WORKERS);
    for (uint32_t frame = 0; frame < FRAME_COUNT; ++frame) {
        FrameCommandContext& context = m_frameCommandContexts[frame];
        for (uint32_t segment = 0; segment < PRIMARY_COMMAND_SEGMENT_COUNT;
             ++segment) {
            hr = m_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&context.primaryAllocators[segment]));
            if (FAILED(hr)) {
                TD_LOG_ERROR(
                    "[D3D12Device] Primary command allocator creation failed: frame={} segment={} hr=0x{:08X}",
                    frame, segment, hr);
                return false;
            }
        }
        for (uint32_t worker = 0; worker < m_parallelWorkerCount; ++worker) {
            hr = m_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&context.workerAllocators[worker]));
            if (FAILED(hr)) {
                TD_LOG_ERROR(
                    "[D3D12Device] Worker command allocator creation failed: frame={} worker={} hr=0x{:08X}",
                    frame, worker, hr);
                return false;
            }
        }
    }

    const auto createClosedList =
        [this](ID3D12CommandAllocator* allocator,
               ComPtr<ID3D12GraphicsCommandList>& list) -> HRESULT {
            HRESULT result = m_device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                IID_PPV_ARGS(&list));
            if (SUCCEEDED(result)) result = list->Close();
            return result;
        };
    for (uint32_t segment = 0; segment < PRIMARY_COMMAND_SEGMENT_COUNT;
         ++segment) {
        hr = createClosedList(
            m_frameCommandContexts[0].primaryAllocators[segment].Get(),
            m_primaryCommandLists[segment]);
        if (FAILED(hr)) {
            TD_LOG_ERROR(
                "[D3D12Device] Primary command list creation failed: segment={} hr=0x{:08X}",
                segment, hr);
            return false;
        }
    }
    for (uint32_t worker = 0; worker < m_parallelWorkerCount; ++worker) {
        hr = createClosedList(
            m_frameCommandContexts[0].workerAllocators[worker].Get(),
            m_parallelCommandLists[worker]);
        if (FAILED(hr)) {
            TD_LOG_ERROR(
                "[D3D12Device] Worker command list creation failed: worker={} hr=0x{:08X}",
                worker, hr);
            return false;
        }
    }
    hr = m_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_temporaryCommandAllocator));
    if (FAILED(hr)) {
        TD_LOG_ERROR(
            "[D3D12Device] Temporary command allocator creation failed: 0x{:08X}",
            hr);
        return false;
    }
    hr = createClosedList(
        m_temporaryCommandAllocator.Get(), m_temporaryCommandList);
    if (FAILED(hr)) {
        TD_LOG_ERROR(
            "[D3D12Device] Temporary command list creation failed: 0x{:08X}",
            hr);
        return false;
    }
    m_commandList = m_primaryCommandLists[0];

    TD_LOG_INFO(
        "[D3D12Device] Command queue created (parallel workers={})",
        m_parallelWorkerCount);
    return true;
}

bool D3D12Device::createSwapChain(void* hwnd, uint32_t width, uint32_t height, bool fullscreen) {
    ComPtr<IDXGIFactory4> factory;
    const HRESULT factoryResult =
        CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(factoryResult) || !factory) {
        TD_LOG_ERROR(
            "[D3D12Device] Swap-chain DXGI factory creation failed: 0x{:08X}",
            factoryResult);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = SWAP_FORMAT;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = FRAME_COUNT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = factory->CreateSwapChainForHwnd(m_commandQueue.Get(),
        static_cast<HWND>(hwnd), &desc, nullptr, nullptr, &sc1);
    if (FAILED(hr)) { TD_LOG_ERROR("[D3D12Device] Swap chain failed: 0x{:08X}", hr); return false; }

    factory->MakeWindowAssociation(static_cast<HWND>(hwnd), DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&m_swapChain);
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    TD_LOG_INFO("[D3D12Device] Swap chain: {}x{}", width, height);
    return true;
}

bool D3D12Device::createBuffers() {
    if (!m_frameUploadArena.initialize(m_device.Get())) return false;
    return m_uiBatch.initialize(m_device.Get(), m_uiPipeline, m_srvDescriptors);
}

bool D3D12Device::createSrvHeap() {
    if (!m_srvDescriptors.initialize(m_device.Get())) return false;
    if (!m_textureStore.initialize(
            m_device.Get(), m_srvDescriptors, m_frameUploadArena,
            m_retirementQueue)) return false;

    TD_LOG_INFO(
        "[D3D12Device] SRV heap created ({} descriptors; world-cache={} UI-texture={} UI-glyph={} fixed-headroom={})",
        m_srvDescriptors.capacity(),
        performance_limits::kWorldTextureSrvCacheBudget,
        performance_limits::kUiTextureSrvCacheBudget,
        performance_limits::kUiGlyphSrvCacheBudget,
        performance_limits::kSrvFixedDescriptorReserve);
    return true;
}

uint32_t D3D12Device::allocateSrvDescriptor() {
    return m_srvDescriptors.allocate();
}

void D3D12Device::freeSrvDescriptor(uint32_t index) {
    if (!m_srvDescriptors.canRetire(index)) {
        m_retirementQueue.reject();
        return;
    }
    const SrvDescriptorRetirementMetadata metadata =
        m_srvDescriptors.retirementMetadata(index);

    // Keep the slot allocated until the queue has passed its last possible
    // descriptor-table reference. This prevents a newly created SRV from
    // overwriting an in-flight descriptor in the shader-visible heap.
    m_retirementQueue.retireDescriptor(
        index, m_frameOrdinal, metadata,
        m_frameOpen || hasUnfencedSubmissions(),
        m_fenceTimeline.lastIssuedValue());
    m_srvDescriptors.markRetiring(index);
}

void D3D12Device::setSrvRetirementIdentity(
    uint32_t index, GpuRetirementIdentity identity) noexcept {
    m_srvDescriptors.setRetirementIdentity(index, identity);
}

void D3D12Device::releaseSrvDescriptorImmediately(uint32_t index) {
    m_srvDescriptors.releaseImmediately(index);
}

render::SrvDescriptorRenderStats D3D12Device::srvDescriptorStats() const noexcept {
    return m_srvDescriptors.stats();
}

render::GpuRetirementRenderStats
D3D12Device::gpuRetirementStats() const noexcept {
    const RetirementFenceHistory history{
        .frameOrdinals = m_frameOrdinalsByContext,
        .fenceValues = container::Span<const uint64_t>(
            m_fenceValues, FRAME_COUNT),
        .completedFence = m_fenceTimeline.completedValue(),
        .currentFrameOrdinal = m_frameOrdinal,
    };
    render::GpuRetirementRenderStats result =
        m_retirementQueue.stats(m_frameOrdinal, history);
    m_srvDescriptors.appendLiveRetirementStats(result);
    return result;
}

StaticBufferRenderStats D3D12Device::staticBufferStats() const noexcept {
    const uint32_t retiringSliceCount =
        m_retirementQueue.retiringStaticBufferCount();
    return m_staticBufferPool.stats(
        retiringSliceCount,
        m_retirementQueue.retiringStaticBufferBytes());
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Device::getSrvCpuHandle(uint32_t index) const {
    return m_srvDescriptors.cpuHandle(index);
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Device::getSrvGpuHandle(uint32_t index) const {
    return m_srvDescriptors.gpuHandle(index, m_frameOpen, m_frameOrdinal);
}

void D3D12Device::bindSrvHeap() {
    if (!m_frameOpen || !m_commandList) return;
    m_srvDescriptors.bind(m_commandList.Get());
}

void D3D12Device::bindMainRenderTargets() {
    if (!m_frameOpen || !m_commandList) return;
    m_presentationTargets.bind(m_commandList.Get(), m_frameIndex);
}

uint32_t D3D12Device::configureWorldMultisampling(
    uint32_t requestedSampleCount) {
    if (!m_device) return 1u;
    if (m_frameOpen) {
        TD_LOG_ERROR(
            "[D3D12Device] MSAA reconfiguration rejected while a frame is open");
        return m_presentationTargets.sampleCount();
    }
    if (!waitForGpu()) return m_presentationTargets.sampleCount();
    return m_presentationTargets.configureMultisampling(requestedSampleCount);
}

bool D3D12Device::beginWorldRenderPass() {
    if (!m_frameOpen || !m_commandList) {
        ++m_currentWorldResourceStateStats.beginCalls;
        ++m_currentWorldResourceStateStats.beginFailures;
        return false;
    }
    flushBatch();
    return m_presentationTargets.beginWorldPass(
        m_commandList.Get(), m_frameIndex, m_clearColor,
        m_currentWorldResourceStateStats);
}

void D3D12Device::resolveWorldRenderPass() {
    if (!m_frameOpen || m_frameIndex >= FRAME_COUNT) {
        ++m_currentWorldResourceStateStats.resolveCalls;
        ++m_currentWorldResourceStateStats.resolveNoopInactive;
        return;
    }
    flushBatch();
    m_presentationTargets.resolveWorldPass(
        m_commandList.Get(), m_frameIndex,
        m_currentWorldResourceStateStats);
}

// ── Texture upload ───────────────────────────────────────────────────────

uint32_t D3D12Device::uploadTexture(const void* rgbaPixels, uint32_t width, uint32_t height) {
    if (!m_frameOpen || !m_commandList) return UINT32_MAX;
    return m_textureStore.uploadRgba8(
        m_commandList.Get(), m_frameIndex, rgbaPixels, width, height);
}

uint32_t D3D12Device::uploadTexture2D(
    uint32_t width, uint32_t height, DXGI_FORMAT format,
    container::Span<const TextureSubresourceUpload> subresources) {
    if (!m_frameOpen || !m_commandList) return UINT32_MAX;
    return m_textureStore.upload2D(
        m_commandList.Get(), m_frameIndex, width, height, format,
        subresources);
}

void D3D12Device::freeTexture(uint32_t srvIndex) {
    static_cast<void>(m_textureStore.retire(
        srvIndex, m_frameOrdinal, m_frameOpen || hasUnfencedSubmissions(),
        m_fenceTimeline.lastIssuedValue()));
}

StaticBufferAllocation D3D12Device::recordStaticBufferUpload(
    const void* data,
    uint64_t byteSize,
    D3D12_RESOURCE_STATES finalState) {
    if (!m_frameOpen || !m_device || !m_commandList || !data || byteSize == 0) {
        TD_LOG_ERROR("[D3D12Device] Static buffer upload requires non-empty data during an open frame");
        return {};
    }
    if (m_primaryCommandSegment != 0u || m_parallelRecordingUsed) {
        TD_LOG_ERROR(
            "[D3D12Device] Static buffer upload cannot be recorded after the leading primary command list was split");
        return {};
    }
    if (finalState != D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER &&
        finalState != D3D12_RESOURCE_STATE_INDEX_BUFFER) {
        TD_LOG_ERROR("[D3D12Device] Unsupported static buffer final state: 0x{:X}",
            static_cast<uint32_t>(finalState));
        return {};
    }
    if (byteSize > std::numeric_limits<uint32_t>::max()) {
        TD_LOG_ERROR("[D3D12Device] Static buffer staging exceeds frame arena limit ({} bytes)",
                     byteSize);
        return {};
    }
    StaticBufferAllocation allocation = m_staticBufferPool.allocate(byteSize);
    if (!allocation) {
        TD_LOG_ERROR("[D3D12Device] Static DEFAULT buffer slice allocation failed ({} bytes)",
                     byteSize);
        return {};
    }
    const FrameUploadAllocation upload = allocateFrameUpload(
        data, static_cast<uint32_t>(byteSize), 16u);
    if (!upload) {
        TD_LOG_ERROR("[D3D12Device] Static buffer frame staging allocation failed ({} bytes)",
                     byteSize);
        retireStaticBufferAllocation(std::move(allocation));
        return {};
    }
    if (!m_staticBufferPool.queueUpload(allocation, upload)) {
        TD_LOG_ERROR("[D3D12Device] Static buffer pending-copy registration failed");
        retireStaticBufferAllocation(std::move(allocation));
        return {};
    }
    return allocation;
}

void D3D12Device::flushStaticBufferUploads() {
    if (!m_frameOpen || !m_commandList || m_primaryCommandSegment != 0u ||
        m_parallelRecordingUsed) {
        return;
    }
    m_staticBufferPool.flush(m_commandList.Get());
}

uint64_t D3D12Device::resourceAllocationBytes(
    ID3D12Resource* resource) const noexcept {
    if (!resource) return 0u;
    const D3D12_RESOURCE_DESC description = resource->GetDesc();
    if (description.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        return description.Width;
    }
    if (!m_device) return 0u;
    return m_device->GetResourceAllocationInfo(
        0u, 1u, &description).SizeInBytes;
}

void D3D12Device::retireResource(
    ComPtr<ID3D12Resource> resource,
    GpuRetirementIdentity identity,
    uint64_t lastUsedFrame,
    uint64_t lastUsedFence) {
    if (!resource) return;

    const uint64_t byteSize = resourceAllocationBytes(resource.Get());

    m_retirementQueue.retireResource(
        std::move(resource), byteSize, m_frameOrdinal, lastUsedFrame,
        lastUsedFence, identity, m_frameOpen || hasUnfencedSubmissions(),
        m_fenceTimeline.lastIssuedValue());
}

void D3D12Device::retireStaticBufferAllocation(
    StaticBufferAllocation allocation,
    GpuRetirementIdentity identity,
    uint64_t lastUsedFrame,
    uint64_t lastUsedFence) {
    if (!allocation) return;
    StaticBufferRetirement pending;
    if (!m_staticBufferPool.beginRetirement(
            allocation.token, m_frameOrdinal, lastUsedFrame, lastUsedFence,
            identity, pending)) {
        m_retirementQueue.reject();
        return;
    }
    try {
        m_retirementQueue.retireStaticBuffer(
            pending, m_frameOpen || hasUnfencedSubmissions(),
            m_fenceTimeline.lastIssuedValue());
    } catch (...) {
        TD_LOG_ERROR(
            "[D3D12Device] Static buffer retirement registration failed; slice remains reserved");
        m_retirementQueue.reject();
        return;
    }
    m_staticBufferPool.commitRetirement(allocation.token);
}

FrameUploadAllocation D3D12Device::allocateFrameUpload(const void* data, uint32_t size,
                                                       uint32_t alignment) {
    if (!m_frameOpen) return {};
    return m_frameUploadArena.allocate(m_frameIndex, data, size, alignment);
}

FrameUploadAllocation D3D12Device::allocateFrameUploadUninitialized(
    uint32_t size, uint32_t alignment) {
    if (!m_frameOpen) return {};
    return m_frameUploadArena.allocateUninitialized(
        m_frameIndex, size, alignment);
}

ConstantBufferAllocation D3D12Device::allocateConstantBuffer(const void* data, uint32_t size) {
    return allocateFrameUpload(data, size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
}

bool D3D12Device::hasUnfencedSubmissions() const {
    for (const bool unresolved : m_unfencedSubmissions) {
        if (unresolved) return true;
    }
    return false;
}

void D3D12Device::resolveUnfencedSubmissions(uint64_t fenceValue) {
    if (fenceValue == 0) return;
    for (uint32_t i = 0; i < FRAME_COUNT; ++i) {
        if (!m_unfencedSubmissions[i]) continue;
        // A later signal on the same direct queue covers every earlier
        // ExecuteCommandLists call, including submissions whose own signal
        // failed. Associate those frame resources with the successful value.
        m_fenceValues[i] = fenceValue;
        m_unfencedSubmissions[i] = false;
    }
    m_srvDescriptors.commitTouches(fenceValue);
}

void D3D12Device::sealPendingRetirements(uint64_t fenceValue) {
    m_retirementQueue.sealPending(fenceValue);
}

void D3D12Device::reclaimFrameResources() {
    if (!m_fenceTimeline.valid()) return;
    const uint64_t completedFence = m_fenceTimeline.completedValue();

    m_retirementQueue.reclaim(
        completedFence, m_completedRetiredDescriptors,
        m_completedRetiredStaticBuffers);
    for (uint32_t index : m_completedRetiredDescriptors) {
        releaseSrvDescriptorImmediately(index);
    }
    for (const StaticBufferRetirement& retirement :
         m_completedRetiredStaticBuffers) {
        if (!m_staticBufferPool.release(retirement)) {
            m_retirementQueue.reject();
        }
    }
}

// ── Draw commands ────────────────────────────────────────────────────────

void D3D12Device::drawSolidQuad(float x, float y, float w, float h, float r, float g, float b, float a) {
    m_uiBatch.drawSolidQuad({m_commandList.Get(), m_frameOrdinal,
        &m_currentRenderBindingStats}, x, y, w, h, r, g, b, a);
}

void D3D12Device::drawSolidGradientLine(
    float startX, float startY, float endX, float endY, float width,
    float startR, float startG, float startB, float startA,
    float endR, float endG, float endB, float endA) {
    m_uiBatch.drawSolidGradientLine({m_commandList.Get(), m_frameOrdinal,
        &m_currentRenderBindingStats}, startX, startY, endX, endY, width,
        startR, startG, startB, startA, endR, endG, endB, endA);
}

void D3D12Device::drawTexturedQuad(float x, float y, float w, float h,
                                     float u0, float v0, float u1, float v1,
                                     float r, float g, float b, float a,
                                     D3D12_GPU_DESCRIPTOR_HANDLE texSrv) {
    m_uiBatch.drawTexturedQuad({m_commandList.Get(), m_frameOrdinal,
        &m_currentRenderBindingStats}, x, y, w, h, u0, v0, u1, v1,
        r, g, b, a, texSrv);
}

void D3D12Device::flushBatch() {
    m_uiBatch.flush({m_commandList.Get(), m_frameOrdinal,
        &m_currentRenderBindingStats});
}

// ── Frame lifecycle ──────────────────────────────────────────────────────

bool D3D12Device::beginGpuTimestamp(
    render::GpuTimestampRange range) noexcept {
    return m_gpuTimestampOwner.begin(
        m_commandList.Get(), m_frameOpen, m_frameIndex, range);
}

bool D3D12Device::endGpuTimestamp(
    render::GpuTimestampRange range) noexcept {
    return m_gpuTimestampOwner.end(
        m_commandList.Get(), m_frameOpen, m_frameIndex, range);
}

bool D3D12Device::beginFrame() {
    if (m_frameOpen) {
        TD_LOG_ERROR("[D3D12Device] beginFrame called while a frame is already open");
        return false;
    }

    // Queue::Signal can fail after ExecuteCommandLists has already accepted
    // work. Do not reuse any frame context until a later successful signal
    // has placed that work back onto the tracked monotonic fence timeline.
    if (hasUnfencedSubmissions()) {
        const uint64_t recoveryFence = m_fenceTimeline.signal();
        if (recoveryFence == 0) {
            TD_LOG_ERROR("[D3D12Device] Cannot begin frame: submitted GPU work has no fence");
            return false;
        }
        resolveUnfencedSubmissions(recoveryFence);
        sealPendingRetirements(recoveryFence);
    } else {
        // A command list abandoned before ExecuteCommandLists has no GPU use.
        m_srvDescriptors.discardTouches();
    }

    const auto fenceVal = m_fenceValues[m_frameIndex];
    if (!m_fenceTimeline.wait(fenceVal)) return false;

    // Every allocator belonging to this frame context is reset together and
    // only after the context fence has completed. Command lists are opened
    // later as their segment is needed.
    FrameCommandContext& commandContext =
        m_frameCommandContexts[m_frameIndex];
    for (uint32_t segment = 0; segment < PRIMARY_COMMAND_SEGMENT_COUNT;
         ++segment) {
        const HRESULT resetResult =
            commandContext.primaryAllocators[segment]->Reset();
        if (FAILED(resetResult)) {
            TD_LOG_ERROR(
                "[D3D12Device] Primary allocator reset failed: frame={} segment={} hr=0x{:08X}",
                m_frameIndex, segment, resetResult);
            return false;
        }
    }
    for (uint32_t worker = 0; worker < m_parallelWorkerCount; ++worker) {
        const HRESULT resetResult =
            commandContext.workerAllocators[worker]->Reset();
        if (FAILED(resetResult)) {
            TD_LOG_ERROR(
                "[D3D12Device] Worker allocator reset failed: frame={} worker={} hr=0x{:08X}",
                m_frameIndex, worker, resetResult);
            return false;
        }
    }

    // The context fence is complete, so this slot's query resolve is now
    // CPU-readable. No extra wait is introduced for timestamp collection.
    m_gpuTimestampOwner.beginFrameSlot(m_frameIndex);

    reclaimFrameResources();
    m_inFlightUploads[m_frameIndex].clear();
    ++m_frameOrdinal;
    m_frameUploadArena.beginFrameSlot(m_frameIndex, m_frameOrdinal);
    m_currentRenderBindingStats = {
        .frameOrdinal = m_frameOrdinal,
    };
    m_frameOrdinalsByContext[m_frameIndex] = m_frameOrdinal;
    m_fenceValues[m_frameIndex] = 0u;
    m_currentWorldResourceStateStats = {
        .frameOrdinal = m_frameOrdinal,
        .sampleCount = m_presentationTargets.sampleCount(),
        .presentationTargetRestored = true,
    };
    m_commandList = m_primaryCommandLists[0];
    HRESULT hr = m_commandList->Reset(
        commandContext.primaryAllocators[0].Get(), nullptr);
    if (FAILED(hr)) {
        TD_LOG_ERROR("[D3D12Device] Command list reset failed: 0x{:08X}", hr);
        return false;
    }
    m_frameSubmissionLists.fill(nullptr);
    m_frameSubmissionListCount = 0;
    m_primaryCommandSegment = 0;
    m_parallelRecordingUsed = false;
    m_frameOpen = true;
    m_presentationTargets.abortFrame();
    recordPendingDummyTextureUpload();
    // An aborted, unsubmitted frame can leave staged immutable copies behind.
    // Replay them before the reset arena offsets are reused by this frame.
    flushStaticBufferUploads();
    static_cast<void>(beginGpuTimestamp(render::GpuTimestampRange::Frame));

    m_uiBatch.beginFrame(m_frameIndex);

    ID3D12Resource* backBuffer =
        m_presentationTargets.backBuffer(m_frameIndex);
    if (!backBuffer) {
        TD_LOG_ERROR("[D3D12Device] Cannot begin frame without a back buffer");
        abortOpenFrame();
        return false;
    }
    auto barrier = makeTransition(backBuffer,
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_commandList->ResourceBarrier(1, &barrier);
    ++m_currentWorldResourceStateStats.transitionBarriers;

    preparePrimaryGraphicsState();

    const auto rtv = m_presentationTargets.backBufferRtv(m_frameIndex);
    const auto dsv = m_presentationTargets.backBufferDsv(m_frameIndex);
    m_commandList->ClearRenderTargetView(rtv, m_clearColor.data(), 0, nullptr);
    m_commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    return true;
}

void D3D12Device::preparePrimaryGraphicsState() noexcept {
    if (!m_commandList || !m_frameOpen) return;

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_presentationTargets.width());
    viewport.Height = static_cast<float>(m_presentationTargets.height());
    viewport.MaxDepth = 1.0f;
    m_commandList->RSSetViewports(1, &viewport);

    D3D12_RECT scissor{};
    scissor.right = static_cast<LONG>(m_presentationTargets.width());
    scissor.bottom = static_cast<LONG>(m_presentationTargets.height());
    m_commandList->RSSetScissorRects(1, &scissor);
    bindMainRenderTargets();
}

void D3D12Device::recordPendingDummyTextureUpload() noexcept {
    if (!m_frameOpen || !m_commandList) return;
    m_textureStore.recordFallbackUpload(m_commandList.Get());
}

void D3D12Device::prepareParallelGraphicsState(
    ID3D12GraphicsCommandList* commandList) const noexcept {
    if (!commandList || !m_frameOpen || m_frameIndex >= FRAME_COUNT) return;
    m_srvDescriptors.bind(commandList);
    m_presentationTargets.bind(commandList, m_frameIndex);
}

void D3D12Device::abortOpenFrame() noexcept {
    if (!m_frameOpen) return;
    if (m_commandList) static_cast<void>(m_commandList->Close());
    m_textureStore.abortFrame();
    m_frameOpen = false;
    m_presentationTargets.abortFrame();
    m_frameSubmissionLists.fill(nullptr);
    m_frameSubmissionListCount = 0;
    m_primaryCommandSegment = 0;
    m_parallelRecordingUsed = false;
    m_uiBatch.discard();
}

bool D3D12Device::recordParallelGraphics(
    container::Span<const ParallelGraphicsRecorder> recorders) {
    if (!m_frameOpen || !m_commandList) {
        TD_LOG_ERROR(
            "[D3D12Device] Parallel graphics recording requires an open frame");
        return false;
    }
    if (!platform::runtime::isCurrentThread(
            platform::runtime::ThreadRole::Render)) {
        TD_LOG_ERROR(
            "[D3D12Device] Parallel graphics recording must start on the render thread");
        return false;
    }
    if (recorders.empty()) return true;
    if (m_parallelRecordingUsed ||
        m_primaryCommandSegment + 1u >= PRIMARY_COMMAND_SEGMENT_COUNT) {
        TD_LOG_ERROR(
            "[D3D12Device] Parallel graphics recording already split this frame");
        return false;
    }
    if (recorders.size() > m_parallelWorkerCount) {
        TD_LOG_ERROR(
            "[D3D12Device] Parallel graphics recorder count {} exceeds frame worker capacity {}",
            recorders.size(), m_parallelWorkerCount);
        return false;
    }

    container::Vector<std::future<void>> tasks;
    try {
        tasks.reserve(recorders.size());
    } catch (...) {
        TD_LOG_ERROR(
            "[D3D12Device] Parallel graphics future allocation failed");
        return false;
    }

    struct WorkerResult final {
        std::exception_ptr callbackException;
        HRESULT closeResult = E_FAIL;
    };
    container::Array<WorkerResult, MAX_PARALLEL_GRAPHICS_WORKERS> results{};
    FrameCommandContext& context = m_frameCommandContexts[m_frameIndex];
    uint32_t openedWorkerLists = 0;
    for (uint32_t worker = 0;
         worker < static_cast<uint32_t>(recorders.size()); ++worker) {
        const HRESULT resetResult = m_parallelCommandLists[worker]->Reset(
            context.workerAllocators[worker].Get(), nullptr);
        if (FAILED(resetResult)) {
            for (uint32_t opened = 0; opened < openedWorkerLists; ++opened) {
                static_cast<void>(m_parallelCommandLists[opened]->Close());
            }
            TD_LOG_ERROR(
                "[D3D12Device] Worker command list reset failed: worker={} hr=0x{:08X}",
                worker, resetResult);
            return false;
        }
        ++openedWorkerLists;
    }

    // Preserve draw ordering across the split: batched primary work must be
    // materialized before this segment is closed.
    flushStaticBufferUploads();
    flushBatch();
    const HRESULT primaryCloseResult = m_commandList->Close();
    if (FAILED(primaryCloseResult)) {
        for (uint32_t worker = 0; worker < openedWorkerLists; ++worker) {
            static_cast<void>(m_parallelCommandLists[worker]->Close());
        }
        TD_LOG_ERROR(
            "[D3D12Device] Primary command list close failed before parallel recording: 0x{:08X}",
            primaryCloseResult);
        abortOpenFrame();
        return false;
    }

    m_frameSubmissionLists[m_frameSubmissionListCount++] = m_commandList.Get();
    m_parallelRecordingUsed = true;

    uint32_t submittedTasks = 0;
    bool submissionFailed = false;
    try {
        for (; submittedTasks < static_cast<uint32_t>(recorders.size());
             ++submittedTasks) {
            const uint32_t worker = submittedTasks;
            ID3D12GraphicsCommandList* workerList =
                m_parallelCommandLists[worker].Get();
            tasks.push_back(platform::runtime::renderWorkerExecutor().async(
                "d3d12-parallel-graphics-record",
                [this, worker, workerList, recorders, &results]() {
                    const platform::runtime::ThreadRoleScope role(
                        platform::runtime::ThreadRole::RenderWorker);
                    try {
                        prepareParallelGraphicsState(workerList);
                        recorders[worker](workerList);
                    } catch (...) {
                        results[worker].callbackException =
                            std::current_exception();
                    }
                    results[worker].closeResult = workerList->Close();
                }));
        }
    } catch (...) {
        submissionFailed = true;
    }

    // Lists reset above but not handed to the executor still need to become
    // closed before the frame context can be recovered or reused.
    for (uint32_t worker = submittedTasks;
         worker < static_cast<uint32_t>(recorders.size()); ++worker) {
        results[worker].closeResult =
            m_parallelCommandLists[worker]->Close();
    }
    for (std::future<void>& task : tasks) {
        try {
            task.get();
        } catch (...) {
            submissionFailed = true;
        }
    }

    bool workerRecordingSucceeded = !submissionFailed;
    for (uint32_t worker = 0;
         worker < static_cast<uint32_t>(recorders.size()); ++worker) {
        if (results[worker].callbackException ||
            FAILED(results[worker].closeResult)) {
            workerRecordingSucceeded = false;
        }
    }

    const uint32_t continuationSegment = m_primaryCommandSegment + 1u;
    ComPtr<ID3D12GraphicsCommandList> continuation =
        m_primaryCommandLists[continuationSegment];
    const HRESULT continuationResetResult = continuation->Reset(
        context.primaryAllocators[continuationSegment].Get(), nullptr);
    if (FAILED(continuationResetResult)) {
        TD_LOG_ERROR(
            "[D3D12Device] Primary continuation reset failed: 0x{:08X}",
            continuationResetResult);
        abortOpenFrame();
        return false;
    }
    m_primaryCommandSegment = continuationSegment;
    m_commandList = std::move(continuation);
    preparePrimaryGraphicsState();

    if (!workerRecordingSucceeded) {
        TD_LOG_ERROR(
            "[D3D12Device] Parallel graphics recording failed; worker lists were discarded");
        return false;
    }
    for (uint32_t worker = 0;
         worker < static_cast<uint32_t>(recorders.size()); ++worker) {
        m_frameSubmissionLists[m_frameSubmissionListCount++] =
            m_parallelCommandLists[worker].Get();
    }
    return true;
}

bool D3D12Device::endFrame() {
    if (!m_frameOpen) {
        TD_LOG_ERROR("[D3D12Device] endFrame called without an open frame");
        return false;
    }

    // Static uploads normally flush immediately before the first world draw.
    // Keep an end-of-frame fallback for uploaded resources not drawn yet.
    flushStaticBufferUploads();
    // Flush any remaining solid batch
    flushBatch();
    resolveWorldRenderPass();

    ID3D12Resource* renderTarget =
        m_presentationTargets.backBuffer(m_frameIndex);
    if (!renderTarget) {
        TD_LOG_ERROR("[D3D12Device] endFrame has no render target");
        abortOpenFrame();
        return false;
    }
    const bool captureThisFrame = m_presentationTargets.captureRequested();
    ID3D12Resource* captureTarget = captureThisFrame
        ? m_presentationTargets.captureTarget(m_frameIndex) : nullptr;
    if (captureThisFrame && !captureTarget) {
        TD_LOG_ERROR("[D3D12Device] Requested present capture has no target");
        abortOpenFrame();
        return false;
    }

    auto barrier = makeTransition(
        renderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET,
        captureThisFrame ? D3D12_RESOURCE_STATE_COPY_SOURCE
                         : D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->ResourceBarrier(1, &barrier);
    if (captureThisFrame) {
        m_commandList->CopyResource(captureTarget, renderTarget);
        barrier = makeTransition(
            renderTarget, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_PRESENT);
        m_commandList->ResourceBarrier(1, &barrier);
    }

    static_cast<void>(endGpuTimestamp(render::GpuTimestampRange::Frame));
    m_gpuTimestampOwner.resolve(m_commandList.Get(), m_frameIndex);

    const HRESULT closeHr = m_commandList->Close();
    if (FAILED(closeHr)) {
        TD_LOG_ERROR("[D3D12Device] Command list close failed: 0x{:08X}", closeHr);
        abortOpenFrame();
        return false;
    }

    m_frameSubmissionLists[m_frameSubmissionListCount++] = m_commandList.Get();
    m_commandQueue->ExecuteCommandLists(
        m_frameSubmissionListCount, m_frameSubmissionLists.data());
    m_textureStore.commitFrame();
    m_unfencedSubmissions[m_frameIndex] = true;
    m_gpuTimestampOwner.sealSubmitted(m_frameIndex, m_frameOrdinal);
    const HRESULT presentHr = m_swapChain->Present(
        m_verticalSyncEnabled ? 1u : 0u, 0);
    if (FAILED(presentHr)) {
        TD_LOG_ERROR("[D3D12Device] Present failed: 0x{:08X}", presentHr);
    }

    const uint64_t v = m_fenceTimeline.signal();
    if (v != 0) {
        resolveUnfencedSubmissions(v);
        sealPendingRetirements(v);
    }
    if (captureThisFrame && SUCCEEDED(presentHr) && v != 0) {
        m_presentationTargets.markCaptureReady(m_frameIndex);
    }
    m_frameOpen = false;
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    m_frameUploadArena.finishFrame();
    drainInfoQueue();
    return SUCCEEDED(presentHr) && v != 0;
}

FrameFenceStatus D3D12Device::frameFenceStatus(
    uint64_t frameOrdinal) const noexcept {
    if (frameOrdinal == 0u) return {};
    const uint64_t completedFence = m_fenceTimeline.completedValue();
    for (uint32_t index = 0; index < FRAME_COUNT; ++index) {
        if (m_frameOrdinalsByContext[index] != frameOrdinal) continue;
        const uint64_t fenceValue = m_fenceValues[index];
        return {
            .fenceValue = fenceValue,
            .completed = fenceValue != 0u && fenceValue <= completedFence,
            .exact = fenceValue != 0u,
        };
    }
    // A frame context is overwritten only after beginFrame waited its fence.
    // Missing older ordinals are therefore known-complete even though their
    // exact historical fence value is intentionally not retained forever.
    return {
        .completed = frameOrdinal < m_frameOrdinal,
        .exact = false,
    };
}

bool D3D12Device::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return false;
    if (width == m_presentationTargets.width() &&
        height == m_presentationTargets.height() &&
        m_presentationTargets.valid()) return true;
    if (m_frameOpen) {
        TD_LOG_ERROR("[D3D12Device] resize rejected while a frame is open");
        return false;
    }
    if (!waitForGpu()) {
        TD_LOG_ERROR("[D3D12Device] resize aborted because the GPU could not be synchronized");
        return false;
    }
    const uint32_t previousWidth = m_presentationTargets.width();
    const uint32_t previousHeight = m_presentationTargets.height();
    m_presentationTargets.releaseForResize();
    HRESULT hr = m_swapChain->ResizeBuffers(FRAME_COUNT, width, height, SWAP_FORMAT, 0);
    if (FAILED(hr)) {
        TD_LOG_ERROR("[D3D12Device] ResizeBuffers failed: 0x{:08X}", hr);
        static_cast<void>(
            m_presentationTargets.recreate(previousWidth, previousHeight));
        return false;
    }
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    if (!m_presentationTargets.recreate(width, height)) {
        TD_LOG_ERROR("[D3D12Device] Failed to recreate targets after resize");
        return false;
    }
    TD_LOG_INFO("[D3D12Device] Resized to {}x{}", width, height);
    return true;
}

bool D3D12Device::waitIdle() {
    return waitForGpu();
}

bool D3D12Device::requestPresentCapture() noexcept {
    return m_device && m_swapChain && m_presentationTargets.requestCapture();
}

bool D3D12Device::readbackLastPresentedFrame(
    RenderTargetReadback& output) {
    output = {};
    if (m_frameOpen) {
        TD_LOG_ERROR(
            "[D3D12Device] Render-target readback rejected while a frame is open");
        return false;
    }
    if (!m_device || !m_commandQueue || !m_temporaryCommandAllocator ||
        !m_temporaryCommandList || !m_presentationTargets.valid() ||
        !m_presentationTargets.captureReady()) {
        TD_LOG_WARN(
            "[D3D12Device] Render-target readback requested without a completed capture");
        return false;
    }
    if (!waitForGpu()) return false;

    ID3D12Resource* source = m_presentationTargets.consumeReadyCapture();
    if (!source) return false;
    const D3D12_RESOURCE_DESC sourceDescription = source->GetDesc();
    if (sourceDescription.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        sourceDescription.Format != SWAP_FORMAT ||
        sourceDescription.Width != m_presentationTargets.width() ||
        sourceDescription.Height != m_presentationTargets.height()) {
        TD_LOG_ERROR(
            "[D3D12Device] Unsupported render-target readback description");
        return false;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rowCount = 0;
    UINT64 rowBytes = 0;
    UINT64 totalBytes = 0;
    m_device->GetCopyableFootprints(
        &sourceDescription, 0, 1, 0, &footprint, &rowCount, &rowBytes,
        &totalBytes);
    const uint32_t captureWidth = m_presentationTargets.width();
    const uint32_t captureHeight = m_presentationTargets.height();
    const uint64_t tightRowBytes = static_cast<uint64_t>(captureWidth) * 4u;
    const uint64_t tightBytes = tightRowBytes * captureHeight;
    if (rowCount != captureHeight || rowBytes < tightRowBytes || totalBytes == 0 ||
        tightBytes > std::numeric_limits<size_t>::max()) {
        TD_LOG_ERROR(
            "[D3D12Device] Invalid render-target readback footprint");
        return false;
    }

    const D3D12_HEAP_PROPERTIES readbackHeap = makeReadbackHeap();
    const D3D12_RESOURCE_DESC readbackDescription = makeBufferDesc(totalBytes);
    ComPtr<ID3D12Resource> readback;
    const HRESULT createResult = m_device->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDescription,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&readback));
    if (FAILED(createResult)) {
        TD_LOG_ERROR(
            "[D3D12Device] Readback buffer creation failed: 0x{:08X}",
            createResult);
        return false;
    }

    const uint32_t submissionIndex = m_frameIndex;
    HRESULT result = m_temporaryCommandAllocator->Reset();
    if (FAILED(result)) {
        TD_LOG_ERROR(
            "[D3D12Device] Readback allocator reset failed: 0x{:08X}",
            result);
        return false;
    }
    result = m_temporaryCommandList->Reset(
        m_temporaryCommandAllocator.Get(), nullptr);
    if (FAILED(result)) {
        TD_LOG_ERROR(
            "[D3D12Device] Readback command-list reset failed: 0x{:08X}",
            result);
        return false;
    }

    const D3D12_RESOURCE_BARRIER toCopy = makeTransition(
        source, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_temporaryCommandList->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
    sourceLocation.pResource = source;
    sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sourceLocation.SubresourceIndex = 0;
    m_temporaryCommandList->CopyTextureRegion(
        &destination, 0, 0, 0, &sourceLocation, nullptr);
    const D3D12_RESOURCE_BARRIER restoreCapture = makeTransition(
        source, D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST);
    m_temporaryCommandList->ResourceBarrier(1, &restoreCapture);
    result = m_temporaryCommandList->Close();
    if (FAILED(result)) {
        TD_LOG_ERROR(
            "[D3D12Device] Readback command-list close failed: 0x{:08X}",
            result);
        return false;
    }

    ID3D12CommandList* lists[] = {m_temporaryCommandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, lists);
    m_unfencedSubmissions[submissionIndex] = true;
    // Keep a second reference in the normal retirement path before signaling.
    // If Signal/Wait fails after ExecuteCommandLists, the GPU may still own
    // this buffer and the local ComPtr must not become its last reference.
    ComPtr<ID3D12Resource> retirementReference = readback;
    retireResource(std::move(retirementReference));
    const uint64_t fenceValue = m_fenceTimeline.signal();
    if (fenceValue == 0) return false;
    resolveUnfencedSubmissions(fenceValue);
    sealPendingRetirements(fenceValue);
    if (!m_fenceTimeline.wait(fenceValue)) return false;

    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(totalBytes)};
    result = readback->Map(0, &readRange, &mapped);
    if (FAILED(result) || !mapped) {
        TD_LOG_ERROR(
            "[D3D12Device] Readback buffer map failed: 0x{:08X}", result);
        return false;
    }
    output.width = captureWidth;
    output.height = captureHeight;
    output.rgba.resize(static_cast<size_t>(tightBytes));
    const auto* sourceBytes = static_cast<const uint8_t*>(mapped) +
        footprint.Offset;
    for (uint32_t row = 0; row < captureHeight; ++row) {
        std::memcpy(
            output.rgba.data() + static_cast<size_t>(row) * tightRowBytes,
            sourceBytes + static_cast<size_t>(row) *
                footprint.Footprint.RowPitch,
            static_cast<size_t>(tightRowBytes));
    }
    const D3D12_RANGE writtenRange{0, 0};
    readback->Unmap(0, &writtenRange);
    drainInfoQueue();
    return output.valid();
}

bool D3D12Device::waitForGpu() {
    if (!m_commandQueue) return true;
    if (!m_fenceTimeline.valid()) return false;
    const uint64_t v = m_fenceTimeline.signal();
    if (v == 0) return false;

    resolveUnfencedSubmissions(v);
    // A queue signal cannot cover the command list currently being recorded.
    // Only seal global pending retirements when no frame is open.
    if (!m_frameOpen) sealPendingRetirements(v);

    if (!m_fenceTimeline.wait(v)) return false;
    reclaimFrameResources();
    return true;
}

void D3D12Device::moveToNextFrame() {
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void D3D12Device::drainInfoQueue() {
    if (!m_infoQueue) return;
    UINT64 numMessages = m_infoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < numMessages; ++i) {
        SIZE_T msgSize = 0;
        m_infoQueue->GetMessage(i, nullptr, &msgSize);
        if (msgSize == 0) continue;
        container::Vector<char> buf(msgSize);
        auto msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        m_infoQueue->GetMessage(i, msg, &msgSize);
        switch (msg->Severity) {
            case D3D12_MESSAGE_SEVERITY_ERROR:
                TD_LOG_ERROR_AGGREGATED(
                    msg->pDescription,
                    "[D3D12 Validation] {}", msg->pDescription);
                break;
            case D3D12_MESSAGE_SEVERITY_WARNING:
                TD_LOG_WARN_AGGREGATED(
                    msg->pDescription,
                    "[D3D12 Validation] {}", msg->pDescription);
                break;
            case D3D12_MESSAGE_SEVERITY_INFO:
                TD_LOG_INFO("[D3D12 Validation] {}", msg->pDescription);
                break;
            default:
                break;
        }
    }
    m_infoQueue->ClearStoredMessages();
}

void D3D12Device::setSamplerMode(uint32_t mode) {
    if (m_uiBatch.samplerMode() == mode) return;
    // Flush any pending solid batch before switching PSO
    flushBatch();
    m_uiBatch.setSamplerMode(mode);
}

void D3D12Device::setVirtualResolution(float vw, float vh) {
    m_uiBatch.setVirtualResolution(vw, vh);
}

void D3D12Device::setClearColor(container::Array<float, 4> color) noexcept {
    for (float& channel : color) {
        if (!std::isfinite(channel)) channel = 0.0f;
        channel = std::clamp(channel, 0.0f, 1.0f);
    }
    color[3] = 1.0f;
    m_clearColor = color;
}

bool D3D12Device::configureDisplayGamma(float gamma) noexcept {
    if (!std::isfinite(gamma)) gamma = 1.0f;
    gamma = std::clamp(gamma, 0.6f, 2.0f);
    if (std::abs(gamma - m_displayGamma) <= 0.0001f &&
        (gamma == 1.0f || m_displayGammaApplied)) {
        return true;
    }
    if (!m_swapChain) return false;

    ComPtr<IDXGIOutput> output;
    const HRESULT outputResult = m_swapChain->GetContainingOutput(&output);
    if (FAILED(outputResult) || !output) {
        TD_LOG_WARN(
            "[D3D12Device] display gamma output lookup failed: 0x{:08X}",
            static_cast<uint32_t>(outputResult));
        return false;
    }

    DXGI_GAMMA_CONTROL control{};
    control.Scale = {1.0f, 1.0f, 1.0f};
    control.Offset = {0.0f, 0.0f, 0.0f};
    const float inverseGamma = 1.0f / gamma;
    for (size_t index = 0; index < std::size(control.GammaCurve); ++index) {
        const float input = static_cast<float>(index) /
            static_cast<float>(std::size(control.GammaCurve) - 1u);
        const float value = std::pow(input, inverseGamma);
        control.GammaCurve[index] = {value, value, value};
    }
    const HRESULT gammaResult = output->SetGammaControl(&control);
    if (FAILED(gammaResult)) {
        // DXGI permits output gamma only in presentation modes supported by
        // the driver (commonly exclusive fullscreen). Keep the old applied
        // value authoritative and report the missing device capability.
        TD_LOG_WARN(
            "[D3D12Device] display gamma {:.3f} unsupported by current output mode: 0x{:08X}",
            gamma, static_cast<uint32_t>(gammaResult));
        return false;
    }
    m_displayGamma = gamma;
    m_displayGammaApplied = gamma != 1.0f;
    TD_LOG_INFO("[D3D12Device] display gamma configured: {:.3f}", gamma);
    return true;
}

} // namespace engine::d3d12
