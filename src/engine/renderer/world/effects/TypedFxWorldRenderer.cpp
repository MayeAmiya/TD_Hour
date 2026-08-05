#include "engine/renderer/world/effects/TypedFxWorldRenderer.h"

#include "engine/renderer/d3d12/runtime/D3D12ShaderPackage.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace engine::render {
namespace {

#ifndef TD_TYPED_FX_WORLD_SHADER_PACKAGE_VERSION
#define TD_TYPED_FX_WORLD_SHADER_PACKAGE_VERSION 1
#endif

#ifndef TD_TYPED_FX_WORLD_SHADER_SOURCE_SHA256
#define TD_TYPED_FX_WORLD_SHADER_SOURCE_SHA256 \
    "0000000000000000000000000000000000000000000000000000000000000000"
#endif

constexpr size_t kMaximumTypedFxWorldEffects = 4096;
constexpr size_t kMaximumTypedFxWorldVertices = 64 * 1024;

struct alignas(16) TypedFxCameraConstants final {
    float viewProjection[16];
    float playableMinimum[2]{};
    float playableMaximum[2]{};
    uint32_t playableBoundsEnabled = 0;
    uint32_t padding[3]{};
};
static_assert(sizeof(TypedFxCameraConstants) == 96u);

[[nodiscard]] RenderVector typedFxPosition(
    const fx::FxTypedAnchor& anchor) noexcept {
    const fx::ParticleVector3 value = fx::worldTransform(anchor).position;
    return {value.x, value.y, value.z};
}

[[nodiscard]] bool finiteTypedFxVector(const RenderVector& value) noexcept {
    return std::isfinite(value.x()) && std::isfinite(value.y()) &&
        std::isfinite(value.z());
}

[[nodiscard]] RenderVector typedFxColor(fx::ParticleColor color) noexcept {
    constexpr float inverseByte = 1.0f / 255.0f;
    return {
        static_cast<float>(color.red) * inverseByte,
        static_cast<float>(color.green) * inverseByte,
        static_cast<float>(color.blue) * inverseByte,
    };
}

} // namespace

TypedFxWorldRenderer::TypedFxWorldRenderer(
    d3d12::D3D12Device& device,
    container::SharedPtr<WorldTextureCache> textures)
    : m_textures(std::move(textures)) {
    static_cast<void>(init(device));
}

TypedFxWorldRenderer::~TypedFxWorldRenderer() { shutdown(); }

bool TypedFxWorldRenderer::init(d3d12::D3D12Device& device) {
    shutdown();
    m_device = &device;
    if (!loadShaderPackage() || !createRootSignature() ||
        !createPipelineState()) {
        shutdown();
        return false;
    }
    m_initialized = true;
    TD_LOG_INFO("[TypedFxWorld] Initialized (effect budget={})",
                kMaximumTypedFxWorldEffects);
    return true;
}

void TypedFxWorldRenderer::shutdown() {
    reset();
    releaseTextures();
    m_pipelineState.Reset();
    m_rootSignature.Reset();
    for (auto& bytecode : m_shaderBytecode) bytecode.clear();
    m_device = nullptr;
    m_initialized = false;
}

void TypedFxWorldRenderer::resetTextureCache() { releaseTextures(); }

bool TypedFxWorldRenderer::configureSampleCount(uint32_t sampleCount) {
    sampleCount = std::max(sampleCount, 1u);
    if (m_sampleCount == sampleCount) return true;
    m_sampleCount = sampleCount;
    if (!m_initialized) return true;
    m_pipelineState.Reset();
    m_rootSignature.Reset();
    return createRootSignature() && createPipelineState();
}

void TypedFxWorldRenderer::reset() {
    m_beams.clear();
    m_modelRays.clear();
    m_reportedModelRayFailures.clear();
    m_vertices.clear();
    m_batches.clear();
    m_stats = {};
}

[[nodiscard]] const TypedFxWorldRenderer::Stats& TypedFxWorldRenderer::stats() const noexcept { return m_stats; }

void TypedFxWorldRenderer::submit(const fx::FxPresentationCommandBatch& commands) {
    const uint32_t rejectedBefore = m_stats.rejectedCommands;
    const auto hasCapacity = [this]() noexcept {
        return m_beams.size() + m_modelRays.size() <
            kMaximumTypedFxWorldEffects;
    };
    for (const fx::FxRayCommand& command : commands.rays) {
        if (!hasCapacity() || !command.templateResolved ||
            command.descriptor.kind !=
                fx::LegacyBeamTemplateKind::ModelRay) {
            ++m_stats.rejectedCommands;
            continue;
        }
        const fx::ParticleVector3 start =
            fx::worldTransform(command.primary).position;
        const fx::ParticleVector3 end =
            fx::worldTransform(command.secondary).position;
        std::optional<fx::LegacyModelRayState> state =
            fx::makeLegacyModelRay(
                command.descriptor, start, end,
                command.identity.variationSeed != 0
                    ? command.identity.variationSeed
                    : command.identity.eventId);
        if (!state) {
            ++m_stats.rejectedCommands;
            continue;
        }
        m_modelRays.push_back({
            .state = std::move(*state),
            .admittedFrame = command.identity.confirmedFrame,
        });
    }
    for (const fx::FxLaserCommand& command : commands.lasers) {
        const uint64_t laserIdentity = command.beamIdentity != 0
            ? command.beamIdentity : command.identity.eventId;
        const auto existing = std::find_if(
            m_beams.begin(), m_beams.end(),
            [laserIdentity](const Beam& beam) {
                return laserIdentity != 0 && beam.laser &&
                    beam.laserIdentity == laserIdentity;
            });
        if (command.control ==
                fx::FxPresentationDirectBeam::Control::End) {
            if (existing != m_beams.end()) m_beams.erase(existing);
            continue;
        }
        if (command.control ==
                fx::FxPresentationDirectBeam::Control::Update &&
            existing != m_beams.end()) {
            existing->startAnchor = command.primary;
            existing->endAnchor = command.secondary;
            existing->laser = command.descriptor;
            existing->targetAttachmentWasAlive = false;
            existing->punchThroughApplied = false;
            applyLaserRadiusCommand(*existing, command);
            continue;
        }
        // Update contains the complete immutable descriptor and can
        // reconstruct a Begin hidden by shroud or a lost device epoch.
        if (existing != m_beams.end()) m_beams.erase(existing);
        if (!hasCapacity()) {
            ++m_stats.rejectedCommands;
            continue;
        }
        appendLaser(command);
    }
    for (const fx::FxRopeCommand& command : commands.ropes) {
        const auto existing = std::find_if(
            m_beams.begin(), m_beams.end(), [&command](const Beam& beam) {
                return beam.rope &&
                    beam.ropeIdentity == command.rope.ropeIdentity;
            });
        if (command.rope.control ==
                fx::FxPresentationRopeControl::End) {
            if (existing != m_beams.end()) m_beams.erase(existing);
            continue;
        }
        if (command.rope.control ==
                fx::FxPresentationRopeControl::Update) {
            if (existing != m_beams.end() &&
                existing->lastRopeFrame != 0 &&
                command.identity.confirmedFrame >
                    existing->lastRopeFrame) {
                fx::advanceLegacyRope(
                    *existing->rope,
                    static_cast<uint32_t>(std::min<uint64_t>(
                        command.identity.confirmedFrame -
                            existing->lastRopeFrame,
                        std::numeric_limits<uint32_t>::max())));
            }
            if (existing != m_beams.end()) {
                existing->startAnchor = command.anchor;
                fx::setLegacyRopeLength(
                    *existing->rope, command.rope.currentLength);
                fx::setLegacyRopeSpeed(
                    *existing->rope, command.rope.currentSpeedPerFrame,
                    command.rope.maximumSpeedPerFrame,
                    command.rope.accelerationPerFrame);
                existing->rope->wobblePhase = command.rope.wobblePhase;
                existing->rope->verticalOffset = command.rope.verticalOffset;
                existing->lastRopeFrame = command.identity.confirmedFrame;
                continue;
            }
            // A Begin may have been locally hidden by shroud. Update
            // carries the full W3DRopeDraw value, so the first visible
            // update is also a valid reconstruction boundary.
        }
        if (existing != m_beams.end()) m_beams.erase(existing);
        if (!hasCapacity()) {
            ++m_stats.rejectedCommands;
            continue;
        }
        std::optional<fx::LegacyRopeState> rope = fx::makeLegacyRope(
            {typedFxPosition(command.anchor).x(),
             typedFxPosition(command.anchor).y(),
             typedFxPosition(command.anchor).z()},
            command.rope.maximumLength, command.rope.width,
            {command.rope.color.x, command.rope.color.y,
             command.rope.color.z, 1.0f},
            command.rope.wobbleLength, command.rope.wobbleAmplitude,
            command.rope.wobbleRatePerFrame,
            command.rope.ropeIdentity);
        if (!rope) {
            ++m_stats.rejectedCommands;
            continue;
        }
        fx::setLegacyRopeLength(*rope, command.rope.currentLength);
        fx::setLegacyRopeSpeed(
            *rope, command.rope.currentSpeedPerFrame,
            command.rope.maximumSpeedPerFrame,
            command.rope.accelerationPerFrame);
        rope->wobblePhase = command.rope.wobblePhase;
        rope->verticalOffset = command.rope.verticalOffset;
        m_beams.push_back({
            .startAnchor = command.anchor,
            .rope = std::move(*rope),
            .ropeIdentity = command.rope.ropeIdentity,
            .lastRopeFrame = command.identity.confirmedFrame,
        });
    }
    for (const fx::FxTracerCommand& command : commands.tracers) {
        if (!hasCapacity()) {
            ++m_stats.rejectedCommands;
            continue;
        }
        const std::optional<render::TypedFxTracerState> tracer =
            render::makeTypedFxTracer(
                typedFxPosition(command.primary),
                typedFxPosition(command.secondary),
                typedFxColor(command.color), command.speed,
                command.decayAt, command.length, command.width);
        if (!tracer) {
            ++m_stats.rejectedCommands;
            continue;
        }
        m_beams.push_back({.tracer = *tracer});
    }
    m_stats.highWaterEffects = std::max<uint32_t>(
        m_stats.highWaterEffects,
        static_cast<uint32_t>(std::min<size_t>(
            m_beams.size() + m_modelRays.size(),
            std::numeric_limits<uint32_t>::max())));
    if (m_stats.rejectedCommands != rejectedBefore &&
        (rejectedBefore == 0 ||
         m_stats.rejectedCommands / 1024u != rejectedBefore / 1024u)) {
        TD_LOG_WARN(
            "[TypedFxWorld] rejected {} beam/rope commands (total={})",
            m_stats.rejectedCommands - rejectedBefore,
            m_stats.rejectedCommands);
    }
}

[[nodiscard]] size_t TypedFxWorldRenderer::render(const RenderCameraSnapshot& camera,
                            float deltaSeconds,
                            const TerrainRenderSnapshot* terrain,
                            const fx::FxRuntime* fxRuntime,
                            uint64_t simulationFrame,
                            const LocalVisibilityRenderSnapshot&
                                localVisibility) {
    const uint32_t rejected = m_stats.rejectedCommands;
    const uint32_t highWater = m_stats.highWaterEffects;
    const uint32_t activeModelRays = m_stats.activeModelRays;
    const uint32_t renderedModelPackets = m_stats.renderedModelPackets;
    m_stats = {.activeModelRays = activeModelRays,
               .renderedModelPackets = renderedModelPackets,
               .rejectedCommands = rejected,
               .highWaterEffects = highWater};
    // Require the pipeline objects, not just m_initialized: configureSampleCount
    // resets both and can fail to recreate them (device stress/removal) while
    // returning false and leaving m_initialized true.  Its caller only logs, so
    // without this check the next frame records SetGraphicsRootSignature(nullptr)
    // and SetPipelineState(nullptr) — invalid D3D12 usage that can escalate a
    // recoverable PSO-rebuild failure into device removal.
    if (!m_initialized || !m_device || !m_device->commandList() ||
        !m_rootSignature || !m_pipelineState ||
        m_device->width() == 0 || m_device->height() == 0) {
        return 0;
    }
    age(deltaSeconds, simulationFrame);
    buildVertices(camera, terrain, fxRuntime, simulationFrame);
    m_stats.activeBeams = static_cast<uint32_t>(m_beams.size());
    if (m_vertices.empty()) return 0;

    const WorldCamera worldCamera = WorldCamera::fromSnapshot(camera);
    const float aspect = static_cast<float>(m_device->width()) /
        static_cast<float>(m_device->height());
    const math::float4x4 viewProjection =
        worldCamera.viewProjectionMatrix(aspect);
    TypedFxCameraConstants constants{};
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
    const size_t bytes64 = m_vertices.size() * sizeof(Vertex);
    if (!cameraAllocation ||
        bytes64 > std::numeric_limits<uint32_t>::max()) {
        return 0;
    }
    const uint32_t bytes = static_cast<uint32_t>(bytes64);
    const d3d12::FrameUploadAllocation vertexAllocation =
        m_device->allocateFrameUpload(
            m_vertices.data(), bytes, alignof(Vertex));
    if (!vertexAllocation) return 0;

    m_device->flushBatch();
    ID3D12GraphicsCommandList* commandList = m_device->commandList();
    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_device->width());
    const uint32_t tacticalHeight = std::clamp(
        static_cast<uint32_t>(static_cast<float>(m_device->height()) *
            camera.tacticalViewportHeightScale + 0.5f),
        1u, std::max(m_device->height(), 1u));
    viewport.Height = static_cast<float>(tacticalHeight);
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{
        0, 0, static_cast<LONG>(m_device->width()),
        static_cast<LONG>(tacticalHeight)};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    m_device->bindSrvHeap();
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_device->recordGraphicsRootSignatureCall();
    commandList->SetPipelineState(m_pipelineState.Get());
    m_device->recordPipelineStateCall();
    commandList->SetGraphicsRootConstantBufferView(
        0, cameraAllocation.gpuAddress);
    const D3D12_VERTEX_BUFFER_VIEW vertexView{
        .BufferLocation = vertexAllocation.gpuAddress,
        .SizeInBytes = bytes,
        .StrideInBytes = sizeof(Vertex),
    };
    commandList->IASetVertexBuffers(0, 1, &vertexView);
    m_device->recordVertexBufferCall();
    commandList->IASetPrimitiveTopology(
        D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (const DrawBatch& batch : m_batches) {
        commandList->SetGraphicsRootDescriptorTable(
            1, m_device->getSrvGpuHandle(batch.textureSrv));
        m_device->recordGraphicsDescriptorTableCall();
        commandList->DrawInstanced(
            batch.vertexCount, 1, batch.firstVertex, 0);
        m_device->recordDrawCall();
    }
    m_stats.renderedTriangles =
        static_cast<uint32_t>(m_vertices.size() / 3u);
    m_stats.drawCalls = static_cast<uint32_t>(m_batches.size());
    return m_stats.renderedTriangles;
}

[[nodiscard]] bool TypedFxWorldRenderer::isInitialized() const noexcept { return m_initialized; }

void TypedFxWorldRenderer::appendActiveBonePoseDemands(
    container::Vector<fx::FxBonePoseDemand>& output) const {
    const auto append = [&output](
        const std::optional<fx::FxTypedAnchor>& anchor) {
        if (!anchor) return;
        const auto* bone = std::get_if<fx::FxBoneAnchor>(&*anchor);
        if (!bone || bone->objectKey == 0 || bone->boneName.empty() ||
            output.size() >= fx::kMaximumFxBonePoseDemands) return;
        output.push_back({
            .objectKey = bone->objectKey,
            .boneName = bone->boneName,
        });
    };
    for (const Beam& beam : m_beams) {
        append(beam.startAnchor);
        append(beam.endAnchor);
    }
}

[[nodiscard]] size_t TypedFxWorldRenderer::appendModelRayDrawPackets(
    W3dAssetCache& assets,
    container::Vector<StaticMeshDrawPacket>& output,
    W3dRestPaletteFrameCache& restPalettes,
    float visualTimeSeconds, uint64_t simulationFrame,
    W3dModelGraphTraversalStats* traversalStats) {
    std::erase_if(m_modelRays, [simulationFrame](ModelRay& ray) {
        if (ray.admittedFrame == 0) ray.admittedFrame = simulationFrame;
        return !fx::legacyModelRayAlive(
            ray.state, ray.admittedFrame, simulationFrame);
    });

    const size_t packetStart = output.size();
    for (const ModelRay& ray : m_modelRays) {
        W3dAssetRequest modelRequest;
        modelRequest.source = ray.state.modelAsset;
        modelRequest.queueGpuUpload = true;
        modelRequest.gpuUploadPriority = W3dGpuUploadPriority::Visible;
        const W3dModelHandle handle = assets.requestAsync(modelRequest);
        if (!handle) continue;
        const std::optional<W3dAssetState> state = assets.state(handle);
        if (state == W3dAssetState::CpuReady) {
            assets.queueGpuUpload(handle, W3dGpuUploadPriority::Visible);
            continue;
        }
        if (state == W3dAssetState::GpuUploadFailed) {
            assets.queueGpuUpload(handle, W3dGpuUploadPriority::Visible);
        }
        if (state == W3dAssetState::Failed ||
            state == W3dAssetState::GpuUploadFailed) {
            if (m_reportedModelRayFailures.insert(
                    ray.state.modelAsset).second) {
                TD_LOG_ERROR(
                    "[TypedFxWorld] model RayEffect asset '{}' failed: {}",
                    ray.state.modelAsset, assets.error(handle));
            }
            continue;
        }
        if (state != W3dAssetState::GpuReady) continue;

        const auto model =
            std::dynamic_pointer_cast<const D3D12W3dModel>(
                assets.gpuModel(handle));
        if (!model || model->retired()) continue;
        const float scale = ray.state.assetScale;
        const math::transform world = math::transform::from_trs(
            {scale, scale, scale}, math::quat::identity(),
            {ray.state.position.x, ray.state.position.y,
             ray.state.position.z});
        const size_t modelPacketStart = output.size();
        static_cast<void>(appendW3dModelGraphDrawPackets(
            assets, handle, world, {}, {}, output,
            {.visualTimeSeconds = visualTimeSeconds,
             .restPalettes = &restPalettes},
            traversalStats));
        for (size_t index = modelPacketStart; index < output.size();
             ++index) {
            output[index].receivesMapBorder = true;
        }
        if (!ray.state.castsDirectionalShadow) {
            for (size_t index = modelPacketStart; index < output.size();
                 ++index) {
                output[index].castsShadow = false;
            }
        }
    }
    m_stats.activeModelRays = static_cast<uint32_t>(std::min<size_t>(
        m_modelRays.size(), std::numeric_limits<uint32_t>::max()));
    m_stats.renderedModelPackets = static_cast<uint32_t>(
        std::min<size_t>(output.size() - packetStart,
                         std::numeric_limits<uint32_t>::max()));
    return output.size() - packetStart;
}

void TypedFxWorldRenderer::appendLaser(const fx::FxLaserCommand& command) {
    const RenderVector start = typedFxPosition(command.primary);
    const RenderVector end = typedFxPosition(command.secondary);
    const RenderVector delta = end - start;
    if (!finiteTypedFxVector(start) || !finiteTypedFxVector(end) ||
        delta.length_sq() <= math::EPSILON * math::EPSILON ||
        command.descriptor.kind != fx::LegacyBeamTemplateKind::Laser) {
        ++m_stats.rejectedCommands;
        return;
    }
    const fx::LegacyLaserTemplate& descriptor = command.descriptor.laser;
    const float minimum = std::max(
        1.0f / 30.0f, descriptor.minimumLifetimeSeconds);
    const float maximum = std::max(minimum,
        descriptor.maximumLifetimeSeconds);
    const uint64_t bits = command.identity.variationSeed != 0
        ? command.identity.variationSeed : command.identity.eventId;
    const float unit = static_cast<float>((bits >> 40u) & 0xffffffu) /
        static_cast<float>(0xffffffu);
    Beam beam{
        .start = start,
        .end = end,
        .startAnchor = command.primary,
        .endAnchor = command.secondary,
        .lifetimeSeconds = minimum + (maximum - minimum) * unit,
        .admittedFrame = command.identity.confirmedFrame,
        .laserIdentity = command.beamIdentity != 0
            ? command.beamIdentity : command.identity.eventId,
        .controlledLaser = command.beamIdentity != 0,
        .laser = command.descriptor,
    };
    applyLaserRadiusCommand(beam, command);
    m_beams.push_back(std::move(beam));
}

void TypedFxWorldRenderer::age(float deltaSeconds, uint64_t simulationFrame) {
    const float step = std::clamp(
        std::isfinite(deltaSeconds) ? deltaSeconds : 0.0f,
        0.0f, 0.25f);
    for (Beam& beam : m_beams) {
        if (beam.tracer) beam.tracer->advance(step);
        else if (beam.laser && beam.admittedFrame != 0 &&
                 simulationFrame >= beam.admittedFrame) {
            beam.ageSeconds = static_cast<float>(
                simulationFrame - beam.admittedFrame) / 30.0f;
        } else beam.ageSeconds += step;
    }
    std::erase_if(m_beams, [simulationFrame](const Beam& beam) {
        if (beam.rope) return false;
        if (beam.tracer) return beam.tracer->expired();
        if (!beam.laser) return beam.ageSeconds >= beam.lifetimeSeconds;
        if (beam.decaying &&
            simulationFrame >= beam.decayFinishFrame) return true;
        const fx::LegacyLaserTemplate& laser = beam.laser->laser;
        const uint64_t envelopeFrames =
            static_cast<uint64_t>(laser.maximumIntensityFrames) +
            static_cast<uint64_t>(laser.fadeFrames);
        if (envelopeFrames != 0 && beam.admittedFrame != 0 &&
            simulationFrame >= beam.admittedFrame &&
            simulationFrame - beam.admittedFrame >= envelopeFrames) {
            return true;
        }
        return !beam.controlledLaser &&
            beam.ageSeconds >= beam.lifetimeSeconds;
    });
}

[[nodiscard]] uint32_t TypedFxWorldRenderer::textureSrv(container::StringView name) {
    if (name.empty() || !m_textures) return 0;
    const container::String key{name};
    if (const auto found = m_textureSrvs.find(key);
        found != m_textureSrvs.end()) return found->second;
    const std::optional<uint32_t> acquired = m_textures->acquire(name);
    if (!acquired) return 0;
    m_textureSrvs.emplace(key, *acquired);
    return *acquired;
}

[[nodiscard]] float TypedFxWorldRenderer::textureAspectRatio(
    container::StringView name) const noexcept {
    if (name.empty() || !m_textures) return 1.0f;
    const std::optional<WorldTextureCache::SourceDimensions> dimensions =
        m_textures->sourceDimensions(name);
    if (!dimensions || dimensions->width == 0u ||
        dimensions->height == 0u) return 1.0f;
    return static_cast<float>(dimensions->width) /
        static_cast<float>(dimensions->height);
}

void TypedFxWorldRenderer::releaseTextures() {
    if (m_textures) {
        for (const auto& [name, ignored] : m_textureSrvs) {
            static_cast<void>(ignored);
            m_textures->release(name);
        }
    }
    m_textureSrvs.clear();
}

void TypedFxWorldRenderer::buildVertices(const RenderCameraSnapshot& camera,
                   const TerrainRenderSnapshot* terrain,
                   const fx::FxRuntime* fxRuntime,
                   uint64_t simulationFrame) {
    m_vertices.clear();
    m_batches.clear();
    m_vertices.reserve(std::min(kMaximumTypedFxWorldVertices,
        m_beams.size() * 6u));
    const auto pushVertex = [this](const RenderVector& position,
                                   const fx::LegacyBeamColor& color,
                                   float alpha, float u, float v) {
        Vertex vertex{};
        vertex.position[0] = position.x();
        vertex.position[1] = position.y();
        vertex.position[2] = position.z();
        vertex.color[0] = color.red;
        vertex.color[1] = color.green;
        vertex.color[2] = color.blue;
        vertex.color[3] = std::clamp(color.alpha * alpha, 0.0f, 1.0f);
        vertex.uv[0] = u;
        vertex.uv[1] = v;
        m_vertices.push_back(vertex);
    };
    constexpr uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    const auto beginBatch = [this](uint32_t texture) {
        if (!m_batches.empty() && m_batches.back().textureSrv == texture &&
            m_batches.back().firstVertex + m_batches.back().vertexCount ==
                m_vertices.size()) return;
        m_batches.push_back({
            .firstVertex = static_cast<uint32_t>(m_vertices.size()),
            .textureSrv = texture,
        });
    };
    const auto appendQuad = [&](RenderVector start, RenderVector end,
                                float width,
                                const fx::LegacyBeamColor& color,
                                float alpha, float tileFactor,
                                float scroll, uint32_t texture) {
        if (!(width > 0.0f) || !std::isfinite(width) ||
            m_vertices.size() + 6u > kMaximumTypedFxWorldVertices) return false;
        const RenderVector direction = end - start;
        RenderVector side = direction.cross(camera.position -
            (start + end) * 0.5f);
        if (side.length_sq() <= math::EPSILON * math::EPSILON) {
            side = direction.cross(camera.up);
        }
        side = side.length_sq() <= math::EPSILON * math::EPSILON
            ? RenderVector{0.0f, 0.0f, 1.0f} : side.normalized();
        side *= width * 0.5f;
        const container::Array<RenderVector, 4> corners{
            start - side, start + side, end + side, end - side};
        const container::Array<math::vec2, 4> uv{{
            {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}}};
        beginBatch(texture);
        for (const uint32_t index : indices) {
            pushVertex(corners[index], color, alpha, uv[index].x(),
                       scroll + uv[index].y() * tileFactor);
        }
        m_batches.back().vertexCount += 6u;
        return true;
    };
    for (Beam& beam : m_beams) {
        if (m_vertices.size() + 6u > kMaximumTypedFxWorldVertices) {
            ++m_stats.rejectedCommands;
            break;
        }
        if (beam.tracer) {
            const RenderVector color = beam.tracer->color;
            static_cast<void>(appendQuad(
                beam.tracer->start(), beam.tracer->end(),
                beam.tracer->width,
                {color.x(), color.y(), color.z(), 1.0f},
                beam.tracer->opacity(), 1.0f, 0.0f, 0));
            continue;
        }
        if (beam.rope) {
            bool ropeVisible = true;
            if (fxRuntime && beam.startAnchor) {
                const fx::FxRuntime::ResolvedAnchor resolved =
                    fxRuntime->resolveCurrentAnchor(*beam.startAnchor);
                if (resolved.attachmentAlive ||
                    std::holds_alternative<fx::FxWorldPositionAnchor>(
                        *beam.startAnchor)) {
                    beam.rope->origin = {
                        resolved.anchor.position.x,
                        resolved.anchor.position.y,
                        resolved.anchor.position.z,
                    };
                } else {
                    ropeVisible = false;
                }
            }
            if (!ropeVisible) continue;
            if (beam.lastRopeFrame != 0 &&
                simulationFrame > beam.lastRopeFrame) {
                const uint64_t delta = simulationFrame -
                    beam.lastRopeFrame;
                fx::advanceLegacyRope(
                    *beam.rope, static_cast<uint32_t>(std::min<uint64_t>(
                        delta, std::numeric_limits<uint32_t>::max())));
                beam.lastRopeFrame = simulationFrame;
            }
            fx::buildLegacyRopeSegmentsInto(
                m_segmentScratch, *beam.rope);
            for (const fx::LegacyLaserSegment& segment :
                 m_segmentScratch) {
                const RenderVector start{
                    segment.start.x, segment.start.y, segment.start.z};
                const RenderVector end{
                    segment.end.x, segment.end.y, segment.end.z};
                const fx::LegacyBeamColor core = beam.rope->color;
                fx::LegacyBeamColor soft = beam.rope->color;
                soft.alpha *= 0.5f;
                if (!appendQuad(start, end, beam.rope->width * 0.5f,
                                core, 1.0f, 1.0f, 0.0f, 0) ||
                    !appendQuad(start, end, beam.rope->width,
                                soft, 1.0f, 1.0f, 0.0f, 0)) {
                    ++m_stats.rejectedCommands;
                    return;
                }
            }
            continue;
        }
        if (!beam.laser) continue;
        const fx::LegacyLaserTemplate& laser = beam.laser->laser;
        if (beam.widening) {
            if (simulationFrame >= beam.widenFinishFrame) {
                beam.widthScale = 1.0f;
                beam.widening = false;
            } else if (beam.widenFinishFrame > beam.widenStartFrame &&
                       simulationFrame >= beam.widenStartFrame) {
                beam.widthScale = std::clamp(
                    static_cast<float>(simulationFrame -
                        beam.widenStartFrame) /
                    static_cast<float>(beam.widenFinishFrame -
                        beam.widenStartFrame),
                    0.0f, 1.0f);
            }
        }
        if (beam.decaying) {
            if (simulationFrame >= beam.decayFinishFrame) {
                beam.widthScale = 0.0f;
            } else if (beam.decayFinishFrame > beam.decayStartFrame &&
                       simulationFrame >= beam.decayStartFrame) {
                beam.widthScale = std::clamp(
                    1.0f - static_cast<float>(simulationFrame -
                        beam.decayStartFrame) /
                    static_cast<float>(beam.decayFinishFrame -
                        beam.decayStartFrame),
                    0.0f, 1.0f);
            }
        }
        if (!(beam.widthScale > 0.0f)) continue;
        float intensity = 1.0f;
        if (beam.admittedFrame != 0 &&
            simulationFrame >= beam.admittedFrame &&
            (laser.maximumIntensityFrames != 0 ||
             laser.fadeFrames != 0)) {
            const uint64_t elapsed =
                simulationFrame - beam.admittedFrame;
            if (elapsed > laser.maximumIntensityFrames) {
                intensity = laser.fadeFrames == 0 ? 0.0f :
                    std::clamp(1.0f -
                        static_cast<float>(elapsed -
                            laser.maximumIntensityFrames) /
                        static_cast<float>(laser.fadeFrames),
                        0.0f, 1.0f);
            }
        }
        if (!(intensity > 0.0f)) continue;
        bool laserSourceVisible = true;
        if (fxRuntime && beam.startAnchor) {
            const fx::FxRuntime::ResolvedAnchor resolved =
                fxRuntime->resolveCurrentAnchor(*beam.startAnchor);
            if (resolved.attachmentAlive) {
                beam.start = {
                    resolved.anchor.position.x,
                    resolved.anchor.position.y,
                    resolved.anchor.position.z,
                };
            } else if (!std::holds_alternative<
                           fx::FxWorldPositionAnchor>(
                           *beam.startAnchor)) {
                laserSourceVisible = false;
            }
        }
        if (!laserSourceVisible) continue;
        if (fxRuntime && beam.endAnchor) {
            const fx::FxRuntime::ResolvedAnchor resolved =
                fxRuntime->resolveCurrentAnchor(*beam.endAnchor);
            if (resolved.attachmentAlive) {
                beam.end = {
                    resolved.anchor.position.x,
                    resolved.anchor.position.y,
                    resolved.anchor.position.z,
                };
                beam.targetAttachmentWasAlive = true;
            } else if (beam.targetAttachmentWasAlive &&
                       !beam.punchThroughApplied &&
                       laser.punchThroughScalar > 0.0f) {
                beam.end = beam.start + (beam.end - beam.start) *
                    laser.punchThroughScalar;
                beam.punchThroughApplied = true;
                beam.targetAttachmentWasAlive = false;
            }
        }
        fx::buildLegacyLaserSegmentsInto(
            m_segmentScratch, laser,
            {beam.start.x(), beam.start.y(), beam.start.z()},
            {beam.end.x(), beam.end.y(), beam.end.z()});
        const uint32_t texture = textureSrv(laser.textureName);
        const float textureRatio =
            textureAspectRatio(laser.textureName);
        for (fx::LegacyLaserSegment& segment : m_segmentScratch) {
            RenderVector start{segment.start.x, segment.start.y,
                               segment.start.z};
            RenderVector end{segment.end.x, segment.end.y,
                             segment.end.z};
            if (laser.arcHeight > 0.0f && laser.segments > 1u) {
                if (const std::optional<float> ground = terrainHeightAt(
                        terrain, start.x(), start.y())) {
                    start[2] = std::max(start.z(), *ground + 2.0f);
                }
                if (const std::optional<float> ground = terrainHeightAt(
                        terrain, end.x(), end.y())) {
                    end[2] = std::max(end.z(), *ground + 2.0f);
                }
            }
            for (int32_t layer = static_cast<int32_t>(
                     std::max(1u, laser.numberOfBeams)) - 1;
                 layer >= 0; --layer) {
                const float scale = laser.numberOfBeams <= 1u ? 0.0f :
                    static_cast<float>(layer) /
                    static_cast<float>(laser.numberOfBeams - 1u);
                const float width = beam.widthScale *
                    (laser.innerBeamWidth + scale *
                     (laser.outerBeamWidth - laser.innerBeamWidth));
                fx::LegacyBeamColor color{
                    laser.innerColor.red + scale *
                        (laser.outerColor.red - laser.innerColor.red),
                    laser.innerColor.green + scale *
                        (laser.outerColor.green - laser.innerColor.green),
                    laser.innerColor.blue + scale *
                        (laser.outerColor.blue - laser.innerColor.blue),
                    laser.innerColor.alpha + scale *
                        (laser.outerColor.alpha - laser.innerColor.alpha),
                };
                const float length = (end - start).length();
                const float tile = fx::legacyLaserTextureTileFactor(
                    laser, length, width, textureRatio);
                if (!appendQuad(start, end, width, color, intensity, tile,
                        beam.ageSeconds * laser.scrollRate, texture)) {
                    ++m_stats.rejectedCommands;
                    return;
                }
            }
        }
    }
}

bool TypedFxWorldRenderer::createRootSignature() {
    D3D12_DESCRIPTOR_RANGE textureRange{};
    textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRange.NumDescriptors = 1;
    textureRange.BaseShaderRegister = 0;
    textureRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = 0;
    // The VS consumes the camera matrix and the PS consumes the playable-map
    // bounds from this shared b0 block.
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &textureRange;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(std::size(parameters));
    description.pParameters = parameters;
    description.NumStaticSamplers = 1;
    description.pStaticSamplers = &sampler;
    description.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(
            &description, D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized, &errors))) {
        return false;
    }
    return SUCCEEDED(m_device->getDevice()->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature)));
}

bool TypedFxWorldRenderer::loadShaderPackage() {
    constexpr d3d12::ShaderPackageEntrySpec entries[] = {
        {"vertex_file", "typed_fx_world_vs.cso",
         "vertex_profile", "vs_5_0"},
        {"pixel_file", "typed_fx_world_ps.cso",
         "pixel_profile", "ps_5_0"},
    };
    container::Vector<container::Vector<uint8_t>> loaded;
    if (!d3d12::loadShaderPackage(
            "typed_fx_world",
            std::to_string(TD_TYPED_FX_WORLD_SHADER_PACKAGE_VERSION),
            TD_TYPED_FX_WORLD_SHADER_SOURCE_SHA256, entries, loaded) ||
        loaded.size() != m_shaderBytecode.size()) {
        TD_LOG_ERROR(
            "[TypedFxWorld] precompiled shader package unavailable");
        return false;
    }
    for (size_t index = 0; index < loaded.size(); ++index) {
        m_shaderBytecode[index] = std::move(loaded[index]);
    }
    return true;
}

bool TypedFxWorldRenderer::createPipelineState() {
    if (m_shaderBytecode[0].empty() || m_shaderBytecode[1].empty()) {
        return false;
    }
    static_assert(sizeof(Vertex) == 36);
    const D3D12_INPUT_ELEMENT_DESC input[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = m_rootSignature.Get();
    description.VS = {
        m_shaderBytecode[0].data(), m_shaderBytecode[0].size()};
    description.PS = {
        m_shaderBytecode[1].data(), m_shaderBytecode[1].size()};
    auto& blend = description.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D12_BLEND_ONE;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ONE;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.FrontCounterClockwise = TRUE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthEnable = TRUE;
    description.DepthStencilState.DepthWriteMask =
        D3D12_DEPTH_WRITE_MASK_ZERO;
    description.DepthStencilState.DepthFunc =
        D3D12_COMPARISON_FUNC_LESS_EQUAL;
    description.InputLayout = {
        input, static_cast<UINT>(std::size(input))};
    description.PrimitiveTopologyType =
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = d3d12::D3D12Device::SWAP_FORMAT;
    description.DSVFormat = d3d12::D3D12Device::DEPTH_FORMAT;
    description.SampleDesc.Count = m_sampleCount;
    return SUCCEEDED(m_device->getDevice()->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(&m_pipelineState)));
}

void TypedFxWorldRenderer::applyLaserRadiusCommand(
    TypedFxWorldRenderer::Beam& beam,
    const fx::FxLaserCommand& command) noexcept {
    const auto finishFrame = [](uint64_t start, uint64_t frames) {
        return start > std::numeric_limits<uint64_t>::max() - frames
            ? std::numeric_limits<uint64_t>::max() : start + frames;
    };
    if (command.sizeDeltaFrames > 0) {
        const uint64_t frames = static_cast<uint64_t>(
            command.sizeDeltaFrames);
        beam.widthScale = 0.0f;
        beam.widening = true;
        beam.decaying = false;
        beam.widenStartFrame = command.identity.confirmedFrame;
        beam.widenFinishFrame = finishFrame(
            beam.widenStartFrame, frames);
    } else if (command.sizeDeltaFrames < 0) {
        const uint64_t frames = static_cast<uint64_t>(
            -static_cast<int64_t>(command.sizeDeltaFrames));
        beam.widthScale = 1.0f;
        beam.widening = false;
        beam.decaying = true;
        beam.decayStartFrame = command.identity.confirmedFrame;
        beam.decayFinishFrame = finishFrame(
            beam.decayStartFrame, frames);
    }
    if (command.decayFrames > 0) {
        beam.widthScale = 1.0f;
        beam.widening = false;
        beam.decaying = true;
        beam.decayStartFrame = command.identity.confirmedFrame;
        beam.decayFinishFrame = finishFrame(
            beam.decayStartFrame, command.decayFrames);
    }
}

[[nodiscard]] std::optional<float> TypedFxWorldRenderer::terrainHeightAt(
    const TerrainRenderSnapshot* terrain, float worldX,
    float worldY) noexcept {
    if (!terrain || !terrain->isValid() || !std::isfinite(worldX) ||
        !std::isfinite(worldY)) return std::nullopt;
    const float gridX = worldX / terrain->cellWorldSize +
        static_cast<float>(terrain->borderSize);
    const float gridY = worldY / terrain->cellWorldSize +
        static_cast<float>(terrain->borderSize);
    if (gridX < 0.0f || gridY < 0.0f ||
        gridX > static_cast<float>(terrain->width - 1) ||
        gridY > static_cast<float>(terrain->height - 1)) return std::nullopt;
    const int32_t x0 = static_cast<int32_t>(std::floor(gridX));
    const int32_t y0 = static_cast<int32_t>(std::floor(gridY));
    const int32_t x1 = std::min(x0 + 1, terrain->width - 1);
    const int32_t y1 = std::min(y0 + 1, terrain->height - 1);
    const float tx = gridX - static_cast<float>(x0);
    const float ty = gridY - static_cast<float>(y0);
    const float top = terrain->heightWorld(x0, y0) +
        (terrain->heightWorld(x1, y0) - terrain->heightWorld(x0, y0)) * tx;
    const float bottom = terrain->heightWorld(x0, y1) +
        (terrain->heightWorld(x1, y1) - terrain->heightWorld(x0, y1)) * tx;
    return top + (bottom - top) * ty;
}

} // namespace engine::render
