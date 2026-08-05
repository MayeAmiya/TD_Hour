#include "engine/renderer/world/effects/WorldPostProcessRenderer.h"

#include "engine/renderer/world/effects/MotionBlurPresentation.h"
#include "engine/renderer/d3d12/runtime/D3D12Device.h"
#include "engine/renderer/d3d12/runtime/D3D12ShaderPackage.h"
#include "debug/debug.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

#ifndef TD_FXAA_SHADER_PACKAGE_VERSION
#define TD_FXAA_SHADER_PACKAGE_VERSION 1
#endif
#ifndef TD_FXAA_SHADER_SOURCE_SHA256
#define TD_FXAA_SHADER_SOURCE_SHA256 \
    "74988ef30c0b51953f0fdeece351cfa80ba7a8f7148d1982eb0c4334b02b5b4b"
#endif
#ifndef TD_SCREEN_FADE_SHADER_PACKAGE_VERSION
#define TD_SCREEN_FADE_SHADER_PACKAGE_VERSION 1
#endif
#ifndef TD_SCREEN_FADE_SHADER_SOURCE_SHA256
#define TD_SCREEN_FADE_SHADER_SOURCE_SHA256 \
    "0000000000000000000000000000000000000000000000000000000000000000"
#endif
#ifndef TD_BLACK_AND_WHITE_SHADER_PACKAGE_VERSION
#define TD_BLACK_AND_WHITE_SHADER_PACKAGE_VERSION 1
#endif
#ifndef TD_BLACK_AND_WHITE_SHADER_SOURCE_SHA256
#define TD_BLACK_AND_WHITE_SHADER_SOURCE_SHA256 \
    "0000000000000000000000000000000000000000000000000000000000000000"
#endif
#ifndef TD_MOTION_BLUR_SHADER_PACKAGE_VERSION
#define TD_MOTION_BLUR_SHADER_PACKAGE_VERSION 1
#endif
#ifndef TD_MOTION_BLUR_SHADER_SOURCE_SHA256
#define TD_MOTION_BLUR_SHADER_SOURCE_SHA256 \
    "0000000000000000000000000000000000000000000000000000000000000000"
#endif

namespace engine::render {
namespace {

constexpr int32_t kMotionBlurMaximumCount = kLegacyMotionBlurMaximumCount;
constexpr int32_t kMotionBlurCountStep = kLegacyMotionBlurCountStep;

D3D12_HEAP_PROPERTIES makeDefaultHeapProperties() noexcept {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    return properties;
}

D3D12_RESOURCE_DESC makeColorTextureDescription(
    uint32_t width, uint32_t height) noexcept {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = d3d12::D3D12Device::SWAP_FORMAT;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_NONE;
    return description;
}

D3D12_RESOURCE_BARRIER makeTransition(
    ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

bool loadShaderPair(
    container::StringView packageName, uint32_t packageVersion,
    container::StringView sourceSha256, container::StringView vertexFile,
    container::StringView pixelFile, container::Vector<uint8_t>& vertex,
    container::Vector<uint8_t>& pixel) {
    const container::Array<d3d12::ShaderPackageEntrySpec, 2> entries{{
        {"vertex_file", vertexFile, "vertex_profile", "vs_5_0"},
        {"pixel_file", pixelFile, "pixel_profile", "ps_5_0"},
    }};
    container::Vector<container::Vector<uint8_t>> loaded;
    if (!d3d12::loadShaderPackage(
            packageName, std::to_string(packageVersion), sourceSha256,
            {entries.data(), entries.size()}, loaded) ||
        loaded.size() != entries.size()) {
        TD_LOG_ERROR(
            "[WorldPostProcessRenderer] precompiled '{}' shader package unavailable",
            packageName);
        return false;
    }
    vertex = std::move(loaded[0]);
    pixel = std::move(loaded[1]);
    return true;
}

bool createSampledFullscreenResources(
    d3d12::D3D12Device& device, container::StringView label,
    uint32_t constantCount, float samplerMaxLod,
    container::Span<const uint8_t> vertexShader,
    container::Span<const uint8_t> pixelShader,
    Microsoft::WRL::ComPtr<ID3D12RootSignature>& rootSignature,
    Microsoft::WRL::ComPtr<ID3D12PipelineState>& pipelineState) {
    if (vertexShader.empty() || pixelShader.empty()) return false;

    D3D12_DESCRIPTOR_RANGE textureRange{};
    textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRange.NumDescriptors = 1;
    textureRange.BaseShaderRegister = 0;
    textureRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.Num32BitValues = constantCount;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &textureRange;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = samplerMaxLod;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDescription{};
    rootDescription.NumParameters = 2;
    rootDescription.pParameters = parameters;
    rootDescription.NumStaticSamplers = 1;
    rootDescription.pStaticSamplers = &sampler;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT serializeResult = D3D12SerializeRootSignature(
        &rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(serializeResult)) {
        if (errors) {
            TD_LOG_ERROR(
                "[WorldPostProcessRenderer] {} root signature serialization failed: {}",
                label, static_cast<const char*>(errors->GetBufferPointer()));
        } else {
            TD_LOG_ERROR(
                "[WorldPostProcessRenderer] {} root signature serialization failed: 0x{:08X}",
                label, static_cast<uint32_t>(serializeResult));
        }
        return false;
    }
    const HRESULT rootResult = device.getDevice()->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&rootSignature));
    if (FAILED(rootResult)) {
        TD_LOG_ERROR(
            "[WorldPostProcessRenderer] {} CreateRootSignature failed: 0x{:08X}",
            label, static_cast<uint32_t>(rootResult));
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = rootSignature.Get();
    description.VS = {vertexShader.data(), vertexShader.size()};
    description.PS = {pixelShader.data(), pixelShader.size()};
    auto& blend = description.BlendState.RenderTarget[0];
    blend.BlendEnable = FALSE;
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ZERO;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthEnable = FALSE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = d3d12::D3D12Device::SWAP_FORMAT;
    description.DSVFormat = d3d12::D3D12Device::DEPTH_FORMAT;
    description.SampleDesc.Count = 1;
    const HRESULT pipelineResult = device.getDevice()->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(&pipelineState));
    if (FAILED(pipelineResult)) {
        TD_LOG_ERROR(
            "[WorldPostProcessRenderer] {} PSO failed: 0x{:08X}",
            label, static_cast<uint32_t>(pipelineResult));
        return false;
    }
    return true;
}

} // namespace

WorldPostProcessRenderer::~WorldPostProcessRenderer() {
    shutdown();
}

bool WorldPostProcessRenderer::init(d3d12::D3D12Device& device) {
    shutdown();
    m_device = &device;
    if (!m_device->getDevice() || !loadShaderPackages() ||
        !createScreenFadeResources() || !createBlackAndWhiteResources() ||
        !createMotionBlurResources()) {
        shutdown();
        return false;
    }

    m_fxaaAvailable = createFxaaResources();
    if (!m_fxaaAvailable) {
        m_fxaaRootSignature.Reset();
        m_fxaaPipelineState.Reset();
        TD_LOG_WARN(
            "[WorldPostProcessRenderer] FXAA unavailable; Display mode will fall back to Off");
    }
    m_initialized = true;
    return true;
}

void WorldPostProcessRenderer::shutdown() {
    releaseViewFilterSceneCopy();
    m_motionBlurPipelineState.Reset();
    m_motionBlurRootSignature.Reset();
    m_fxaaPipelineState.Reset();
    m_fxaaRootSignature.Reset();
    m_blackAndWhitePipelineState.Reset();
    m_blackAndWhiteRootSignature.Reset();
    for (auto& pipeline : m_screenFadePipelineStates) pipeline.Reset();
    m_screenFadeRootSignature.Reset();
    for (auto& bytecode : m_shaderBytecode) bytecode.clear();
    m_screenFadeCursor = {};
    m_viewFilterPresentationEpoch = 0;
    m_viewFilterPresentationSequence = 0;
    m_activeViewFilter = ScriptViewFilter::None;
    m_blackAndWhite = {};
    m_motionBlur = {};
    m_lastViewFilterCamera = {};
    m_hasLastViewFilterCamera = false;
    m_fxaaAvailable = false;
    m_fxaaEnabled = false;
    m_initialized = false;
    m_device = nullptr;
}

void WorldPostProcessRenderer::resetPresentationEpoch(
    uint64_t presentationEpoch) noexcept {
    m_screenFadeCursor = {};
    m_screenFadeCursor.presentationEpoch = presentationEpoch;
    m_viewFilterPresentationEpoch = presentationEpoch;
    m_viewFilterPresentationSequence = 0;
    m_activeViewFilter = ScriptViewFilter::None;
    m_blackAndWhite = {};
    m_motionBlur = {};
    m_lastViewFilterCamera = {};
    m_hasLastViewFilterCamera = false;
}

bool WorldPostProcessRenderer::loadShaderPackages() {
    for (auto& bytecode : m_shaderBytecode) bytecode.clear();
    return loadShaderPair(
               "screen_fade", TD_SCREEN_FADE_SHADER_PACKAGE_VERSION,
               TD_SCREEN_FADE_SHADER_SOURCE_SHA256,
               "screen_fade_vs.cso", "screen_fade_ps.cso",
               m_shaderBytecode[static_cast<size_t>(ShaderBytecode::ScreenFadeVertex)],
               m_shaderBytecode[static_cast<size_t>(ShaderBytecode::ScreenFadePixel)]) &&
        loadShaderPair(
               "black_and_white", TD_BLACK_AND_WHITE_SHADER_PACKAGE_VERSION,
               TD_BLACK_AND_WHITE_SHADER_SOURCE_SHA256,
               "black_and_white_vs.cso", "black_and_white_ps.cso",
               m_shaderBytecode[static_cast<size_t>(ShaderBytecode::BlackAndWhiteVertex)],
               m_shaderBytecode[static_cast<size_t>(ShaderBytecode::BlackAndWhitePixel)]) &&
        loadShaderPair(
               "motion_blur", TD_MOTION_BLUR_SHADER_PACKAGE_VERSION,
               TD_MOTION_BLUR_SHADER_SOURCE_SHA256,
               "motion_blur_vs.cso", "motion_blur_ps.cso",
               m_shaderBytecode[static_cast<size_t>(ShaderBytecode::MotionBlurVertex)],
               m_shaderBytecode[static_cast<size_t>(ShaderBytecode::MotionBlurPixel)]);
}

bool WorldPostProcessRenderer::createScreenFadeResources() {
    const auto& vertexShader = m_shaderBytecode[
        static_cast<size_t>(ShaderBytecode::ScreenFadeVertex)];
    const auto& pixelShader = m_shaderBytecode[
        static_cast<size_t>(ShaderBytecode::ScreenFadePixel)];
    if (vertexShader.empty() || pixelShader.empty()) return false;

    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants.ShaderRegister = 0;
    parameter.Constants.Num32BitValues = 1;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rootDescription{};
    rootDescription.NumParameters = 1;
    rootDescription.pParameters = &parameter;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT serializeResult = D3D12SerializeRootSignature(
        &rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(serializeResult)) {
        TD_LOG_ERROR(
            "[WorldPostProcessRenderer] screen fade root signature serialization failed: 0x{:08X}",
            static_cast<uint32_t>(serializeResult));
        return false;
    }
    const HRESULT rootResult = m_device->getDevice()->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_screenFadeRootSignature));
    if (FAILED(rootResult)) {
        TD_LOG_ERROR(
            "[WorldPostProcessRenderer] screen fade CreateRootSignature failed: 0x{:08X}",
            static_cast<uint32_t>(rootResult));
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = m_screenFadeRootSignature.Get();
    description.VS = {vertexShader.data(), vertexShader.size()};
    description.PS = {pixelShader.data(), pixelShader.size()};
    auto& blend = description.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthEnable = FALSE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = d3d12::D3D12Device::SWAP_FORMAT;
    description.DSVFormat = d3d12::D3D12Device::DEPTH_FORMAT;
    description.SampleDesc.Count = 1;

    const auto createPipeline = [this, &description, &blend](
        ScreenFadeBlendMode mode, D3D12_BLEND source,
        D3D12_BLEND destination, D3D12_BLEND_OP operation) {
        blend.SrcBlend = source;
        blend.DestBlend = destination;
        blend.BlendOp = operation;
        const HRESULT result = m_device->getDevice()->CreateGraphicsPipelineState(
            &description,
            IID_PPV_ARGS(&m_screenFadePipelineStates[static_cast<size_t>(mode)]));
        if (FAILED(result)) {
            TD_LOG_ERROR(
                "[WorldPostProcessRenderer] screen fade mode {} PSO failed: 0x{:08X}",
                static_cast<uint32_t>(mode), static_cast<uint32_t>(result));
            return false;
        }
        return true;
    };

    return createPipeline(ScreenFadeBlendMode::Add,
               D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD) &&
        createPipeline(ScreenFadeBlendMode::Subtract,
               D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_REV_SUBTRACT) &&
        createPipeline(ScreenFadeBlendMode::Saturate,
               D3D12_BLEND_DEST_COLOR, D3D12_BLEND_SRC_COLOR,
               D3D12_BLEND_OP_ADD) &&
        createPipeline(ScreenFadeBlendMode::Multiply,
               D3D12_BLEND_ZERO, D3D12_BLEND_SRC_COLOR, D3D12_BLEND_OP_ADD);
}

bool WorldPostProcessRenderer::createBlackAndWhiteResources() {
    return createSampledFullscreenResources(
        *m_device, "black-and-white", 1, D3D12_FLOAT32_MAX,
        m_shaderBytecode[static_cast<size_t>(ShaderBytecode::BlackAndWhiteVertex)],
        m_shaderBytecode[static_cast<size_t>(ShaderBytecode::BlackAndWhitePixel)],
        m_blackAndWhiteRootSignature, m_blackAndWhitePipelineState);
}

bool WorldPostProcessRenderer::createMotionBlurResources() {
    return createSampledFullscreenResources(
        *m_device, "motion-blur", 12, D3D12_FLOAT32_MAX,
        m_shaderBytecode[static_cast<size_t>(ShaderBytecode::MotionBlurVertex)],
        m_shaderBytecode[static_cast<size_t>(ShaderBytecode::MotionBlurPixel)],
        m_motionBlurRootSignature, m_motionBlurPipelineState);
}

bool WorldPostProcessRenderer::createFxaaResources() {
    container::Vector<uint8_t> vertexShader;
    container::Vector<uint8_t> pixelShader;
    if (!loadShaderPair(
            "fxaa", TD_FXAA_SHADER_PACKAGE_VERSION,
            TD_FXAA_SHADER_SOURCE_SHA256, "fxaa_vs.cso", "fxaa_ps.cso",
            vertexShader, pixelShader)) {
        return false;
    }
    return createSampledFullscreenResources(
        *m_device, "FXAA", 8, 0.0f, vertexShader, pixelShader,
        m_fxaaRootSignature, m_fxaaPipelineState);
}

bool WorldPostProcessRenderer::ensureViewFilterSceneCopy() {
    if (!m_device || !m_device->getDevice() || m_device->width() == 0 ||
        m_device->height() == 0) {
        return false;
    }
    if (m_viewFilterSceneCopy &&
        m_viewFilterSceneCopyWidth == m_device->width() &&
        m_viewFilterSceneCopyHeight == m_device->height() &&
        m_viewFilterSceneCopySrv != UINT32_MAX) {
        return true;
    }

    releaseViewFilterSceneCopy();
    const uint32_t descriptor = m_device->allocateSrvDescriptor();
    if (descriptor == UINT32_MAX) return false;

    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    const D3D12_HEAP_PROPERTIES heap = makeDefaultHeapProperties();
    const D3D12_RESOURCE_DESC description =
        makeColorTextureDescription(m_device->width(), m_device->height());
    ++m_sceneColorStats.allocationAttempts;
    const D3D12_RESOURCE_ALLOCATION_INFO allocation =
        m_device->getDevice()->GetResourceAllocationInfo(0u, 1u, &description);
    if (allocation.SizeInBytes == 0 || allocation.SizeInBytes == UINT64_MAX) {
        ++m_sceneColorStats.allocationFailures;
        m_device->freeSrvDescriptor(descriptor);
        return false;
    }
    const HRESULT result = m_device->getDevice()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
        IID_PPV_ARGS(&texture));
    if (FAILED(result)) {
        ++m_sceneColorStats.allocationFailures;
        TD_LOG_ERROR(
            "[WorldPostProcessRenderer] view-filter scene copy allocation failed: 0x{:08X}",
            static_cast<uint32_t>(result));
        m_device->freeSrvDescriptor(descriptor);
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = d3d12::D3D12Device::SWAP_FORMAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    m_device->getDevice()->CreateShaderResourceView(
        texture.Get(), &srv, m_device->getSrvCpuHandle(descriptor));
    m_viewFilterSceneCopy = std::move(texture);
    m_viewFilterSceneCopySrv = descriptor;
    m_viewFilterSceneCopyWidth = m_device->width();
    m_viewFilterSceneCopyHeight = m_device->height();
    m_viewFilterSceneCopyAllocationBytes = allocation.SizeInBytes;
    m_sceneColorStats.residentAllocationBytes += allocation.SizeInBytes;
    m_sceneColorStats.lifetimeResidentHighWaterBytes = std::max(
        m_sceneColorStats.lifetimeResidentHighWaterBytes,
        m_sceneColorStats.residentAllocationBytes);
    return true;
}

void WorldPostProcessRenderer::releaseViewFilterSceneCopy() noexcept {
    const uint64_t allocationBytes = m_viewFilterSceneCopyAllocationBytes;
    if (m_viewFilterSceneCopy) {
        ++m_sceneColorStats.releaseCalls;
        m_sceneColorStats.releasedAllocationBytes += allocationBytes;
        if (m_device) {
            ++m_sceneColorStats.retirementRequests;
            m_sceneColorStats.retirementRequestedBytes += allocationBytes;
            m_device->retireResource(std::move(m_viewFilterSceneCopy));
        } else {
            m_viewFilterSceneCopy.Reset();
        }
    }
    m_sceneColorStats.residentAllocationBytes =
        m_sceneColorStats.residentAllocationBytes >= allocationBytes
        ? m_sceneColorStats.residentAllocationBytes - allocationBytes
        : 0;
    m_viewFilterSceneCopyAllocationBytes = 0;
    if (m_viewFilterSceneCopySrv != UINT32_MAX && m_device) {
        m_device->freeSrvDescriptor(m_viewFilterSceneCopySrv);
    }
    m_viewFilterSceneCopySrv = UINT32_MAX;
    m_viewFilterSceneCopyWidth = 0;
    m_viewFilterSceneCopyHeight = 0;
}

bool WorldPostProcessRenderer::captureViewFilterScene() {
    if (!m_initialized || !m_device || !ensureViewFilterSceneCopy()) return false;
    ID3D12GraphicsCommandList* commandList = m_device->commandList();
    ID3D12Resource* target = m_device->currentRenderTarget();
    if (!commandList || !target || !m_viewFilterSceneCopy) return false;
    const bool timestampActive = m_device->beginGpuTimestamp(
        GpuTimestampRange::SceneColorCopy);

    m_device->flushBatch();
    D3D12_RESOURCE_BARRIER beginCopy[2] = {
        makeTransition(target, D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_COPY_SOURCE),
        makeTransition(m_viewFilterSceneCopy.Get(),
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COPY_DEST),
    };
    commandList->ResourceBarrier(2, beginCopy);
    commandList->CopyResource(m_viewFilterSceneCopy.Get(), target);
    ++m_sceneColorStats.copyCalls;
    m_sceneColorStats.copiedBytes += m_viewFilterSceneCopyAllocationBytes;
    D3D12_RESOURCE_BARRIER finishCopy[2] = {
        makeTransition(target, D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_RENDER_TARGET),
        makeTransition(m_viewFilterSceneCopy.Get(),
                       D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
    };
    commandList->ResourceBarrier(2, finishCopy);
    if (timestampActive) {
        static_cast<void>(m_device->endGpuTimestamp(
            GpuTimestampRange::SceneColorCopy));
    }
    return true;
}

RenderCameraSnapshot WorldPostProcessRenderer::prepareScriptViewFilters(
    const RenderCameraSnapshot& cameraSnapshot, uint64_t simulationFrame,
    const BlackAndWhiteRenderState& blackAndWhite,
    const MotionBlurRenderState& motionBlur) noexcept {
    const uint64_t incomingEpoch =
        blackAndWhite.presentationEpoch > motionBlur.presentationEpoch
        ? blackAndWhite.presentationEpoch
        : motionBlur.presentationEpoch;
    if (incomingEpoch == 0) {
        m_lastViewFilterCamera = cameraSnapshot;
        m_hasLastViewFilterCamera = true;
        return cameraSnapshot;
    }
    if (m_viewFilterPresentationEpoch == 0 ||
        incomingEpoch > m_viewFilterPresentationEpoch) {
        m_viewFilterPresentationEpoch = incomingEpoch;
        m_viewFilterPresentationSequence = 0;
        m_activeViewFilter = ScriptViewFilter::None;
        m_blackAndWhite = {};
        m_motionBlur = {};
        m_hasLastViewFilterCamera = false;
    } else if (incomingEpoch < m_viewFilterPresentationEpoch) {
        return cameraSnapshot;
    }

    const uint64_t journalTrimmedThroughSequence = std::max(
        blackAndWhite.commandsTrimmedThroughSequence,
        motionBlur.commandsTrimmedThroughSequence);
    if (journalTrimmedThroughSequence > m_viewFilterPresentationSequence) {
        // The renderer missed an ordered command prefix.  The accompanying
        // latest values are an explicit resynchronization point, so discard
        // stale local transition state before applying the retained suffix.
        m_viewFilterPresentationSequence = journalTrimmedThroughSequence;
        m_activeViewFilter = ScriptViewFilter::None;
        m_blackAndWhite = {};
        m_motionBlur = {};
        m_hasLastViewFilterCamera = false;
    }

    const auto applyBlackAndWhite = [this](
        uint64_t sequence, bool enabled, int32_t transitionFrames) noexcept {
        if (sequence == 0 || sequence <= m_viewFilterPresentationSequence) return;
        m_viewFilterPresentationSequence = sequence;
        if (enabled) {
            m_activeViewFilter = ScriptViewFilter::BlackAndWhite;
            m_motionBlur.active = false;
            m_motionBlur.jumpAfterCurrentPass = false;
            m_blackAndWhite = {
                .transitionFrames = transitionFrames,
                .currentFrame = 0,
                .direction = 1,
                .mix = 0.0f,
                .active = true,
            };
        } else if (m_activeViewFilter == ScriptViewFilter::BlackAndWhite &&
                   m_blackAndWhite.active) {
            m_blackAndWhite.transitionFrames = transitionFrames;
            m_blackAndWhite.currentFrame = 0;
            m_blackAndWhite.direction = -1;
        }
    };

    const auto beginMotionBlur = [this](
        const MotionBlurRenderCommand& command) noexcept {
        if (command.presentationSequence == 0 ||
            command.presentationSequence <= m_viewFilterPresentationSequence ||
            command.mode == MotionBlurRenderMode::Count) {
            return;
        }
        m_viewFilterPresentationSequence = command.presentationSequence;
        MotionBlurConsumer& filter = m_motionBlur;
        if (command.mode == MotionBlurRenderMode::EndFollow) {
            const bool retainPan =
                m_activeViewFilter == ScriptViewFilter::MotionBlur &&
                (filter.mode == MotionBlurRenderMode::Follow ||
                 filter.mode == MotionBlurRenderMode::EndFollow) &&
                filter.active;
            if (!retainPan) {
                const bool hasTranslation = filter.hasPresentationTranslation;
                const math::vec3 translation = filter.presentationTranslation;
                filter = {};
                filter.hasPresentationTranslation = hasTranslation;
                filter.presentationTranslation = translation;
            }
            m_activeViewFilter = ScriptViewFilter::MotionBlur;
            m_blackAndWhite.active = false;
            filter.active = true;
            filter.mode = MotionBlurRenderMode::EndFollow;
            filter.decrement = false;
            filter.endAfterCurrentPass = false;
            return;
        }
        if (command.mode == MotionBlurRenderMode::ZoomJump &&
            !command.hasJumpTarget) {
            return;
        }

        const bool hasTranslation = filter.hasPresentationTranslation;
        const math::vec3 translation = filter.presentationTranslation;
        filter = {};
        filter.hasPresentationTranslation = hasTranslation;
        filter.presentationTranslation = translation;
        filter.mode = command.mode;
        filter.active = true;
        filter.saturate = command.saturate;
        filter.followAmount = command.followAmount;
        filter.hasJumpTarget = command.hasJumpTarget;
        filter.jumpTarget = command.jumpTarget;
        if (command.mode == MotionBlurRenderMode::ZoomOut) {
            filter.maxCount = kMotionBlurMaximumCount;
            filter.decrement = true;
        }
        m_activeViewFilter = ScriptViewFilter::MotionBlur;
        m_blackAndWhite.active = false;
    };

    size_t blackIndex = 0;
    size_t motionIndex = 0;
    const auto skipBlack = [&]() noexcept {
        while (blackIndex < blackAndWhite.commands.size()) {
            const BlackAndWhiteRenderCommand& command =
                blackAndWhite.commands[blackIndex];
            if (command.presentationEpoch == m_viewFilterPresentationEpoch &&
                command.presentationSequence > m_viewFilterPresentationSequence) {
                break;
            }
            ++blackIndex;
        }
    };
    const auto skipMotion = [&]() noexcept {
        while (motionIndex < motionBlur.commands.size()) {
            const MotionBlurRenderCommand& command = motionBlur.commands[motionIndex];
            if (command.presentationEpoch == m_viewFilterPresentationEpoch &&
                command.presentationSequence > m_viewFilterPresentationSequence) {
                break;
            }
            ++motionIndex;
        }
    };
    for (;;) {
        skipBlack();
        skipMotion();
        const bool haveBlack = blackIndex < blackAndWhite.commands.size();
        const bool haveMotion = motionIndex < motionBlur.commands.size();
        if (!haveBlack && !haveMotion) break;
        if (haveBlack &&
            (!haveMotion ||
             blackAndWhite.commands[blackIndex].presentationSequence <
                 motionBlur.commands[motionIndex].presentationSequence)) {
            const BlackAndWhiteRenderCommand& command =
                blackAndWhite.commands[blackIndex++];
            applyBlackAndWhite(
                command.presentationSequence, command.enabled,
                command.transitionFrames);
        } else {
            beginMotionBlur(motionBlur.commands[motionIndex++]);
        }
    }

    const bool blackFallback =
        blackAndWhite.presentationEpoch == m_viewFilterPresentationEpoch &&
        blackAndWhite.presentationSequence > m_viewFilterPresentationSequence;
    const bool motionFallback =
        motionBlur.presentationEpoch == m_viewFilterPresentationEpoch &&
        motionBlur.presentationSequence > m_viewFilterPresentationSequence;
    if (blackFallback &&
        (!motionFallback ||
         blackAndWhite.presentationSequence < motionBlur.presentationSequence)) {
        applyBlackAndWhite(
            blackAndWhite.presentationSequence, blackAndWhite.enabled,
            blackAndWhite.transitionFrames);
    }
    if (motionFallback &&
        motionBlur.presentationSequence > m_viewFilterPresentationSequence) {
        beginMotionBlur({
            .presentationEpoch = motionBlur.presentationEpoch,
            .presentationSequence = motionBlur.presentationSequence,
            .mode = motionBlur.mode,
            .saturate = motionBlur.saturate,
            .hasJumpTarget = motionBlur.hasJumpTarget,
            .jumpTarget = motionBlur.jumpTarget,
            .followAmount = motionBlur.followAmount,
        });
    }
    if (blackFallback &&
        blackAndWhite.presentationSequence > m_viewFilterPresentationSequence) {
        applyBlackAndWhite(
            blackAndWhite.presentationSequence, blackAndWhite.enabled,
            blackAndWhite.transitionFrames);
    }

    if (m_activeViewFilter == ScriptViewFilter::BlackAndWhite &&
        m_blackAndWhite.active && m_blackAndWhite.direction != 0) {
        if (m_blackAndWhite.currentFrame < std::numeric_limits<int64_t>::max()) {
            ++m_blackAndWhite.currentFrame;
        }
        const int64_t frames =
            static_cast<int64_t>(m_blackAndWhite.transitionFrames);
        if (m_blackAndWhite.direction > 0) {
            if (m_blackAndWhite.currentFrame < frames) {
                m_blackAndWhite.mix =
                    static_cast<float>(m_blackAndWhite.currentFrame) /
                    static_cast<float>(m_blackAndWhite.transitionFrames);
            } else {
                m_blackAndWhite.mix = 1.0f;
                m_blackAndWhite.direction = 0;
            }
        } else if (m_blackAndWhite.currentFrame < frames) {
            m_blackAndWhite.mix =
                1.0f - static_cast<float>(m_blackAndWhite.currentFrame) /
                    static_cast<float>(m_blackAndWhite.transitionFrames);
        } else {
            m_blackAndWhite.mix = 0.0f;
            m_blackAndWhite.direction = 0;
            m_blackAndWhite.active = false;
            m_activeViewFilter = ScriptViewFilter::None;
        }
    }

    RenderCameraSnapshot output = cameraSnapshot;
    if (m_motionBlur.hasPresentationTranslation) {
        output.position += m_motionBlur.presentationTranslation;
        output.target += m_motionBlur.presentationTranslation;
    }
    if (m_activeViewFilter == ScriptViewFilter::MotionBlur &&
        m_motionBlur.active) {
        MotionBlurConsumer& filter = m_motionBlur;
        if (filter.mode == MotionBlurRenderMode::Follow) {
            math::vec2 delta{};
            if (m_hasLastViewFilterCamera && m_device &&
                m_device->width() != 0 && m_device->height() != 0) {
                const WorldCamera camera = WorldCamera::fromSnapshot(output);
                const float aspect = static_cast<float>(m_device->width()) /
                    static_cast<float>(m_device->height());
                const math::float4x4 projection =
                    camera.viewProjectionMatrix(aspect);
                const math::vec3 priorPoint{
                    m_lastViewFilterCamera.target.x(),
                    m_lastViewFilterCamera.target.y(), output.target.z()};
                const math::vec4 prior = projection.transform_vec4(
                    {priorPoint.x(), priorPoint.y(), priorPoint.z(), 1.0f});
                const math::vec4 current = projection.transform_vec4(
                    {output.target.x(), output.target.y(), output.target.z(), 1.0f});
                if (std::isfinite(prior.w()) && std::isfinite(current.w()) &&
                    std::abs(prior.w()) > math::EPSILON &&
                    std::abs(current.w()) > math::EPSILON) {
                    delta = {
                        (current.x() / current.w() - prior.x() / prior.w()) * 0.5f,
                        -(current.y() / current.w() - prior.y() / prior.w()) * 0.5f,
                    };
                }
            }
            const int32_t panFactor =
                legacyMotionBlurPanFactor(filter.followAmount);
            const float length = std::sqrt(delta.length_sq());
            filter.maxCount = legacyMotionBlurPanCount(length, panFactor);
            filter.panUvDelta = delta;
        } else if (filter.mode == MotionBlurRenderMode::EndFollow) {
            if (filter.maxCount > std::numeric_limits<int32_t>::min()) {
                --filter.maxCount;
            }
            if (filter.maxCount < 2) filter.endAfterCurrentPass = true;
        } else if (!filter.hasSimulationFrame ||
                   filter.lastSimulationFrame != simulationFrame) {
            filter.lastSimulationFrame = simulationFrame;
            filter.hasSimulationFrame = true;
            if (filter.decrement) {
                filter.maxCount -= kMotionBlurCountStep;
                if (filter.maxCount < 1) filter.endAfterCurrentPass = true;
            } else {
                filter.maxCount += kMotionBlurCountStep;
                if (filter.maxCount >= kMotionBlurMaximumCount) {
                    filter.maxCount = kMotionBlurMaximumCount;
                    if (filter.mode == MotionBlurRenderMode::ZoomJump &&
                        filter.hasJumpTarget) {
                        filter.jumpBaseTarget = cameraSnapshot.target;
                        filter.jumpAfterCurrentPass = true;
                        filter.decrement = true;
                    } else {
                        filter.endAfterCurrentPass = true;
                    }
                }
            }
        }
    }
    m_lastViewFilterCamera = output;
    m_hasLastViewFilterCamera = true;
    return output;
}

void WorldPostProcessRenderer::renderScriptViewFilters() {
    const auto finishMotionBlurPass = [this]() noexcept {
        if (m_activeViewFilter != ScriptViewFilter::MotionBlur ||
            !m_motionBlur.active) {
            return;
        }
        if (m_motionBlur.jumpAfterCurrentPass && m_motionBlur.hasJumpTarget) {
            m_motionBlur.presentationTranslation =
                m_motionBlur.jumpTarget - m_motionBlur.jumpBaseTarget;
            m_motionBlur.hasPresentationTranslation = true;
            m_motionBlur.jumpAfterCurrentPass = false;
            m_motionBlur.captureSceneNextPass = true;
        }
        if (m_motionBlur.endAfterCurrentPass) {
            m_motionBlur.active = false;
            m_motionBlur.endAfterCurrentPass = false;
            m_activeViewFilter = ScriptViewFilter::None;
        }
    };

    if (m_activeViewFilter == ScriptViewFilter::MotionBlur &&
        m_motionBlur.active) {
        const bool radial =
            m_motionBlur.mode == MotionBlurRenderMode::ZoomIn ||
            m_motionBlur.mode == MotionBlurRenderMode::ZoomOut ||
            m_motionBlur.mode == MotionBlurRenderMode::ZoomJump;
        const LegacyMotionBlurGeometry geometry = radial
            ? LegacyMotionBlurGeometry::Radial
            : (m_motionBlur.mode == MotionBlurRenderMode::EndFollow
                ? LegacyMotionBlurGeometry::EndPan
                : LegacyMotionBlurGeometry::Pan);
        const LegacyMotionBlurSamplePlan samplePlan =
            legacyMotionBlurSamplePlan(
                m_motionBlur.maxCount, m_motionBlur.saturate, geometry,
                m_motionBlur.panUvDelta.x(), m_motionBlur.panUvDelta.y());
        const bool rendererReady = m_initialized && m_device &&
            m_motionBlurRootSignature && m_motionBlurPipelineState;
        const bool retainedCaptureValid = rendererReady &&
            m_viewFilterSceneCopy && m_viewFilterSceneCopySrv != UINT32_MAX &&
            m_viewFilterSceneCopyWidth == m_device->width() &&
            m_viewFilterSceneCopyHeight == m_device->height();
        const bool needsCapture = !radial ||
            m_motionBlur.captureSceneNextPass || !retainedCaptureValid;
        const bool captureReady = rendererReady &&
            (needsCapture ? captureViewFilterScene() : retainedCaptureValid);
        if (captureReady && radial) m_motionBlur.captureSceneNextPass = false;
        if (captureReady) {
            ID3D12GraphicsCommandList* commandList = m_device->commandList();
            if (commandList) {
                D3D12_VIEWPORT viewport{};
                viewport.Width = static_cast<float>(m_device->width());
                const uint32_t tacticalHeight = std::clamp(
                    static_cast<uint32_t>(
                        static_cast<float>(m_device->height()) *
                            m_lastViewFilterCamera.tacticalViewportHeightScale +
                        0.5f),
                    1u, std::max(m_device->height(), 1u));
                viewport.Height = static_cast<float>(tacticalHeight);
                viewport.MinDepth = 0.0f;
                viewport.MaxDepth = 1.0f;
                const D3D12_RECT scissor{
                    0, 0, static_cast<LONG>(m_device->width()),
                    static_cast<LONG>(tacticalHeight)};
                const float constants[12] = {
                    samplePlan.centerX,
                    samplePlan.centerY,
                    samplePlan.baseScale,
                    static_cast<float>(tacticalHeight) /
                        static_cast<float>(std::max(m_device->height(), 1u)),
                    samplePlan.stepScaleX,
                    samplePlan.stepScaleY,
                    samplePlan.sampleAlpha,
                    samplePlan.additive ? 1.0f : 0.0f,
                    static_cast<float>(samplePlan.tapCount),
                    0.0f,
                    0.0f,
                    0.0f,
                };
                commandList->RSSetViewports(1, &viewport);
                commandList->RSSetScissorRects(1, &scissor);
                m_device->bindSrvHeap();
                commandList->SetGraphicsRootSignature(
                    m_motionBlurRootSignature.Get());
                m_device->recordGraphicsRootSignatureCall();
                commandList->SetPipelineState(m_motionBlurPipelineState.Get());
                m_device->recordPipelineStateCall();
                commandList->SetGraphicsRoot32BitConstants(
                    0, 12, constants, 0);
                commandList->SetGraphicsRootDescriptorTable(
                    1, m_device->getSrvGpuHandle(m_viewFilterSceneCopySrv));
                m_device->recordGraphicsDescriptorTableCall();
                commandList->IASetPrimitiveTopology(
                    D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                commandList->DrawInstanced(3, 1, 0, 0);
                m_device->recordDrawCall();
            }
        }
        finishMotionBlurPass();
        return;
    }

    if (m_activeViewFilter != ScriptViewFilter::BlackAndWhite ||
        !m_blackAndWhite.active || m_blackAndWhite.mix <= 0.0f ||
        !m_initialized || !m_device || !m_blackAndWhiteRootSignature ||
        !m_blackAndWhitePipelineState || !captureViewFilterScene()) {
        return;
    }
    ID3D12GraphicsCommandList* commandList = m_device->commandList();
    if (!commandList) return;
    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_device->width());
    const uint32_t tacticalHeight = std::clamp(
        static_cast<uint32_t>(static_cast<float>(m_device->height()) *
                m_lastViewFilterCamera.tacticalViewportHeightScale +
            0.5f),
        1u, std::max(m_device->height(), 1u));
    viewport.Height = static_cast<float>(tacticalHeight);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{
        0, 0, static_cast<LONG>(m_device->width()),
        static_cast<LONG>(tacticalHeight)};
    const float mix = std::clamp(m_blackAndWhite.mix, 0.0f, 1.0f);
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    m_device->bindSrvHeap();
    commandList->SetGraphicsRootSignature(m_blackAndWhiteRootSignature.Get());
    m_device->recordGraphicsRootSignatureCall();
    commandList->SetPipelineState(m_blackAndWhitePipelineState.Get());
    m_device->recordPipelineStateCall();
    commandList->SetGraphicsRoot32BitConstants(0, 1, &mix, 0);
    commandList->SetGraphicsRootDescriptorTable(
        1, m_device->getSrvGpuHandle(m_viewFilterSceneCopySrv));
    m_device->recordGraphicsDescriptorTableCall();
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);
    m_device->recordDrawCall();
}

bool WorldPostProcessRenderer::configureFxaa(
    bool enabled, float subpixel, float edgeThreshold,
    float edgeThresholdMin) noexcept {
    m_fxaaSubpixel = std::clamp(
        std::isfinite(subpixel) ? subpixel : 0.75f, 0.0f, 1.0f);
    m_fxaaEdgeThreshold = std::clamp(
        std::isfinite(edgeThreshold) ? edgeThreshold : 0.166f,
        0.0312f, 0.333f);
    m_fxaaEdgeThresholdMin = std::clamp(
        std::isfinite(edgeThresholdMin) ? edgeThresholdMin : 0.0833f,
        0.0f, 0.125f);
    m_fxaaEnabled = enabled && m_fxaaAvailable &&
        m_fxaaRootSignature && m_fxaaPipelineState;
    return !enabled || m_fxaaEnabled;
}

bool WorldPostProcessRenderer::renderFxaa(
    float tacticalViewportHeightScale) {
    if (!m_fxaaEnabled || !m_initialized || !m_device ||
        !m_fxaaRootSignature || !m_fxaaPipelineState ||
        m_device->width() == 0u || m_device->height() == 0u ||
        !captureViewFilterScene()) {
        return false;
    }
    ID3D12GraphicsCommandList* commandList = m_device->commandList();
    if (!commandList) return false;
    const bool timestampActive = m_device->beginGpuTimestamp(
        GpuTimestampRange::Fxaa);
    const uint32_t tacticalHeight = std::clamp(
        static_cast<uint32_t>(static_cast<float>(m_device->height()) *
                std::clamp(tacticalViewportHeightScale, 0.1f, 1.0f) +
            0.5f),
        1u, std::max(m_device->height(), 1u));
    const float tacticalHeightScale = static_cast<float>(tacticalHeight) /
        static_cast<float>(std::max(m_device->height(), 1u));
    const float constants[8] = {
        1.0f / static_cast<float>(m_device->width()),
        1.0f / static_cast<float>(m_device->height()),
        tacticalHeightScale,
        m_fxaaSubpixel,
        m_fxaaEdgeThreshold,
        m_fxaaEdgeThresholdMin,
        0.0f,
        0.0f,
    };
    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_device->width());
    viewport.Height = static_cast<float>(tacticalHeight);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{
        0, 0, static_cast<LONG>(m_device->width()),
        static_cast<LONG>(tacticalHeight)};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    m_device->bindSrvHeap();
    commandList->SetGraphicsRootSignature(m_fxaaRootSignature.Get());
    m_device->recordGraphicsRootSignatureCall();
    commandList->SetPipelineState(m_fxaaPipelineState.Get());
    m_device->recordPipelineStateCall();
    commandList->SetGraphicsRoot32BitConstants(0, 8, constants, 0);
    commandList->SetGraphicsRootDescriptorTable(
        1, m_device->getSrvGpuHandle(m_viewFilterSceneCopySrv));
    m_device->recordGraphicsDescriptorTableCall();
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);
    m_device->recordDrawCall();
    if (timestampActive) {
        static_cast<void>(m_device->endGpuTimestamp(GpuTimestampRange::Fxaa));
    }
    return true;
}

void WorldPostProcessRenderer::renderScreenFade(
    const ScreenFadeRenderState& fade, uint64_t simulationFrame) {
    if (fade.presentationEpoch != 0) {
        if (m_screenFadeCursor.presentationEpoch == 0 ||
            fade.presentationEpoch > m_screenFadeCursor.presentationEpoch) {
            m_screenFadeCursor = {
                .presentationEpoch = fade.presentationEpoch,
                .simulationFrame = simulationFrame,
                .presentationSequence = fade.presentationSequence,
            };
        } else if (
            fade.presentationEpoch < m_screenFadeCursor.presentationEpoch ||
            simulationFrame < m_screenFadeCursor.simulationFrame ||
            (simulationFrame == m_screenFadeCursor.simulationFrame &&
             fade.presentationSequence <
                 m_screenFadeCursor.presentationSequence)) {
            return;
        } else {
            m_screenFadeCursor.simulationFrame = simulationFrame;
            m_screenFadeCursor.presentationSequence =
                fade.presentationSequence;
        }
    } else if (m_screenFadeCursor.presentationEpoch != 0) {
        return;
    }

    if (!fade.active || !m_initialized || !m_device ||
        m_device->width() == 0 || m_device->height() == 0) {
        return;
    }
    const size_t modeIndex = static_cast<size_t>(fade.blendMode);
    if (fade.blendMode == ScreenFadeBlendMode::None ||
        modeIndex >= m_screenFadePipelineStates.size() ||
        !m_screenFadeRootSignature ||
        !m_screenFadePipelineStates[modeIndex]) {
        return;
    }
    if (fade.intensity == 0 &&
        (fade.blendMode == ScreenFadeBlendMode::Add ||
         fade.blendMode == ScreenFadeBlendMode::Subtract)) {
        return;
    }

    ID3D12GraphicsCommandList* commandList = m_device->commandList();
    if (!commandList) return;
    m_device->flushBatch();
    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_device->width());
    const uint32_t tacticalHeight = std::clamp(
        static_cast<uint32_t>(static_cast<float>(m_device->height()) *
                m_lastViewFilterCamera.tacticalViewportHeightScale +
            0.5f),
        1u, std::max(m_device->height(), 1u));
    viewport.Height = static_cast<float>(tacticalHeight);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{
        0, 0, static_cast<LONG>(m_device->width()),
        static_cast<LONG>(tacticalHeight)};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->SetGraphicsRootSignature(m_screenFadeRootSignature.Get());
    m_device->recordGraphicsRootSignatureCall();
    commandList->SetPipelineState(
        m_screenFadePipelineStates[modeIndex].Get());
    m_device->recordPipelineStateCall();
    const float intensity = static_cast<float>(fade.intensity) / 255.0f;
    commandList->SetGraphicsRoot32BitConstants(0, 1, &intensity, 0);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);
    m_device->recordDrawCall();

    if (fade.blendMode == ScreenFadeBlendMode::Saturate) {
        commandList->DrawInstanced(3, 1, 0, 0);
        m_device->recordDrawCall();
    }
}

} // namespace engine::render
