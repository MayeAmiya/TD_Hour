#include "engine/renderer/world/pipeline/WorldRenderer.h"

#include "debug/debug.h"
#include "engine/renderer/d3d12/runtime/D3D12Device.h"
#include "engine/renderer/world/model/W3dStaticModel.h"
#include "engine/renderer/world/pipeline/WorldRendererGpuLayout.h"
#include "engine/renderer/world/pipeline/WorldRendererMaterialPacking.h"
#include "engine/renderer/world/pipeline/WorldRendererShadowSettings.h"
#include "engine/renderer/world/pipeline/WorldRendererUploadOwner.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <tuple>

namespace engine::render {
namespace {

using world_renderer_detail::StaticMeshGpuInstance;
using world_renderer_detail::WorldRendererUploadOwner;

struct alignas(16) ShadowCameraConstants final {
    float lightViewProjection[16];
};
static_assert(sizeof(ShadowCameraConstants) == 64);

struct alignas(16) ShadowDrawConstants final {
    uint32_t skinBoneCount = 0;
    uint32_t samplerMode = 0;
    uint32_t alphaTestMode = 0;
    float alphaCutoff = 0x60 / 255.0f;
    uint32_t detailSamplerMode = 0;
    uint32_t hasDetailTexture = 0;
    uint32_t detailAlphaFunc = 0;
    float materialDiffuseAlpha = 1.0f;
    float mapperScaleOffset[2][4]{
        {1.0f, 1.0f, 0.0f, 0.0f},
        {1.0f, 1.0f, 0.0f, 0.0f},
    };
    float mapperMotionCenter[2][4]{};
    uint32_t mapperTypes[2]{};
    uint32_t mapperClampFix[2]{};
    float mapperTimeSeconds = 0.0f;
    float mapperPadding[3]{};
};
static_assert(sizeof(ShadowDrawConstants) == 128);

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

[[nodiscard]] bool isFiniteVector(const math::vec3& value) noexcept {
    return std::isfinite(value.x()) && std::isfinite(value.y()) &&
        std::isfinite(value.z());
}

[[nodiscard]] bool canInstanceShadowTogether(
    const StaticMeshDrawPacket& lhs,
    const StaticMeshDrawPacket& rhs) noexcept {
    if (lhs.blendMode != StaticMeshBlendMode::Opaque ||
        rhs.blendMode != StaticMeshBlendMode::Opaque ||
        lhs.skinBoneCount != 0 || rhs.skinBoneCount != 0) {
        return false;
    }
    if (lhs.vertexBuffer.BufferLocation != rhs.vertexBuffer.BufferLocation ||
        lhs.vertexBuffer.SizeInBytes != rhs.vertexBuffer.SizeInBytes ||
        lhs.vertexBuffer.StrideInBytes != rhs.vertexBuffer.StrideInBytes ||
        lhs.indexBuffer.BufferLocation != rhs.indexBuffer.BufferLocation ||
        lhs.indexBuffer.SizeInBytes != rhs.indexBuffer.SizeInBytes ||
        lhs.indexBuffer.Format != rhs.indexBuffer.Format ||
        lhs.firstIndex != rhs.firstIndex ||
        lhs.indexCount != rhs.indexCount ||
        lhs.baseVertex != rhs.baseVertex ||
        lhs.twoSided != rhs.twoSided ||
        lhs.alphaTestMode != rhs.alphaTestMode) {
        return false;
    }
    if (lhs.alphaTestMode == StaticMeshAlphaTestMode::Disabled) return true;
    return lhs.textureSrv.ptr == rhs.textureSrv.ptr &&
        lhs.detailTextureSrv.ptr == rhs.detailTextureSrv.ptr &&
        lhs.textureMappers == rhs.textureMappers &&
        lhs.visualTimeSeconds == rhs.visualTimeSeconds &&
        lhs.samplerMode == rhs.samplerMode &&
        lhs.detailSamplerMode == rhs.detailSamplerMode &&
        lhs.detailAlphaFunc == rhs.detailAlphaFunc &&
        lhs.hasDetailTexture == rhs.hasDetailTexture &&
        lhs.diffuse.w() == rhs.diffuse.w();
}

} // namespace

bool WorldRenderer::createDirectionalShadowResources() {
    if (!m_device || !m_device->getDevice()) return false;

    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heapDescription.NumDescriptors = 1;
    const HRESULT heapResult = m_device->getDevice()->CreateDescriptorHeap(
        &heapDescription, IID_PPV_ARGS(&m_shadowDsvHeap));
    if (FAILED(heapResult)) {
        TD_LOG_ERROR("[WorldRenderer] Shadow DSV heap creation failed: 0x{:08X}",
                     heapResult);
        return false;
    }

    D3D12_RESOURCE_DESC resourceDescription{};
    resourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDescription.Width = world_renderer_shadow::kMapSize;
    resourceDescription.Height = world_renderer_shadow::kMapSize;
    resourceDescription.DepthOrArraySize = 1;
    resourceDescription.MipLevels = 1;
    resourceDescription.Format = DXGI_FORMAT_R32_TYPELESS;
    resourceDescription.SampleDesc.Count = 1;
    resourceDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    const HRESULT resourceResult = m_device->getDevice()->CreateCommittedResource(
        &heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDescription,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
        IID_PPV_ARGS(&m_shadowMap));
    if (FAILED(resourceResult)) {
        TD_LOG_ERROR("[WorldRenderer] Shadow map allocation failed: 0x{:08X}",
                     resourceResult);
        return false;
    }
    m_shadowMapState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDescription{};
    dsvDescription.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDescription.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    m_device->getDevice()->CreateDepthStencilView(
        m_shadowMap.Get(), &dsvDescription,
        m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart());

    m_shadowMapSrv = m_device->allocateSrvDescriptor();
    if (m_shadowMapSrv == UINT32_MAX) {
        TD_LOG_ERROR("[WorldRenderer] Shadow map SRV allocation failed");
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDescription{};
    srvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDescription.Format = DXGI_FORMAT_R32_FLOAT;
    srvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDescription.Texture2D.MipLevels = 1;
    m_device->getDevice()->CreateShaderResourceView(
        m_shadowMap.Get(), &srvDescription,
        m_device->getSrvCpuHandle(m_shadowMapSrv));
    return true;
}

bool WorldRenderer::createDirectionalShadowPipelineStates() {
    if (!m_device || !m_device->getDevice() || !m_rootSignature) return false;
    const auto& vertexShader = m_shaderBytecode[static_cast<size_t>(
        ShaderBytecode::DirectionalShadowVertex)];
    const auto& alphaPixelShader = m_shaderBytecode[static_cast<size_t>(
        ShaderBytecode::DirectionalShadowPixel)];
    if (vertexShader.empty() || alphaPixelShader.empty()) return false;

    const D3D12_INPUT_ELEMENT_DESC inputElements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
         static_cast<UINT>(offsetof(StaticMeshVertex, position)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
         static_cast<UINT>(offsetof(StaticMeshVertex, texcoord)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0,
         static_cast<UINT>(offsetof(StaticMeshVertex, detailTexcoord)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
         static_cast<UINT>(offsetof(StaticMeshVertex, color)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R32_UINT, 0,
         static_cast<UINT>(offsetof(StaticMeshVertex, boneIndex)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"INSTANCEWORLD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, world) + sizeof(float) * 0),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INSTANCEWORLD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, world) + sizeof(float) * 4),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INSTANCEWORLD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, world) + sizeof(float) * 8),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INSTANCEWORLD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, world) + sizeof(float) * 12),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INSTANCEEFFECT", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, treePushAsideDirection)),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PREVIOUSWORLD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, previousWorld) + sizeof(float) * 0),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PREVIOUSWORLD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, previousWorld) + sizeof(float) * 4),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PREVIOUSWORLD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, previousWorld) + sizeof(float) * 8),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"PREVIOUSWORLD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, previousWorld) + sizeof(float) * 12),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INTERPOLATION", 0, DXGI_FORMAT_R32_FLOAT, 1,
         static_cast<UINT>(offsetof(StaticMeshGpuInstance, interpolationAlpha)),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = m_rootSignature.Get();
    description.VS = {vertexShader.data(), vertexShader.size()};
    description.InputLayout = {inputElements, static_cast<UINT>(std::size(inputElements))};
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.FrontCounterClockwise = TRUE;
    description.RasterizerState.DepthBias =
        world_renderer_shadow::kRasterDepthBias;
    description.RasterizerState.DepthBiasClamp = 0.0f;
    description.RasterizerState.SlopeScaledDepthBias =
        world_renderer_shadow::kRasterSlopeScaledDepthBias;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthEnable = TRUE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 0;
    description.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    description.SampleDesc.Count = 1;

    const auto createPair = [this, &description](
        PipelineStatePair& output,
        const container::Vector<uint8_t>* pixelShader) {
        description.PS = pixelShader
            ? D3D12_SHADER_BYTECODE{pixelShader->data(), pixelShader->size()}
            : D3D12_SHADER_BYTECODE{};
        description.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        HRESULT result = m_device->getDevice()->CreateGraphicsPipelineState(
            &description, IID_PPV_ARGS(&output.backFaceCulled));
        if (FAILED(result)) return false;
        description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        result = m_device->getDevice()->CreateGraphicsPipelineState(
            &description, IID_PPV_ARGS(&output.twoSided));
        return SUCCEEDED(result);
    };
    return createPair(m_shadowOpaquePipelineState, nullptr) &&
           createPair(m_shadowAlphaTestPipelineState, &alphaPixelShader);
}

bool WorldRenderer::createDirectionalShadowFallbackSrv() {
    if (!m_device || !m_device->getDevice()) return false;
    if (m_shadowFallbackSrv != UINT32_MAX) return true;

    m_shadowFallbackSrv = m_device->allocateSrvDescriptor();
    if (m_shadowFallbackSrv == UINT32_MAX) {
        TD_LOG_ERROR("[WorldRenderer] Shadow fallback SRV allocation failed");
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC description{};
    description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    description.Format = DXGI_FORMAT_R32_FLOAT;
    description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    description.Texture2D.MipLevels = 1;
    // A null descriptor is legal D3D12 state and preserves the Texture2D<float>
    // comparison-sampling type even when shadowStrength disables sampling.
    m_device->getDevice()->CreateShaderResourceView(
        nullptr, &description, m_device->getSrvCpuHandle(m_shadowFallbackSrv));
    return true;
}

void WorldRenderer::releaseDirectionalShadowResources() noexcept {
    m_directionalShadowValid = false;
    m_directionalShadowAvailable = false;
    m_shadowOpaquePipelineState.backFaceCulled.Reset();
    m_shadowOpaquePipelineState.twoSided.Reset();
    m_shadowAlphaTestPipelineState.backFaceCulled.Reset();
    m_shadowAlphaTestPipelineState.twoSided.Reset();
    if (m_shadowMapSrv != UINT32_MAX && m_device && m_device->getDevice()) {
        m_device->freeSrvDescriptor(m_shadowMapSrv);
    }
    if (m_shadowFallbackSrv != UINT32_MAX && m_device && m_device->getDevice()) {
        m_device->freeSrvDescriptor(m_shadowFallbackSrv);
    }
    m_shadowMapSrv = UINT32_MAX;
    m_shadowFallbackSrv = UINT32_MAX;
    m_shadowMap.Reset();
    m_shadowDsvHeap.Reset();
    m_shadowMapState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_shadowViewProjection = {};
}

bool WorldRenderer::renderDirectionalShadowMap(
    container::Span<const StaticMeshDrawPacket> drawPackets,
    const RenderCameraSnapshot& cameraSnapshot,
    const WorldLightEnvironment& lightEnvironment) {
    m_directionalShadowValid = false;
    if (!m_directionalShadowAvailable || !m_shadowMap || !m_shadowDsvHeap ||
        drawPackets.empty() || !m_device || !m_device->commandList()) {
        return false;
    }

    container::Vector<const StaticMeshDrawPacket*>& casters =
        m_shadowCasterScratch;
    casters.clear();
    struct ClearCasterPointersOnExit final {
        container::Vector<const StaticMeshDrawPacket*>& values;
        ~ClearCasterPointersOnExit() { values.clear(); }
    } clearCasterPointersOnExit{casters};
    const size_t casterCapacityBefore = casters.capacity();
    casters.reserve(drawPackets.size());
    if (casters.capacity() > casterCapacityBefore &&
        m_lastStaticMeshStats.shadowCasterScratchCapacityGrowths !=
            std::numeric_limits<uint32_t>::max()) {
        ++m_lastStaticMeshStats.shadowCasterScratchCapacityGrowths;
    }
    const uint32_t casterCapacity = static_cast<uint32_t>(
        std::min<size_t>(casters.capacity(),
                         std::numeric_limits<uint32_t>::max()));
    m_shadowCasterScratchHighWater = std::max(
        m_shadowCasterScratchHighWater, casterCapacity);
    m_lastStaticMeshStats.shadowCasterScratchCapacity = casterCapacity;
    m_lastStaticMeshStats.shadowCasterScratchCapacityHighWater =
        m_shadowCasterScratchHighWater;
    for (const StaticMeshDrawPacket& draw : drawPackets) {
        const bool shadowBlendEligible =
            draw.blendMode == StaticMeshBlendMode::Opaque ||
            draw.blendMode == StaticMeshBlendMode::Alpha;
        if (!draw.castsShadow || !shadowBlendEligible) {
            ++m_lastStaticMeshStats.shadowPolicyRejectedPackets;
            continue;
        }
        if (draw.indexCount == 0 ||
            draw.vertexBuffer.BufferLocation == 0 ||
            draw.vertexBuffer.SizeInBytes == 0 ||
            draw.vertexBuffer.StrideInBytes != sizeof(StaticMeshVertex) ||
            draw.indexBuffer.BufferLocation == 0 ||
            draw.indexBuffer.SizeInBytes == 0 ||
            draw.indexBuffer.Format != DXGI_FORMAT_R32_UINT ||
            (draw.skinBoneCount != 0 &&
             (!draw.skinPalette || draw.skinBoneCount >
                  world_renderer_detail::kMaximumSkinBones))) {
            ++m_lastStaticMeshStats.shadowInvalidPackets;
            continue;
        }
        const uint64_t indexEnd = static_cast<uint64_t>(draw.firstIndex) +
            draw.indexCount;
        if (indexEnd * sizeof(uint32_t) > draw.indexBuffer.SizeInBytes) {
            ++m_lastStaticMeshStats.shadowInvalidPackets;
            continue;
        }
        casters.push_back(&draw);
    }
    if (casters.empty()) return false;
    m_lastStaticMeshStats.shadowCasterPackets = static_cast<uint32_t>(
        std::min<size_t>(casters.size(), std::numeric_limits<uint32_t>::max()));

    const auto casterSortKey = [](const StaticMeshDrawPacket* draw) {
        return std::tuple{
            draw->materialPass,
            draw->twoSided,
            static_cast<uint8_t>(draw->alphaTestMode),
            draw->vertexBuffer.BufferLocation,
            draw->indexBuffer.BufferLocation,
            draw->firstIndex,
            draw->indexCount,
            draw->baseVertex,
            draw->textureSrv.ptr,
        };
    };
    std::stable_sort(casters.begin(), casters.end(),
                     [&casterSortKey](const StaticMeshDrawPacket* left,
                                      const StaticMeshDrawPacket* right) {
                         return casterSortKey(left) < casterSortKey(right);
                     });

    math::vec3 directionToLight =
        lightEnvironment.directionalLights[0].directionToLight;
    const float lightLength = directionToLight.length();
    if (!isFiniteVector(directionToLight) || !std::isfinite(lightLength) ||
        lightLength <= math::EPSILON) {
        return false;
    }
    directionToLight = directionToLight / lightLength;
    math::vec3 center = isFiniteVector(cameraSnapshot.target)
        ? cameraSnapshot.target : cameraSnapshot.position;
    const float sourceRange = std::isfinite(cameraSnapshot.visibilityDistance) &&
                              cameraSnapshot.visibilityDistance > 0.0f
        ? cameraSnapshot.visibilityDistance : cameraSnapshot.farClip;
    const float shadowRadius = std::clamp(
        std::isfinite(sourceRange) ? sourceRange * 0.34f : 350.0f,
        120.0f, 700.0f);
    math::vec3 lightForward = -directionToLight;
    math::vec3 lightUp = std::abs(lightForward.z()) < 0.94f
        ? math::vec3{0.0f, 0.0f, 1.0f}
        : math::vec3{0.0f, 1.0f, 0.0f};
    math::vec3 lightRight = lightForward.cross(lightUp).normalized();
    lightUp = lightRight.cross(lightForward).normalized();
    const float shadowMapSize =
        static_cast<float>(world_renderer_shadow::kMapSize);
    const float worldUnitsPerTexel = (shadowRadius * 2.0f) / shadowMapSize;
    const float centerRight = center.dot(lightRight);
    const float centerUp = center.dot(lightUp);
    center += lightRight *
        (std::round(centerRight / worldUnitsPerTexel) * worldUnitsPerTexel - centerRight);
    center += lightUp *
        (std::round(centerUp / worldUnitsPerTexel) * worldUnitsPerTexel - centerUp);
    const float lightDistance = shadowRadius * 2.5f + 500.0f;
    const math::vec3 eye = center + directionToLight * lightDistance;
    const math::float4x4 lightView =
        math::float4x4::look_at_rh(eye, center, lightUp);
    const math::float4x4 lightProjection{
        DirectX::XMMatrixOrthographicRH(
            shadowRadius * 2.0f, shadowRadius * 2.0f,
            1.0f, lightDistance * 2.0f)};
    m_shadowViewProjection = lightView * lightProjection;

    ShadowCameraConstants shadowCamera{};
    std::memcpy(shadowCamera.lightViewProjection,
                &m_shadowViewProjection.m,
                sizeof(shadowCamera.lightViewProjection));
    const d3d12::ConstantBufferAllocation cameraAllocation =
        m_device->allocateConstantBuffer(&shadowCamera, sizeof(shadowCamera));
    if (!cameraAllocation) return false;

    ID3D12GraphicsCommandList* commandList = m_device->commandList();
    m_device->flushBatch();
    if (m_shadowMapState != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
        const D3D12_RESOURCE_BARRIER barrier = transitionBarrier(
            m_shadowMap.Get(), m_shadowMapState,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        commandList->ResourceBarrier(1, &barrier);
        m_shadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv =
        m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
    commandList->ClearDepthStencilView(
        shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    commandList->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsv);
    D3D12_VIEWPORT viewport{};
    viewport.Width = shadowMapSize;
    viewport.Height = shadowMapSize;
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{0, 0,
        static_cast<LONG>(world_renderer_shadow::kMapSize),
        static_cast<LONG>(world_renderer_shadow::kMapSize)};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    m_device->bindSrvHeap();
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_device->recordGraphicsRootSignatureCall();
    commandList->SetGraphicsRootConstantBufferView(0, cameraAllocation.gpuAddress);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    if (!m_uploadOwner) {
        m_uploadOwner = std::make_unique<WorldRendererUploadOwner>();
    }
    auto skinPaletteUploads = m_uploadOwner->beginPaletteUploads(
        *m_device, casters.size(), m_lastStaticMeshStats);
    ID3D12PipelineState* activePipeline = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE activeTexture{};
    D3D12_GPU_DESCRIPTOR_HANDLE activeDetailTexture{};
    D3D12_GPU_VIRTUAL_ADDRESS activeVertexBuffer = 0;
    D3D12_GPU_VIRTUAL_ADDRESS activeIndexBuffer = 0;
    container::Vector<StaticMeshGpuInstance>& gpuInstances =
        m_uploadOwner->instances();
    constexpr size_t kMaximumInstancesPerUpload =
        std::numeric_limits<uint32_t>::max() / sizeof(StaticMeshGpuInstance);

    size_t packetIndex = 0;
    while (packetIndex < casters.size()) {
        const StaticMeshDrawPacket& draw = *casters[packetIndex];
        size_t batchEnd = packetIndex + 1;
        while (batchEnd < casters.size() &&
               batchEnd - packetIndex < kMaximumInstancesPerUpload &&
               canInstanceShadowTogether(draw, *casters[batchEnd])) {
            ++batchEnd;
        }

        ShadowDrawConstants drawConstants{};
        drawConstants.skinBoneCount = draw.skinBoneCount;
        drawConstants.samplerMode = std::min<uint32_t>(draw.samplerMode, 3u);
        drawConstants.alphaTestMode = static_cast<uint32_t>(draw.alphaTestMode);
        drawConstants.alphaCutoff =
            draw.alphaTestMode == StaticMeshAlphaTestMode::LessEqual
                ? (0xff - 0x60) / 255.0f
                : world_renderer_detail::kW3dAlphaTestCutoff;
        drawConstants.detailSamplerMode =
            std::min<uint32_t>(draw.detailSamplerMode, 3u);
        drawConstants.hasDetailTexture = draw.hasDetailTexture ? 1u : 0u;
        drawConstants.detailAlphaFunc =
            std::min<uint32_t>(draw.detailAlphaFunc, 3u);
        drawConstants.materialDiffuseAlpha = draw.diffuse.w();
        for (size_t stage = 0; stage < draw.textureMappers.size(); ++stage) {
            world_renderer_detail::packTextureMapper(
                              draw.textureMappers[stage],
                              drawConstants.mapperScaleOffset[stage],
                              drawConstants.mapperMotionCenter[stage],
                              drawConstants.mapperTypes[stage],
                              drawConstants.mapperClampFix[stage]);
        }
        drawConstants.mapperTimeSeconds =
            std::isfinite(draw.visualTimeSeconds) && draw.visualTimeSeconds >= 0.0f
                ? draw.visualTimeSeconds : 0.0f;
        const d3d12::ConstantBufferAllocation drawAllocation =
            m_device->allocateConstantBuffer(&drawConstants, sizeof(drawConstants));
        if (!drawAllocation) break;

        const d3d12::ConstantBufferAllocation skinAllocation =
            skinPaletteUploads.resolve(draw);
        if (!skinAllocation) break;

        gpuInstances.clear();
        const size_t instanceCapacityBefore = gpuInstances.capacity();
        gpuInstances.reserve(batchEnd - packetIndex);
        m_uploadOwner->noteInstanceReserve(
            instanceCapacityBefore, m_lastStaticMeshStats);
        for (size_t instanceIndex = packetIndex;
             instanceIndex < batchEnd; ++instanceIndex) {
            StaticMeshGpuInstance gpu{};
            std::memcpy(gpu.world,
                        &casters[instanceIndex]->worldTransform.m,
                        sizeof(gpu.world));
            std::memcpy(gpu.previousWorld,
                        &casters[instanceIndex]->previousWorldTransform.m,
                        sizeof(gpu.previousWorld));
            gpu.interpolationAlpha = std::clamp(
                casters[instanceIndex]->interpolationAlpha, 0.0f, 1.0f);
            const StaticMeshDrawPacket& instanceDraw =
                *casters[instanceIndex];
            gpu.treePushAsideDirection[0] =
                instanceDraw.treePushAsideDirection.x();
            gpu.treePushAsideDirection[1] =
                instanceDraw.treePushAsideDirection.y();
            gpu.treePushAsideAmount = instanceDraw.treePushAsideAmount;
            gpu.treePushAsideDistanceFactor =
                instanceDraw.treePushAsideDistanceFactor;
            gpuInstances.push_back(gpu);
        }
        const uint32_t instanceBytes = static_cast<uint32_t>(
            gpuInstances.size() * sizeof(StaticMeshGpuInstance));
        const d3d12::FrameUploadAllocation instanceAllocation =
            m_device->allocateFrameUpload(
                gpuInstances.data(), instanceBytes,
                alignof(StaticMeshGpuInstance));
        if (!instanceAllocation) break;

        const bool alphaTest =
            draw.alphaTestMode != StaticMeshAlphaTestMode::Disabled;
        const PipelineStatePair& pipelines = alphaTest
            ? m_shadowAlphaTestPipelineState : m_shadowOpaquePipelineState;
        ID3D12PipelineState* desiredPipeline = draw.twoSided
            ? pipelines.twoSided.Get() : pipelines.backFaceCulled.Get();
        if (!desiredPipeline) break;
        if (desiredPipeline != activePipeline) {
            commandList->SetPipelineState(desiredPipeline);
            m_device->recordPipelineStateCall();
            activePipeline = desiredPipeline;
        }
        commandList->SetGraphicsRootConstantBufferView(
            1, drawAllocation.gpuAddress);
        commandList->SetGraphicsRootConstantBufferView(
            2, skinAllocation.gpuAddress);
        if (alphaTest) {
            const D3D12_GPU_DESCRIPTOR_HANDLE texture =
                draw.textureSrv.ptr != 0
                    ? draw.textureSrv : m_device->getSrvGpuHandle(0);
            const D3D12_GPU_DESCRIPTOR_HANDLE detailTexture =
                draw.detailTextureSrv.ptr != 0
                    ? draw.detailTextureSrv : m_device->getSrvGpuHandle(0);
            if (texture.ptr != activeTexture.ptr) {
                commandList->SetGraphicsRootDescriptorTable(3, texture);
                m_device->recordGraphicsDescriptorTableCall();
                activeTexture = texture;
            }
            if (detailTexture.ptr != activeDetailTexture.ptr) {
                commandList->SetGraphicsRootDescriptorTable(4, detailTexture);
                m_device->recordGraphicsDescriptorTableCall();
                activeDetailTexture = detailTexture;
            }
        }
        if (draw.vertexBuffer.BufferLocation != activeVertexBuffer) {
            commandList->IASetVertexBuffers(0, 1, &draw.vertexBuffer);
            m_device->recordVertexBufferCall();
            activeVertexBuffer = draw.vertexBuffer.BufferLocation;
        }
        const D3D12_VERTEX_BUFFER_VIEW instanceView{
            .BufferLocation = instanceAllocation.gpuAddress,
            .SizeInBytes = instanceBytes,
            .StrideInBytes = sizeof(StaticMeshGpuInstance),
        };
        commandList->IASetVertexBuffers(1, 1, &instanceView);
        m_device->recordVertexBufferCall();
        if (draw.indexBuffer.BufferLocation != activeIndexBuffer) {
            commandList->IASetIndexBuffer(&draw.indexBuffer);
            m_device->recordIndexBufferCall();
            activeIndexBuffer = draw.indexBuffer.BufferLocation;
        }
        const uint32_t instanceCount = static_cast<uint32_t>(gpuInstances.size());
        commandList->DrawIndexedInstanced(
            draw.indexCount, instanceCount, draw.firstIndex,
            draw.baseVertex, 0);
        m_device->recordDrawCall();
        ++m_lastStaticMeshStats.shadowDrawCalls;
        m_lastStaticMeshStats.shadowTriangles +=
            static_cast<uint64_t>(draw.indexCount / 3u) * instanceCount;
        packetIndex = batchEnd;
    }

    const bool completedAllCasterBatches = packetIndex == casters.size();
    if (m_shadowMapState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        const D3D12_RESOURCE_BARRIER barrier = transitionBarrier(
            m_shadowMap.Get(), m_shadowMapState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1, &barrier);
        m_shadowMapState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    m_device->bindMainRenderTargets();
    m_directionalShadowValid = completedAllCasterBatches &&
        m_lastStaticMeshStats.shadowDrawCalls != 0;
    if (!completedAllCasterBatches) {
        TD_LOG_WARN(
            "[WorldRenderer] Directional shadow pass incomplete: renderedDraws={} casterPackets={}; "
            "using the fallback SRV for this frame",
            m_lastStaticMeshStats.shadowDrawCalls,
            m_lastStaticMeshStats.shadowCasterPackets);
    }
    return m_directionalShadowValid;
}

} // namespace engine::render
