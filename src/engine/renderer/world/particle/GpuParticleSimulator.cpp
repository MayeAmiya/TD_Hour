#include "engine/renderer/world/particle/GpuParticleSimulator.h"

#include "engine/fx/runtime/GpuParticleCommandNormalization.h"
#include "engine/renderer/d3d12/runtime/D3D12Device.h"
#include "debug/debug.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace engine::render {
namespace {

[[nodiscard]] D3D12_RESOURCE_DESC bufferDescription(
    uint64_t bytes, D3D12_RESOURCE_FLAGS flags) noexcept {
    D3D12_RESOURCE_DESC result{};
    result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    result.Alignment = 0;
    result.Width = bytes;
    result.Height = 1;
    result.DepthOrArraySize = 1;
    result.MipLevels = 1;
    result.Format = DXGI_FORMAT_UNKNOWN;
    result.SampleDesc.Count = 1;
    result.SampleDesc.Quality = 0;
    result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    result.Flags = flags;
    return result;
}

[[nodiscard]] D3D12_HEAP_PROPERTIES defaultHeap() noexcept {
    D3D12_HEAP_PROPERTIES result{};
    result.Type = D3D12_HEAP_TYPE_DEFAULT;
    return result;
}

[[nodiscard]] D3D12_HEAP_PROPERTIES uploadHeap() noexcept {
    D3D12_HEAP_PROPERTIES result{};
    result.Type = D3D12_HEAP_TYPE_UPLOAD;
    return result;
}

[[nodiscard]] D3D12_HEAP_PROPERTIES readbackHeap() noexcept {
    D3D12_HEAP_PROPERTIES result{};
    result.Type = D3D12_HEAP_TYPE_READBACK;
    return result;
}

[[nodiscard]] D3D12_RESOURCE_BARRIER uavBarrier(
    ID3D12Resource* resource) noexcept {
    D3D12_RESOURCE_BARRIER result{};
    result.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    result.UAV.pResource = resource;
    return result;
}

[[nodiscard]] D3D12_RESOURCE_BARRIER transitionBarrier(
    ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER result{};
    result.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    result.Transition.pResource = resource;
    result.Transition.StateBefore = before;
    result.Transition.StateAfter = after;
    result.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return result;
}

} // namespace

GpuParticleSimulator::~GpuParticleSimulator() {
    shutdown();
}

bool GpuParticleSimulator::init(
    d3d12::D3D12Device& device, uint32_t capacity) {
    shutdown();
    if (!device.getDevice() || capacity == 0u) return false;
    m_device = &device;
    if (!m_pipeline.initialize(device.getDevice()) ||
        !createBuffersAndDescriptors(capacity) ||
        !createCommandUploads(capacity)) {
        shutdown();
        return false;
    }
    m_stats.capacity = capacity;
    m_resetPending = true;
    m_initialized = true;
    TD_LOG_INFO(
        "[GpuParticleSimulator] initialized capacity={} stateBytes={} contractVersion={}",
        capacity,
        static_cast<uint64_t>(capacity) *
            sizeof(fx::gpu_particle::GpuParticleState),
        fx::gpu_particle::kContractVersion);
    return true;
}

void GpuParticleSimulator::shutdown() noexcept {
    releaseDescriptors();
    for (size_t index = 0; index < m_commandUploads.size(); ++index) {
        if (m_commandUploads[index] && m_mappedCommandUploads[index]) {
            m_commandUploads[index]->Unmap(0, nullptr);
        }
        m_mappedCommandUploads[index] = nullptr;
        m_commandUploads[index].Reset();
        if (m_visibilityAuthorityUploads[index] &&
            m_mappedVisibilityAuthority[index]) {
            m_visibilityAuthorityUploads[index]->Unmap(0, nullptr);
        }
        m_mappedVisibilityAuthority[index] = nullptr;
        m_visibilityAuthorityUploads[index].Reset();
        m_visibilityAuthorityFrameOrdinals[index] = 0;
        if (m_diagnosticCounterReadbacks[index] &&
            m_mappedDiagnosticCounterReadbacks[index]) {
            m_diagnosticCounterReadbacks[index]->Unmap(0, nullptr);
        }
        m_mappedDiagnosticCounterReadbacks[index] = nullptr;
        m_diagnosticCounterReadbacks[index].Reset();
        m_diagnosticCounterReadbackPending[index] = false;
        if (m_diagnosticStateReadbacks[index] &&
            m_mappedDiagnosticStateReadbacks[index]) {
            m_diagnosticStateReadbacks[index]->Unmap(0, nullptr);
        }
        m_mappedDiagnosticStateReadbacks[index] = nullptr;
        m_diagnosticStateReadbacks[index].Reset();
        m_diagnosticStateReadbackCounts[index] = 0;
    }
    m_particleCounters.Reset();
    if (m_templateMaterialMapUpload && m_mappedTemplateMaterialBins) {
        m_templateMaterialMapUpload->Unmap(0u, nullptr);
    }
    m_mappedTemplateMaterialBins = nullptr;
    m_templateMaterialMapUpload.Reset();
    m_materialParticleIndices.Reset();
    m_materialIndirectArgs.Reset();
    m_materialBinCursors.Reset();
    m_materialBinOffsets.Reset();
    m_materialBinCounts.Reset();
    m_aliveParticleIndices.Reset();
    m_visibleParticleIndices.Reset();
    m_particleStates.Reset();
    m_pipeline.shutdown();
    m_normalizedCommands = {};
    m_commandNormalizationScratch = {};
    m_stats = {};
    m_resetPending = false;
    m_resourcesInUavState = false;
    m_indirectDrawResourcesInGraphicsState = false;
    m_materialIndirectArgsInIndirectState = false;
    m_initialized = false;
    m_device = nullptr;
}

void GpuParticleSimulator::requestReset(uint64_t authorityEpoch) noexcept {
    m_stats.authorityEpoch = authorityEpoch;
    m_resetPending = true;
}

bool GpuParticleSimulator::createBuffersAndDescriptors(uint32_t capacity) {
    if (!m_device || !m_device->getDevice()) return false;
    const D3D12_HEAP_PROPERTIES heap = defaultHeap();
    const D3D12_RESOURCE_DESC stateDescription = bufferDescription(
        static_cast<uint64_t>(capacity) *
            sizeof(fx::gpu_particle::GpuParticleState),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    HRESULT result = m_device->getDevice()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &stateDescription,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&m_particleStates));
    if (FAILED(result)) return false;
    m_particleStates->SetName(L"GpuParticleSimulator.States");

    constexpr uint64_t counterBytes = sizeof(AliveCompactReadbackHeader);
    const D3D12_RESOURCE_DESC counterDescription = bufferDescription(
        counterBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    result = m_device->getDevice()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &counterDescription,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&m_particleCounters));
    if (FAILED(result)) return false;
    m_particleCounters->SetName(L"GpuParticleSimulator.Counters");

    const D3D12_RESOURCE_DESC aliveIndexDescription = bufferDescription(
        static_cast<uint64_t>(capacity) * sizeof(uint32_t),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    result = m_device->getDevice()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &aliveIndexDescription,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&m_aliveParticleIndices));
    if (FAILED(result)) return false;
    m_aliveParticleIndices->SetName(L"GpuParticleSimulator.AliveIndices");

    const D3D12_RESOURCE_DESC visibleIndexDescription = bufferDescription(
        static_cast<uint64_t>(capacity) * sizeof(uint32_t),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    result = m_device->getDevice()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &visibleIndexDescription,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&m_visibleParticleIndices));
    if (FAILED(result)) return false;
    m_visibleParticleIndices->SetName(
        L"GpuParticleSimulator.VisibleIndices");

    const D3D12_RESOURCE_DESC templateMapDescription = bufferDescription(
        static_cast<uint64_t>(kTemplateMaterialMapCapacity) *
            sizeof(uint32_t), D3D12_RESOURCE_FLAG_NONE);
    const D3D12_HEAP_PROPERTIES upload = uploadHeap();
    result = m_device->getDevice()->CreateCommittedResource(
        &upload, D3D12_HEAP_FLAG_NONE, &templateMapDescription,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_templateMaterialMapUpload));
    if (FAILED(result)) return false;
    m_templateMaterialMapUpload->SetName(
        L"GpuParticleSimulator.TemplateMaterialMap");
    void* mappedTemplateBins = nullptr;
    result = m_templateMaterialMapUpload->Map(
        0u, nullptr, &mappedTemplateBins);
    if (FAILED(result) || !mappedTemplateBins) return false;
    m_mappedTemplateMaterialBins = static_cast<uint32_t*>(mappedTemplateBins);
    std::fill_n(
        m_mappedTemplateMaterialBins, kTemplateMaterialMapCapacity,
        UINT32_MAX);

    const auto createMaterialBuffer = [&](const wchar_t* name,
                                          auto& resource) {
        const D3D12_RESOURCE_DESC description = bufferDescription(
            static_cast<uint64_t>(capacity) * sizeof(uint32_t),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const HRESULT createResult = m_device->getDevice()->
            CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COMMON, nullptr,
                IID_PPV_ARGS(&resource));
        if (SUCCEEDED(createResult)) resource->SetName(name);
        return SUCCEEDED(createResult);
    };
    if (!createMaterialBuffer(
            L"GpuParticleSimulator.MaterialBinCounts", m_materialBinCounts) ||
        !createMaterialBuffer(
            L"GpuParticleSimulator.MaterialBinOffsets", m_materialBinOffsets) ||
        !createMaterialBuffer(
            L"GpuParticleSimulator.MaterialBinCursors", m_materialBinCursors) ||
        !createMaterialBuffer(
            L"GpuParticleSimulator.MaterialIndices", m_materialParticleIndices)) {
        return false;
    }
    static_assert(sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) == 20u);
    const D3D12_RESOURCE_DESC indirectDescription = bufferDescription(
        static_cast<uint64_t>(capacity) *
            sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    result = m_device->getDevice()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &indirectDescription,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&m_materialIndirectArgs));
    if (FAILED(result)) return false;
    m_materialIndirectArgs->SetName(
        L"GpuParticleSimulator.MaterialIndirectArgs");

    m_particleStatesUav = m_device->allocateSrvDescriptor();
    m_particleStatesSrv = m_device->allocateSrvDescriptor();
    m_particleCountersUav = m_device->allocateSrvDescriptor();
    m_aliveParticleIndicesUav = m_device->allocateSrvDescriptor();
    m_visibleParticleIndicesUav = m_device->allocateSrvDescriptor();
    m_templateMaterialMapSrv = m_device->allocateSrvDescriptor();
    m_materialBinCountsUav = m_device->allocateSrvDescriptor();
    m_materialBinOffsetsUav = m_device->allocateSrvDescriptor();
    m_materialBinCursorsUav = m_device->allocateSrvDescriptor();
    m_materialParticleIndicesUav = m_device->allocateSrvDescriptor();
    m_materialParticleIndicesSrv = m_device->allocateSrvDescriptor();
    m_materialIndirectArgsUav = m_device->allocateSrvDescriptor();
    if (m_particleStatesUav == UINT32_MAX ||
        m_particleStatesSrv == UINT32_MAX ||
        m_particleCountersUav == UINT32_MAX ||
        m_aliveParticleIndicesUav == UINT32_MAX ||
        m_visibleParticleIndicesUav == UINT32_MAX ||
        m_templateMaterialMapSrv == UINT32_MAX ||
        m_materialBinCountsUav == UINT32_MAX ||
        m_materialBinOffsetsUav == UINT32_MAX ||
        m_materialBinCursorsUav == UINT32_MAX ||
        m_materialParticleIndicesUav == UINT32_MAX ||
        m_materialParticleIndicesSrv == UINT32_MAX ||
        m_materialIndirectArgsUav == UINT32_MAX) {
        return false;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC stateUav{};
    stateUav.Format = DXGI_FORMAT_UNKNOWN;
    stateUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    stateUav.Buffer.NumElements = capacity;
    stateUav.Buffer.StructureByteStride =
        sizeof(fx::gpu_particle::GpuParticleState);
    m_device->getDevice()->CreateUnorderedAccessView(
        m_particleStates.Get(), nullptr, &stateUav,
        m_device->getSrvCpuHandle(m_particleStatesUav));

    D3D12_SHADER_RESOURCE_VIEW_DESC stateSrv{};
    stateSrv.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    stateSrv.Format = DXGI_FORMAT_UNKNOWN;
    stateSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    stateSrv.Buffer.NumElements = capacity;
    stateSrv.Buffer.StructureByteStride =
        sizeof(fx::gpu_particle::GpuParticleState);
    m_device->getDevice()->CreateShaderResourceView(
        m_particleStates.Get(), &stateSrv,
        m_device->getSrvCpuHandle(m_particleStatesSrv));

    D3D12_UNORDERED_ACCESS_VIEW_DESC counterUav{};
    counterUav.Format = DXGI_FORMAT_R32_TYPELESS;
    counterUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    counterUav.Buffer.NumElements =
        sizeof(AliveCompactReadbackHeader) / sizeof(uint32_t);
    counterUav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    m_device->getDevice()->CreateUnorderedAccessView(
        m_particleCounters.Get(), nullptr, &counterUav,
        m_device->getSrvCpuHandle(m_particleCountersUav));

    D3D12_UNORDERED_ACCESS_VIEW_DESC aliveIndexUav{};
    aliveIndexUav.Format = DXGI_FORMAT_R32_UINT;
    aliveIndexUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    aliveIndexUav.Buffer.NumElements = capacity;
    m_device->getDevice()->CreateUnorderedAccessView(
        m_aliveParticleIndices.Get(), nullptr, &aliveIndexUav,
        m_device->getSrvCpuHandle(m_aliveParticleIndicesUav));

    D3D12_UNORDERED_ACCESS_VIEW_DESC visibleIndexUav{};
    visibleIndexUav.Format = DXGI_FORMAT_R32_UINT;
    visibleIndexUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    visibleIndexUav.Buffer.NumElements = capacity;
    m_device->getDevice()->CreateUnorderedAccessView(
        m_visibleParticleIndices.Get(), nullptr, &visibleIndexUav,
        m_device->getSrvCpuHandle(m_visibleParticleIndicesUav));

    D3D12_SHADER_RESOURCE_VIEW_DESC templateMapSrv{};
    templateMapSrv.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    templateMapSrv.Format = DXGI_FORMAT_UNKNOWN;
    templateMapSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    templateMapSrv.Buffer.NumElements = kTemplateMaterialMapCapacity;
    templateMapSrv.Buffer.StructureByteStride = sizeof(uint32_t);
    m_device->getDevice()->CreateShaderResourceView(
        m_templateMaterialMapUpload.Get(), &templateMapSrv,
        m_device->getSrvCpuHandle(m_templateMaterialMapSrv));

    const auto createUintUav = [&](ID3D12Resource* resource,
                                   uint32_t descriptor) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC description{};
        description.Format = DXGI_FORMAT_R32_UINT;
        description.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        description.Buffer.NumElements = capacity;
        m_device->getDevice()->CreateUnorderedAccessView(
            resource, nullptr, &description,
            m_device->getSrvCpuHandle(descriptor));
    };
    createUintUav(m_materialBinCounts.Get(), m_materialBinCountsUav);
    createUintUav(m_materialBinOffsets.Get(), m_materialBinOffsetsUav);
    createUintUav(m_materialBinCursors.Get(), m_materialBinCursorsUav);
    createUintUav(
        m_materialParticleIndices.Get(), m_materialParticleIndicesUav);
    D3D12_SHADER_RESOURCE_VIEW_DESC materialIndicesSrv{};
    materialIndicesSrv.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    materialIndicesSrv.Format = DXGI_FORMAT_UNKNOWN;
    materialIndicesSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    materialIndicesSrv.Buffer.NumElements = capacity;
    materialIndicesSrv.Buffer.StructureByteStride = sizeof(uint32_t);
    m_device->getDevice()->CreateShaderResourceView(
        m_materialParticleIndices.Get(), &materialIndicesSrv,
        m_device->getSrvCpuHandle(m_materialParticleIndicesSrv));
    D3D12_UNORDERED_ACCESS_VIEW_DESC indirectArgsUav{};
    indirectArgsUav.Format = DXGI_FORMAT_UNKNOWN;
    indirectArgsUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    indirectArgsUav.Buffer.NumElements = capacity;
    indirectArgsUav.Buffer.StructureByteStride =
        sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    m_device->getDevice()->CreateUnorderedAccessView(
        m_materialIndirectArgs.Get(), nullptr, &indirectArgsUav,
        m_device->getSrvCpuHandle(m_materialIndirectArgsUav));
    m_stats.aliveIndexCapacity = capacity;
    m_stats.visibleIndexCapacity = capacity;
    m_stats.materialBinCapacity = capacity;
    return true;
}

bool GpuParticleSimulator::createCommandUploads(uint32_t capacity) {
    static_assert(
        kBufferedFrames == d3d12::D3D12Device::FRAME_COUNT,
        "GPU particle command uploads must follow the device fence ring");
    if (!m_device || !m_device->getDevice()) return false;
    const uint64_t birthBytes = static_cast<uint64_t>(capacity) *
        sizeof(fx::gpu_particle::GpuParticleBirthCommand);
    const uint64_t retireBytes = static_cast<uint64_t>(capacity) *
        sizeof(fx::gpu_particle::GpuParticleRetireCommand);
    const D3D12_RESOURCE_DESC description = bufferDescription(
        birthBytes + retireBytes, D3D12_RESOURCE_FLAG_NONE);
    const D3D12_RESOURCE_DESC visibilityDescription = bufferDescription(
        static_cast<uint64_t>(capacity) * sizeof(uint32_t),
        D3D12_RESOURCE_FLAG_NONE);
    const D3D12_HEAP_PROPERTIES heap = uploadHeap();
    for (uint32_t frame = 0; frame < kBufferedFrames; ++frame) {
        HRESULT result = m_device->getDevice()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_commandUploads[frame]));
        if (FAILED(result)) return false;
        const container::String name =
            "GpuParticleSimulator.CommandUpload." + std::to_string(frame);
        const std::wstring wideName(name.begin(), name.end());
        m_commandUploads[frame]->SetName(wideName.c_str());
        void* mapped = nullptr;
        result = m_commandUploads[frame]->Map(0, nullptr, &mapped);
        if (FAILED(result) || !mapped) return false;
        m_mappedCommandUploads[frame] = static_cast<uint8_t*>(mapped);

        result = m_device->getDevice()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &visibilityDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_visibilityAuthorityUploads[frame]));
        if (FAILED(result)) return false;
        const std::wstring visibilityName =
            L"GpuParticleSimulator.VisibilityAuthority." +
            std::to_wstring(frame);
        m_visibilityAuthorityUploads[frame]->SetName(
            visibilityName.c_str());
        void* mappedVisibility = nullptr;
        result = m_visibilityAuthorityUploads[frame]->Map(
            0u, nullptr, &mappedVisibility);
        if (FAILED(result) || !mappedVisibility) return false;
        m_mappedVisibilityAuthority[frame] =
            static_cast<uint32_t*>(mappedVisibility);
        std::fill_n(
            m_mappedVisibilityAuthority[frame], capacity, 0u);

        m_birthCommandSrvs[frame] = m_device->allocateSrvDescriptor();
        m_retireCommandSrvs[frame] = m_device->allocateSrvDescriptor();
        m_visibilityAuthoritySrvs[frame] =
            m_device->allocateSrvDescriptor();
        if (m_birthCommandSrvs[frame] == UINT32_MAX ||
            m_retireCommandSrvs[frame] == UINT32_MAX ||
            m_visibilityAuthoritySrvs[frame] == UINT32_MAX) {
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC birthSrv{};
        birthSrv.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        birthSrv.Format = DXGI_FORMAT_UNKNOWN;
        birthSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        birthSrv.Buffer.FirstElement = 0;
        birthSrv.Buffer.NumElements = capacity;
        birthSrv.Buffer.StructureByteStride =
            sizeof(fx::gpu_particle::GpuParticleBirthCommand);
        m_device->getDevice()->CreateShaderResourceView(
            m_commandUploads[frame].Get(), &birthSrv,
            m_device->getSrvCpuHandle(m_birthCommandSrvs[frame]));

        D3D12_SHADER_RESOURCE_VIEW_DESC retireSrv{};
        retireSrv.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        retireSrv.Format = DXGI_FORMAT_UNKNOWN;
        retireSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        retireSrv.Buffer.FirstElement = birthBytes /
            sizeof(fx::gpu_particle::GpuParticleRetireCommand);
        retireSrv.Buffer.NumElements = capacity;
        retireSrv.Buffer.StructureByteStride =
            sizeof(fx::gpu_particle::GpuParticleRetireCommand);
        m_device->getDevice()->CreateShaderResourceView(
            m_commandUploads[frame].Get(), &retireSrv,
            m_device->getSrvCpuHandle(m_retireCommandSrvs[frame]));

        D3D12_SHADER_RESOURCE_VIEW_DESC visibilitySrv{};
        visibilitySrv.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        visibilitySrv.Format = DXGI_FORMAT_UNKNOWN;
        visibilitySrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        visibilitySrv.Buffer.NumElements = capacity;
        visibilitySrv.Buffer.StructureByteStride = sizeof(uint32_t);
        m_device->getDevice()->CreateShaderResourceView(
            m_visibilityAuthorityUploads[frame].Get(), &visibilitySrv,
            m_device->getSrvCpuHandle(
                m_visibilityAuthoritySrvs[frame]));

        const D3D12_HEAP_PROPERTIES readback = readbackHeap();
        const D3D12_RESOURCE_DESC readbackDescription = bufferDescription(
            sizeof(AliveCompactReadbackHeader), D3D12_RESOURCE_FLAG_NONE);
        result = m_device->getDevice()->CreateCommittedResource(
            &readback, D3D12_HEAP_FLAG_NONE, &readbackDescription,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&m_diagnosticCounterReadbacks[frame]));
        if (FAILED(result)) return false;
        const std::wstring readbackName =
            L"GpuParticleSimulator.CounterReadback." +
            std::to_wstring(frame);
        m_diagnosticCounterReadbacks[frame]->SetName(readbackName.c_str());
        void* mappedReadback = nullptr;
        const D3D12_RANGE readableRange{
            0u, sizeof(AliveCompactReadbackHeader)};
        result = m_diagnosticCounterReadbacks[frame]->Map(
            0u, &readableRange, &mappedReadback);
        if (FAILED(result) || !mappedReadback) return false;
        m_mappedDiagnosticCounterReadbacks[frame] =
            static_cast<uint8_t*>(mappedReadback);

        constexpr uint64_t diagnosticStateBytes =
            static_cast<uint64_t>(d3d12::performance_limits::
                kGpuParticleAbStateSampleCapacity) *
            sizeof(fx::gpu_particle::GpuParticleState);
        const D3D12_RESOURCE_DESC stateReadbackDescription =
            bufferDescription(
                diagnosticStateBytes, D3D12_RESOURCE_FLAG_NONE);
        result = m_device->getDevice()->CreateCommittedResource(
            &readback, D3D12_HEAP_FLAG_NONE, &stateReadbackDescription,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&m_diagnosticStateReadbacks[frame]));
        if (FAILED(result)) return false;
        const std::wstring stateReadbackName =
            L"GpuParticleSimulator.StateReadback." +
            std::to_wstring(frame);
        m_diagnosticStateReadbacks[frame]->SetName(
            stateReadbackName.c_str());
        void* mappedStates = nullptr;
        const D3D12_RANGE readableStates{0u, diagnosticStateBytes};
        result = m_diagnosticStateReadbacks[frame]->Map(
            0u, &readableStates, &mappedStates);
        if (FAILED(result) || !mappedStates) return false;
        m_mappedDiagnosticStateReadbacks[frame] =
            static_cast<uint8_t*>(mappedStates);
    }
    return true;
}

void GpuParticleSimulator::releaseDescriptors() noexcept {
    if (m_device) {
        for (uint32_t frame = 0; frame < kBufferedFrames; ++frame) {
            if (m_birthCommandSrvs[frame] != UINT32_MAX) {
                m_device->freeSrvDescriptor(m_birthCommandSrvs[frame]);
            }
            if (m_retireCommandSrvs[frame] != UINT32_MAX) {
                m_device->freeSrvDescriptor(m_retireCommandSrvs[frame]);
            }
            if (m_visibilityAuthoritySrvs[frame] != UINT32_MAX) {
                m_device->freeSrvDescriptor(
                    m_visibilityAuthoritySrvs[frame]);
            }
        }
        if (m_particleStatesUav != UINT32_MAX) {
            m_device->freeSrvDescriptor(m_particleStatesUav);
        }
        if (m_particleStatesSrv != UINT32_MAX) {
            m_device->freeSrvDescriptor(m_particleStatesSrv);
        }
        if (m_particleCountersUav != UINT32_MAX) {
            m_device->freeSrvDescriptor(m_particleCountersUav);
        }
        if (m_aliveParticleIndicesUav != UINT32_MAX) {
            m_device->freeSrvDescriptor(m_aliveParticleIndicesUav);
        }
        if (m_visibleParticleIndicesUav != UINT32_MAX) {
            m_device->freeSrvDescriptor(m_visibleParticleIndicesUav);
        }
        if (m_templateMaterialMapSrv != UINT32_MAX) {
            m_device->freeSrvDescriptor(m_templateMaterialMapSrv);
        }
        if (m_materialBinCountsUav != UINT32_MAX) {
            m_device->freeSrvDescriptor(m_materialBinCountsUav);
        }
        if (m_materialBinOffsetsUav != UINT32_MAX) {
            m_device->freeSrvDescriptor(m_materialBinOffsetsUav);
        }
        if (m_materialBinCursorsUav != UINT32_MAX) {
            m_device->freeSrvDescriptor(m_materialBinCursorsUav);
        }
        if (m_materialParticleIndicesUav != UINT32_MAX) {
            m_device->freeSrvDescriptor(m_materialParticleIndicesUav);
        }
        if (m_materialParticleIndicesSrv != UINT32_MAX) {
            m_device->freeSrvDescriptor(m_materialParticleIndicesSrv);
        }
        if (m_materialIndirectArgsUav != UINT32_MAX) {
            m_device->freeSrvDescriptor(m_materialIndirectArgsUav);
        }
    }
    m_birthCommandSrvs.fill(UINT32_MAX);
    m_retireCommandSrvs.fill(UINT32_MAX);
    m_visibilityAuthoritySrvs.fill(UINT32_MAX);
    m_particleStatesUav = UINT32_MAX;
    m_particleStatesSrv = UINT32_MAX;
    m_particleCountersUav = UINT32_MAX;
    m_aliveParticleIndicesUav = UINT32_MAX;
    m_visibleParticleIndicesUav = UINT32_MAX;
    m_templateMaterialMapSrv = UINT32_MAX;
    m_materialBinCountsUav = UINT32_MAX;
    m_materialBinOffsetsUav = UINT32_MAX;
    m_materialBinCursorsUav = UINT32_MAX;
    m_materialParticleIndicesUav = UINT32_MAX;
    m_materialParticleIndicesSrv = UINT32_MAX;
    m_materialIndirectArgsUav = UINT32_MAX;
}

void GpuParticleSimulator::bindComputeState(
    ID3D12PipelineState* pipeline, uint32_t activeCount,
    uint32_t authoredFrames, uint32_t birthCount,
    uint32_t retireCount, uint32_t aliveIndexCapacity,
    uint32_t visibleIndexCapacity) {
    ID3D12GraphicsCommandList* commands = m_device->commandList();
    const uint32_t constants[11] = {
        m_stats.capacity,
        std::min(activeCount, m_stats.capacity),
        authoredFrames,
        static_cast<uint32_t>(m_stats.authorityEpoch),
        birthCount,
        retireCount,
        std::min(aliveIndexCapacity, m_stats.aliveIndexCapacity),
        std::min(visibleIndexCapacity, m_stats.visibleIndexCapacity),
        m_stats.materialTemplateMapCount,
        m_stats.materialBinCount,
        0u,
    };
    const uint32_t frame = m_device->frameIndex();
    m_device->bindSrvHeap();
    commands->SetComputeRootSignature(m_pipeline.rootSignature());
    m_device->recordComputeRootSignatureCall();
    commands->SetPipelineState(pipeline);
    m_device->recordPipelineStateCall();
    commands->SetComputeRoot32BitConstants(0, 11, constants, 0);
    commands->SetComputeRootDescriptorTable(
        1, m_device->getSrvGpuHandle(m_birthCommandSrvs[frame]));
    m_device->recordComputeDescriptorTableCall();
    commands->SetComputeRootDescriptorTable(
        2, m_device->getSrvGpuHandle(m_retireCommandSrvs[frame]));
    m_device->recordComputeDescriptorTableCall();
    commands->SetComputeRootDescriptorTable(
        3, m_device->getSrvGpuHandle(m_templateMaterialMapSrv));
    m_device->recordComputeDescriptorTableCall();
    commands->SetComputeRootDescriptorTable(
        4, m_device->getSrvGpuHandle(m_visibilityAuthoritySrvs[frame]));
    m_device->recordComputeDescriptorTableCall();
    commands->SetComputeRootDescriptorTable(
        5, m_device->getSrvGpuHandle(m_particleStatesUav));
    m_device->recordComputeDescriptorTableCall();
    commands->SetComputeRootDescriptorTable(
        6, m_device->getSrvGpuHandle(m_particleCountersUav));
    m_device->recordComputeDescriptorTableCall();
    commands->SetComputeRootDescriptorTable(
        7, m_device->getSrvGpuHandle(m_aliveParticleIndicesUav));
    m_device->recordComputeDescriptorTableCall();
    commands->SetComputeRootDescriptorTable(
        8, m_device->getSrvGpuHandle(m_visibleParticleIndicesUav));
    m_device->recordComputeDescriptorTableCall();
    commands->SetComputeRootDescriptorTable(
        9, m_device->getSrvGpuHandle(m_materialBinCountsUav));
    m_device->recordComputeDescriptorTableCall();
    commands->SetComputeRootDescriptorTable(
        10, m_device->getSrvGpuHandle(m_materialBinOffsetsUav));
    m_device->recordComputeDescriptorTableCall();
    commands->SetComputeRootDescriptorTable(
        11, m_device->getSrvGpuHandle(m_materialBinCursorsUav));
    m_device->recordComputeDescriptorTableCall();
    commands->SetComputeRootDescriptorTable(
        12, m_device->getSrvGpuHandle(m_materialParticleIndicesUav));
    m_device->recordComputeDescriptorTableCall();
    commands->SetComputeRootDescriptorTable(
        13, m_device->getSrvGpuHandle(m_materialIndirectArgsUav));
    m_device->recordComputeDescriptorTableCall();
}

void GpuParticleSimulator::recordUavBarriers() {
    const D3D12_RESOURCE_BARRIER barriers[] = {
        uavBarrier(m_particleStates.Get()),
        uavBarrier(m_particleCounters.Get()),
        uavBarrier(m_aliveParticleIndices.Get()),
        uavBarrier(m_visibleParticleIndices.Get()),
        uavBarrier(m_materialBinCounts.Get()),
        uavBarrier(m_materialBinOffsets.Get()),
        uavBarrier(m_materialBinCursors.Get()),
        uavBarrier(m_materialParticleIndices.Get()),
    };
    m_device->commandList()->ResourceBarrier(
        static_cast<UINT>(std::size(barriers)), barriers);
    m_stats.uavBarriers += std::size(barriers);
}

void GpuParticleSimulator::recordInitialTransitions() {
    if (m_indirectDrawResourcesInGraphicsState) {
        const D3D12_RESOURCE_BARRIER drawInputsToCompute[] = {
            transitionBarrier(
                m_particleStates.Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            transitionBarrier(
                m_materialParticleIndices.Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        m_device->commandList()->ResourceBarrier(
            static_cast<UINT>(std::size(drawInputsToCompute)),
            drawInputsToCompute);
        m_stats.transitionBarriers += std::size(drawInputsToCompute);
        m_stats.indirectDrawResourceTransitions +=
            std::size(drawInputsToCompute);
        m_indirectDrawResourcesInGraphicsState = false;
    }
    if (m_resourcesInUavState) return;
    const D3D12_RESOURCE_BARRIER barriers[] = {
        transitionBarrier(
            m_particleStates.Get(), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transitionBarrier(
            m_particleCounters.Get(), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transitionBarrier(
            m_aliveParticleIndices.Get(), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transitionBarrier(
            m_visibleParticleIndices.Get(), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transitionBarrier(
            m_materialBinCounts.Get(), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transitionBarrier(
            m_materialBinOffsets.Get(), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transitionBarrier(
            m_materialBinCursors.Get(), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transitionBarrier(
            m_materialParticleIndices.Get(), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transitionBarrier(
            m_materialIndirectArgs.Get(), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    m_device->commandList()->ResourceBarrier(
        static_cast<UINT>(std::size(barriers)), barriers);
    m_stats.transitionBarriers += std::size(barriers);
    m_resourcesInUavState = true;
    m_materialIndirectArgsInIndirectState = false;
}

bool GpuParticleSimulator::recordPendingReset() {
    if (!m_initialized || !m_device || !m_device->commandList()) return false;
    recordInitialTransitions();
    if (!m_resetPending) return true;
    bindComputeState(
        m_pipeline.pipeline(GpuParticlePipelineKind::Reset), 0u, 0u);
    m_device->commandList()->Dispatch((m_stats.capacity + 63u) / 64u, 1, 1);
    m_device->recordDispatchCall();
    recordUavBarriers();
    ++m_stats.resetDispatches;
    m_resetPending = false;
    return true;
}

bool GpuParticleSimulator::recordCommands(
    container::Span<const fx::gpu_particle::GpuParticleBirthCommand> births,
    container::Span<const fx::gpu_particle::GpuParticleRetireCommand> retires) {
    if (!m_initialized || !m_device || !m_device->commandList()) return false;
    if (!recordPendingReset()) return false;
    const fx::ParticleGpuCommandNormalizationSummary normalized =
        fx::normalizeGpuParticleCommandsInto(
            m_normalizedCommands, m_commandNormalizationScratch,
            static_cast<uint32_t>(m_stats.authorityEpoch), births, retires,
            m_stats.capacity);
    m_stats.rejectedCommands += normalized.rejectedCommands;
    const auto& normalizationStats = m_commandNormalizationScratch.stats();
    m_stats.normalizationSlotScratchCapacity =
        normalizationStats.slotCapacity;
    m_stats.normalizationSlotScratchHighWater =
        normalizationStats.slotHighWater;
    m_stats.normalizationBirthScratchCapacity =
        normalizationStats.birthOutputCapacity;
    m_stats.normalizationRetireScratchCapacity =
        normalizationStats.retireOutputCapacity;
    m_stats.normalizationScratchCapacityGrowths =
        normalizationStats.capacityGrowths;
    births = m_normalizedCommands.births;
    retires = m_normalizedCommands.retires;
    const uint32_t birthCount = static_cast<uint32_t>(births.size());
    const uint32_t retireCount = static_cast<uint32_t>(retires.size());
    if (birthCount == 0u && retireCount == 0u) return true;

    const uint32_t frame = m_device->frameIndex();
    uint8_t* const upload = m_mappedCommandUploads[frame];
    if (!upload) return false;
    const size_t birthCapacityBytes = static_cast<size_t>(m_stats.capacity) *
        sizeof(fx::gpu_particle::GpuParticleBirthCommand);
    if (birthCount != 0u) {
        std::memcpy(
            upload, births.data(), static_cast<size_t>(birthCount) *
                sizeof(fx::gpu_particle::GpuParticleBirthCommand));
    }
    if (birthCount != 0u) {
        bindComputeState(
            m_pipeline.pipeline(GpuParticlePipelineKind::ApplyBirth), 0u, 0u,
            birthCount, retireCount);
        m_device->commandList()->Dispatch(
            (birthCount + 63u) / 64u, 1, 1);
        m_device->recordDispatchCall();
        recordUavBarriers();
        ++m_stats.commandDispatches;
    }
    if (retireCount != 0u) {
        std::memcpy(
            upload + birthCapacityBytes, retires.data(),
            static_cast<size_t>(retireCount) *
                sizeof(fx::gpu_particle::GpuParticleRetireCommand));
        bindComputeState(
            m_pipeline.pipeline(GpuParticlePipelineKind::ApplyRetire), 0u, 0u,
            birthCount, retireCount);
        m_device->commandList()->Dispatch(
            (retireCount + 63u) / 64u, 1, 1);
        m_device->recordDispatchCall();
        recordUavBarriers();
        ++m_stats.commandDispatches;
    }
    m_stats.submittedBirthCommands += birthCount;
    m_stats.submittedRetireCommands += retireCount;
    return true;
}

bool GpuParticleSimulator::recordIntegration(
    uint32_t activeCount, uint32_t authoredFrames) {
    if (!m_initialized || !m_device || !m_device->commandList()) return false;
    if (!recordPendingReset()) return false;
    activeCount = std::min(activeCount, m_stats.capacity);
    if (activeCount == 0u || authoredFrames == 0u) return true;
    bindComputeState(
        m_pipeline.pipeline(GpuParticlePipelineKind::Integrate),
        activeCount, authoredFrames);
    m_device->commandList()->Dispatch((activeCount + 63u) / 64u, 1, 1);
    m_device->recordDispatchCall();
    recordUavBarriers();
    ++m_stats.integrationDispatches;
    return true;
}

bool GpuParticleSimulator::recordAliveCompact(uint32_t outputCapacity) {
    if (!m_initialized || !m_device || !m_device->commandList() ||
        !m_pipeline.pipeline(GpuParticlePipelineKind::ResetAliveCompact) ||
        !m_pipeline.pipeline(GpuParticlePipelineKind::AliveCompact) ||
        !m_aliveParticleIndices) {
        return false;
    }
    if (!recordPendingReset()) return false;
    m_stats.diagnosticAliveCountsValid = false;
    outputCapacity = std::min(outputCapacity, m_stats.aliveIndexCapacity);

    // Counter reset is a separate dispatch. A group barrier inside the scan
    // kernel cannot order thread groups globally, whereas this UAV barrier
    // makes the append base and overflow count unambiguous every frame.
    bindComputeState(
        m_pipeline.pipeline(GpuParticlePipelineKind::ResetAliveCompact),
        m_stats.capacity, 0u,
        0u, 0u, outputCapacity);
    m_device->commandList()->Dispatch(1u, 1u, 1u);
    m_device->recordDispatchCall();
    recordUavBarriers();
    ++m_stats.aliveCompactCounterResetDispatches;

    bindComputeState(
        m_pipeline.pipeline(GpuParticlePipelineKind::AliveCompact),
        m_stats.capacity, 0u,
        0u, 0u, outputCapacity);
    m_device->commandList()->Dispatch(
        (m_stats.capacity + 63u) / 64u, 1u, 1u);
    m_device->recordDispatchCall();
    recordUavBarriers();
    ++m_stats.aliveCompactDispatches;
    return true;
}

bool GpuParticleSimulator::publishVisibilityAuthority(
    container::Span<const GpuParticleVisibilityGeneration> visible) {
    if (!m_initialized || !m_device || visible.size() > m_stats.capacity) {
        ++m_stats.visibilityAuthorityRejects;
        return false;
    }
    const uint32_t frame = m_device->frameIndex();
    uint32_t* const generations = m_mappedVisibilityAuthority[frame];
    if (!generations ||
        m_visibilityAuthoritySrvs[frame] == UINT32_MAX) {
        ++m_stats.visibilityAuthorityRejects;
        return false;
    }
    std::fill_n(generations, m_stats.capacity, 0u);
    uint64_t accepted = 0;
    for (const GpuParticleVisibilityGeneration& entry : visible) {
        if (entry.stateSlot >= m_stats.capacity ||
            entry.particleGeneration == 0u) {
            ++m_stats.visibilityAuthorityRejects;
            continue;
        }
        generations[entry.stateSlot] = entry.particleGeneration;
        ++accepted;
    }
    m_visibilityAuthorityFrameOrdinals[frame] =
        m_device->frameOrdinal();
    ++m_stats.visibilityAuthorityPublishes;
    m_stats.visibilityAuthorityEntries += accepted;
    m_stats.visibilityAuthorityBytes +=
        static_cast<uint64_t>(m_stats.capacity) * sizeof(uint32_t);
    return true;
}

bool GpuParticleSimulator::recordVisibleCompact(uint32_t outputCapacity) {
    if (!m_initialized || !m_device || !m_device->commandList() ||
        !m_pipeline.pipeline(GpuParticlePipelineKind::ResetVisibleCompact) ||
        !m_pipeline.pipeline(GpuParticlePipelineKind::VisibleCompact) ||
        !m_aliveParticleIndices ||
        !m_visibleParticleIndices) {
        return false;
    }
    const uint32_t frame = m_device->frameIndex();
    if (m_visibilityAuthorityFrameOrdinals[frame] !=
        m_device->frameOrdinal()) {
        ++m_stats.visibilityAuthorityRejects;
        return false;
    }
    if (!recordPendingReset()) return false;
    outputCapacity = std::min(outputCapacity, m_stats.visibleIndexCapacity);

    bindComputeState(
        m_pipeline.pipeline(GpuParticlePipelineKind::ResetVisibleCompact),
        m_stats.capacity, 0u,
        0u, 0u, m_stats.aliveIndexCapacity, outputCapacity);
    m_device->commandList()->Dispatch(1u, 1u, 1u);
    m_device->recordDispatchCall();
    recordUavBarriers();
    ++m_stats.visibleCompactCounterResetDispatches;

    bindComputeState(
        m_pipeline.pipeline(GpuParticlePipelineKind::VisibleCompact),
        m_stats.capacity, 0u,
        0u, 0u, m_stats.aliveIndexCapacity, outputCapacity);
    m_device->commandList()->Dispatch(
        (m_stats.aliveIndexCapacity + 63u) / 64u, 1u, 1u);
    m_device->recordDispatchCall();
    recordUavBarriers();
    ++m_stats.visibleCompactDispatches;
    return true;
}

bool GpuParticleSimulator::configureMaterialBins(
    container::Span<const uint32_t> templateToBin,
    uint32_t materialBinCount) {
    if (!m_initialized || !m_device || !m_mappedTemplateMaterialBins ||
        templateToBin.size() > kTemplateMaterialMapCapacity ||
        materialBinCount > m_stats.materialBinCapacity) {
        return false;
    }
    for (uint32_t bin : templateToBin) {
        if (bin != UINT32_MAX && bin >= materialBinCount) return false;
    }
    // Catalog replacement is a rare safe-frame operation. Waiting here makes
    // replacement of the single persistent upload mapping fence-correct and
    // avoids per-frame copies or a second authority generation.
    if (!m_device->waitIdle()) return false;
    std::fill_n(
        m_mappedTemplateMaterialBins, kTemplateMaterialMapCapacity,
        UINT32_MAX);
    if (!templateToBin.empty()) {
        std::copy(
            templateToBin.begin(), templateToBin.end(),
            m_mappedTemplateMaterialBins);
    }
    m_stats.materialTemplateMapCount =
        static_cast<uint32_t>(templateToBin.size());
    m_stats.materialBinCount = materialBinCount;
    return true;
}

bool GpuParticleSimulator::recordMaterialBins() {
    if (!m_initialized || !m_device || !m_device->commandList() ||
        !m_pipeline.pipeline(GpuParticlePipelineKind::MaterialBinReset) ||
        !m_pipeline.pipeline(GpuParticlePipelineKind::MaterialBinCount) ||
        !m_pipeline.pipeline(GpuParticlePipelineKind::MaterialBinPrefix) ||
        !m_pipeline.pipeline(GpuParticlePipelineKind::MaterialBinScatter) ||
        !m_materialBinCounts ||
        !m_materialBinOffsets || !m_materialBinCursors ||
        !m_materialParticleIndices) {
        return false;
    }
    if (m_materialIndirectArgsInIndirectState) {
        const D3D12_RESOURCE_BARRIER toUav = transitionBarrier(
            m_materialIndirectArgs.Get(),
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_device->commandList()->ResourceBarrier(1u, &toUav);
        ++m_stats.transitionBarriers;
        ++m_stats.indirectArgumentTransitions;
        m_materialIndirectArgsInIndirectState = false;
    }
    if (!recordPendingReset()) return false;

    bindComputeState(
        m_pipeline.pipeline(GpuParticlePipelineKind::MaterialBinReset),
        m_stats.capacity, 0u,
        0u, 0u, m_stats.aliveIndexCapacity,
        m_stats.visibleIndexCapacity);
    m_device->commandList()->Dispatch(
        std::max(1u, (m_stats.materialBinCount + 63u) / 64u), 1u, 1u);
    m_device->recordDispatchCall();
    recordUavBarriers();
    ++m_stats.materialBinResetDispatches;
    if (m_stats.materialBinCount == 0u) return true;

    bindComputeState(
        m_pipeline.pipeline(GpuParticlePipelineKind::MaterialBinCount),
        m_stats.capacity, 0u,
        0u, 0u, m_stats.aliveIndexCapacity,
        m_stats.visibleIndexCapacity);
    m_device->commandList()->Dispatch(
        (m_stats.visibleIndexCapacity + 63u) / 64u, 1u, 1u);
    m_device->recordDispatchCall();
    recordUavBarriers();
    ++m_stats.materialBinCountDispatches;

    bindComputeState(
        m_pipeline.pipeline(GpuParticlePipelineKind::MaterialBinPrefix),
        m_stats.capacity, 0u,
        0u, 0u, m_stats.aliveIndexCapacity,
        m_stats.visibleIndexCapacity);
    m_device->commandList()->Dispatch(1u, 1u, 1u);
    m_device->recordDispatchCall();
    recordUavBarriers();
    ++m_stats.materialBinPrefixDispatches;

    bindComputeState(
        m_pipeline.pipeline(GpuParticlePipelineKind::MaterialBinScatter),
        m_stats.capacity, 0u,
        0u, 0u, m_stats.aliveIndexCapacity,
        m_stats.visibleIndexCapacity);
    m_device->commandList()->Dispatch(
        (m_stats.visibleIndexCapacity + 63u) / 64u, 1u, 1u);
    m_device->recordDispatchCall();
    recordUavBarriers();
    ++m_stats.materialBinScatterDispatches;

    const D3D12_RESOURCE_BARRIER argumentsReady = uavBarrier(
        m_materialIndirectArgs.Get());
    m_device->commandList()->ResourceBarrier(1u, &argumentsReady);
    ++m_stats.uavBarriers;
    const D3D12_RESOURCE_BARRIER toIndirect = transitionBarrier(
        m_materialIndirectArgs.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    m_device->commandList()->ResourceBarrier(1u, &toIndirect);
    ++m_stats.transitionBarriers;
    ++m_stats.indirectArgumentTransitions;
    m_stats.indirectArgumentBuilds += m_stats.materialBinCount;
    m_materialIndirectArgsInIndirectState = true;
    return true;
}

bool GpuParticleSimulator::recordPrepareIndirectDraw() {
    if (!m_initialized || !m_device || !m_device->commandList() ||
        !m_particleStates || !m_materialParticleIndices ||
        m_particleStatesSrv == UINT32_MAX ||
        m_materialParticleIndicesSrv == UINT32_MAX ||
        !materialIndirectArgsReady() || !m_resourcesInUavState) {
        return false;
    }
    if (m_indirectDrawResourcesInGraphicsState) return true;

    const D3D12_RESOURCE_BARRIER drawInputsReady[] = {
        transitionBarrier(
            m_particleStates.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transitionBarrier(
            m_materialParticleIndices.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    m_device->commandList()->ResourceBarrier(
        static_cast<UINT>(std::size(drawInputsReady)), drawInputsReady);
    m_stats.transitionBarriers += std::size(drawInputsReady);
    m_stats.indirectDrawResourceTransitions += std::size(drawInputsReady);
    m_indirectDrawResourcesInGraphicsState = true;
    return true;
}

bool GpuParticleSimulator::recordDiagnosticCounterReadback() {
    if (!m_initialized || !m_device || !m_device->commandList() ||
        !m_particleCounters || !m_resourcesInUavState) {
        return false;
    }
    const uint32_t frame = m_device->frameIndex();
    if (frame >= kBufferedFrames ||
        !m_diagnosticCounterReadbacks[frame] ||
        !m_mappedDiagnosticCounterReadbacks[frame]) {
        return false;
    }

    const D3D12_RESOURCE_BARRIER ordered =
        uavBarrier(m_particleCounters.Get());
    m_device->commandList()->ResourceBarrier(1u, &ordered);
    ++m_stats.uavBarriers;
    const D3D12_RESOURCE_BARRIER toCopy = transitionBarrier(
        m_particleCounters.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_device->commandList()->ResourceBarrier(1u, &toCopy);
    m_device->commandList()->CopyBufferRegion(
        m_diagnosticCounterReadbacks[frame].Get(), 0u,
        m_particleCounters.Get(), 0u,
        sizeof(AliveCompactReadbackHeader));
    const D3D12_RESOURCE_BARRIER toCompute = transitionBarrier(
        m_particleCounters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_device->commandList()->ResourceBarrier(1u, &toCompute);
    m_stats.transitionBarriers += 2u;
    ++m_stats.diagnosticCounterReadbacks;
    m_diagnosticCounterReadbackPending[frame] = true;
    return true;
}

std::optional<GpuParticleSimulator::AliveCompactReadbackHeader>
GpuParticleSimulator::consumeDiagnosticCounterReadback() noexcept {
    if (!m_initialized || !m_device) return std::nullopt;
    const uint32_t frame = m_device->frameIndex();
    if (frame >= kBufferedFrames ||
        !m_diagnosticCounterReadbackPending[frame] ||
        !m_mappedDiagnosticCounterReadbacks[frame]) {
        return std::nullopt;
    }
    AliveCompactReadbackHeader result{};
    std::memcpy(
        &result, m_mappedDiagnosticCounterReadbacks[frame],
        sizeof(result));
    m_diagnosticCounterReadbackPending[frame] = false;
    return result;
}

bool GpuParticleSimulator::recordDiagnosticStateReadback(
    container::Span<const uint32_t> stateSlots) {
    constexpr uint32_t maximumSamples = d3d12::performance_limits::
        kGpuParticleAbStateSampleCapacity;
    if (!m_initialized || !m_device || !m_device->commandList() ||
        !m_particleStates || !m_resourcesInUavState ||
        stateSlots.empty() || stateSlots.size() > maximumSamples) {
        return false;
    }
    for (uint32_t slot : stateSlots) {
        if (slot >= m_stats.capacity) return false;
    }
    if (m_indirectDrawResourcesInGraphicsState) {
        recordInitialTransitions();
    }
    const uint32_t frame = m_device->frameIndex();
    if (frame >= kBufferedFrames || !m_diagnosticStateReadbacks[frame] ||
        !m_mappedDiagnosticStateReadbacks[frame]) {
        return false;
    }

    const D3D12_RESOURCE_BARRIER ordered =
        uavBarrier(m_particleStates.Get());
    m_device->commandList()->ResourceBarrier(1u, &ordered);
    ++m_stats.uavBarriers;
    const D3D12_RESOURCE_BARRIER toCopy = transitionBarrier(
        m_particleStates.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_device->commandList()->ResourceBarrier(1u, &toCopy);
    constexpr uint64_t stateBytes =
        sizeof(fx::gpu_particle::GpuParticleState);
    for (size_t index = 0; index < stateSlots.size(); ++index) {
        m_device->commandList()->CopyBufferRegion(
            m_diagnosticStateReadbacks[frame].Get(),
            static_cast<uint64_t>(index) * stateBytes,
            m_particleStates.Get(),
            static_cast<uint64_t>(stateSlots[index]) * stateBytes,
            stateBytes);
    }
    const D3D12_RESOURCE_BARRIER toCompute = transitionBarrier(
        m_particleStates.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_device->commandList()->ResourceBarrier(1u, &toCompute);
    m_stats.transitionBarriers += 2u;
    ++m_stats.diagnosticStateReadbacks;
    m_stats.diagnosticStateCopies += stateSlots.size();
    m_diagnosticStateReadbackCounts[frame] =
        static_cast<uint32_t>(stateSlots.size());
    return true;
}

std::optional<GpuParticleSimulator::DiagnosticStateSampleBatch>
GpuParticleSimulator::consumeDiagnosticStateReadback() noexcept {
    if (!m_initialized || !m_device) return std::nullopt;
    const uint32_t frame = m_device->frameIndex();
    if (frame >= kBufferedFrames ||
        m_diagnosticStateReadbackCounts[frame] == 0u ||
        !m_mappedDiagnosticStateReadbacks[frame]) {
        return std::nullopt;
    }
    DiagnosticStateSampleBatch result{};
    result.count = std::min(
        m_diagnosticStateReadbackCounts[frame],
        d3d12::performance_limits::kGpuParticleAbStateSampleCapacity);
    std::memcpy(
        result.states.data(), m_mappedDiagnosticStateReadbacks[frame],
        static_cast<size_t>(result.count) *
            sizeof(fx::gpu_particle::GpuParticleState));
    m_diagnosticStateReadbackCounts[frame] = 0;
    return result;
}

bool GpuParticleSimulator::recordStateReadback(
    ID3D12Resource& destination, uint32_t firstState,
    uint32_t stateCount) {
    if (m_indirectDrawResourcesInGraphicsState && m_device &&
        m_device->commandList()) {
        recordInitialTransitions();
    }
    if (!m_initialized || !m_device || !m_device->commandList() ||
        !m_particleStates || !m_resourcesInUavState || stateCount == 0u ||
        firstState >= m_stats.capacity ||
        stateCount > m_stats.capacity - firstState) {
        return false;
    }
    const uint64_t stateBytes = sizeof(fx::gpu_particle::GpuParticleState);
    const uint64_t copyBytes = static_cast<uint64_t>(stateCount) * stateBytes;
    if (destination.GetDesc().Width < copyBytes) return false;

    const D3D12_RESOURCE_BARRIER toCopy = transitionBarrier(
        m_particleStates.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_device->commandList()->ResourceBarrier(1u, &toCopy);
    m_device->commandList()->CopyBufferRegion(
        &destination, 0u, m_particleStates.Get(),
        static_cast<uint64_t>(firstState) * stateBytes, copyBytes);
    const D3D12_RESOURCE_BARRIER toCompute = transitionBarrier(
        m_particleStates.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_device->commandList()->ResourceBarrier(1u, &toCompute);
    m_stats.transitionBarriers += 2u;
    return true;
}

bool GpuParticleSimulator::recordAliveCompactReadback(
    ID3D12Resource& destination) {
    if (!m_initialized || !m_device || !m_device->commandList() ||
        !m_particleCounters || !m_aliveParticleIndices ||
        !m_resourcesInUavState ||
        destination.GetDesc().Width <
            aliveCompactReadbackBytes(m_stats.aliveIndexCapacity)) {
        return false;
    }

    const D3D12_RESOURCE_BARRIER toCopy[] = {
        transitionBarrier(
            m_particleCounters.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE),
        transitionBarrier(
            m_aliveParticleIndices.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE),
    };
    m_device->commandList()->ResourceBarrier(
        static_cast<UINT>(std::size(toCopy)), toCopy);
    m_device->commandList()->CopyBufferRegion(
        &destination, 0u, m_particleCounters.Get(), 0u,
        sizeof(AliveCompactReadbackHeader));
    m_device->commandList()->CopyBufferRegion(
        &destination, sizeof(AliveCompactReadbackHeader),
        m_aliveParticleIndices.Get(), 0u,
        static_cast<uint64_t>(m_stats.aliveIndexCapacity) * sizeof(uint32_t));
    const D3D12_RESOURCE_BARRIER toCompute[] = {
        transitionBarrier(
            m_particleCounters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transitionBarrier(
            m_aliveParticleIndices.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    m_device->commandList()->ResourceBarrier(
        static_cast<UINT>(std::size(toCompute)), toCompute);
    m_stats.transitionBarriers += std::size(toCopy) + std::size(toCompute);
    return true;
}

bool GpuParticleSimulator::recordVisibleCompactReadback(
    ID3D12Resource& destination) {
    if (!m_initialized || !m_device || !m_device->commandList() ||
        !m_particleCounters || !m_visibleParticleIndices ||
        !m_resourcesInUavState ||
        destination.GetDesc().Width <
            aliveCompactReadbackBytes(m_stats.visibleIndexCapacity)) {
        return false;
    }

    const D3D12_RESOURCE_BARRIER toCopy[] = {
        transitionBarrier(
            m_particleCounters.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE),
        transitionBarrier(
            m_visibleParticleIndices.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE),
    };
    m_device->commandList()->ResourceBarrier(
        static_cast<UINT>(std::size(toCopy)), toCopy);
    m_device->commandList()->CopyBufferRegion(
        &destination, 0u, m_particleCounters.Get(), 0u,
        sizeof(AliveCompactReadbackHeader));
    m_device->commandList()->CopyBufferRegion(
        &destination, sizeof(AliveCompactReadbackHeader),
        m_visibleParticleIndices.Get(), 0u,
        static_cast<uint64_t>(m_stats.visibleIndexCapacity) *
            sizeof(uint32_t));
    const D3D12_RESOURCE_BARRIER toCompute[] = {
        transitionBarrier(
            m_particleCounters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transitionBarrier(
            m_visibleParticleIndices.Get(),
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    m_device->commandList()->ResourceBarrier(
        static_cast<UINT>(std::size(toCompute)), toCompute);
    m_stats.transitionBarriers += std::size(toCopy) + std::size(toCompute);
    return true;
}

bool GpuParticleSimulator::recordMaterialBinsReadback(
    ID3D12Resource& destination) {
    if (m_indirectDrawResourcesInGraphicsState && m_device &&
        m_device->commandList()) {
        recordInitialTransitions();
    }
    const uint32_t binCount = m_stats.materialBinCount;
    if (!m_initialized || !m_device || !m_device->commandList() ||
        !m_particleCounters || !m_materialBinCounts ||
        !m_materialBinOffsets || !m_materialParticleIndices ||
        !m_resourcesInUavState ||
        destination.GetDesc().Width < materialBinReadbackBytes(
            binCount, m_stats.capacity)) {
        return false;
    }

    const bool restoreIndirect = m_materialIndirectArgsInIndirectState;
    const D3D12_RESOURCE_BARRIER toCopy[] = {
        transitionBarrier(
            m_particleCounters.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE),
        transitionBarrier(
            m_materialBinCounts.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE),
        transitionBarrier(
            m_materialBinOffsets.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE),
        transitionBarrier(
            m_materialParticleIndices.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE),
        transitionBarrier(
            m_materialIndirectArgs.Get(),
            restoreIndirect ? D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT
                            : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE),
    };
    m_device->commandList()->ResourceBarrier(
        static_cast<UINT>(std::size(toCopy)), toCopy);
    uint64_t outputOffset = 0u;
    m_device->commandList()->CopyBufferRegion(
        &destination, outputOffset, m_particleCounters.Get(), 0u,
        sizeof(AliveCompactReadbackHeader));
    outputOffset += sizeof(AliveCompactReadbackHeader);
    const uint64_t binBytes =
        static_cast<uint64_t>(binCount) * sizeof(uint32_t);
    if (binBytes != 0u) {
        m_device->commandList()->CopyBufferRegion(
            &destination, outputOffset, m_materialBinCounts.Get(), 0u,
            binBytes);
        outputOffset += binBytes;
        m_device->commandList()->CopyBufferRegion(
            &destination, outputOffset, m_materialBinOffsets.Get(), 0u,
            binBytes);
        outputOffset += binBytes;
        const uint64_t argumentBytes =
            static_cast<uint64_t>(binCount) *
            sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        m_device->commandList()->CopyBufferRegion(
            &destination, outputOffset, m_materialIndirectArgs.Get(), 0u,
            argumentBytes);
        outputOffset += argumentBytes;
    }
    m_device->commandList()->CopyBufferRegion(
        &destination, outputOffset, m_materialParticleIndices.Get(), 0u,
        static_cast<uint64_t>(m_stats.capacity) * sizeof(uint32_t));
    const D3D12_RESOURCE_BARRIER toCompute[] = {
        transitionBarrier(
            m_particleCounters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transitionBarrier(
            m_materialBinCounts.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transitionBarrier(
            m_materialBinOffsets.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transitionBarrier(
            m_materialParticleIndices.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transitionBarrier(
            m_materialIndirectArgs.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
            restoreIndirect ? D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT
                            : D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    m_device->commandList()->ResourceBarrier(
        static_cast<UINT>(std::size(toCompute)), toCompute);
    m_stats.transitionBarriers += std::size(toCopy) + std::size(toCompute);
    if (restoreIndirect) m_stats.indirectArgumentTransitions += 2u;
    return true;
}

} // namespace engine::render
