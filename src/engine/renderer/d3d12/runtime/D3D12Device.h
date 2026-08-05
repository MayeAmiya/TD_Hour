#pragma once

#include "core/container/container_types.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdint>
#include <functional>
#include <utility>
#include "engine/renderer/runtime/RendererStats.h"
#include "D3D12PerformanceSettings.h"
#include "D3D12GpuTimestampOwner.h"
#include "D3D12FrameUploadArena.h"
#include "D3D12SrvDescriptorHeap.h"
#include "D3D12GpuRetirementQueue.h"
#include "D3D12StaticBufferPool.h"
#include "D3D12TextureStore.h"
#include "D3D12UiPipeline.h"
#include "D3D12UiBatch.h"
#include "D3D12FenceTimeline.h"
#include "D3D12PresentationTargets.h"
#include "debug/debug.h"

using Microsoft::WRL::ComPtr;

namespace engine::d3d12 {


// A parallel recorder owns only draw-state and draw-call recording. Resource
// transitions and uploads remain the responsibility of the primary list.
using ParallelGraphicsRecorder =
    std::function<void(ID3D12GraphicsCommandList*)>;

struct FrameFenceStatus final {
    uint64_t fenceValue = 0;
    bool completed = false;
    bool exact = false;
};

// Tightly packed RGBA8 copy of a requested presented swap-chain image.
// Readback is synchronous and intended for diagnostics/regression capture,
// not the normal render loop.
struct RenderTargetReadback final {
    uint32_t width = 0;
    uint32_t height = 0;
    container::Vector<uint8_t> rgba;

    [[nodiscard]] bool valid() const noexcept {
        return width != 0 && height != 0 &&
            rgba.size() == static_cast<size_t>(width) * height * 4u;
    }
};

class D3D12Device {
public:
    ~D3D12Device();

    static constexpr uint32_t FRAME_COUNT = 2;
    static constexpr uint32_t MAX_VERTICES = D3D12UiBatch::kMaximumVertices;
    static constexpr uint32_t MAX_INDICES = D3D12UiBatch::kMaximumIndices;
    static constexpr DXGI_FORMAT SWAP_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr DXGI_FORMAT DEPTH_FORMAT = DXGI_FORMAT_D32_FLOAT;
    static constexpr uint32_t MAX_SRV_DESCRIPTORS =
        performance_limits::kSrvDescriptorCapacity;
    static constexpr uint32_t FRAME_UPLOAD_BYTES =
        D3D12FrameUploadArena::kPrimaryBytes;
    static constexpr uint32_t FRAME_UPLOAD_SPILL_PAGE_BYTES =
        D3D12FrameUploadArena::kSpillPageBytes;
    static constexpr uint32_t MAX_FRAME_UPLOAD_SPILL_BYTES =
        D3D12FrameUploadArena::kMaximumSpillBytes;

    bool init(void* hwnd, uint32_t width, uint32_t height, bool fullscreen);
    void shutdown();

    [[nodiscard]] bool beginFrame();
    [[nodiscard]] bool endFrame();
    // Splits the active primary direct list, records one independent direct
    // list per callback on resource workers, then activates a fresh primary
    // continuation. Valid only on the render thread during an open frame.
    [[nodiscard]] bool recordParallelGraphics(
        container::Span<const ParallelGraphicsRecorder> recorders);
    [[nodiscard]] uint32_t parallelGraphicsWorkerCount() const noexcept {
        return m_parallelWorkerCount;
    }
    [[nodiscard]] bool resize(uint32_t width, uint32_t height);
    // Returns false when queue signalling/waiting failed (for example device
    // removal). Teardown may continue, but callers must not report a clean
    // synchronization guarantee in that case.
    bool waitIdle();
    // Arms a one-shot copy of the next successfully presented frame. The
    // caller may consume it with readbackLastPresentedFrame after endFrame.
    // Normal frames pay no back-buffer copy cost.
    [[nodiscard]] bool requestPresentCapture() noexcept;
    [[nodiscard]] bool readbackLastPresentedFrame(
        RenderTargetReadback& output);

    // Selected by the world producer before beginFrame from detached
    // environment data; the backend never queries game/map state itself.
    void setClearColor(container::Array<float, 4> color) noexcept;

    // Applies the session-frozen legacy display-gamma curve at the DXGI
    // output boundary. This intentionally affects world and UI together,
    // matching the original device gamma ramp rather than a world shader.
    [[nodiscard]] bool configureDisplayGamma(float gamma) noexcept;
    [[nodiscard]] float displayGamma() const noexcept { return m_displayGamma; }
    // Present pacing is a display setting, independent from simulation FPS
    // and the optional client-side frame limiter.
    void configureVerticalSync(bool enabled) noexcept {
        m_verticalSyncEnabled = enabled;
    }
    [[nodiscard]] bool verticalSyncEnabled() const noexcept {
        return m_verticalSyncEnabled;
    }
    [[nodiscard]] uint32_t configureWorldMultisampling(
        uint32_t requestedSampleCount);
    [[nodiscard]] uint32_t worldSampleCount() const noexcept {
        return m_presentationTargets.sampleCount();
    }
    [[nodiscard]] bool beginWorldRenderPass();
    void resolveWorldRenderPass();

    // Draw commands (batched — flushed at endFrame)
    void drawSolidQuad(float x, float y, float w, float h, float r, float g, float b, float a);
    void drawSolidGradientLine(float startX, float startY,
                               float endX, float endY, float width,
                               float startR, float startG, float startB, float startA,
                               float endR, float endG, float endB, float endA);
    void drawTexturedQuad(float x, float y, float w, float h,
                          float u0, float v0, float u1, float v1,
                          float r, float g, float b, float a,
                          D3D12_GPU_DESCRIPTOR_HANDLE texSrv);

    // Flush batched draws (called automatically at endFrame)
    void flushBatch();

    // Sampler mode: 0 = linear (UI images), 1 = point (font glyphs)
    void setSamplerMode(uint32_t mode);

    // SRV allocation
    D3D12_CPU_DESCRIPTOR_HANDLE getSrvCpuHandle(uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE getSrvGpuHandle(uint32_t index) const;
    uint32_t allocateSrvDescriptor();
    void freeSrvDescriptor(uint32_t index);
    void setSrvRetirementIdentity(
        uint32_t index, GpuRetirementIdentity identity) noexcept;
    // Bind the device-owned shader-visible CBV/SRV/UAV heap on the current
    // frame command list. Dedicated render passes call this before setting
    // descriptor tables from getSrvGpuHandle().
    void bindSrvHeap();
    ID3D12Device* getDevice() const { return m_device.Get(); }

    // Texture upload. Calls made during a frame are recorded on that frame's
    // command list, so the resource is ready before its first draw.
    uint32_t uploadTexture(const void* rgbaPixels, uint32_t width, uint32_t height);
    uint32_t uploadTexture2D(uint32_t width, uint32_t height,
                             DXGI_FORMAT format,
                             container::Span<const TextureSubresourceUpload> subresources);
    void freeTexture(uint32_t srvIndex);

    // Stage an immutable buffer into a slice of a long-lived DEFAULT page.
    // Copies are accumulated until flushStaticBufferUploads(), which records
    // one transition pair per touched page on the leading primary list.
    StaticBufferAllocation recordStaticBufferUpload(
        const void* data,
        uint64_t byteSize,
        D3D12_RESOURCE_STATES finalState);
    void flushStaticBufferUploads();
    void retireStaticBufferAllocation(
        StaticBufferAllocation allocation,
        GpuRetirementIdentity identity = {},
        uint64_t lastUsedFrame = 0,
        uint64_t lastUsedFence = 0);

    // Transfer an arbitrary resource to fence-delayed destruction. Moving a
    // ComPtr into this method makes the ownership hand-off explicit.
    void retireResource(
        ComPtr<ID3D12Resource> resource,
        GpuRetirementIdentity identity = {},
        uint64_t lastUsedFrame = 0,
        uint64_t lastUsedFence = 0);

    // D3D12 backend access for dedicated render passes. These do not expose
    // renderer-facing math or asset types; world/UI renderers own those.
    ID3D12GraphicsCommandList* commandList() const { return m_commandList.Get(); }
    // Dedicated world passes may query the active world color target. It is
    // the multisample target before resolve and the swap-chain buffer after;
    // post-process callers therefore resolve before copying it.
    ID3D12Resource* currentRenderTarget() const noexcept {
        return m_presentationTargets.currentColorTarget(m_frameIndex);
    }
    uint32_t frameIndex() const { return m_frameIndex; }
    uint64_t frameOrdinal() const noexcept { return m_frameOrdinal; }
    [[nodiscard]] FrameFenceStatus frameFenceStatus(
        uint64_t frameOrdinal) const noexcept;
    uint32_t width() const { return m_presentationTargets.width(); }
    uint32_t height() const { return m_presentationTargets.height(); }
    // Restores the active world/presentation RTV and matching depth target
    // after a dedicated offscreen pass changed OM render targets.
    void bindMainRenderTargets();
    FrameUploadAllocation allocateFrameUpload(const void* data, uint32_t size,
                                               uint32_t alignment = 16);
    // Reserves writable frame-scoped upload memory without an intermediate
    // CPU container. Texture uploads use resource/resourceOffset to describe
    // a placed footprint in the shared upload arena.
    FrameUploadAllocation allocateFrameUploadUninitialized(
        uint32_t size, uint32_t alignment = 16);
    ConstantBufferAllocation allocateConstantBuffer(const void* data, uint32_t size);
    [[nodiscard]] const render::FrameUploadRenderStats& currentFrameUploadStats() const noexcept {
        return m_frameUploadArena.currentStats();
    }
    [[nodiscard]] uint64_t frameUploadRemainingBytes() const noexcept {
        return m_frameUploadArena.remainingBytes();
    }
    [[nodiscard]] const render::FrameUploadRenderStats& lastFrameUploadStats() const noexcept {
        return m_frameUploadArena.lastStats();
    }
    [[nodiscard]] render::WorldResourceStateRenderStats
    currentWorldResourceStateStats() const noexcept {
        render::WorldResourceStateRenderStats result =
            m_currentWorldResourceStateStats;
        result.frameOpen = m_frameOpen;
        result.multisamplePassActive =
            m_presentationTargets.multisamplePassActive();
        return result;
    }
    [[nodiscard]] render::SrvDescriptorRenderStats srvDescriptorStats() const noexcept;
    [[nodiscard]] render::GpuRetirementRenderStats gpuRetirementStats() const noexcept;
    [[nodiscard]] StaticBufferRenderStats staticBufferStats() const noexcept;
    // Device-owned asynchronous timestamp markers. Frame is bracketed by the
    // device automatically; the other ranges are explicit renderer markers.
    // begin/end only record commands; results are consumed when beginFrame
    // reuses a fence-complete FRAME_COUNT slot, never by an in-frame GPU wait.
    [[nodiscard]] bool beginGpuTimestamp(
        render::GpuTimestampRange range) noexcept;
    [[nodiscard]] bool endGpuTimestamp(
        render::GpuTimestampRange range) noexcept;
    [[nodiscard]] bool gpuTimestampsEnabled() const noexcept {
        return m_gpuTimestampOwner.enabled();
    }
    [[nodiscard]] const render::GpuTimestampRenderStats&
    gpuTimestampStats() const noexcept {
        return m_gpuTimestampOwner.stats();
    }
    // Explicit observation hooks for dedicated render passes that record on
    // commandList(). They count raw API calls and deliberately perform no
    // state comparison, suppression, or command reordering.
    void recordPipelineStateCall(uint32_t count = 1u) noexcept {
        m_currentRenderBindingStats.pipelineStateCalls += count;
    }
    void recordGraphicsRootSignatureCall(uint32_t count = 1u) noexcept {
        m_currentRenderBindingStats.graphicsRootSignatureCalls += count;
    }
    void recordComputeRootSignatureCall(uint32_t count = 1u) noexcept {
        m_currentRenderBindingStats.computeRootSignatureCalls += count;
    }
    void recordGraphicsDescriptorTableCall(uint32_t count = 1u) noexcept {
        m_currentRenderBindingStats.graphicsDescriptorTableCalls += count;
    }
    void recordComputeDescriptorTableCall(uint32_t count = 1u) noexcept {
        m_currentRenderBindingStats.computeDescriptorTableCalls += count;
    }
    void recordVertexBufferCall(uint32_t count = 1u) noexcept {
        m_currentRenderBindingStats.vertexBufferCalls += count;
    }
    void recordIndexBufferCall(uint32_t count = 1u) noexcept {
        m_currentRenderBindingStats.indexBufferCalls += count;
    }
    void recordDrawCall(uint32_t count = 1u) noexcept {
        m_currentRenderBindingStats.drawCalls += count;
    }
    void recordDispatchCall(uint32_t count = 1u) noexcept {
        m_currentRenderBindingStats.dispatchCalls += count;
    }
    void recordExecuteIndirectCall(uint32_t count = 1u) noexcept {
        m_currentRenderBindingStats.executeIndirectCalls += count;
    }
    [[nodiscard]] const render::RenderBindingStats&
    renderBindingStats() const noexcept {
        return m_currentRenderBindingStats;
    }
    [[nodiscard]] uint64_t srvLastUsedFrame(uint32_t index) const noexcept {
        return m_srvDescriptors.lastUsedFrame(index);
    }
    [[nodiscard]] uint64_t srvLastUsedFence(uint32_t index) const noexcept {
        return m_srvDescriptors.lastUsedFence(index);
    }

    void setVirtualResolution(float vw, float vh);

private:
    bool createDevice();
    bool createCommandQueue();
    bool createSwapChain(void* hwnd, uint32_t width, uint32_t height, bool fullscreen);
    bool createBuffers();
    bool createSrvHeap();

    bool waitForGpu();
    bool hasUnfencedSubmissions() const;
    void resolveUnfencedSubmissions(uint64_t fenceValue);
    void sealPendingRetirements(uint64_t fenceValue);
    void reclaimFrameResources();
    [[nodiscard]] uint64_t resourceAllocationBytes(
        ID3D12Resource* resource) const noexcept;
    void releaseSrvDescriptorImmediately(uint32_t index);
    void moveToNextFrame();
    void drainInfoQueue();
    void abortOpenFrame() noexcept;
    void recordPendingDummyTextureUpload() noexcept;
    void preparePrimaryGraphicsState() noexcept;
    void prepareParallelGraphicsState(
        ID3D12GraphicsCommandList* commandList) const noexcept;

    static constexpr uint32_t PRIMARY_COMMAND_SEGMENT_COUNT = 2u;
    static constexpr uint32_t MAX_PARALLEL_GRAPHICS_WORKERS = 8u;
    static constexpr uint32_t MAX_FRAME_COMMAND_LISTS =
        PRIMARY_COMMAND_SEGMENT_COUNT + MAX_PARALLEL_GRAPHICS_WORKERS;

    struct FrameCommandContext {
        container::Array<ComPtr<ID3D12CommandAllocator>,
                         PRIMARY_COMMAND_SEGMENT_COUNT> primaryAllocators;
        container::Array<ComPtr<ID3D12CommandAllocator>,
                         MAX_PARALLEL_GRAPHICS_WORKERS> workerAllocators;
    };

    ComPtr<ID3D12Device>           m_device;
    ComPtr<IDXGISwapChain3>        m_swapChain;
    ComPtr<ID3D12CommandQueue>     m_commandQueue;
    container::Array<FrameCommandContext, FRAME_COUNT> m_frameCommandContexts;
    container::Array<ComPtr<ID3D12GraphicsCommandList>,
                     PRIMARY_COMMAND_SEGMENT_COUNT> m_primaryCommandLists;
    container::Array<ComPtr<ID3D12GraphicsCommandList>,
                     MAX_PARALLEL_GRAPHICS_WORKERS> m_parallelCommandLists;
    ComPtr<ID3D12CommandAllocator> m_temporaryCommandAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_temporaryCommandList;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    container::Array<ID3D12CommandList*, MAX_FRAME_COMMAND_LISTS>
        m_frameSubmissionLists{};
    uint32_t m_frameSubmissionListCount = 0;
    uint32_t m_primaryCommandSegment = 0;
    uint32_t m_parallelWorkerCount = 1;
    bool m_parallelRecordingUsed = false;
    ComPtr<ID3D12InfoQueue>        m_infoQueue;

    // SRV heap
    D3D12SrvDescriptorHeap m_srvDescriptors;
    D3D12TextureStore m_textureStore;

    D3D12UiPipeline m_uiPipeline;
    D3D12UiBatch m_uiBatch;

    static_assert(FRAME_COUNT == D3D12FrameUploadArena::kFrameSlotCount);
    D3D12FrameUploadArena m_frameUploadArena;
    render::WorldResourceStateRenderStats m_currentWorldResourceStateStats;
    uint64_t m_frameOrdinal = 0;

    D3D12StaticBufferPool m_staticBufferPool;

    static_assert(FRAME_COUNT == D3D12GpuTimestampOwner::kFrameSlotCount);
    D3D12GpuTimestampOwner m_gpuTimestampOwner;
    render::RenderBindingStats m_currentRenderBindingStats;


    // Fence
    D3D12FenceTimeline             m_fenceTimeline;
    D3D12PresentationTargets       m_presentationTargets;
    uint64_t                       m_fenceValues[FRAME_COUNT] = {};
    container::Array<uint64_t, FRAME_COUNT> m_frameOrdinalsByContext{};
    container::Array<bool, FRAME_COUNT>  m_unfencedSubmissions{};

    uint32_t m_frameIndex = 0;
    container::Array<float, 4> m_clearColor{0.035f, 0.075f, 0.135f, 1.0f};
    float m_displayGamma = 1.0f;
    bool m_displayGammaApplied = false;
    bool m_verticalSyncEnabled = false;
    struct InFlightUpload {
        ComPtr<ID3D12Resource> uploadBuffer;
    };
    container::Array<container::Vector<InFlightUpload>, FRAME_COUNT> m_inFlightUploads;

    D3D12GpuRetirementQueue m_retirementQueue;
    container::Vector<uint32_t> m_completedRetiredDescriptors;
    container::Vector<StaticBufferRetirement> m_completedRetiredStaticBuffers;
    bool m_frameOpen = false;
};

} // namespace engine::d3d12
