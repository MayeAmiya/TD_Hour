#include "engine/renderer/world/particle/ParticleRenderer.h"

#include "engine/renderer/world/particle/GpuParticleSimulator.h"
#include "engine/renderer/d3d12/runtime/D3D12Device.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace engine::render {
namespace {

class ScopedGpuTimestamp final {
public:
    ScopedGpuTimestamp(
        d3d12::D3D12Device& device,
        GpuTimestampRange range) noexcept
        : m_device(&device), m_range(range),
          m_active(device.beginGpuTimestamp(range)) {}
    ~ScopedGpuTimestamp() {
        if (m_active) {
            static_cast<void>(m_device->endGpuTimestamp(m_range));
        }
    }

private:
    d3d12::D3D12Device* m_device = nullptr;
    GpuTimestampRange m_range = GpuTimestampRange::Frame;
    bool m_active = false;
};

struct ParticleCameraConstants final {
    float viewProjection[16]{};
    float cameraRight[4]{};
    float cameraUp[4]{};
    float playableMinimum[2]{};
    float playableMaximum[2]{};
    uint32_t playableBoundsEnabled = 0;
    uint32_t padding[3]{};
};
static_assert(sizeof(ParticleCameraConstants) == 128u);

[[nodiscard]] uint32_t particleVisibilitySignatureA(
    uint32_t slot, uint32_t generation) noexcept {
    uint32_t value = slot ^ (generation * 0x9e3779b9u);
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

[[nodiscard]] uint32_t particleVisibilitySignatureB(
    uint32_t slot, uint32_t generation) noexcept {
    uint32_t value = generation ^ (slot * 0x85ebca6bu);
    value ^= value >> 15u;
    value *= 0xc2b2ae35u;
    value ^= value >> 13u;
    return value;
}

[[nodiscard]] bool gpuReferenceNear(float left, float right) noexcept {
    if (!std::isfinite(left) || !std::isfinite(right)) return false;
    const float scale = std::max({1.0f, std::abs(left), std::abs(right)});
    return std::abs(left - right) <= 1.0e-4f * scale;
}

[[nodiscard]] bool gpuStateMatchesReference(
    const fx::gpu_particle::GpuParticleState& state,
    const GpuParticleReferenceSample& reference) noexcept {
    if ((state.identityAndFlags[3] & fx::gpu_particle::StateAlive) == 0u ||
        state.authorityTokens[0] != reference.stateSlot ||
        state.authorityTokens[1] != reference.particleGeneration) {
        return false;
    }
    for (size_t axis = 0; axis < 3u; ++axis) {
        if (!gpuReferenceNear(
                state.positionAndAge[axis], reference.position[axis]) ||
            !gpuReferenceNear(
                state.previousAndLifetime[axis],
                reference.previousPosition[axis])) {
            return false;
        }
    }
    if (!gpuReferenceNear(state.sizeDynamicsAndAngle[0], reference.size) ||
        !gpuReferenceNear(state.sizeDynamicsAndAngle[3], reference.angle) ||
        !gpuReferenceNear(state.angularDynamicsAndAlpha[3],
                          reference.color[3])) {
        return false;
    }
    for (size_t channel = 0; channel < 3u; ++channel) {
        if (!gpuReferenceNear(
                state.colorAndWindRandomness[channel],
                reference.color[channel])) {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<D3D12_GPU_VIRTUAL_ADDRESS>
ParticleRenderer::prepareGpuBillboardIndirect(
    const GpuParticleBillboardConstants& constants) {
    if (!m_device || !m_gpuSimulator ||
        !m_gpuBillboardRootSignature ||
        !m_gpuBillboardCommandSignature ||
        m_gpuMaterialBins.bins.empty() ||
        !m_gpuSimulator->materialIndirectArgsReady() ||
        !m_gpuSimulator->recordPrepareIndirectDraw()) {
        if (!m_reportedGpuDrawFailure) {
            TD_LOG_WARN(
                "[ParticleRenderer] GPU billboard indirect shadow draw unavailable; CPU billboard draw remains effective");
            m_reportedGpuDrawFailure = true;
        }
        return std::nullopt;
    }
    const uint32_t stateSrv = m_gpuSimulator->particleStatesSrv();
    const uint32_t groupedIndicesSrv =
        m_gpuSimulator->materialParticleIndicesSrv();
    if (stateSrv == UINT32_MAX || groupedIndicesSrv == UINT32_MAX) {
        return std::nullopt;
    }
    if (m_gpuMaterialBinTextureGeneration != m_textureBindingGeneration ||
        m_gpuMaterialBinTextureSrvs.size() !=
            m_gpuMaterialBins.bins.size()) {
        return std::nullopt;
    }
    for (const GpuParticleMaterialBin& bin : m_gpuMaterialBins.bins) {
        const size_t shaderIndex = static_cast<size_t>(bin.shader);
        if ((bin.shader != fx::ParticleShader::Additive &&
             bin.shader != fx::ParticleShader::AlphaTest) ||
            shaderIndex >= m_gpuBillboardPipelineStates.size() ||
            !m_gpuBillboardPipelineStates[shaderIndex] ||
            !m_gpuBillboardShadowPipelineStates[shaderIndex]) {
            return std::nullopt;
        }
    }
    const d3d12::ConstantBufferAllocation constantsAllocation =
        m_device->allocateConstantBuffer(&constants, sizeof(constants));
    ID3D12GraphicsCommandList* commandList = m_device->commandList();
    ID3D12Resource* indirectArguments =
        m_gpuSimulator->materialIndirectArgs();
    if (!constantsAllocation || !commandList || !indirectArguments) {
        return std::nullopt;
    }
    m_reportedGpuDrawFailure = false;
    return constantsAllocation.gpuAddress;
}

size_t ParticleRenderer::recordGpuBillboardIndirectStage(
    D3D12_GPU_VIRTUAL_ADDRESS constants,
    fx::ParticleShader stageShader, bool visible) {
    if (!m_device || !m_gpuSimulator || constants == 0 ||
        !m_gpuBillboardRootSignature ||
        !m_gpuBillboardCommandSignature) {
        return 0;
    }
    const uint32_t stateSrv = m_gpuSimulator->particleStatesSrv();
    const uint32_t groupedIndicesSrv =
        m_gpuSimulator->materialParticleIndicesSrv();
    ID3D12GraphicsCommandList* commandList = m_device->commandList();
    ID3D12Resource* indirectArguments =
        m_gpuSimulator->materialIndirectArgs();
    if (stateSrv == UINT32_MAX || groupedIndicesSrv == UINT32_MAX ||
        !commandList || !indirectArguments) {
        return 0;
    }

    m_device->bindSrvHeap();
    commandList->SetGraphicsRootSignature(
        m_gpuBillboardRootSignature.Get());
    m_device->recordGraphicsRootSignatureCall();
    commandList->SetGraphicsRootConstantBufferView(
        0, constants);
    commandList->SetGraphicsRootDescriptorTable(
        1, m_device->getSrvGpuHandle(stateSrv));
    m_device->recordGraphicsDescriptorTableCall();
    commandList->SetGraphicsRootDescriptorTable(
        2, m_device->getSrvGpuHandle(groupedIndicesSrv));
    m_device->recordGraphicsDescriptorTableCall();
    commandList->IASetVertexBuffers(0, 1, &m_quadVertexView);
    m_device->recordVertexBufferCall();
    commandList->IASetIndexBuffer(&m_quadIndexView);
    m_device->recordIndexBufferCall();
    commandList->IASetPrimitiveTopology(
        D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    size_t executeCalls = 0;
    ID3D12PipelineState* activePipeline = nullptr;
    for (size_t binIndex = 0;
        binIndex < m_gpuMaterialBins.bins.size(); ++binIndex) {
        const GpuParticleMaterialBin& bin = m_gpuMaterialBins.bins[binIndex];
        if (bin.shader != stageShader) continue;
        const size_t shaderIndex = static_cast<size_t>(bin.shader);
        const auto& pipelines = visible
            ? m_gpuBillboardPipelineStates
            : m_gpuBillboardShadowPipelineStates;
        if (shaderIndex >= pipelines.size() || !pipelines[shaderIndex]) {
            continue;
        }
        ID3D12PipelineState* const pipeline =
            pipelines[shaderIndex].Get();
        if (activePipeline != pipeline) {
            commandList->SetPipelineState(pipeline);
            m_device->recordPipelineStateCall();
            activePipeline = pipeline;
        }
        const uint32_t textureSrvIndex =
            m_gpuMaterialBinTextureGeneration == m_textureBindingGeneration &&
            binIndex < m_gpuMaterialBinTextureSrvs.size()
            ? m_gpuMaterialBinTextureSrvs[binIndex] : 0u;
        commandList->SetGraphicsRootDescriptorTable(
            3, m_device->getSrvGpuHandle(textureSrvIndex));
        m_device->recordGraphicsDescriptorTableCall();
        commandList->ExecuteIndirect(
            m_gpuBillboardCommandSignature.Get(), 1u,
            indirectArguments,
            static_cast<UINT64>(binIndex) *
                sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
            nullptr, 0u);
        m_device->recordExecuteIndirectCall();
        ++executeCalls;
    }
    if (visible) {
        m_gpuBillboardIndirectVisibleExecuteCalls += executeCalls;
    } else {
        m_gpuBillboardIndirectShadowExecuteCalls += executeCalls;
    }
    m_executionStats.gpuIndirectDrawCalls += static_cast<uint32_t>(
        std::min<size_t>(executeCalls,
            std::numeric_limits<uint32_t>::max() -
                m_executionStats.gpuIndirectDrawCalls));
    return executeCalls;
}

size_t ParticleRenderer::render(const ParticleRenderDrawList& drawList,
                                const RenderCameraSnapshot& cameraSnapshot,
                                const LocalVisibilityRenderSnapshot&
                                    localVisibility) {
    // configureTextureSampling resets the root signature and all CPU pipeline
    // states and returns false on a failed rebuild while leaving m_initialized
    // true; its caller only logs.  Recording a null root signature plus a root
    // CBV bind against it is invalid D3D12 usage, so stay inert instead.
    if (!m_initialized || !m_device || !m_rootSignature ||
        std::any_of(m_pipelineStates.begin(), m_pipelineStates.end(),
                    [](const auto& pipeline) { return !pipeline; })) {
        return 0;
    }
    m_executionStats = {};
    bool gpuMaterialBinsRecorded = false;
    {
    ScopedGpuTimestamp computeTimestamp(
        *m_device, GpuTimestampRange::ParticleCompute);
    if (m_gpuSimulator) {
        const uint32_t frameIndex = m_device->frameIndex();
        if (frameIndex < m_gpuAbCountExpectations.size()) {
            const std::optional<
                GpuParticleSimulator::AliveCompactReadbackHeader> readback =
                m_gpuSimulator->consumeDiagnosticCounterReadback();
            GpuAbCountExpectation& expected =
                m_gpuAbCountExpectations[frameIndex];
            if (readback && expected.valid) {
                const bool identityMatches =
                    readback->authorityEpoch == expected.authorityEpoch &&
                    readback->contractVersion ==
                        fx::gpu_particle::kContractVersion;
                if (identityMatches) {
                    const uint32_t gpuCount =
                        readback->materialBinnedCount - std::min(
                            readback->materialBinnedCount,
                            readback->materialOverflowCount);
                    ++m_gpuAbCountSamples;
                    m_gpuAbLastCpuCount = expected.cpuCompatibleCount;
                    m_gpuAbLastGpuCount = gpuCount;
                    m_gpuAbLastCountValid = true;
                    if (gpuCount == expected.cpuCompatibleCount) {
                        ++m_gpuAbCountMatches;
                        if (m_gpuAbCountConsecutiveFrames !=
                            std::numeric_limits<uint32_t>::max()) {
                            ++m_gpuAbCountConsecutiveFrames;
                        }
                    } else {
                        ++m_gpuAbCountMismatches;
                        m_gpuAbCountConsecutiveFrames = 0;
                        m_gpuAbVisibilityConsecutiveFrames = 0;
                    }
                    const uint32_t gpuVisibleCount =
                        readback->visibleCount - std::min(
                            readback->visibleCount,
                            readback->visibleOverflowCount);
                    const bool membershipMatches =
                        gpuVisibleCount == expected.cpuCompatibleCount &&
                        readback->visibleSignatureA ==
                            expected.visibilitySignatureA &&
                        readback->visibleSignatureB ==
                            expected.visibilitySignatureB;
                    if (membershipMatches) {
                        if (m_gpuAbVisibilityConsecutiveFrames !=
                            std::numeric_limits<uint32_t>::max()) {
                            ++m_gpuAbVisibilityConsecutiveFrames;
                        }
                    } else {
                        m_gpuAbVisibilityConsecutiveFrames = 0;
                    }
                } else {
                    ++m_gpuAbCountStaleSkipped;
                    m_gpuAbLastCountValid = false;
                    m_gpuAbCountConsecutiveFrames = 0;
                    m_gpuAbVisibilityConsecutiveFrames = 0;
                }
            }
            expected.valid = false;
        }
        if (frameIndex < m_gpuAbStateExpectations.size()) {
            const std::optional<
                GpuParticleSimulator::DiagnosticStateSampleBatch>
                stateReadback =
                    m_gpuSimulator->consumeDiagnosticStateReadback();
            GpuAbStateExpectation& expected =
                m_gpuAbStateExpectations[frameIndex];
            if (stateReadback && expected.valid) {
                if (expected.authorityEpoch !=
                    m_gpuSimulator->stats().authorityEpoch) {
                    ++m_gpuAbStateStaleSkipped;
                    m_gpuAbLastStateValid = false;
                    m_gpuAbStateConsecutiveFrames = 0;
                } else if (stateReadback->count != expected.count) {
                    ++m_gpuAbStateSamples;
                    ++m_gpuAbStateMismatches;
                    m_gpuAbLastStateValid = false;
                    m_gpuAbStateConsecutiveFrames = 0;
                } else {
                    bool batchMatches = true;
                    for (uint32_t sampleIndex = 0;
                         sampleIndex < expected.count; ++sampleIndex) {
                        ++m_gpuAbStateSamples;
                        if (gpuStateMatchesReference(
                                stateReadback->states[sampleIndex],
                                expected.samples[sampleIndex])) {
                            ++m_gpuAbStateMatches;
                        } else {
                            ++m_gpuAbStateMismatches;
                            batchMatches = false;
                        }
                    }
                    m_gpuAbLastStateValid = batchMatches;
                    if (batchMatches) {
                        if (m_gpuAbStateConsecutiveFrames !=
                            std::numeric_limits<uint32_t>::max()) {
                            ++m_gpuAbStateConsecutiveFrames;
                        }
                    } else {
                        m_gpuAbStateConsecutiveFrames = 0;
                    }
                }
            }
            expected.valid = false;
        }
        if (m_pendingGpuCommands.authorityEpoch != 0 &&
            m_pendingGpuCommands.authorityEpoch !=
                m_gpuSimulator->stats().authorityEpoch) {
            m_gpuSimulator->requestReset(
                m_pendingGpuCommands.authorityEpoch);
        }
        const bool commandsRecorded = m_gpuSimulator->recordCommands(
            m_pendingGpuCommands.births,
            m_pendingGpuCommands.retires);
        // ParticleHandle indices are sparse generational slots rather than a
        // dense [0, particleCount) range. Dispatch the bounded state capacity;
        // IntegrateCS rejects every lane without the alive bit.
        const bool integrationRecorded = commandsRecorded &&
            m_gpuSimulator->recordIntegration(
                m_gpuSimulator->stats().capacity,
                m_pendingGpuAuthoredFrames);
        // Lifecycle commands and authored frames become consumed once the
        // integration dispatch has been recorded.  Alive compaction is a
        // downstream derived-list pass: if it ever fails, retry that pass on
        // the next frame without replaying births or rewinding shadow state.
        if (integrationRecorded) {
            m_pendingGpuCommands.births.clear();
            m_pendingGpuCommands.retires.clear();
            m_pendingGpuAuthoredFrames = 0;
        }
        const bool compactRecorded = integrationRecorded &&
            m_gpuSimulator->recordAliveCompact();
        const bool visibleRecorded = compactRecorded &&
            m_gpuSimulator->recordVisibleCompact();
        const bool materialBinsRecorded = visibleRecorded &&
            m_gpuSimulator->recordMaterialBins();
        gpuMaterialBinsRecorded = materialBinsRecorded;
        const bool countComparable =
            m_gpuPresentationRequested &&
            m_gpuPresentationParticleBudget >=
                m_gpuPresentationParticleCount &&
            m_gpuMaterialBins.rejectedCompatibleTemplates == 0;
        if (materialBinsRecorded && countComparable &&
            m_gpuSimulator->recordDiagnosticCounterReadback()) {
            const uint32_t frameIndex = m_device->frameIndex();
            if (frameIndex < m_gpuAbCountExpectations.size()) {
                GpuAbCountExpectation expectation{
                    .authorityEpoch = m_gpuSimulator->stats().authorityEpoch,
                    .cpuCompatibleCount = static_cast<uint32_t>(
                        std::min<size_t>(
                            drawList.stats.gpuCompatibleSelectedInstances,
                            std::numeric_limits<uint32_t>::max())),
                    .valid = true,
                };
                for (const GpuParticleVisibilityGeneration& entry :
                     drawList.gpuVisibilityGenerations) {
                    expectation.visibilitySignatureA ^=
                        particleVisibilitySignatureA(
                            entry.stateSlot,
                            entry.particleGeneration);
                    expectation.visibilitySignatureB ^=
                        particleVisibilitySignatureB(
                            entry.stateSlot,
                            entry.particleGeneration);
                }
                m_gpuAbCountExpectations[frameIndex] = expectation;
            }
        }
        if (materialBinsRecorded && countComparable &&
            drawList.gpuReferenceSampleCount != 0u) {
            container::Array<uint32_t,
                d3d12::performance_limits::
                    kGpuParticleAbStateSampleCapacity> stateSlots{};
            for (uint32_t sampleIndex = 0;
                 sampleIndex < drawList.gpuReferenceSampleCount;
                 ++sampleIndex) {
                stateSlots[sampleIndex] =
                    drawList.gpuReferenceSamples[sampleIndex].stateSlot;
            }
            if (m_gpuSimulator->recordDiagnosticStateReadback({
                    stateSlots.data(),
                    drawList.gpuReferenceSampleCount})) {
                const uint32_t frameIndex = m_device->frameIndex();
                if (frameIndex < m_gpuAbStateExpectations.size()) {
                    GpuAbStateExpectation& expected =
                        m_gpuAbStateExpectations[frameIndex];
                    expected = {};
                    expected.authorityEpoch =
                        m_gpuSimulator->stats().authorityEpoch;
                    expected.count = drawList.gpuReferenceSampleCount;
                    std::copy_n(
                        drawList.gpuReferenceSamples.begin(),
                        expected.count, expected.samples.begin());
                    expected.valid = true;
                }
            }
        }
        if (materialBinsRecorded) {
            m_reportedGpuResetFailure = false;
            m_reportedGpuCommandFailure = false;
        } else if (!m_reportedGpuCommandFailure) {
            TD_LOG_WARN(
                "[ParticleRenderer] GPU shadow integration or compact recording failed; CPU backend remains effective");
            m_reportedGpuCommandFailure = true;
        }
        if (m_gpuSimulator->resetPending() &&
            !m_reportedGpuResetFailure) {
            TD_LOG_WARN(
                "[ParticleRenderer] GPU reset remains pending; CPU backend remains effective");
            m_reportedGpuResetFailure = true;
        }
    }
    }
    ScopedGpuTimestamp drawTimestamp(
        *m_device, GpuTimestampRange::ParticleDraw);
    const WorldCamera camera = WorldCamera::fromSnapshot(cameraSnapshot);
    const float aspectRatio = m_device->height() != 0
        ? static_cast<float>(m_device->width()) /
            static_cast<float>(m_device->height())
        : 1.0f;
    const math::float4x4 viewProjection =
        camera.viewProjectionMatrix(aspectRatio);
    math::vec3 forward = camera.target() - camera.position();
    forward = forward.length_sq() > math::EPSILON * math::EPSILON
        ? forward.normalized() : math::vec3{0.0f, 1.0f, 0.0f};
    math::vec3 up = camera.up().length_sq() >
            math::EPSILON * math::EPSILON
        ? camera.up().normalized() : WorldCamera::worldUp();
    math::vec3 right = forward.cross(up);
    if (right.length_sq() <= math::EPSILON * math::EPSILON) {
        up = std::abs(forward.z()) < 0.999f
            ? WorldCamera::worldUp() : math::vec3{0.0f, 1.0f, 0.0f};
        right = forward.cross(up);
    }
    right = right.normalized();
    up = right.cross(forward).normalized();

    ParticleCameraConstants constants{};
    std::memcpy(
        constants.viewProjection, &viewProjection.m,
        sizeof(constants.viewProjection));
    constants.cameraRight[0] = right.x();
    constants.cameraRight[1] = right.y();
    constants.cameraRight[2] = right.z();
    constants.cameraUp[0] = up.x();
    constants.cameraUp[1] = up.y();
    constants.cameraUp[2] = up.z();
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

    ID3D12GraphicsCommandList* commandList = m_device->commandList();
    if (!commandList) return 0;
    m_device->flushBatch();
    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_device->width());
    const uint32_t tacticalHeight =
        camera.tacticalViewportHeight(m_device->height());
    viewport.Height = static_cast<float>(tacticalHeight);
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{
        0, 0, static_cast<LONG>(m_device->width()),
        static_cast<LONG>(tacticalHeight)};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    bool gpuVisibleAuthority = false;
    std::optional<D3D12_GPU_VIRTUAL_ADDRESS> gpuDrawConstants;
    if (gpuMaterialBinsRecorded && m_gpuBillboardShaderPackageReady &&
        !m_gpuMaterialBins.bins.empty()) {
        GpuParticleBillboardConstants gpuConstants{};
        std::memcpy(
            gpuConstants.viewProjection, constants.viewProjection,
            sizeof(gpuConstants.viewProjection));
        std::copy_n(constants.cameraRight, 4u, gpuConstants.cameraRight);
        std::copy_n(constants.cameraUp, 4u, gpuConstants.cameraUp);
        gpuConstants.particleCapacity = m_gpuSimulator->stats().capacity;
        gpuConstants.interpolationAlpha = drawList.interpolationAlpha;
        std::copy_n(
            constants.playableMinimum, 2u,
            gpuConstants.playableMinimum);
        std::copy_n(
            constants.playableMaximum, 2u,
            gpuConstants.playableMaximum);
        gpuConstants.playableBoundsEnabled =
            constants.playableBoundsEnabled;
        gpuDrawConstants = prepareGpuBillboardIndirect(gpuConstants);
        gpuVisibleAuthority = gpuDrawConstants.has_value() &&
            m_gpuPresentationGate.effectiveGpuPresentation;
        if (!gpuDrawConstants &&
            m_gpuPresentationGate.effectiveGpuPresentation) {
            ++m_gpuPresentationFallbackFrames;
        }
    }

    if (drawList.instances.empty() || drawList.batches.empty()) return 0;
    if (drawList.instances.size() > std::numeric_limits<uint32_t>::max() ||
        drawList.instances.size() > std::numeric_limits<uint32_t>::max() /
            sizeof(ParticleRenderInstance)) {
        return 0;
    }

    const uint32_t instanceBytes = static_cast<uint32_t>(
        drawList.instances.size() * sizeof(ParticleRenderInstance));
    const auto uploadStarted = std::chrono::steady_clock::now();
    const d3d12::FrameUploadAllocation instanceAllocation =
        m_device->allocateFrameUpload(drawList.instances.data(), instanceBytes,
                                      alignof(ParticleRenderInstance));
    if (!instanceAllocation) {
        m_executionStats.instanceUploadMicroseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - uploadStarted).count());
        return 0;
    }

    const d3d12::ConstantBufferAllocation cameraAllocation =
        m_device->allocateConstantBuffer(&constants, sizeof(constants));
    m_executionStats.instanceUploadMicroseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - uploadStarted).count());
    m_executionStats.instanceUploadBytes = instanceBytes;
    if (!cameraAllocation) return 0;

    D3D12_VERTEX_BUFFER_VIEW views[2] = {m_quadVertexView, {}};
    views[1].BufferLocation = instanceAllocation.gpuAddress;
    views[1].SizeInBytes = instanceBytes;
    views[1].StrideInBytes = sizeof(ParticleRenderInstance);
    m_device->bindSrvHeap();
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_device->recordGraphicsRootSignatureCall();
    commandList->SetGraphicsRootConstantBufferView(0, cameraAllocation.gpuAddress);
    commandList->IASetVertexBuffers(0, 2, views);
    m_device->recordVertexBufferCall();
    commandList->IASetIndexBuffer(&m_quadIndexView);
    m_device->recordIndexBufferCall();
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    size_t renderedInstances = 0;
    const auto drawStarted = std::chrono::steady_clock::now();
    ID3D12PipelineState* activePipeline = nullptr;
    if (gpuDrawConstants) {
        static_cast<void>(recordGpuBillboardIndirectStage(
            *gpuDrawConstants, fx::ParticleShader::AlphaTest,
            gpuVisibleAuthority));
        if (!gpuVisibleAuthority) {
            static_cast<void>(recordGpuBillboardIndirectStage(
                *gpuDrawConstants, fx::ParticleShader::Additive, false));
        }
        // GPU stage binding replaced the CPU root/IA state.
        m_device->bindSrvHeap();
        commandList->SetGraphicsRootSignature(m_rootSignature.Get());
        m_device->recordGraphicsRootSignatureCall();
        commandList->SetGraphicsRootConstantBufferView(
            0, cameraAllocation.gpuAddress);
        commandList->IASetVertexBuffers(0, 2, views);
        m_device->recordVertexBufferCall();
        commandList->IASetIndexBuffer(&m_quadIndexView);
        m_device->recordIndexBufferCall();
        commandList->IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }
    for (const ParticleRenderBatch& batch : drawList.batches) {
        if (gpuVisibleAuthority &&
            batch.route == ParticleRenderRoute::GpuCompatibleReference) {
            continue;
        }
        if (batch.instanceCount == 0 ||
            static_cast<uint64_t>(batch.firstInstance) + batch.instanceCount >
                drawList.instances.size()) {
            continue;
        }
        const size_t shaderIndex = static_cast<size_t>(batch.shader);
        if (shaderIndex >= m_pipelineStates.size() || !m_pipelineStates[shaderIndex]) continue;
        if (activePipeline != m_pipelineStates[shaderIndex].Get()) {
            activePipeline = m_pipelineStates[shaderIndex].Get();
            commandList->SetPipelineState(activePipeline);
            m_device->recordPipelineStateCall();
        }
        const uint32_t textureSrvIndex =
            drawList.textureBindingGeneration == m_textureBindingGeneration
            ? batch.textureSrvIndex : 0;
        commandList->SetGraphicsRootDescriptorTable(
            1, m_device->getSrvGpuHandle(textureSrvIndex));
        m_device->recordGraphicsDescriptorTableCall();
        commandList->DrawIndexedInstanced(6, batch.instanceCount, 0, 0,
                                          batch.firstInstance);
        m_device->recordDrawCall();
        ++m_executionStats.cpuDrawCalls;
        renderedInstances += batch.instanceCount;
    }
    if (gpuVisibleAuthority && gpuDrawConstants) {
        static_cast<void>(recordGpuBillboardIndirectStage(
            *gpuDrawConstants, fx::ParticleShader::Additive, true));
        renderedInstances +=
            drawList.stats.gpuCompatibleSelectedInstances;
    }
    m_executionStats.drawRecordMicroseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - drawStarted).count());
    return renderedInstances;
}


} // namespace engine::render
