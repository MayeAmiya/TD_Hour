#include "core/container/container_types.h"
#include "engine/renderer/world/effects/ProjectileTrailRenderer.h"
#include "presentation/render/ProjectileStreamJoinPresentation.h"
#include "engine/renderer/d3d12/runtime/D3D12QualitySettings.h"
#include "engine/renderer/d3d12/runtime/D3D12ShaderPackage.h"

#include "engine/renderer/d3d12/runtime/D3D12Device.h"
#include "engine/renderer/world/pipeline/WorldCamera.h"
#include "engine/renderer/world/resource/WorldTextureCache.h"
#include "debug/debug.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <tuple>
#include <type_traits>

#ifndef TD_PROJECTILE_TRAIL_SHADER_PACKAGE_VERSION
#define TD_PROJECTILE_TRAIL_SHADER_PACKAGE_VERSION 1
#endif

#ifndef TD_PROJECTILE_TRAIL_SHADER_SOURCE_SHA256
#define TD_PROJECTILE_TRAIL_SHADER_SOURCE_SHA256 \
    "60d2e45f7506c4254a6f473a3e0af6203c1cc4b8e67c90b2a87ead9fbd105a9d"
#endif

namespace engine::render {
namespace {

constexpr uint32_t kMaximumProjectileStreams = 2048;
constexpr size_t kMaximumPointsPerStream = 64;
constexpr size_t kMaximumRenderedSegments = 16 * 1024;

struct alignas(16) TrailCameraConstants final {
    float viewProjection[16];
    float playableMinimum[2]{};
    float playableMaximum[2]{};
    uint32_t playableBoundsEnabled = 0;
    uint32_t padding[3]{};
};
static_assert(sizeof(TrailCameraConstants) == 96);

[[nodiscard]] bool finiteVector(const RenderVector& value) noexcept {
    return std::isfinite(value.x()) && std::isfinite(value.y()) &&
           std::isfinite(value.z());
}

[[nodiscard]] container::String streamKey(const ProjectileRenderSnapshot& source) {
    return std::to_string(source.launcherId) + "|" +
        std::to_string(source.trailStreamInstance) + "|" +
        std::to_string(source.launchSlot) + "|" +
        std::to_string(source.trailOwnerGeneration) + "|" +
        std::to_string(source.trailChainIdentity) + "|" +
        source.trailStreamName;
}

[[nodiscard]] size_t pipelineIndex(ProjectileTrailBlendMode blend,
                                   ProjectileTrailDepthMode depth) noexcept {
    const size_t blendIndex = std::min<size_t>(static_cast<size_t>(blend), 3u);
    const size_t depthIndex = std::min<size_t>(static_cast<size_t>(depth), 2u);
    return blendIndex * 3u + depthIndex;
}

[[nodiscard]] float pointFreshness(uint64_t frame, uint64_t observed,
                                   float lifetimeSeconds,
                                   uint32_t logicFramesPerSecond) noexcept {
    if (frame <= observed) return 1.0f;
    if (!std::isfinite(lifetimeSeconds) || lifetimeSeconds <= 0.0f) return 0.0f;
    const float age = static_cast<float>(frame - observed) /
        static_cast<float>(std::max<uint32_t>(1u, logicFramesPerSecond));
    return std::clamp(1.0f - age / lifetimeSeconds, 0.0f, 1.0f);
}

[[nodiscard]] container::Array<float, 4> endpointColor(
    const ProjectileRenderSnapshot& descriptor, float freshness) noexcept {
    const float alpha = std::clamp(descriptor.trailAlpha, 0.0f, 4.0f);
    container::Array<float, 4> output{
        std::clamp(descriptor.trailColor.x(), 0.0f, 4.0f),
        std::clamp(descriptor.trailColor.y(), 0.0f, 4.0f),
        std::clamp(descriptor.trailColor.z(), 0.0f, 4.0f),
        std::clamp(freshness * alpha, 0.0f, 1.0f),
    };
    if (descriptor.trailBlend == ProjectileTrailBlendMode::Additive) {
        const float scale = freshness * alpha;
        output[0] *= scale;
        output[1] *= scale;
        output[2] *= scale;
    } else if (descriptor.trailBlend == ProjectileTrailBlendMode::Multiply) {
        const float scale = std::clamp(freshness * alpha, 0.0f, 1.0f);
        output[0] = 1.0f + (output[0] - 1.0f) * scale;
        output[1] = 1.0f + (output[1] - 1.0f) * scale;
        output[2] = 1.0f + (output[2] - 1.0f) * scale;
        output[3] = 1.0f;
    } else if (descriptor.trailBlend == ProjectileTrailBlendMode::Opaque) {
        output[3] = 1.0f;
    }
    return output;
}

} // namespace

ProjectileTrailRenderer::ProjectileTrailRenderer(
    d3d12::D3D12Device& device,
    container::SharedPtr<WorldTextureCache> textures) {
    static_cast<void>(init(device, std::move(textures)));
}

ProjectileTrailRenderer::~ProjectileTrailRenderer() {
    shutdown();
}

bool ProjectileTrailRenderer::init(
    d3d12::D3D12Device& device,
    container::SharedPtr<WorldTextureCache> textures) {
    shutdown();
    m_device = &device;
    m_textures = std::move(textures);
    if (!m_textures || !createRootSignature() || !createPipelineStates()) {
        shutdown();
        return false;
    }
    m_initialized = true;
    TD_LOG_INFO(
        "[ProjectileTrailRenderer] Initialized (streams={}, points/stream={}, PSOs={})",
        kMaximumProjectileStreams, kMaximumPointsPerStream, kPipelineCount);
    return true;
}

void ProjectileTrailRenderer::shutdown() {
    reset();
    resetTextureCache();
    for (auto& pipeline : m_pipelineStates) pipeline.Reset();
    m_rootSignature.Reset();
    m_textures.reset();
    m_device = nullptr;
    m_initialized = false;
}

void ProjectileTrailRenderer::reset() {
    m_streams.clear();
    m_stats = {};
    m_lastSimulationFrame = 0;
    m_trailHighWater = 0;
}

void ProjectileTrailRenderer::resetTextureCache() {
    if (m_textures) {
        for (const auto& [name, ignored] : m_textureSrvs) {
            static_cast<void>(ignored);
            m_textures->release(name);
        }
    }
    m_textureSrvs.clear();
    ++m_textureBindingGeneration;
    if (m_textureBindingGeneration == 0) ++m_textureBindingGeneration;
}

bool ProjectileTrailRenderer::configureTextureSampling(
    uint32_t textureFilter, uint32_t anisotropyLevel,
    uint32_t sampleCount) {
    const d3d12::TextureSamplingQuality current =
        d3d12::textureSamplingQuality(
            m_textureFilter, m_anisotropyLevel);
    const d3d12::TextureSamplingQuality requested =
        d3d12::textureSamplingQuality(textureFilter, anisotropyLevel);
    m_textureFilter = textureFilter;
    m_anisotropyLevel = anisotropyLevel;
    const uint32_t previousSampleCount = m_sampleCount;
    m_sampleCount = sampleCount;
    if (!m_initialized ||
        (current.filter == requested.filter &&
         current.maximumAnisotropy == requested.maximumAnisotropy &&
         current.maximumLod == requested.maximumLod &&
         previousSampleCount == sampleCount)) {
        return true;
    }
    for (auto& pipeline : m_pipelineStates) pipeline.Reset();
    m_rootSignature.Reset();
    return createRootSignature() && createPipelineStates();
}

void ProjectileTrailRenderer::observeProjectiles(
    container::Span<const PreparedProjectileRenderSnapshot> projectiles,
    uint64_t simulationFrame) {
    if (m_lastSimulationFrame != 0 && simulationFrame < m_lastSimulationFrame) {
        m_streams.clear();
    }
    m_lastSimulationFrame = simulationFrame;

    for (auto& [ignored, stream] : m_streams) {
        static_cast<void>(ignored);
        for (StreamPoint& point : stream.points) {
            point.observedThisFrame = false;
        }
    }

    for (const PreparedProjectileRenderSnapshot& prepared : projectiles) {
        const ProjectileRenderSnapshot& projectile = prepared.projectile;
        if (!projectile.trailEnabled || projectile.objectId == 0 ||
            projectile.launcherId == 0 || projectile.trailStreamName.empty() ||
            projectile.trailTexture.empty() || !finiteVector(projectile.position) ||
            !std::isfinite(projectile.trailWidth) || projectile.trailWidth <= 0.0f) {
            continue;
        }

        const container::String key = streamKey(projectile);
        auto found = m_streams.find(key);
        if (found == m_streams.end()) {
            if (m_streams.size() >= kMaximumProjectileStreams) {
                ++m_stats.rejectedTrails;
                continue;
            }
            found = m_streams.emplace(key, StreamState{}).first;
            m_trailHighWater = std::max<uint32_t>(
                m_trailHighWater, static_cast<uint32_t>(m_streams.size()));
        }
        StreamState& stream = found->second;
        stream.descriptor = projectile;
        auto point = std::find_if(
            stream.points.begin(), stream.points.end(),
            [&projectile](const StreamPoint& candidate) {
                return candidate.objectId == projectile.objectId;
            });
        if (point == stream.points.end()) {
            if (stream.points.size() >= kMaximumPointsPerStream) {
                const auto oldest = std::min_element(
                    stream.points.begin(), stream.points.end(),
                    [](const StreamPoint& left, const StreamPoint& right) {
                        return std::tie(left.sourceShotSequence, left.spawnedTick,
                                        left.objectId) <
                               std::tie(right.sourceShotSequence, right.spawnedTick,
                                        right.objectId);
                    });
                if (oldest != stream.points.end()) stream.points.erase(oldest);
                ++m_stats.rejectedSegments;
            }
            stream.points.push_back({});
            point = std::prev(stream.points.end());
        }
        *point = {
            .objectId = projectile.objectId,
            .intendedTargetId = projectile.intendedTargetId,
            .sourceShotSequence = projectile.sourceShotSequence,
            .spawnedTick = projectile.spawnedTick,
            .lastObservedSimulationFrame = simulationFrame,
            .position = projectile.position,
            .visibilityExempt = projectile.visibilityExempt,
            .observedThisFrame = true,
        };
    }

    for (auto stream = m_streams.begin(); stream != m_streams.end();) {
        const float lifetime = stream->second.descriptor.trailLifetimeSeconds;
        const uint32_t logicFramesPerSecond =
            stream->second.descriptor.trailLogicFramesPerSecond;
        const bool anyObserved = std::any_of(
            stream->second.points.begin(), stream->second.points.end(),
            [](const StreamPoint& point) {
                return point.observedThisFrame;
            });
        const bool allExpired = std::all_of(
            stream->second.points.begin(), stream->second.points.end(),
            [simulationFrame, lifetime,
             logicFramesPerSecond](const StreamPoint& point) {
                return pointFreshness(simulationFrame,
                                      point.lastObservedSimulationFrame,
                                      lifetime, logicFramesPerSecond) <= 0.0f;
            });
        if (stream->second.points.empty() || (!anyObserved && allExpired)) {
            stream = m_streams.erase(stream);
        } else {
            ++stream;
        }
    }
}

ProjectileTrailRenderDrawList ProjectileTrailRenderer::buildDrawList(
    container::Span<const PreparedProjectileRenderSnapshot> projectiles,
    const RenderCameraSnapshot& camera,
    uint64_t simulationFrame,
    const LocalVisibilityRenderSnapshot& localVisibility) {
    ProjectileTrailRenderDrawList output;
    buildDrawListIntoRetained(
        output, projectiles, camera, simulationFrame, localVisibility);
    return output;
}

void ProjectileTrailRenderer::buildDrawListIntoRetained(
    ProjectileTrailRenderDrawList& output,
    container::Span<const PreparedProjectileRenderSnapshot> projectiles,
    const RenderCameraSnapshot& camera,
    uint64_t simulationFrame,
    const LocalVisibilityRenderSnapshot& localVisibility) {
    observeProjectiles(projectiles, simulationFrame);
    const uint32_t rejectedTrails = m_stats.rejectedTrails;
    const uint32_t rejectedSegments = m_stats.rejectedSegments;

    output.textureBindingGeneration = 0;
    output.vertices.clear();
    output.batches.clear();
    output.streamCount = 0;
    output.pointCount = 0;
    output.segmentCount = 0;
    output.rejectedSegments = 0;
    auto& orderedStreams = m_orderedStreamsScratch;
    orderedStreams.clear();
    orderedStreams.reserve(m_streams.size());
    for (const auto& [key, stream] : m_streams) {
        orderedStreams.emplace_back(key, &stream);
    }
    std::sort(orderedStreams.begin(), orderedStreams.end(),
              [](const auto& left, const auto& right) {
                  return left.first < right.first;
              });

    for (const auto& [ignoredKey, streamPointer] : orderedStreams) {
        static_cast<void>(ignoredKey);
        const StreamState& stream = *streamPointer;
        auto& points = m_pointScratch;
        points.clear();
        points.reserve(stream.points.size());
        for (const StreamPoint& point : stream.points) points.push_back(&point);
        std::sort(points.begin(), points.end(),
                  [](const StreamPoint* left, const StreamPoint* right) {
                      return std::tie(left->sourceShotSequence, left->spawnedTick,
                                      left->objectId) <
                             std::tie(right->sourceShotSequence, right->spawnedTick,
                                      right->objectId);
                  });
        const uint32_t maximumPoints = stream.descriptor.trailMaximumSegments;
        if (maximumPoints != 0 && points.size() > maximumPoints) {
            points.erase(points.begin(), points.end() - maximumPoints);
        }
        if (points.empty()) continue;
        ++output.streamCount;
        output.pointCount += static_cast<uint32_t>(points.size());
        if (points.size() < 2 ||
            !stream.descriptor.trailOwnerAnchorVisible) {
            continue;
        }

        const float tileFactor = std::max(0.0f,
            std::isfinite(stream.descriptor.trailTileFactor)
                ? stream.descriptor.trailTileFactor : 1.0f);
        const float scroll = (std::isfinite(stream.descriptor.trailScrollRate)
                ? stream.descriptor.trailScrollRate : 0.0f) *
            (static_cast<float>(simulationFrame) /
             static_cast<float>(std::max<uint32_t>(
                 1u, stream.descriptor.trailLogicFramesPerSecond)));

        const auto appendRun =
            [&](container::Vector<ProjectileStreamJoinPoint>& run) {
                if (run.size() < 2u) {
                    run.clear();
                    return;
                }
                const size_t available = kMaximumRenderedSegments -
                    std::min<size_t>(output.segmentCount,
                                     kMaximumRenderedSegments);
                if (run.size() - 1u > available) {
                    output.rejectedSegments += static_cast<uint32_t>(
                        run.size() - 1u - available);
                    run.resize(available + 1u);
                }
                if (run.size() < 2u) {
                    run.clear();
                    return;
                }
                buildProjectileStreamJoinMeshInto(
                    m_joinMeshScratch, run, camera.position,
                    std::clamp(stream.descriptor.trailWidth,
                               0.01f, 32.0f));
                const ProjectileStreamJoinMesh& mesh = m_joinMeshScratch;
                if (mesh.logicalSegmentCount == 0u ||
                    mesh.vertices.empty()) {
                    run.clear();
                    return;
                }
                if (output.batches.empty() ||
                    output.batches.back().texture !=
                        stream.descriptor.trailTexture ||
                    output.batches.back().blend !=
                        stream.descriptor.trailBlend ||
                    output.batches.back().depth !=
                        stream.descriptor.trailDepth) {
                    output.batches.push_back({
                        .texture = stream.descriptor.trailTexture,
                        .blend = stream.descriptor.trailBlend,
                        .depth = stream.descriptor.trailDepth,
                        .firstVertex = static_cast<uint32_t>(
                            output.vertices.size()),
                    });
                }
                for (const ProjectileStreamJoinVertex& source :
                     mesh.vertices) {
                    ProjectileTrailRenderVertex vertex;
                    vertex.position[0] = source.worldPosition.x();
                    vertex.position[1] = source.worldPosition.y();
                    vertex.position[2] = source.worldPosition.z();
                    std::copy(source.color.begin(), source.color.end(),
                              vertex.color);
                    vertex.uv[0] = source.textureU;
                    vertex.uv[1] = source.textureV;
                    output.vertices.push_back(vertex);
                }
                output.batches.back().vertexCount +=
                    static_cast<uint32_t>(mesh.vertices.size());
                output.batches.back().segmentCount +=
                    mesh.logicalSegmentCount;
                output.segmentCount += mesh.logicalSegmentCount;
                run.clear();
            };

        auto& run = m_runScratch;
        run.clear();
        run.reserve(points.size());
        for (size_t ordinal = 0; ordinal < points.size(); ++ordinal) {
            const StreamPoint& point = *points[ordinal];
            const float freshness = pointFreshness(
                simulationFrame, point.lastObservedSimulationFrame,
                stream.descriptor.trailLifetimeSeconds,
                stream.descriptor.trailLogicFramesPerSecond);
            const bool renderable = finiteVector(point.position) &&
                freshness > 0.0f &&
                localVisibility.isInsidePlayableBounds(point.position);
            const bool connected = renderable &&
                (run.empty() ||
                 points[ordinal - 1u]->intendedTargetId ==
                     point.intendedTargetId);
            if (!connected) appendRun(run);
            if (!renderable) {
                continue;
            }
            run.push_back({
                .worldPosition = point.position,
                .color = endpointColor(stream.descriptor, freshness),
                .textureV = scroll +
                    static_cast<float>(ordinal) * tileFactor,
            });
        }
        appendRun(run);
    }

    m_stats = {
        .activeTrails = output.streamCount,
        .activePoints = output.pointCount,
        .renderedSegments = output.segmentCount,
        .rejectedTrails = rejectedTrails,
        .rejectedSegments = rejectedSegments + output.rejectedSegments,
        .trailHighWater = m_trailHighWater,
        .cachedTextures = static_cast<uint32_t>(m_textureSrvs.size()),
    };
    m_orderedStreamsScratch.clear();
    m_pointScratch.clear();
    m_runScratch.clear();
}

void ProjectileTrailRenderer::prepareTextureBindings(
    ProjectileTrailRenderDrawList& drawList) {
    for (ProjectileTrailRenderBatch& batch : drawList.batches) {
        batch.textureSrvIndex = textureSrv(batch.texture);
    }
    drawList.textureBindingGeneration = m_textureBindingGeneration;
}

size_t ProjectileTrailRenderer::render(
    const ProjectileTrailRenderDrawList& drawList,
    const RenderCameraSnapshot& camera,
    const LocalVisibilityRenderSnapshot& localVisibility) {
    m_stats.drawCalls = 0;
    // configureTextureSampling resets the root signature and every pipeline
    // state and can fail to recreate them; it returns false but leaves
    // m_initialized true, and its caller only logs.  Without these checks the
    // next frame binds a null root signature and sets root parameters against
    // it, which is invalid D3D12 usage even though the per-batch PSO checks
    // stop the draws themselves.
    if (!m_initialized || !m_device || !m_device->commandList() ||
        !m_rootSignature ||
        std::any_of(m_pipelineStates.begin(), m_pipelineStates.end(),
                    [](const auto& pipeline) { return !pipeline; }) ||
        m_device->width() == 0 || m_device->height() == 0 ||
        drawList.vertices.empty()) {
        return 0;
    }

    const WorldCamera worldCamera = WorldCamera::fromSnapshot(camera);
    const float aspect = static_cast<float>(m_device->width()) /
        static_cast<float>(m_device->height());
    const math::float4x4 viewProjection = worldCamera.viewProjectionMatrix(aspect);
    TrailCameraConstants constants{};
    std::memcpy(constants.viewProjection, &viewProjection.m,
                sizeof(constants.viewProjection));
    if (localVisibility.hasPlayableBounds()) {
        constants.playableMinimum[0] =
            localVisibility.playableMinimum.x();
        constants.playableMinimum[1] =
            localVisibility.playableMinimum.y();
        constants.playableMaximum[0] =
            localVisibility.playableMaximum.x();
        constants.playableMaximum[1] =
            localVisibility.playableMaximum.y();
        constants.playableBoundsEnabled = 1u;
    }
    const d3d12::ConstantBufferAllocation cameraAllocation =
        m_device->allocateConstantBuffer(&constants, sizeof(constants));
    const size_t vertexBytes64 =
        drawList.vertices.size() * sizeof(ProjectileTrailRenderVertex);
    if (!cameraAllocation || vertexBytes64 > std::numeric_limits<uint32_t>::max()) {
        return 0;
    }
    const uint32_t vertexBytes = static_cast<uint32_t>(vertexBytes64);
    const d3d12::FrameUploadAllocation vertexAllocation =
        m_device->allocateFrameUpload(drawList.vertices.data(), vertexBytes,
                                      alignof(ProjectileTrailRenderVertex));
    if (!vertexAllocation) return 0;

    m_device->flushBatch();
    ID3D12GraphicsCommandList* commandList = m_device->commandList();
    const uint32_t tacticalHeight =
        worldCamera.tacticalViewportHeight(m_device->height());
    const D3D12_VIEWPORT viewport{
        0.0f, 0.0f, static_cast<float>(m_device->width()),
        static_cast<float>(tacticalHeight), 0.0f, 1.0f};
    const D3D12_RECT scissor{
        0, 0, static_cast<LONG>(m_device->width()),
        static_cast<LONG>(tacticalHeight)};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    m_device->bindSrvHeap();
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_device->recordGraphicsRootSignatureCall();
    commandList->SetGraphicsRootConstantBufferView(0, cameraAllocation.gpuAddress);
    const D3D12_VERTEX_BUFFER_VIEW vertexView{
        .BufferLocation = vertexAllocation.gpuAddress,
        .SizeInBytes = vertexBytes,
        .StrideInBytes = sizeof(ProjectileTrailRenderVertex),
    };
    commandList->IASetVertexBuffers(0, 1, &vertexView);
    m_device->recordVertexBufferCall();
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D12PipelineState* activePipeline = nullptr;
    size_t renderedSegments = 0;
    for (const ProjectileTrailRenderBatch& batch : drawList.batches) {
        if (batch.vertexCount == 0 ||
            static_cast<uint64_t>(batch.firstVertex) + batch.vertexCount >
                drawList.vertices.size()) {
            continue;
        }
        const size_t index = pipelineIndex(batch.blend, batch.depth);
        if (index >= m_pipelineStates.size() || !m_pipelineStates[index]) continue;
        if (activePipeline != m_pipelineStates[index].Get()) {
            activePipeline = m_pipelineStates[index].Get();
            commandList->SetPipelineState(activePipeline);
            m_device->recordPipelineStateCall();
        }
        const uint32_t textureSrvIndex =
            drawList.textureBindingGeneration == m_textureBindingGeneration
            ? batch.textureSrvIndex : 0;
        commandList->SetGraphicsRootDescriptorTable(
            1, m_device->getSrvGpuHandle(textureSrvIndex));
        m_device->recordGraphicsDescriptorTableCall();
        commandList->DrawInstanced(batch.vertexCount, 1, batch.firstVertex, 0);
        m_device->recordDrawCall();
        renderedSegments += batch.segmentCount;
        ++m_stats.drawCalls;
    }
    m_stats.cachedTextures = static_cast<uint32_t>(m_textureSrvs.size());
    return renderedSegments;
}

size_t ProjectileTrailRenderer::render(
    container::Span<const PreparedProjectileRenderSnapshot> projectiles,
    const RenderCameraSnapshot& camera,
    uint64_t simulationFrame,
    float deltaSeconds,
    const LocalVisibilityRenderSnapshot& localVisibility) {
    static_cast<void>(deltaSeconds);
    buildDrawListIntoRetained(
        m_retainedDrawList, projectiles, camera, simulationFrame,
        localVisibility);
    prepareTextureBindings(m_retainedDrawList);
    return render(m_retainedDrawList, camera, localVisibility);
}

bool ProjectileTrailRenderer::createRootSignature() {
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
    // b0 contains both the VS camera matrix and the playable bounds consumed
    // by the PS, so restricting it to the vertex stage invalidates the PSO.
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
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxAnisotropy = sampling.maximumAnisotropy;
    sampler.MaxLOD = sampling.maximumLod;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(std::size(parameters));
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
            "[ProjectileTrailRenderer] root signature serialization failed: 0x{:08X}",
            static_cast<uint32_t>(serializeResult));
        return false;
    }
    const HRESULT createResult = m_device->getDevice()->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature));
    if (FAILED(createResult)) {
        TD_LOG_ERROR(
            "[ProjectileTrailRenderer] root signature creation failed: 0x{:08X}",
            static_cast<uint32_t>(createResult));
        return false;
    }
    return true;
}

bool ProjectileTrailRenderer::createPipelineStates() {
    if (!m_device || !m_device->getDevice() || !m_rootSignature) return false;
    const container::String packageVersion = std::to_string(
        TD_PROJECTILE_TRAIL_SHADER_PACKAGE_VERSION);
    constexpr d3d12::ShaderPackageEntrySpec entries[] = {
        {
            .fileManifestKey = "vertex_file",
            .expectedFile = "projectile_trail_vs.cso",
            .profileManifestKey = "vertex_profile",
            .expectedProfile = "vs_5_0",
        },
        {
            .fileManifestKey = "pixel_file",
            .expectedFile = "projectile_trail_ps.cso",
            .profileManifestKey = "pixel_profile",
            .expectedProfile = "ps_5_0",
        },
    };
    container::Vector<container::Vector<uint8_t>> shaders;
    if (!d3d12::loadShaderPackage(
            "projectile_trail", packageVersion,
            TD_PROJECTILE_TRAIL_SHADER_SOURCE_SHA256, entries, shaders) ||
        shaders.size() != std::size(entries)) {
        TD_LOG_ERROR(
            "[ProjectileTrailRenderer] precompiled shader package unavailable");
        return false;
    }

    static_assert(std::is_standard_layout_v<ProjectileTrailRenderVertex>);
    static_assert(offsetof(ProjectileTrailRenderVertex, position) == 0);
    static_assert(offsetof(ProjectileTrailRenderVertex, color) == 12);
    static_assert(offsetof(ProjectileTrailRenderVertex, uv) == 28);
    static_assert(sizeof(ProjectileTrailRenderVertex) == 36);
    const D3D12_INPUT_ELEMENT_DESC input[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
         static_cast<UINT>(offsetof(ProjectileTrailRenderVertex, position)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
         static_cast<UINT>(offsetof(ProjectileTrailRenderVertex, color)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
         static_cast<UINT>(offsetof(ProjectileTrailRenderVertex, uv)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = m_rootSignature.Get();
    description.VS = {shaders[0].data(), shaders[0].size()};
    description.PS = {shaders[1].data(), shaders[1].size()};
    description.InputLayout = {input, static_cast<UINT>(std::size(input))};
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.SampleMask = UINT_MAX;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = d3d12::D3D12Device::SWAP_FORMAT;
    description.DSVFormat = d3d12::D3D12Device::DEPTH_FORMAT;
    description.SampleDesc.Count = m_sampleCount;

    for (size_t blendIndex = 0; blendIndex < 4; ++blendIndex) {
        const auto blendMode = static_cast<ProjectileTrailBlendMode>(blendIndex);
        for (size_t depthIndex = 0; depthIndex < 3; ++depthIndex) {
            const auto depthMode = static_cast<ProjectileTrailDepthMode>(depthIndex);
            D3D12_RENDER_TARGET_BLEND_DESC blend{};
            blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            switch (blendMode) {
            case ProjectileTrailBlendMode::Additive:
                blend.BlendEnable = TRUE;
                blend.SrcBlend = D3D12_BLEND_ONE;
                blend.DestBlend = D3D12_BLEND_ONE;
                blend.BlendOp = D3D12_BLEND_OP_ADD;
                blend.SrcBlendAlpha = D3D12_BLEND_ONE;
                blend.DestBlendAlpha = D3D12_BLEND_ONE;
                blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                break;
            case ProjectileTrailBlendMode::Alpha:
                blend.BlendEnable = TRUE;
                blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
                blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                blend.BlendOp = D3D12_BLEND_OP_ADD;
                blend.SrcBlendAlpha = D3D12_BLEND_ONE;
                blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                break;
            case ProjectileTrailBlendMode::Multiply:
                blend.BlendEnable = TRUE;
                blend.SrcBlend = D3D12_BLEND_ZERO;
                blend.DestBlend = D3D12_BLEND_SRC_COLOR;
                blend.BlendOp = D3D12_BLEND_OP_ADD;
                blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
                blend.DestBlendAlpha = D3D12_BLEND_ONE;
                blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                break;
            case ProjectileTrailBlendMode::Opaque:
                blend.BlendEnable = FALSE;
                blend.SrcBlend = D3D12_BLEND_ONE;
                blend.DestBlend = D3D12_BLEND_ZERO;
                blend.BlendOp = D3D12_BLEND_OP_ADD;
                blend.SrcBlendAlpha = D3D12_BLEND_ONE;
                blend.DestBlendAlpha = D3D12_BLEND_ZERO;
                blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                break;
            }
            description.BlendState.RenderTarget[0] = blend;
            description.DepthStencilState.DepthEnable =
                depthMode == ProjectileTrailDepthMode::Disabled ? FALSE : TRUE;
            description.DepthStencilState.DepthWriteMask =
                depthMode == ProjectileTrailDepthMode::TestWrite
                    ? D3D12_DEPTH_WRITE_MASK_ALL
                    : D3D12_DEPTH_WRITE_MASK_ZERO;
            description.DepthStencilState.DepthFunc =
                D3D12_COMPARISON_FUNC_LESS_EQUAL;
            const size_t index = pipelineIndex(blendMode, depthMode);
            const HRESULT result =
                m_device->getDevice()->CreateGraphicsPipelineState(
                    &description, IID_PPV_ARGS(&m_pipelineStates[index]));
            if (FAILED(result)) {
                TD_LOG_ERROR(
                    "[ProjectileTrailRenderer] PSO blend={} depth={} failed: 0x{:08X}",
                    blendIndex, depthIndex, static_cast<uint32_t>(result));
                return false;
            }
        }
    }
    return true;
}

uint32_t ProjectileTrailRenderer::textureSrv(container::StringView textureName) {
    if (!m_device || !m_textures || textureName.empty()) return 0;
    const container::String key(textureName);
    if (const auto found = m_textureSrvs.find(key);
        found != m_textureSrvs.end()) {
        return found->second;
    }
    const std::optional<uint32_t> acquired = m_textures->acquire(textureName);
    if (!acquired) return 0;
    m_textureSrvs.emplace(key, *acquired);
    return *acquired;
}

} // namespace engine::render
