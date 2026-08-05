#include "core/container/container_types.h"
#include "engine/renderer/world/particle/ParticleRenderer.h"
#include "engine/renderer/world/particle/ParticleRendererScratch.h"
#include "engine/renderer/world/particle/GpuParticleSimulator.h"
#include "engine/renderer/runtime/RendererStats.h"
#include "engine/renderer/d3d12/runtime/D3D12QualitySettings.h"
#include "engine/renderer/d3d12/runtime/D3D12PerformanceSettings.h"
#include "engine/renderer/d3d12/runtime/D3D12ShaderPackage.h"

#include "engine/renderer/world/resource/WorldTextureCache.h"
#include "presentation/render/RenderViewSnapshot.h"
#include "presentation/render/TerrainRenderSnapshot.h"
#include "engine/renderer/d3d12/runtime/D3D12Device.h"
#include "debug/debug.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <iterator>
#include <limits>

#ifndef TD_PARTICLE_SHADER_PACKAGE_VERSION
#define TD_PARTICLE_SHADER_PACKAGE_VERSION 1
#endif

#ifndef TD_PARTICLE_SHADER_SOURCE_SHA256
#define TD_PARTICLE_SHADER_SOURCE_SHA256 \
    "0000000000000000000000000000000000000000000000000000000000000000"
#endif

#ifndef TD_PARTICLE_GPU_BILLBOARD_SHADER_PACKAGE_VERSION
#define TD_PARTICLE_GPU_BILLBOARD_SHADER_PACKAGE_VERSION 1
#endif

#ifndef TD_PARTICLE_GPU_BILLBOARD_SHADER_SOURCE_SHA256
#define TD_PARTICLE_GPU_BILLBOARD_SHADER_SOURCE_SHA256 \
    "0000000000000000000000000000000000000000000000000000000000000000"
#endif

static_assert(
    TD_PARTICLE_GPU_BILLBOARD_SHADER_PACKAGE_VERSION < 1000 ||
    TD_PARTICLE_GPU_BILLBOARD_SHADER_PACKAGE_VERSION / 1000 ==
        engine::fx::gpu_particle::kContractVersion,
    "GPU billboard package major version must match GpuParticleContract");

#define TD_PARTICLE_STRINGIFY_INNER(value) #value
#define TD_PARTICLE_STRINGIFY(value) TD_PARTICLE_STRINGIFY_INNER(value)

namespace engine::render {
using particle_render_detail::SmudgeVertex;
namespace {

constexpr size_t kSmudgesPerDraw = 500;
constexpr float kMaximumSmudgeUvOffset = 0.06f;

struct SmudgeCameraConstants final {
    float viewProjection[16]{};
    float cameraRight[4]{};
    float cameraUp[4]{};
    float playableMinimum[2]{};
    float playableMaximum[2]{};
    uint32_t playableBoundsEnabled = 0;
    uint32_t padding[3]{};
};
static_assert(sizeof(SmudgeCameraConstants) == 128u);

} // namespace

namespace {

[[nodiscard]] D3D12_RESOURCE_BARRIER transitionBarrier(
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

} // namespace

ParticleRenderer::ParticleRenderer() = default;

ParticleRenderer::ParticleRenderer(d3d12::D3D12Device& device,
                                   container::SharedPtr<WorldTextureCache> textures) {
    (void)init(device, std::move(textures));
}

ParticleRenderer::~ParticleRenderer() {
    shutdown();
}

bool ParticleRenderer::init(d3d12::D3D12Device& device,
                            container::SharedPtr<WorldTextureCache> textures) {
    shutdown();
    if (!textures) return false;
    m_device = &device;
    m_textures = std::move(textures);
    if (!loadShaderPackage() || !createRootSignature() || !createPipelineStates() ||
        !createStaticQuad() || !createSmudgePipeline()) {
        shutdown();
        return false;
    }
    // The optional graphics path is built independently. Failure cannot take
    // down the CPU reference renderer while GPU draw authority is gated.
    if (loadGpuBillboardShaderPackage() &&
        !createGpuBillboardPipeline()) {
        TD_LOG_WARN(
            "[ParticleRenderer] GPU billboard pipeline unavailable; CPU billboard draw remains effective");
        m_gpuBillboardShaderPackageReady = false;
    }
    m_initialized = true;
    return true;
}

bool ParticleRenderer::loadShaderPackage() {
    const container::Array<d3d12::ShaderPackageEntrySpec, 5> entries{{
        {"particle_vertex_file", "particle_vs.cso",
         "particle_vertex_profile", "vs_5_0"},
        {"particle_pixel_file", "particle_ps.cso",
         "particle_pixel_profile", "ps_5_0"},
        {"particle_alpha_test_file", "particle_alpha_test_ps.cso",
         "particle_alpha_test_profile", "ps_5_0"},
        {"smudge_vertex_file", "particle_smudge_vs.cso",
         "smudge_vertex_profile", "vs_5_0"},
        {"smudge_pixel_file", "particle_smudge_ps.cso",
         "smudge_pixel_profile", "ps_5_0"},
    }};
    container::Vector<container::Vector<uint8_t>> loaded;
    if (!d3d12::loadShaderPackage(
            "particle", TD_PARTICLE_STRINGIFY(
                TD_PARTICLE_SHADER_PACKAGE_VERSION),
            TD_PARTICLE_SHADER_SOURCE_SHA256,
            {entries.data(), entries.size()}, loaded) ||
        loaded.size() != m_shaderBytecode.size()) {
        TD_LOG_ERROR(
            "[ParticleRenderer] particle shader package unavailable; particle rendering disabled");
        return false;
    }
    for (size_t index = 0; index < loaded.size(); ++index) {
        m_shaderBytecode[index] = std::move(loaded[index]);
    }
    return true;
}

bool ParticleRenderer::loadGpuBillboardShaderPackage() {
    const container::Array<d3d12::ShaderPackageEntrySpec, 3> entries{{
        {"vertex_file", "particle_gpu_billboard_vs.cso",
         "vertex_profile", "vs_5_0"},
        {"additive_pixel_file", "particle_gpu_billboard_additive_ps.cso",
         "additive_pixel_profile", "ps_5_0"},
        {"alpha_test_pixel_file", "particle_gpu_billboard_alpha_test_ps.cso",
         "alpha_test_pixel_profile", "ps_5_0"},
    }};
    container::Vector<container::Vector<uint8_t>> loaded;
    if (!d3d12::loadShaderPackage(
            "particle_gpu_billboard",
            TD_PARTICLE_STRINGIFY(
                TD_PARTICLE_GPU_BILLBOARD_SHADER_PACKAGE_VERSION),
            TD_PARTICLE_GPU_BILLBOARD_SHADER_SOURCE_SHA256,
            {entries.data(), entries.size()}, loaded) ||
        loaded.size() != m_gpuBillboardShaderBytecode.size()) {
        TD_LOG_WARN(
            "[ParticleRenderer] GPU billboard shader package unavailable; CPU billboard draw remains effective");
        return false;
    }
    for (size_t index = 0; index < loaded.size(); ++index) {
        m_gpuBillboardShaderBytecode[index] = std::move(loaded[index]);
    }
    m_gpuBillboardShaderPackageReady = true;
    TD_LOG_INFO(
        "[ParticleRenderer] GPU billboard shader package loaded (shadow draw contract)");
    return true;
}

void ParticleRenderer::shutdown() {
    m_gpuSimulator.reset();
    m_pendingGpuCommands = {};
    m_gpuCommandNormalizationWork = {};
    m_gpuCommandNormalizationScratch = {};
    m_lastQueuedGpuAuthoredSample = 0;
    m_pendingGpuAuthoredFrames = 0;
    m_gpuMaterialBins = {};
    m_gpuMaterialBinTextureSrvs.clear();
    m_gpuMaterialBinTextureGeneration = 0;
    resetTextureCache();
    releaseSmudgeSceneTargets();
    m_textureCacheHighWater = 0;
    m_reportedGpuResetFailure = false;
    m_reportedGpuCommandFailure = false;
    m_reportedGpuDrawFailure = false;
    m_gpuBillboardIndirectShadowExecuteCalls = 0;
    m_gpuBillboardIndirectVisibleExecuteCalls = 0;
    m_gpuPresentationFallbackFrames = 0;
    m_gpuPresentationGate = {};
    m_gpuPresentationParticleCount = 0;
    m_gpuPresentationParticleBudget = 0;
    m_gpuAbCountExpectations = {};
    m_gpuAbCountSamples = 0;
    m_gpuAbCountMatches = 0;
    m_gpuAbCountMismatches = 0;
    m_gpuAbCountStaleSkipped = 0;
    m_gpuAbCountConsecutiveFrames = 0;
    m_gpuAbVisibilityConsecutiveFrames = 0;
    m_gpuAbLastCpuCount = 0;
    m_gpuAbLastGpuCount = 0;
    m_gpuAbLastCountValid = false;
    m_gpuAbStateExpectations = {};
    m_gpuAbStateSamples = 0;
    m_gpuAbStateMatches = 0;
    m_gpuAbStateMismatches = 0;
    m_gpuAbStateStaleSkipped = 0;
    m_gpuAbStateConsecutiveFrames = 0;
    m_gpuAbLastStateValid = false;
    m_gpuPresentationRequested = false;
    m_gpuVisibilityContractReady = false;
    m_gpuOutputParityVerified = false;
    m_gpuProfileApproved = false;
    m_gpuProfileMinimumParticleCount = 0;
    m_gpuQualificationRevision = 0;
    m_gpuBillboardShaderPackageReady = false;
    m_quadIndexBuffer.Reset();
    m_quadVertexBuffer.Reset();
    m_quadIndexView = {};
    m_quadVertexView = {};
    for (auto& pipeline : m_pipelineStates) pipeline.Reset();
    for (auto& pipeline : m_gpuBillboardPipelineStates) pipeline.Reset();
    for (auto& pipeline : m_gpuBillboardShadowPipelineStates) pipeline.Reset();
    m_gpuBillboardCommandSignature.Reset();
    m_gpuBillboardRootSignature.Reset();
    m_smudgePipelineState.Reset();
    m_smudgeRootSignature.Reset();
    m_rootSignature.Reset();
    for (auto& bytecode : m_shaderBytecode) bytecode.clear();
    for (auto& bytecode : m_gpuBillboardShaderBytecode) bytecode.clear();
    m_buildScratch.reset();
    m_textures.reset();
    m_device = nullptr;
    m_initialized = false;
}

bool ParticleRenderer::configureGpuSimulationInfrastructure(bool enabled) {
    if (!enabled) {
        // The shadow graphics path can leave ExecuteIndirect references in
        // the current queue. Runtime feature changes are rare safe-frame
        // events; fence them before destroying simulator-owned resources.
        if (m_gpuSimulator && m_device && !m_device->waitIdle()) {
            TD_LOG_WARN(
                "[ParticleRenderer] could not fence GPU particle shutdown; infrastructure remains enabled");
            return false;
        }
        m_gpuSimulator.reset();
        m_pendingGpuCommands = {};
        m_gpuCommandNormalizationWork = {};
        m_gpuCommandNormalizationScratch = {};
        m_lastQueuedGpuAuthoredSample = 0;
        m_pendingGpuAuthoredFrames = 0;
        m_reportedGpuResetFailure = false;
        m_reportedGpuCommandFailure = false;
        m_reportedGpuDrawFailure = false;
        m_gpuBillboardIndirectShadowExecuteCalls = 0;
        m_gpuBillboardIndirectVisibleExecuteCalls = 0;
        m_gpuPresentationFallbackFrames = 0;
        m_gpuPresentationGate = {};
        m_gpuPresentationParticleCount = 0;
        m_gpuPresentationParticleBudget = 0;
        m_gpuAbCountExpectations = {};
        m_gpuAbCountSamples = 0;
        m_gpuAbCountMatches = 0;
        m_gpuAbCountMismatches = 0;
        m_gpuAbCountStaleSkipped = 0;
        m_gpuAbCountConsecutiveFrames = 0;
        m_gpuAbVisibilityConsecutiveFrames = 0;
        m_gpuAbLastCpuCount = 0;
        m_gpuAbLastGpuCount = 0;
        m_gpuAbLastCountValid = false;
        m_gpuAbStateExpectations = {};
        m_gpuAbStateSamples = 0;
        m_gpuAbStateMatches = 0;
        m_gpuAbStateMismatches = 0;
        m_gpuAbStateStaleSkipped = 0;
        m_gpuAbStateConsecutiveFrames = 0;
        m_gpuAbLastStateValid = false;
        m_gpuPresentationRequested = false;
        m_gpuVisibilityContractReady = false;
        m_gpuOutputParityVerified = false;
        m_gpuProfileApproved = false;
        m_gpuProfileMinimumParticleCount = 0;
        m_gpuQualificationRevision = 0;
        return true;
    }
    if (!m_initialized || !m_device) return false;
    if (m_gpuSimulator && m_gpuSimulator->isInitialized()) return true;
    auto simulator = std::make_unique<GpuParticleSimulator>();
    if (!simulator->init(
            *m_device, static_cast<uint32_t>(
                particle_render::performance_limits::
                    kHardMaximumSourceParticles))) {
        TD_LOG_WARN(
            "[ParticleRenderer] GPU Compute infrastructure unavailable; CPU backend remains effective");
        return false;
    }
    if (!m_gpuMaterialBins.templateToBin.empty() &&
        !simulator->configureMaterialBins(
            m_gpuMaterialBins.templateToBin,
            static_cast<uint32_t>(m_gpuMaterialBins.bins.size()))) {
        TD_LOG_WARN(
            "[ParticleRenderer] GPU material mapping unavailable; CPU backend remains effective");
        return false;
    }
    m_gpuSimulator = std::move(simulator);
    m_reportedGpuResetFailure = false;
    m_reportedGpuCommandFailure = false;
    m_reportedGpuDrawFailure = false;
    m_gpuBillboardIndirectShadowExecuteCalls = 0;
    m_gpuBillboardIndirectVisibleExecuteCalls = 0;
    m_gpuPresentationFallbackFrames = 0;
    m_gpuAbCountExpectations = {};
    m_gpuAbCountSamples = 0;
    m_gpuAbCountMatches = 0;
    m_gpuAbCountMismatches = 0;
    m_gpuAbCountStaleSkipped = 0;
    m_gpuAbCountConsecutiveFrames = 0;
    m_gpuAbVisibilityConsecutiveFrames = 0;
    m_gpuAbLastCpuCount = 0;
    m_gpuAbLastGpuCount = 0;
    m_gpuAbLastCountValid = false;
    m_gpuAbStateExpectations = {};
    m_gpuAbStateSamples = 0;
    m_gpuAbStateMatches = 0;
    m_gpuAbStateMismatches = 0;
    m_gpuAbStateStaleSkipped = 0;
    m_gpuAbStateConsecutiveFrames = 0;
    m_gpuAbLastStateValid = false;
    return true;
}

void ParticleRenderer::requestGpuSimulationReset(
    uint64_t authorityEpoch) noexcept {
    if (m_gpuSimulator) m_gpuSimulator->requestReset(authorityEpoch);
    m_gpuVisibilityContractReady = false;
    m_gpuAbCountConsecutiveFrames = 0;
    m_gpuAbVisibilityConsecutiveFrames = 0;
    m_gpuAbStateConsecutiveFrames = 0;
}

void ParticleRenderer::queueGpuSimulationCommands(
    fx::ParticleGpuCommandBatch batch,
    uint64_t authoredSampleOrdinal,
    uint32_t authoredFrames) {
    if (!m_gpuSimulator || !m_gpuSimulator->isInitialized()) return;

    const bool authorityChanged =
        m_pendingGpuCommands.authorityEpoch == 0 ||
        m_pendingGpuCommands.authorityEpoch != batch.authorityEpoch;
    fx::mergeGpuParticleCommandBatchRetained(
        m_pendingGpuCommands, std::move(batch),
        m_gpuSimulator->stats().capacity,
        m_gpuCommandNormalizationScratch,
        m_gpuCommandNormalizationWork);
    if (authorityChanged) {
        m_lastQueuedGpuAuthoredSample = 0;
        m_pendingGpuAuthoredFrames = 0;
    }

    if (authoredSampleOrdinal != 0 &&
        authoredSampleOrdinal != m_lastQueuedGpuAuthoredSample) {
        const uint64_t accumulatedFrames =
            static_cast<uint64_t>(m_pendingGpuAuthoredFrames) +
            authoredFrames;
        m_pendingGpuAuthoredFrames = static_cast<uint32_t>(
            std::min<uint64_t>(
                accumulatedFrames,
                std::numeric_limits<uint32_t>::max()));
        m_lastQueuedGpuAuthoredSample = authoredSampleOrdinal;
    }
}

bool ParticleRenderer::gpuSimulationInfrastructureReady() const noexcept {
    return m_gpuSimulator && m_gpuSimulator->isInitialized();
}

GpuParticleSimulationRenderStats
ParticleRenderer::gpuSimulationStats() const noexcept {
    GpuParticleSimulationRenderStats result;
    result.presentationRejectionMask = static_cast<uint32_t>(
        m_gpuPresentationGate.rejection);
    result.presentationParticleCount = static_cast<uint32_t>(
        std::min<size_t>(m_gpuPresentationParticleCount,
                         std::numeric_limits<uint32_t>::max()));
    result.presentationParticleBudget = static_cast<uint32_t>(
        std::min<size_t>(m_gpuPresentationParticleBudget,
                         std::numeric_limits<uint32_t>::max()));
    result.effectiveGpuPresentation =
        m_gpuPresentationGate.effectiveGpuPresentation;
    if (!m_gpuSimulator) return result;

    const GpuParticleSimulator::Stats& source = m_gpuSimulator->stats();
    result.resetDispatches = source.resetDispatches;
    result.commandDispatches = source.commandDispatches;
    result.integrationDispatches = source.integrationDispatches;
    result.aliveCompactDispatches = source.aliveCompactDispatches;
    result.aliveCompactCounterResetDispatches =
        source.aliveCompactCounterResetDispatches;
    result.visibleCompactDispatches = source.visibleCompactDispatches;
    result.visibleCompactCounterResetDispatches =
        source.visibleCompactCounterResetDispatches;
    result.materialBinResetDispatches = source.materialBinResetDispatches;
    result.materialBinCountDispatches = source.materialBinCountDispatches;
    result.materialBinPrefixDispatches = source.materialBinPrefixDispatches;
    result.materialBinScatterDispatches = source.materialBinScatterDispatches;
    result.indirectArgumentBuilds = source.indirectArgumentBuilds;
    result.indirectArgumentTransitions = source.indirectArgumentTransitions;
    result.indirectDrawResourceTransitions =
        source.indirectDrawResourceTransitions;
    result.indirectShadowExecuteCalls =
        m_gpuBillboardIndirectShadowExecuteCalls;
    result.indirectVisibleExecuteCalls =
        m_gpuBillboardIndirectVisibleExecuteCalls;
    result.presentationFallbackFrames =
        m_gpuPresentationFallbackFrames;
    result.visibilityAuthorityPublishes =
        source.visibilityAuthorityPublishes;
    result.visibilityAuthorityEntries =
        source.visibilityAuthorityEntries;
    result.visibilityAuthorityBytes =
        source.visibilityAuthorityBytes;
    result.visibilityAuthorityRejects =
        source.visibilityAuthorityRejects;
    result.abCountReadbacks = source.diagnosticCounterReadbacks;
    result.abCountSamples = m_gpuAbCountSamples;
    result.abCountMatches = m_gpuAbCountMatches;
    result.abCountMismatches = m_gpuAbCountMismatches;
    result.abCountStaleSkipped = m_gpuAbCountStaleSkipped;
    result.abLastCpuCount = m_gpuAbLastCpuCount;
    result.abLastGpuCount = m_gpuAbLastGpuCount;
    result.abLastCountValid = m_gpuAbLastCountValid;
    result.abCountConsecutiveFrames =
        m_gpuAbCountConsecutiveFrames;
    result.abVisibilityConsecutiveFrames =
        m_gpuAbVisibilityConsecutiveFrames;
    result.abStateReadbacks = source.diagnosticStateReadbacks;
    result.abStateCopies = source.diagnosticStateCopies;
    result.abStateSamples = m_gpuAbStateSamples;
    result.abStateMatches = m_gpuAbStateMatches;
    result.abStateMismatches = m_gpuAbStateMismatches;
    result.abStateStaleSkipped = m_gpuAbStateStaleSkipped;
    result.abLastStateValid = m_gpuAbLastStateValid;
    result.abStateConsecutiveFrames =
        m_gpuAbStateConsecutiveFrames;
    result.submittedBirthCommands = source.submittedBirthCommands;
    result.submittedRetireCommands = source.submittedRetireCommands;
    result.rejectedCommands = source.rejectedCommands;
    result.transitionBarriers = source.transitionBarriers;
    result.uavBarriers = source.uavBarriers;
    const auto& queueScratch = m_gpuCommandNormalizationScratch.stats();
    result.normalizationSlotScratchCapacity = std::max(
        queueScratch.slotCapacity,
        static_cast<size_t>(source.normalizationSlotScratchCapacity));
    result.normalizationSlotScratchHighWater = std::max(
        queueScratch.slotHighWater,
        static_cast<size_t>(source.normalizationSlotScratchHighWater));
    result.normalizationBirthScratchCapacity = std::max(
        queueScratch.birthOutputCapacity,
        static_cast<size_t>(source.normalizationBirthScratchCapacity));
    result.normalizationRetireScratchCapacity = std::max(
        queueScratch.retireOutputCapacity,
        static_cast<size_t>(source.normalizationRetireScratchCapacity));
    result.normalizationScratchCapacityGrowths =
        queueScratch.capacityGrowths +
        source.normalizationScratchCapacityGrowths;
    result.aliveIndexCapacity = source.aliveIndexCapacity;
    result.visibleIndexCapacity = source.visibleIndexCapacity;
    result.materialTemplateMapCount = source.materialTemplateMapCount;
    result.materialBinCount = source.materialBinCount;
    result.materialBinCapacity = source.materialBinCapacity;
    result.diagnosticAliveCount = source.diagnosticAliveCount;
    result.diagnosticAliveOverflow = source.diagnosticAliveOverflow;
    result.diagnosticAliveCountsValid =
        source.diagnosticAliveCountsValid;
    result.capacity = source.capacity;
    result.authorityEpoch = source.authorityEpoch;
    result.infrastructureReady = m_gpuSimulator->isInitialized();
    result.indirectShadowDrawReady =
        m_gpuBillboardShaderPackageReady &&
        m_gpuBillboardRootSignature &&
        m_gpuBillboardCommandSignature;
    result.qualificationRevision = m_gpuQualificationRevision;
    result.profileMinimumParticleCount = static_cast<uint32_t>(
        std::min<size_t>(m_gpuProfileMinimumParticleCount,
            std::numeric_limits<uint32_t>::max()));
    result.outputParityApproved = m_gpuOutputParityVerified;
    result.profileApproved = m_gpuProfileApproved;
    return result;
}

void ParticleRenderer::updateGpuPresentationGate(
    bool requested, size_t particleCount, size_t particleBudget) noexcept {
    m_gpuPresentationRequested = requested;
    m_gpuPresentationParticleCount = particleCount;
    m_gpuPresentationParticleBudget = particleBudget;
    m_gpuPresentationGate = evaluateGpuParticlePresentationGate({
        .requested = requested,
        .infrastructureReady = gpuSimulationInfrastructureReady(),
        .graphicsReady = m_gpuBillboardShaderPackageReady &&
            m_gpuBillboardRootSignature &&
            m_gpuBillboardCommandSignature,
        .visibilityContractReady = m_gpuVisibilityContractReady,
        .outputParityVerified = m_gpuOutputParityVerified,
        .countParityVerified = m_gpuAbCountConsecutiveFrames >=
                d3d12::performance_limits::
                    kGpuParticleCountParityRequiredSamples,
        .stateParityVerified = m_gpuAbStateConsecutiveFrames >=
                d3d12::performance_limits::
                    kGpuParticleStateParityRequiredSamples,
        .visibilityParityVerified =
            m_gpuAbVisibilityConsecutiveFrames >=
                d3d12::performance_limits::
                    kGpuParticleCountParityRequiredSamples,
        .profileApproved = m_gpuProfileApproved,
        .materialMappingComplete =
            m_gpuMaterialBins.rejectedCompatibleTemplates == 0,
        .particleCount = particleCount,
        .particleBudget = particleBudget,
        .profileMinimumParticleCount =
            m_gpuProfileMinimumParticleCount,
    });
}

void ParticleRenderer::configureGpuPresentationQualification(
    GpuParticlePresentationQualification qualification) noexcept {
    if (qualification.revision == 0 ||
        qualification.revision < m_gpuQualificationRevision) {
        return;
    }
    if (qualification.revision == m_gpuQualificationRevision &&
        m_gpuOutputParityVerified ==
            qualification.outputParityApproved &&
        m_gpuProfileApproved == qualification.profileApproved &&
        m_gpuProfileMinimumParticleCount ==
            qualification.minimumParticleCount) {
        return;
    }
    m_gpuQualificationRevision = qualification.revision;
    m_gpuOutputParityVerified = qualification.outputParityApproved;
    m_gpuProfileApproved = qualification.profileApproved;
    m_gpuProfileMinimumParticleCount =
        qualification.minimumParticleCount;
}

GpuParticleMaterialBinCompilation ParticleRenderer::compileGpuMaterialBins(
    const fx::ParticleSystemCatalog& catalog, size_t maximumBins) {
    maximumBins = std::min(
        maximumBins,
        particle_render::performance_limits::kHardMaximumSourceParticles);
    return compileGpuParticleMaterialBins(catalog, maximumBins);
}

void ParticleRenderer::configureGpuMaterialBins(
    const fx::ParticleSystemCatalog& catalog) {
    GpuParticleMaterialBinCompilation compiled =
        compileGpuMaterialBins(catalog);
    if (m_gpuSimulator && m_gpuSimulator->isInitialized() &&
        !m_gpuSimulator->configureMaterialBins(
            compiled.templateToBin,
            static_cast<uint32_t>(compiled.bins.size()))) {
        TD_LOG_WARN(
            "[ParticleRenderer] GPU material mapping publish failed; retaining prior shadow mapping and CPU backend");
        return;
    }
    TD_LOG_INFO(
        "[ParticleRenderer] GPU material mapping compiled templates={} bins={} rejected={} (shadow backend)",
        compiled.mappedTemplates, compiled.bins.size(),
        compiled.rejectedCompatibleTemplates);
    m_gpuMaterialBins = std::move(compiled);
    m_gpuMaterialBinTextureSrvs.clear();
    m_gpuMaterialBinTextureGeneration = 0;
    m_gpuAbCountConsecutiveFrames = 0;
    m_gpuAbVisibilityConsecutiveFrames = 0;
    m_gpuAbStateConsecutiveFrames = 0;
    m_gpuOutputParityVerified = false;
    m_gpuProfileApproved = false;
    m_gpuProfileMinimumParticleCount = 0;
    m_gpuQualificationRevision = 0;
}

void ParticleRenderer::resetTextureCache() {
    if (m_textures) {
        for (const auto& [textureName, srv] : m_textureSrvs) {
            (void)srv;
            if (!textureName.empty()) m_textures->release(textureName);
        }
    }
    m_textureSrvs.clear();
    ++m_textureBindingGeneration;
    if (m_textureBindingGeneration == 0) ++m_textureBindingGeneration;
    m_gpuMaterialBinTextureSrvs.clear();
    m_gpuMaterialBinTextureGeneration = 0;
    m_gpuOutputParityVerified = false;
    m_gpuQualificationRevision = 0;
}

bool ParticleRenderer::configureTextureSampling(
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
    const bool cpuReady = createRootSignature() && createPipelineStates();
    if (!cpuReady) return false;
    if (m_gpuBillboardShaderPackageReady) {
        for (auto& pipeline : m_gpuBillboardPipelineStates) pipeline.Reset();
        for (auto& pipeline : m_gpuBillboardShadowPipelineStates) {
            pipeline.Reset();
        }
        m_gpuBillboardCommandSignature.Reset();
        m_gpuBillboardRootSignature.Reset();
        if (!createGpuBillboardPipeline()) {
            TD_LOG_WARN(
                "[ParticleRenderer] GPU billboard pipeline rebuild failed; CPU billboard draw remains effective");
            m_gpuBillboardShaderPackageReady = false;
        }
    }
    return true;
}

bool ParticleRenderer::publishGpuVisibilityAuthority(
    const ParticleRenderDrawList& drawList) {
    if (!m_gpuSimulator || !m_gpuSimulator->isInitialized()) {
        m_gpuVisibilityContractReady = false;
        return false;
    }
    m_gpuVisibilityContractReady =
        m_gpuSimulator->publishVisibilityAuthority(
            drawList.gpuVisibilityGenerations);
    return m_gpuVisibilityContractReady;
}

void ParticleRenderer::prepareTextureBindings(ParticleRenderDrawList& drawList) {
    const auto started = std::chrono::steady_clock::now();
    for (ParticleRenderBatch& batch : drawList.batches) {
        batch.textureSrvIndex = textureSrv(batch.textureName);
    }
    drawList.textureBindingGeneration = m_textureBindingGeneration;
    m_gpuMaterialBinTextureSrvs.assign(m_gpuMaterialBins.bins.size(), 0u);
    if (m_gpuPresentationGate.effectiveGpuPresentation) {
        for (size_t index = 0; index < m_gpuMaterialBins.bins.size(); ++index) {
            const GpuParticleMaterialBin& bin = m_gpuMaterialBins.bins[index];
            // Bins deduplicate on a case-folded key while keeping the first
            // template's authored casing, so activity must be tested the same
            // way; a case-sensitive compare would leave this descriptor
            // unbound whenever only a differently-cased template is visible.
            const bool active = std::any_of(
                drawList.batches.begin(), drawList.batches.end(),
                [&bin](const ParticleRenderBatch& batch) {
                    return gpuParticleMaterialBinMatches(
                        bin, batch.shader, batch.textureName);
                });
            if (active) {
                m_gpuMaterialBinTextureSrvs[index] =
                    textureSrv(bin.textureName);
            }
        }
    } else {
        // Shadow validation suppresses color/depth writes and only verifies
        // simulation/output contracts. Loading every catalog material here
        // eagerly touched unused stock definitions whose textures were never
        // shipped. CPU-authoritative batches above already acquire exactly
        // the textures visible this frame, matching legacy demand loading.
    }
    m_gpuMaterialBinTextureGeneration = m_textureBindingGeneration;
    drawList.stats.textureBindingMicroseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count());
}

size_t ParticleRenderer::renderSmudges(
    const ParticleRenderDrawList& drawList,
    const RenderCameraSnapshot& cameraSnapshot,
    bool heatEffectsEnabled,
    const LocalVisibilityRenderSnapshot& localVisibility) {
    if (!heatEffectsEnabled || drawList.smudgeInstances.empty() ||
        !m_initialized || !m_device || !m_smudgePipelineState ||
        !m_smudgeRootSignature || m_device->width() == 0 ||
        m_device->height() == 0 || !m_device->commandList() ||
        !ensureSmudgeSceneTargets(m_device->width(), m_device->height())) {
        return 0;
    }

    const uint32_t frameIndex = m_device->frameIndex();
    if (frameIndex >= m_smudgeSceneTargets.size() ||
        !m_smudgeSceneTargets[frameIndex] ||
        m_smudgeSceneSrvs[frameIndex] == UINT32_MAX) {
        return 0;
    }
    ID3D12Resource* sceneTarget = m_smudgeSceneTargets[frameIndex].Get();
    ID3D12Resource* renderTarget = m_device->currentRenderTarget();
    ID3D12GraphicsCommandList* commandList = m_device->commandList();
    if (!sceneTarget || !renderTarget) return 0;
    const bool sceneCopyTimestampActive = m_device->beginGpuTimestamp(
        GpuTimestampRange::SceneColorCopy);

    m_device->flushBatch();
    container::Array<D3D12_RESOURCE_BARRIER, 2> toCopy{};
    size_t toCopyCount = 0;
    if (m_smudgeSceneStates[frameIndex] !=
        D3D12_RESOURCE_STATE_COPY_DEST) {
        toCopy[toCopyCount++] = transitionBarrier(
            sceneTarget, m_smudgeSceneStates[frameIndex],
            D3D12_RESOURCE_STATE_COPY_DEST);
    }
    toCopy[toCopyCount++] = transitionBarrier(
        renderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->ResourceBarrier(static_cast<UINT>(toCopyCount),
                                 toCopy.data());
    commandList->CopyResource(sceneTarget, renderTarget);
    ++m_sceneColorStats.copyCalls;
    m_sceneColorStats.copiedBytes +=
        m_smudgeSceneTargetAllocationBytes[frameIndex];
    container::Array<D3D12_RESOURCE_BARRIER, 2> toDraw{
        transitionBarrier(sceneTarget, D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        transitionBarrier(renderTarget, D3D12_RESOURCE_STATE_COPY_SOURCE,
                          D3D12_RESOURCE_STATE_RENDER_TARGET),
    };
    commandList->ResourceBarrier(static_cast<UINT>(toDraw.size()),
                                 toDraw.data());
    m_smudgeSceneStates[frameIndex] =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_device->bindMainRenderTargets();
    if (sceneCopyTimestampActive) {
        static_cast<void>(m_device->endGpuTimestamp(
            GpuTimestampRange::SceneColorCopy));
    }

    const WorldCamera camera = WorldCamera::fromSnapshot(cameraSnapshot);
    const float aspectRatio = static_cast<float>(m_device->width()) /
        static_cast<float>(m_device->height());
    const math::float4x4 viewProjection =
        camera.viewProjectionMatrix(aspectRatio);
    math::vec3 forward = camera.target() - camera.position();
    forward = forward.length_sq() > math::EPSILON * math::EPSILON
        ? forward.normalized() : math::vec3{0.0f, 1.0f, 0.0f};
    math::vec3 up = camera.up().length_sq() > math::EPSILON * math::EPSILON
        ? camera.up().normalized() : WorldCamera::worldUp();
    math::vec3 right = forward.cross(up);
    if (right.length_sq() <= math::EPSILON * math::EPSILON) {
        up = std::abs(forward.z()) < 0.999f
            ? WorldCamera::worldUp() : math::vec3{0.0f, 1.0f, 0.0f};
        right = forward.cross(up);
    }
    right = right.normalized();
    up = right.cross(forward).normalized();

    if (!m_buildScratch) {
        m_buildScratch = std::make_unique<ParticleRendererScratch>();
    }
    container::Vector<SmudgeVertex>& vertices = m_buildScratch->smudgeVertices;
    vertices.clear();
    const size_t maximumSmudges = std::min(
        drawList.smudgeInstances.size(),
        static_cast<size_t>(std::numeric_limits<uint32_t>::max() / 12u));
    vertices.reserve(maximumSmudges * 12u);
    constexpr uint32_t triangleCorners[12] = {
        0, 4, 1, 1, 4, 2, 2, 4, 3, 3, 4, 0};
    for (size_t index = 0; index < maximumSmudges; ++index) {
        const SmudgeRenderInstance& smudge = drawList.smudgeInstances[index];
        if (!std::isfinite(smudge.position[0]) ||
            !std::isfinite(smudge.position[1]) ||
            !std::isfinite(smudge.position[2]) ||
            !std::isfinite(smudge.size) || smudge.size <= 0.0f ||
            !std::isfinite(smudge.opacity) || smudge.opacity <= 0.0f) {
            continue;
        }
        const math::vec3 center{smudge.position[0], smudge.position[1],
                                smudge.position[2]};
        const float halfSize = smudge.size * 0.5f;
        const container::Array<math::vec3, 5> points{
            center - right * halfSize - up * halfSize,
            center - right * halfSize + up * halfSize,
            center + right * halfSize + up * halfSize,
            center + right * halfSize - up * halfSize,
            center,
        };
        for (const uint32_t pointIndex : triangleCorners) {
            SmudgeVertex vertex;
            vertex.position[0] = points[pointIndex].x();
            vertex.position[1] = points[pointIndex].y();
            vertex.position[2] = points[pointIndex].z();
            if (pointIndex == 4) {
                vertex.uvOffset[0] = std::clamp(
                    smudge.uvOffset[0], -kMaximumSmudgeUvOffset,
                    kMaximumSmudgeUvOffset);
                vertex.uvOffset[1] = std::clamp(
                    smudge.uvOffset[1], -kMaximumSmudgeUvOffset,
                    kMaximumSmudgeUvOffset);
                vertex.alpha = std::clamp(smudge.opacity, 0.0f, 1.0f);
            }
            vertices.push_back(vertex);
        }
    }
    if (vertices.empty()) {
        return 0;
    }
    if (vertices.size() > std::numeric_limits<uint32_t>::max() /
            sizeof(SmudgeVertex)) {
        vertices.clear();
        return 0;
    }

    const uint32_t vertexBytes = static_cast<uint32_t>(
        vertices.size() * sizeof(SmudgeVertex));
    const size_t renderedSmudges = vertices.size() / 12u;
    const auto uploadStarted = std::chrono::steady_clock::now();
    const d3d12::FrameUploadAllocation vertexAllocation =
        m_device->allocateFrameUpload(vertices.data(), vertexBytes,
                                      alignof(SmudgeVertex));
    SmudgeCameraConstants constants{};
    std::memcpy(constants.viewProjection, &viewProjection.m,
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
    const d3d12::ConstantBufferAllocation cameraAllocation =
        m_device->allocateConstantBuffer(&constants, sizeof(constants));
    m_executionStats.smudgeUploadBytes = vertexBytes;
    m_executionStats.smudgeUploadMicroseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - uploadStarted).count());
    // The upload arena owns the copied bytes until the backbuffer fence; no
    // live CPU element is retained, only bounded vector capacity.
    vertices.clear();
    if (!vertexAllocation || !cameraAllocation) return 0;

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_device->width());
    const uint32_t tacticalHeight =
        camera.tacticalViewportHeight(m_device->height());
    viewport.Height = static_cast<float>(tacticalHeight);
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(m_device->width()),
                             static_cast<LONG>(tacticalHeight)};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    m_device->bindSrvHeap();
    commandList->SetGraphicsRootSignature(m_smudgeRootSignature.Get());
    m_device->recordGraphicsRootSignatureCall();
    commandList->SetPipelineState(m_smudgePipelineState.Get());
    m_device->recordPipelineStateCall();
    commandList->SetGraphicsRootConstantBufferView(
        0, cameraAllocation.gpuAddress);
    commandList->SetGraphicsRootDescriptorTable(
        1, m_device->getSrvGpuHandle(m_smudgeSceneSrvs[frameIndex]));
    m_device->recordGraphicsDescriptorTableCall();
    const D3D12_VERTEX_BUFFER_VIEW vertexView{
        .BufferLocation = vertexAllocation.gpuAddress,
        .SizeInBytes = vertexBytes,
        .StrideInBytes = sizeof(SmudgeVertex),
    };
    commandList->IASetVertexBuffers(0, 1, &vertexView);
    m_device->recordVertexBufferCall();
    commandList->IASetIndexBuffer(nullptr);
    m_device->recordIndexBufferCall();
    commandList->IASetPrimitiveTopology(
        D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const auto drawStarted = std::chrono::steady_clock::now();
    for (size_t first = 0; first < renderedSmudges;
         first += kSmudgesPerDraw) {
        const size_t count = std::min(
            kSmudgesPerDraw, renderedSmudges - first);
        commandList->DrawInstanced(
            static_cast<UINT>(count * 12u), 1,
            static_cast<UINT>(first * 12u), 0);
        m_device->recordDrawCall();
        ++m_executionStats.smudgeDrawCalls;
    }
    m_executionStats.smudgeDrawRecordMicroseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - drawStarted).count());
    return renderedSmudges;
}

} // namespace engine::render
