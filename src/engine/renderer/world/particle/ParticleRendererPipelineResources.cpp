#include "engine/renderer/world/particle/ParticleRenderer.h"

#include "engine/renderer/d3d12/runtime/D3D12Device.h"
#include "engine/renderer/d3d12/runtime/D3D12QualitySettings.h"
#include "debug/debug.h"

#include <algorithm>

namespace engine::render {

bool ParticleRenderer::createRootSignature() {
    if (!m_device || !m_device->getDevice()) return false;

    D3D12_DESCRIPTOR_RANGE textureRange{};
    textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRange.NumDescriptors = 1;
    textureRange.BaseShaderRegister = 0;
    textureRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = 0;
    // Playable-bounds clipping is evaluated by the pixel shaders while the
    // camera basis is consumed by the vertex shader.  Both stages therefore
    // share the same immutable per-draw b0 constants.
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &textureRange;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    const d3d12::TextureSamplingQuality sampling =
        d3d12::textureSamplingQuality(
            m_textureFilter, m_anisotropyLevel);
    sampler.Filter = sampling.filter;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxAnisotropy = sampling.maximumAnisotropy;
    sampler.MaxLOD = sampling.maximumLod;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = 2;
    description.pParameters = parameters;
    description.NumStaticSamplers = 1;
    description.pStaticSamplers = &sampler;
    description.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT serializeResult = D3D12SerializeRootSignature(
        &description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(serializeResult)) {
        TD_LOG_ERROR(
            "[ParticleRenderer] root signature serialization failed: 0x{:08X}",
            static_cast<uint32_t>(serializeResult));
        return false;
    }
    const HRESULT createResult = m_device->getDevice()->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature));
    if (FAILED(createResult)) {
        TD_LOG_ERROR(
            "[ParticleRenderer] CreateRootSignature failed: 0x{:08X}",
            static_cast<uint32_t>(createResult));
        return false;
    }
    return true;
}

bool ParticleRenderer::createPipelineStates() {
    if (!m_device || !m_device->getDevice() || !m_rootSignature) return false;
    if (m_shaderBytecode[0].empty() || m_shaderBytecode[1].empty() ||
        m_shaderBytecode[2].empty()) {
        return false;
    }

    const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"INSTANCE_POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INSTANCE_SIZE", 0, DXGI_FORMAT_R32_FLOAT, 1, 12,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INSTANCE_END_POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 16,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INSTANCE_END_SIZE", 0, DXGI_FORMAT_R32_FLOAT, 1, 28,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INSTANCE_COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INSTANCE_END_COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INSTANCE_ANGLE", 0, DXGI_FORMAT_R32_FLOAT, 1, 64,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INSTANCE_FLAGS", 0, DXGI_FORMAT_R32_UINT, 1, 68,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = m_rootSignature.Get();
    description.VS = {
        m_shaderBytecode[0].data(), m_shaderBytecode[0].size()};
    description.InputLayout = {
        inputLayout, static_cast<UINT>(std::size(inputLayout))};
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.BlendState.AlphaToCoverageEnable = FALSE;
    description.BlendState.IndependentBlendEnable = FALSE;
    description.SampleMask = UINT_MAX;
    description.PrimitiveTopologyType =
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = d3d12::D3D12Device::SWAP_FORMAT;
    description.DSVFormat = d3d12::D3D12Device::DEPTH_FORMAT;
    description.SampleDesc.Count = m_sampleCount;
    description.DepthStencilState.DepthEnable = TRUE;
    description.DepthStencilState.DepthWriteMask =
        D3D12_DEPTH_WRITE_MASK_ZERO;
    description.DepthStencilState.DepthFunc =
        D3D12_COMPARISON_FUNC_LESS_EQUAL;
    description.DepthStencilState.StencilEnable = FALSE;

    for (size_t index = 0; index < m_pipelineStates.size(); ++index) {
        const fx::ParticleShader shader =
            static_cast<fx::ParticleShader>(index);
        description.PS = shader == fx::ParticleShader::AlphaTest
            ? D3D12_SHADER_BYTECODE{
                  m_shaderBytecode[2].data(), m_shaderBytecode[2].size()}
            : D3D12_SHADER_BYTECODE{
                  m_shaderBytecode[1].data(), m_shaderBytecode[1].size()};
        D3D12_RENDER_TARGET_BLEND_DESC blend{};
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        switch (shader) {
        case fx::ParticleShader::Additive:
            blend.BlendEnable = TRUE;
            blend.SrcBlend = D3D12_BLEND_ONE;
            blend.DestBlend = D3D12_BLEND_ONE;
            blend.BlendOp = D3D12_BLEND_OP_ADD;
            blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlendAlpha = D3D12_BLEND_ONE;
            blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            break;
        case fx::ParticleShader::Multiply:
            blend.BlendEnable = TRUE;
            blend.SrcBlend = D3D12_BLEND_ZERO;
            blend.DestBlend = D3D12_BLEND_SRC_COLOR;
            blend.BlendOp = D3D12_BLEND_OP_ADD;
            blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
            blend.DestBlendAlpha = D3D12_BLEND_ONE;
            blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            break;
        case fx::ParticleShader::AlphaTest:
            blend.BlendEnable = FALSE;
            blend.SrcBlend = D3D12_BLEND_ONE;
            blend.DestBlend = D3D12_BLEND_ZERO;
            blend.BlendOp = D3D12_BLEND_OP_ADD;
            blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlendAlpha = D3D12_BLEND_ZERO;
            blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            break;
        case fx::ParticleShader::None:
        case fx::ParticleShader::Alpha:
        case fx::ParticleShader::Count:
            blend.BlendEnable = TRUE;
            blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            blend.BlendOp = D3D12_BLEND_OP_ADD;
            blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            break;
        }
        description.BlendState.RenderTarget[0] = blend;
        description.DepthStencilState.DepthWriteMask =
            shader == fx::ParticleShader::AlphaTest
            ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        const HRESULT result =
            m_device->getDevice()->CreateGraphicsPipelineState(
                &description, IID_PPV_ARGS(&m_pipelineStates[index]));
        if (FAILED(result)) {
            TD_LOG_ERROR(
                "[ParticleRenderer] shader mode {} PSO failed: 0x{:08X}",
                index, static_cast<uint32_t>(result));
            return false;
        }
    }
    return true;
}

bool ParticleRenderer::createGpuBillboardPipeline() {
    if (!m_device || !m_device->getDevice() ||
        !m_gpuBillboardShaderPackageReady ||
        m_gpuBillboardShaderBytecode[0].empty() ||
        m_gpuBillboardShaderBytecode[1].empty() ||
        m_gpuBillboardShaderBytecode[2].empty()) {
        return false;
    }

    D3D12_DESCRIPTOR_RANGE stateRange{};
    stateRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    stateRange.NumDescriptors = 1;
    stateRange.BaseShaderRegister = 0;
    stateRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_DESCRIPTOR_RANGE groupedIndicesRange = stateRange;
    groupedIndicesRange.BaseShaderRegister = 1;
    D3D12_DESCRIPTOR_RANGE textureRange = stateRange;
    textureRange.BaseShaderRegister = 2;

    D3D12_ROOT_PARAMETER parameters[4]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &stateRange;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[2].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &groupedIndicesRange;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[3].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[3].DescriptorTable.NumDescriptorRanges = 1;
    parameters[3].DescriptorTable.pDescriptorRanges = &textureRange;
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    const d3d12::TextureSamplingQuality sampling =
        d3d12::textureSamplingQuality(
            m_textureFilter, m_anisotropyLevel);
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = sampling.filter;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxAnisotropy = sampling.maximumAnisotropy;
    sampler.MaxLOD = sampling.maximumLod;

    D3D12_ROOT_SIGNATURE_DESC rootDescription{};
    rootDescription.NumParameters = static_cast<UINT>(std::size(parameters));
    rootDescription.pParameters = parameters;
    rootDescription.NumStaticSamplers = 1;
    rootDescription.pStaticSamplers = &sampler;
    rootDescription.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT result = D3D12SerializeRootSignature(
        &rootDescription, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &errors);
    if (FAILED(result) || !serialized) {
        TD_LOG_ERROR(
            "[ParticleRenderer] GPU billboard root signature serialization failed: 0x{:08X}",
            static_cast<uint32_t>(result));
        return false;
    }
    result = m_device->getDevice()->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_gpuBillboardRootSignature));
    if (FAILED(result)) {
        TD_LOG_ERROR(
            "[ParticleRenderer] GPU billboard root signature creation failed: 0x{:08X}",
            static_cast<uint32_t>(result));
        return false;
    }
    m_gpuBillboardRootSignature->SetName(
        L"ParticleRenderer.GpuBillboardRootSignature");

    const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = m_gpuBillboardRootSignature.Get();
    description.VS = {
        m_gpuBillboardShaderBytecode[0].data(),
        m_gpuBillboardShaderBytecode[0].size()};
    description.InputLayout = {
        inputLayout, static_cast<UINT>(std::size(inputLayout))};
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.BlendState.AlphaToCoverageEnable = FALSE;
    description.BlendState.IndependentBlendEnable = FALSE;
    description.SampleMask = UINT_MAX;
    description.PrimitiveTopologyType =
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = d3d12::D3D12Device::SWAP_FORMAT;
    description.DSVFormat = d3d12::D3D12Device::DEPTH_FORMAT;
    description.SampleDesc.Count = m_sampleCount;
    description.DepthStencilState.DepthEnable = TRUE;
    description.DepthStencilState.DepthFunc =
        D3D12_COMPARISON_FUNC_LESS_EQUAL;
    description.DepthStencilState.StencilEnable = FALSE;

    const fx::ParticleShader supportedShaders[] = {
        fx::ParticleShader::Additive,
        fx::ParticleShader::AlphaTest,
    };
    for (const fx::ParticleShader shader : supportedShaders) {
        const size_t shaderIndex = static_cast<size_t>(shader);
        description.PS = shader == fx::ParticleShader::AlphaTest
            ? D3D12_SHADER_BYTECODE{
                  m_gpuBillboardShaderBytecode[2].data(),
                  m_gpuBillboardShaderBytecode[2].size()}
            : D3D12_SHADER_BYTECODE{
                  m_gpuBillboardShaderBytecode[1].data(),
                  m_gpuBillboardShaderBytecode[1].size()};
        D3D12_RENDER_TARGET_BLEND_DESC blend{};
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (shader == fx::ParticleShader::Additive) {
            blend.BlendEnable = TRUE;
            blend.SrcBlend = D3D12_BLEND_ONE;
            blend.DestBlend = D3D12_BLEND_ONE;
            blend.BlendOp = D3D12_BLEND_OP_ADD;
            blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlendAlpha = D3D12_BLEND_ONE;
            blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            description.DepthStencilState.DepthWriteMask =
                D3D12_DEPTH_WRITE_MASK_ZERO;
        } else {
            blend.BlendEnable = FALSE;
            blend.SrcBlend = D3D12_BLEND_ONE;
            blend.DestBlend = D3D12_BLEND_ZERO;
            blend.BlendOp = D3D12_BLEND_OP_ADD;
            blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlendAlpha = D3D12_BLEND_ZERO;
            blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            description.DepthStencilState.DepthWriteMask =
                D3D12_DEPTH_WRITE_MASK_ALL;
        }
        description.BlendState.RenderTarget[0] = blend;
        result = m_device->getDevice()->CreateGraphicsPipelineState(
            &description,
            IID_PPV_ARGS(&m_gpuBillboardPipelineStates[shaderIndex]));
        if (FAILED(result)) return false;

        // Same VS/PS/root/texture contract, but no presentation side effects.
        // This lets Debug/GPU validation exercise ExecuteIndirect while CPU
        // remains the visible authority during the parity gate.
        description.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
        description.DepthStencilState.DepthEnable = FALSE;
        description.DepthStencilState.DepthWriteMask =
            D3D12_DEPTH_WRITE_MASK_ZERO;
        result = m_device->getDevice()->CreateGraphicsPipelineState(
            &description,
            IID_PPV_ARGS(
                &m_gpuBillboardShadowPipelineStates[shaderIndex]));
        if (FAILED(result)) return false;
        description.DepthStencilState.DepthEnable = TRUE;
    }

    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC commandSignature{};
    commandSignature.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    commandSignature.NumArgumentDescs = 1;
    commandSignature.pArgumentDescs = &argument;
    result = m_device->getDevice()->CreateCommandSignature(
        &commandSignature, nullptr,
        IID_PPV_ARGS(&m_gpuBillboardCommandSignature));
    if (FAILED(result)) {
        TD_LOG_ERROR(
            "[ParticleRenderer] GPU billboard command signature creation failed: 0x{:08X}",
            static_cast<uint32_t>(result));
        return false;
    }
    m_gpuBillboardCommandSignature->SetName(
        L"ParticleRenderer.GpuBillboardDrawIndexedSignature");
    return true;
}

bool ParticleRenderer::createSmudgePipeline() {
    if (!m_device || !m_device->getDevice()) return false;

    D3D12_DESCRIPTOR_RANGE textureRange{};
    textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRange.NumDescriptors = 1;
    textureRange.BaseShaderRegister = 0;
    textureRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &textureRange;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxLOD = 0.0f;
    D3D12_ROOT_SIGNATURE_DESC rootDescription{};
    rootDescription.NumParameters = 2;
    rootDescription.pParameters = parameters;
    rootDescription.NumStaticSamplers = 1;
    rootDescription.pStaticSamplers = &sampler;
    rootDescription.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(
            &rootDescription, D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized, &errors)) || !serialized) {
        TD_LOG_ERROR("[ParticleRenderer] smudge root signature serialization failed");
        return false;
    }
    if (FAILED(m_device->getDevice()->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&m_smudgeRootSignature)))) {
        TD_LOG_ERROR("[ParticleRenderer] smudge root signature creation failed");
        return false;
    }

    if (m_shaderBytecode[3].empty() || m_shaderBytecode[4].empty()) {
        return false;
    }
    const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"ALPHA", 0, DXGI_FORMAT_R32_FLOAT, 0, 20,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = m_smudgeRootSignature.Get();
    description.VS = {
        m_shaderBytecode[3].data(), m_shaderBytecode[3].size()};
    description.PS = {
        m_shaderBytecode[4].data(), m_shaderBytecode[4].size()};
    description.InputLayout = {inputLayout,
                               static_cast<UINT>(std::size(inputLayout))};
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.BlendState.AlphaToCoverageEnable = FALSE;
    description.BlendState.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC blend{};
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.BlendState.RenderTarget[0] = blend;
    description.SampleMask = UINT_MAX;
    description.PrimitiveTopologyType =
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = d3d12::D3D12Device::SWAP_FORMAT;
    description.DSVFormat = d3d12::D3D12Device::DEPTH_FORMAT;
    description.SampleDesc.Count = 1;
    description.DepthStencilState.DepthEnable = FALSE;
    description.DepthStencilState.DepthWriteMask =
        D3D12_DEPTH_WRITE_MASK_ZERO;
    description.DepthStencilState.DepthFunc =
        D3D12_COMPARISON_FUNC_ALWAYS;
    const HRESULT result =
        m_device->getDevice()->CreateGraphicsPipelineState(
            &description, IID_PPV_ARGS(&m_smudgePipelineState));
    if (FAILED(result)) {
        TD_LOG_ERROR("[ParticleRenderer] smudge PSO failed: 0x{:08X}",
                     static_cast<uint32_t>(result));
        return false;
    }
    return true;
}


} // namespace engine::render
