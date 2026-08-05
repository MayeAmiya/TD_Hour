#pragma once

#include "core/container/container_types.h"
#include "engine/fx/runtime/GpuParticleCommandNormalization.h"
#include "engine/renderer/d3d12/runtime/D3D12PerformanceSettings.h"
#include "engine/renderer/world/particle/ParticleVisibilityContract.h"
#include "engine/renderer/world/particle/GpuParticlePipeline.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace engine::d3d12 {
class D3D12Device;
}

namespace engine::render {

// Renderer-owned GPU Hybrid infrastructure. It never owns emitters or
// gameplay state: CPU admission produces commands, while this object owns
// only GPU particle state, counters, compute PSOs and their barriers.
class GpuParticleSimulator final {
public:
    struct AliveCompactReadbackHeader final {
        uint32_t aliveCount = 0;
        uint32_t overflowCount = 0;
        uint32_t authorityEpoch = 0;
        uint32_t contractVersion = 0;
        uint32_t visibleCount = 0;
        uint32_t visibleOverflowCount = 0;
        uint32_t materialBinnedCount = 0;
        uint32_t materialOverflowCount = 0;
        uint32_t visibleSignatureA = 0;
        uint32_t visibleSignatureB = 0;
    };

    static_assert(sizeof(AliveCompactReadbackHeader) == 40u);

    struct Stats final {
        uint64_t resetDispatches = 0;
        uint64_t commandDispatches = 0;
        uint64_t integrationDispatches = 0;
        uint64_t aliveCompactDispatches = 0;
        uint64_t aliveCompactCounterResetDispatches = 0;
        uint64_t visibleCompactDispatches = 0;
        uint64_t visibleCompactCounterResetDispatches = 0;
        uint64_t materialBinResetDispatches = 0;
        uint64_t materialBinCountDispatches = 0;
        uint64_t materialBinPrefixDispatches = 0;
        uint64_t materialBinScatterDispatches = 0;
        uint64_t indirectArgumentBuilds = 0;
        uint64_t indirectArgumentTransitions = 0;
        uint64_t indirectDrawResourceTransitions = 0;
        uint64_t diagnosticCounterReadbacks = 0;
        uint64_t diagnosticStateReadbacks = 0;
        uint64_t diagnosticStateCopies = 0;
        uint64_t visibilityAuthorityPublishes = 0;
        uint64_t visibilityAuthorityEntries = 0;
        uint64_t visibilityAuthorityBytes = 0;
        uint64_t visibilityAuthorityRejects = 0;
        uint64_t submittedBirthCommands = 0;
        uint64_t submittedRetireCommands = 0;
        uint64_t rejectedCommands = 0;
        uint64_t transitionBarriers = 0;
        uint64_t uavBarriers = 0;
        size_t normalizationSlotScratchCapacity = 0;
        size_t normalizationSlotScratchHighWater = 0;
        size_t normalizationBirthScratchCapacity = 0;
        size_t normalizationRetireScratchCapacity = 0;
        uint64_t normalizationScratchCapacityGrowths = 0;
        // Alive/overflow values remain GPU-resident for the future indirect
        // path. Runtime stats deliberately report no synthetic CPU value;
        // recordAliveCompactReadback() is a device-probe-only hook.
        uint32_t aliveIndexCapacity = 0;
        uint32_t visibleIndexCapacity = 0;
        uint32_t materialTemplateMapCount = 0;
        uint32_t materialBinCount = 0;
        uint32_t materialBinCapacity = 0;
        uint32_t diagnosticAliveCount = 0;
        uint32_t diagnosticAliveOverflow = 0;
        bool diagnosticAliveCountsValid = false;
        uint32_t capacity = 0;
        uint64_t authorityEpoch = 0;
    };

    struct DiagnosticStateSampleBatch final {
        uint32_t count = 0;
        container::Array<fx::gpu_particle::GpuParticleState,
            d3d12::performance_limits::
                kGpuParticleAbStateSampleCapacity> states{};
    };

    GpuParticleSimulator() = default;
    ~GpuParticleSimulator();

    GpuParticleSimulator(const GpuParticleSimulator&) = delete;
    GpuParticleSimulator& operator=(const GpuParticleSimulator&) = delete;

    [[nodiscard]] bool init(
        d3d12::D3D12Device& device, uint32_t capacity);
    void shutdown() noexcept;
    void requestReset(uint64_t authorityEpoch) noexcept;
    [[nodiscard]] bool recordPendingReset();
    [[nodiscard]] bool recordCommands(
        container::Span<const fx::gpu_particle::GpuParticleBirthCommand> births,
        container::Span<const fx::gpu_particle::GpuParticleRetireCommand> retires);
    [[nodiscard]] bool recordIntegration(
        uint32_t activeCount, uint32_t authoredFrames);
    // Rebuilds the bounded GPU-resident list of live sparse state slots.
    // Atomic append order is intentionally unspecified; membership and the
    // alive/overflow counters are deterministic for a sealed state snapshot.
    [[nodiscard]] bool recordAliveCompact(
        uint32_t outputCapacity = UINT32_MAX);
    // Publishes the CPU-authoritative per-slot visibility membership for the
    // current fence-ring frame. A generation match prevents a stale visible
    // decision from admitting a particle that reused the sparse state slot.
    [[nodiscard]] bool publishVisibilityAuthority(
        container::Span<const GpuParticleVisibilityGeneration> visible);
    // Filters the current alive list using the exact compatible Billboard
    // CPU draw eligibility rules. This remains GPU shadow data until the
    // full visibility, A/B and performance gates permit presentation use.
    [[nodiscard]] bool recordVisibleCompact(
        uint32_t outputCapacity = UINT32_MAX);
    // Publishes the renderer-compiled stable template->bin mapping. Catalog
    // changes are infrequent safe-frame events; the method waits for prior
    // GPU readers before replacing the persistent upload contents.
    [[nodiscard]] bool configureMaterialBins(
        container::Span<const uint32_t> templateToBin,
        uint32_t materialBinCount);
    // Count -> serial prefix -> scatter over the current visible list. Per-bin
    // member order is intentionally unspecified; compatible materials do not
    // require global back-to-front ordering.
    [[nodiscard]] bool recordMaterialBins();
    // Makes the two vertex-shader inputs readable without exposing resource
    // ownership to ParticleRenderer. The indirect argument buffer remains in
    // INDIRECT_ARGUMENT until the next material-bin rebuild.
    [[nodiscard]] bool recordPrepareIndirectDraw();
    // Optional no-stall A/B count channel. The copy is recorded into the
    // current backbuffer's readback slot; consume returns it only when that
    // slot is reused after D3D12Device::beginFrame() has waited its fence.
    [[nodiscard]] bool recordDiagnosticCounterReadback();
    [[nodiscard]] std::optional<AliveCompactReadbackHeader>
        consumeDiagnosticCounterReadback() noexcept;
    [[nodiscard]] bool recordDiagnosticStateReadback(
        container::Span<const uint32_t> stateSlots);
    [[nodiscard]] std::optional<DiagnosticStateSampleBatch>
        consumeDiagnosticStateReadback() noexcept;
    // Debug/device validation hook only; never part of the runtime simulation
    // or render path. The caller supplies a COPY_DEST readback buffer large
    // enough for stateCount contiguous contract records.
    [[nodiscard]] bool recordStateReadback(
        ID3D12Resource& destination, uint32_t firstState,
        uint32_t stateCount);
    // Debug/device validation hook. Layout is AliveCompactReadbackHeader
    // followed by aliveIndexCapacity uint32 slot indices. Runtime never calls
    // this method, so the Compute path has no compulsory CPU synchronization.
    [[nodiscard]] bool recordAliveCompactReadback(
        ID3D12Resource& destination);
    // Same counter header as alive readback, followed by the bounded visible
    // slot list. Probe-only; the runtime never synchronizes this to the CPU.
    [[nodiscard]] bool recordVisibleCompactReadback(
        ID3D12Resource& destination);
    // Probe-only layout: counter header, bin counts, bin offsets, then the
    // grouped slot list. Runtime/renderer never require a CPU readback.
    [[nodiscard]] bool recordMaterialBinsReadback(
        ID3D12Resource& destination);
    [[nodiscard]] static constexpr uint64_t aliveCompactReadbackBytes(
        uint32_t capacity) noexcept {
        return sizeof(AliveCompactReadbackHeader) +
            static_cast<uint64_t>(capacity) * sizeof(uint32_t);
    }
    [[nodiscard]] static constexpr uint64_t materialBinReadbackBytes(
        uint32_t binCount, uint32_t particleCapacity) noexcept {
        return sizeof(AliveCompactReadbackHeader) +
            static_cast<uint64_t>(binCount) * sizeof(uint32_t) * 2u +
            static_cast<uint64_t>(binCount) *
                sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) +
            static_cast<uint64_t>(particleCapacity) * sizeof(uint32_t);
    }
    static constexpr uint32_t kTemplateMaterialMapCapacity = 65536u;

    [[nodiscard]] bool isInitialized() const noexcept {
        return m_initialized;
    }
    [[nodiscard]] bool resetPending() const noexcept { return m_resetPending; }
    [[nodiscard]] const Stats& stats() const noexcept { return m_stats; }
    [[nodiscard]] ID3D12Resource* materialIndirectArgs() const noexcept {
        return m_materialIndirectArgs.Get();
    }
    [[nodiscard]] bool materialIndirectArgsReady() const noexcept {
        return m_materialIndirectArgsInIndirectState &&
            m_stats.materialBinCount != 0u;
    }
    [[nodiscard]] uint32_t particleStatesSrv() const noexcept {
        return m_particleStatesSrv;
    }
    [[nodiscard]] uint32_t materialParticleIndicesSrv() const noexcept {
        return m_materialParticleIndicesSrv;
    }

private:
    [[nodiscard]] bool createBuffersAndDescriptors(uint32_t capacity);
    [[nodiscard]] bool createCommandUploads(uint32_t capacity);
    void releaseDescriptors() noexcept;
    void bindComputeState(ID3D12PipelineState* pipeline,
                          uint32_t activeCount,
                          uint32_t authoredFrames,
                          uint32_t birthCount = 0,
                          uint32_t retireCount = 0,
                          uint32_t aliveIndexCapacity = 0,
                          uint32_t visibleIndexCapacity = 0);
    void recordInitialTransitions();
    void recordUavBarriers();

    d3d12::D3D12Device* m_device = nullptr;
    GpuParticlePipeline m_pipeline;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_particleStates;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_particleCounters;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_aliveParticleIndices;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_visibleParticleIndices;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_templateMaterialMapUpload;
    uint32_t* m_mappedTemplateMaterialBins = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_materialBinCounts;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_materialBinOffsets;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_materialBinCursors;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_materialParticleIndices;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_materialIndirectArgs;
    static constexpr uint32_t kBufferedFrames = 2;
    container::Array<Microsoft::WRL::ComPtr<ID3D12Resource>, kBufferedFrames>
        m_commandUploads;
    container::Array<uint8_t*, kBufferedFrames> m_mappedCommandUploads{};
    container::Array<Microsoft::WRL::ComPtr<ID3D12Resource>, kBufferedFrames>
        m_visibilityAuthorityUploads;
    container::Array<uint32_t*, kBufferedFrames>
        m_mappedVisibilityAuthority{};
    container::Array<uint32_t, kBufferedFrames> m_visibilityAuthoritySrvs{
        UINT32_MAX, UINT32_MAX};
    container::Array<uint64_t, kBufferedFrames>
        m_visibilityAuthorityFrameOrdinals{};
    container::Array<Microsoft::WRL::ComPtr<ID3D12Resource>, kBufferedFrames>
        m_diagnosticCounterReadbacks;
    container::Array<uint8_t*, kBufferedFrames>
        m_mappedDiagnosticCounterReadbacks{};
    container::Array<bool, kBufferedFrames>
        m_diagnosticCounterReadbackPending{};
    container::Array<Microsoft::WRL::ComPtr<ID3D12Resource>, kBufferedFrames>
        m_diagnosticStateReadbacks;
    container::Array<uint8_t*, kBufferedFrames>
        m_mappedDiagnosticStateReadbacks{};
    container::Array<uint32_t, kBufferedFrames>
        m_diagnosticStateReadbackCounts{};
    container::Array<uint32_t, kBufferedFrames> m_birthCommandSrvs{
        UINT32_MAX, UINT32_MAX};
    container::Array<uint32_t, kBufferedFrames> m_retireCommandSrvs{
        UINT32_MAX, UINT32_MAX};
    uint32_t m_particleStatesUav = UINT32_MAX;
    uint32_t m_particleStatesSrv = UINT32_MAX;
    uint32_t m_particleCountersUav = UINT32_MAX;
    uint32_t m_aliveParticleIndicesUav = UINT32_MAX;
    uint32_t m_visibleParticleIndicesUav = UINT32_MAX;
    uint32_t m_templateMaterialMapSrv = UINT32_MAX;
    uint32_t m_materialBinCountsUav = UINT32_MAX;
    uint32_t m_materialBinOffsetsUav = UINT32_MAX;
    uint32_t m_materialBinCursorsUav = UINT32_MAX;
    uint32_t m_materialParticleIndicesUav = UINT32_MAX;
    uint32_t m_materialParticleIndicesSrv = UINT32_MAX;
    uint32_t m_materialIndirectArgsUav = UINT32_MAX;
    fx::ParticleGpuCommandBatch m_normalizedCommands;
    fx::ParticleGpuCommandNormalizationScratch m_commandNormalizationScratch;
    Stats m_stats{};
    bool m_resetPending = false;
    bool m_resourcesInUavState = false;
    bool m_indirectDrawResourcesInGraphicsState = false;
    bool m_materialIndirectArgsInIndirectState = false;
    bool m_initialized = false;
};

} // namespace engine::render
