#include "D3D12UiPipeline.h"

#include "D3D12ShaderPackage.h"
#include "debug/debug.h"

#include <d3dcompiler.h>
#include <iterator>

namespace engine::d3d12 {

#ifndef TD_UI_2D_SHADER_PACKAGE_VERSION
#define TD_UI_2D_SHADER_PACKAGE_VERSION 1
#endif
#ifndef TD_UI_2D_SHADER_SOURCE_SHA256
#define TD_UI_2D_SHADER_SOURCE_SHA256 \
    "0000000000000000000000000000000000000000000000000000000000000000"
#endif

bool D3D12UiPipeline::initialize(
    ID3D12Device* device,
    DXGI_FORMAT renderTargetFormat,
    DXGI_FORMAT depthFormat) {
    shutdown();
    if (!device ||
        !createRootSignature(device, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            m_linearRoot) ||
        !createRootSignature(device, D3D12_FILTER_MIN_MAG_MIP_POINT,
            m_pointRoot)) {
        shutdown();
        return false;
    }

    constexpr ShaderPackageEntrySpec entries[] = {
        {"vertex_file", "ui_2d_vs.cso", "vertex_profile", "vs_5_0"},
        {"solid_pixel_file", "ui_2d_solid_ps.cso",
         "solid_pixel_profile", "ps_5_0"},
        {"textured_pixel_file", "ui_2d_textured_ps.cso",
         "textured_pixel_profile", "ps_5_0"},
    };
    container::Vector<container::Vector<uint8_t>> shaders;
    if (!loadShaderPackage(
            "ui_2d", std::to_string(TD_UI_2D_SHADER_PACKAGE_VERSION),
            TD_UI_2D_SHADER_SOURCE_SHA256, entries, shaders) ||
        shaders.size() != std::size(entries)) {
        TD_LOG_ERROR("[D3D12UiPipeline] Precompiled shader package unavailable");
        shutdown();
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = m_linearRoot.Get();
    description.InputLayout = {layout, _countof(layout)};
    description.VS = {shaders[0].data(), shaders[0].size()};
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = FALSE;
    auto& blend = description.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = UINT_MAX;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1u;
    description.RTVFormats[0] = renderTargetFormat;
    description.DSVFormat = depthFormat;
    description.DepthStencilState.DepthEnable = FALSE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    description.DepthStencilState.StencilEnable = FALSE;
    description.SampleDesc.Count = 1u;

    description.PS = {shaders[1].data(), shaders[1].size()};
    HRESULT result = device->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(&m_solid));
    if (FAILED(result)) {
        TD_LOG_ERROR("[D3D12UiPipeline] Solid PSO creation failed: 0x{:08X}", result);
        shutdown();
        return false;
    }
    description.PS = {shaders[2].data(), shaders[2].size()};
    result = device->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(&m_texturedLinear));
    if (FAILED(result)) {
        TD_LOG_ERROR("[D3D12UiPipeline] Textured PSO creation failed: 0x{:08X}", result);
        shutdown();
        return false;
    }
    description.pRootSignature = m_pointRoot.Get();
    result = device->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(&m_texturedPoint));
    if (FAILED(result)) {
        TD_LOG_ERROR("[D3D12UiPipeline] Point PSO creation failed: 0x{:08X}", result);
        shutdown();
        return false;
    }
    return true;
}

bool D3D12UiPipeline::createRootSignature(
    ID3D12Device* device,
    D3D12_FILTER filter,
    Microsoft::WRL::ComPtr<ID3D12RootSignature>& output) {
    D3D12_DESCRIPTOR_RANGE1 range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1u;
    range.BaseShaderRegister = 0u;
    range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    D3D12_ROOT_PARAMETER1 parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = 0u;
    parameters[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1u;
    parameters[1].DescriptorTable.pDescriptorRanges = &range;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = filter;
    sampler.AddressU = sampler.AddressV = sampler.AddressW =
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC description{};
    description.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    description.Desc_1_1.NumParameters = 2u;
    description.Desc_1_1.pParameters = parameters;
    description.Desc_1_1.NumStaticSamplers = 1u;
    description.Desc_1_1.pStaticSamplers = &sampler;
    description.Desc_1_1.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    HRESULT result = D3D12SerializeVersionedRootSignature(
        &description, &serialized, &error);
    if (FAILED(result)) {
        if (error) TD_LOG_ERROR("[D3D12UiPipeline] Root signature: {}",
            static_cast<const char*>(error->GetBufferPointer()));
        return false;
    }
    result = device->CreateRootSignature(
        0u, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&output));
    return SUCCEEDED(result) && output;
}

void D3D12UiPipeline::shutdown() noexcept {
    m_texturedPoint.Reset();
    m_texturedLinear.Reset();
    m_solid.Reset();
    m_pointRoot.Reset();
    m_linearRoot.Reset();
}

ID3D12PipelineState* D3D12UiPipeline::pipeline(
    UiPipelineKind kind) const noexcept {
    switch (kind) {
    case UiPipelineKind::Solid: return m_solid.Get();
    case UiPipelineKind::TexturedLinear: return m_texturedLinear.Get();
    case UiPipelineKind::TexturedPoint: return m_texturedPoint.Get();
    }
    return nullptr;
}

} // namespace engine::d3d12
