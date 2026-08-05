#include "engine/renderer/world/particle/GpuParticlePipeline.h"

#include "engine/fx/runtime/GpuParticleCommandNormalization.h"
#include "engine/renderer/d3d12/runtime/D3D12ShaderPackage.h"
#include "debug/debug.h"

#include <array>
#include <iterator>
#include <string>
#include <utility>

#ifndef TD_PARTICLE_COMPUTE_SHADER_PACKAGE_VERSION
#define TD_PARTICLE_COMPUTE_SHADER_PACKAGE_VERSION 1
#endif

#ifndef TD_PARTICLE_COMPUTE_SHADER_SOURCE_SHA256
#define TD_PARTICLE_COMPUTE_SHADER_SOURCE_SHA256 \
    "0000000000000000000000000000000000000000000000000000000000000000"
#endif

static_assert(
    TD_PARTICLE_COMPUTE_SHADER_PACKAGE_VERSION < 1000 ||
    TD_PARTICLE_COMPUTE_SHADER_PACKAGE_VERSION / 1000 ==
        engine::fx::gpu_particle::kContractVersion,
    "particle Compute package major version must match GpuParticleContract");

namespace engine::render {
namespace {

constexpr d3d12::ShaderPackageEntrySpec ShaderEntries[] = {
    {"reset_compute_file", "particle_compute_reset_cs.cso",
     "reset_compute_profile", "cs_5_0"},
    {"retire_compute_file", "particle_compute_retire_cs.cso",
     "retire_compute_profile", "cs_5_0"},
    {"birth_compute_file", "particle_compute_birth_cs.cso",
     "birth_compute_profile", "cs_5_0"},
    {"integrate_compute_file", "particle_compute_integrate_cs.cso",
     "integrate_compute_profile", "cs_5_0"},
    {"reset_alive_compact_compute_file",
     "particle_compute_reset_alive_compact_cs.cso",
     "reset_alive_compact_compute_profile", "cs_5_0"},
    {"alive_compact_compute_file", "particle_compute_alive_compact_cs.cso",
     "alive_compact_compute_profile", "cs_5_0"},
    {"visible_compact_reset_compute_file",
     "particle_compute_visible_compact_reset_cs.cso",
     "visible_compact_reset_compute_profile", "cs_5_0"},
    {"visible_compact_compute_file",
     "particle_compute_visible_compact_cs.cso",
     "visible_compact_compute_profile", "cs_5_0"},
    {"material_bin_reset_compute_file",
     "particle_compute_material_bin_reset_cs.cso",
     "material_bin_reset_compute_profile", "cs_5_0"},
    {"material_bin_count_compute_file",
     "particle_compute_material_bin_count_cs.cso",
     "material_bin_count_compute_profile", "cs_5_0"},
    {"material_bin_prefix_compute_file",
     "particle_compute_material_bin_prefix_cs.cso",
     "material_bin_prefix_compute_profile", "cs_5_0"},
    {"material_bin_scatter_compute_file",
     "particle_compute_material_bin_scatter_cs.cso",
     "material_bin_scatter_compute_profile", "cs_5_0"},
};

constexpr const wchar_t* PipelineNames[] = {
    L"GpuParticleSimulator.ResetPSO",
    L"GpuParticleSimulator.ApplyRetirePSO",
    L"GpuParticleSimulator.ApplyBirthPSO",
    L"GpuParticleSimulator.IntegratePSO",
    L"GpuParticleSimulator.ResetAliveCompactPSO",
    L"GpuParticleSimulator.AliveCompactPSO",
    L"GpuParticleSimulator.ResetVisibleCompactPSO",
    L"GpuParticleSimulator.VisibleCompactPSO",
    L"GpuParticleSimulator.MaterialBinResetPSO",
    L"GpuParticleSimulator.MaterialBinCountPSO",
    L"GpuParticleSimulator.MaterialBinPrefixPSO",
    L"GpuParticleSimulator.MaterialBinScatterPSO",
};

static_assert(std::size(ShaderEntries) ==
    static_cast<size_t>(GpuParticlePipelineKind::Count));
static_assert(std::size(PipelineNames) == std::size(ShaderEntries));

} // namespace

bool GpuParticlePipeline::initialize(ID3D12Device* device) {
    shutdown();
    if (!device || !loadShaderPackage() || !createRootSignature(device) ||
        !createPipelineStates(device)) {
        shutdown();
        return false;
    }
    return true;
}

void GpuParticlePipeline::shutdown() noexcept {
    for (auto& pipeline : m_pipelineStates) pipeline.Reset();
    m_rootSignature.Reset();
    for (auto& bytecode : m_shaderBytecode) bytecode.clear();
}

bool GpuParticlePipeline::loadShaderPackage() {
    container::Vector<container::Vector<uint8_t>> loaded;
    if (!d3d12::loadShaderPackage(
            "particle_compute",
            std::to_string(TD_PARTICLE_COMPUTE_SHADER_PACKAGE_VERSION),
            TD_PARTICLE_COMPUTE_SHADER_SOURCE_SHA256,
            ShaderEntries, loaded) ||
        loaded.size() != m_shaderBytecode.size()) {
        TD_LOG_ERROR(
            "[GpuParticleSimulator] compute shader package unavailable");
        return false;
    }
    for (size_t index = 0; index < loaded.size(); ++index) {
        m_shaderBytecode[index] = std::move(loaded[index]);
    }
    return true;
}

bool GpuParticlePipeline::createRootSignature(ID3D12Device* device) {
    D3D12_DESCRIPTOR_RANGE ranges[13]{};
    for (uint32_t index = 0; index < 13u; ++index) {
        ranges[index].RangeType = index < 4u
            ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
            : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[index].NumDescriptors = 1;
        ranges[index].BaseShaderRegister = index < 4u ? index : index - 4u;
        ranges[index].OffsetInDescriptorsFromTableStart =
            D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    }
    D3D12_ROOT_PARAMETER parameters[14]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.Num32BitValues = 11;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (uint32_t index = 0; index < 13u; ++index) {
        parameters[index + 1u].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[index + 1u].DescriptorTable.NumDescriptorRanges = 1;
        parameters[index + 1u].DescriptorTable.pDescriptorRanges =
            &ranges[index];
        parameters[index + 1u].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(std::size(parameters));
    description.pParameters = parameters;
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT serializeResult = D3D12SerializeRootSignature(
        &description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(serializeResult)) {
        TD_LOG_ERROR(
            "[GpuParticleSimulator] root signature serialization failed: {}",
            errors ? static_cast<const char*>(errors->GetBufferPointer())
                   : "no diagnostic");
        return false;
    }
    const HRESULT createResult = device->CreateRootSignature(
        0u, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature));
    if (FAILED(createResult)) {
        TD_LOG_ERROR(
            "[GpuParticleSimulator] root signature creation failed: 0x{:08X}",
            static_cast<uint32_t>(createResult));
        return false;
    }
    m_rootSignature->SetName(L"GpuParticleSimulator.RootSignature");
    return true;
}

bool GpuParticlePipeline::createPipelineStates(ID3D12Device* device) {
    if (!m_rootSignature) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
    description.pRootSignature = m_rootSignature.Get();
    for (size_t index = 0; index < m_pipelineStates.size(); ++index) {
        if (m_shaderBytecode[index].empty()) return false;
        description.CS = {
            m_shaderBytecode[index].data(), m_shaderBytecode[index].size()};
        const HRESULT result = device->CreateComputePipelineState(
            &description, IID_PPV_ARGS(&m_pipelineStates[index]));
        if (FAILED(result)) {
            TD_LOG_ERROR(
                "[GpuParticleSimulator] compute PSO {} creation failed: 0x{:08X}",
                index, static_cast<uint32_t>(result));
            return false;
        }
        m_pipelineStates[index]->SetName(PipelineNames[index]);
    }
    return true;
}

} // namespace engine::render
