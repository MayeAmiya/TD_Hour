#include "DX12RendererWorldAssetRuntime.h"
#include "UiSrvInvalidation.h"
#include "core/debug/debug.h"
#include "engine/renderer/d3d12/runtime/D3D12PerformanceSettings.h"
#include "engine/renderer/d3d12/runtime/D3D12QualitySettings.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace engine {

namespace {

void writeLittleEndian16(container::Array<uint8_t, 54>& header,
                         size_t offset, uint16_t value) noexcept {
    header[offset] = static_cast<uint8_t>(value & 0xffu);
    header[offset + 1u] = static_cast<uint8_t>((value >> 8u) & 0xffu);
}

void writeLittleEndian32(container::Array<uint8_t, 54>& header,
                         size_t offset, uint32_t value) noexcept {
    for (uint32_t byte = 0; byte < 4u; ++byte) {
        header[offset + byte] = static_cast<uint8_t>(
            (value >> (byte * 8u)) & 0xffu);
    }
}

[[nodiscard]] bool writeReadbackBmp(
    const d3d12::RenderTargetReadback& readback,
    const std::filesystem::path& filename) {
    if (!readback.valid() || filename.empty() ||
        readback.width > static_cast<uint32_t>(
            std::numeric_limits<int32_t>::max()) ||
        readback.height > static_cast<uint32_t>(
            std::numeric_limits<int32_t>::max())) {
        return false;
    }
    const uint64_t pixelBytes64 = readback.rgba.size();
    const uint64_t fileBytes64 = 54u + pixelBytes64;
    if (fileBytes64 > std::numeric_limits<uint32_t>::max()) return false;
    std::error_code directoryError;
    if (filename.has_parent_path()) {
        std::filesystem::create_directories(
            filename.parent_path(), directoryError);
        if (directoryError) return false;
    }
    container::Array<uint8_t, 54> header{};
    header[0] = 'B';
    header[1] = 'M';
    writeLittleEndian32(
        header, 2, static_cast<uint32_t>(fileBytes64));
    writeLittleEndian32(header, 10, 54u);
    writeLittleEndian32(header, 14, 40u);
    writeLittleEndian32(header, 18, readback.width);
    writeLittleEndian32(
        header, 22,
        static_cast<uint32_t>(-static_cast<int32_t>(readback.height)));
    writeLittleEndian16(header, 26, 1u);
    writeLittleEndian16(header, 28, 32u);
    writeLittleEndian32(
        header, 34, static_cast<uint32_t>(pixelBytes64));
    container::Vector<uint8_t> bgra(readback.rgba.size());
    for (size_t pixel = 0; pixel < readback.rgba.size(); pixel += 4u) {
        bgra[pixel] = readback.rgba[pixel + 2u];
        bgra[pixel + 1u] = readback.rgba[pixel + 1u];
        bgra[pixel + 2u] = readback.rgba[pixel];
        bgra[pixel + 3u] = readback.rgba[pixel + 3u];
    }
    std::ofstream output(filename, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(
        reinterpret_cast<const char*>(header.data()),
        static_cast<std::streamsize>(header.size()));
    output.write(
        reinterpret_cast<const char*>(bgra.data()),
        static_cast<std::streamsize>(bgra.size()));
    output.flush();
    return output.good();
}

} // namespace

void DX12Renderer::beginFrame() {
    if (!m_d3d12.beginFrame()) {
        throw std::runtime_error("D3D12 beginFrame failed");
    }
    processUiSrvInvalidations();
    ++m_uiSrvCacheFrame;
    if (m_uiSrvCacheFrame == 0) m_uiSrvCacheFrame = 1;
    if (m_uiSrvCacheFrame %
            d3d12::performance_limits::kUiSrvPruneIntervalFrames == 0) {
        pruneUiSrvCaches();
    }
    if (m_worldAssets) {
        m_worldAssets->residency.assets.notifyBeginFrameComplete();
        m_worldAssets->residency.assets.beginResidencyFrame(m_d3d12.frameOrdinal());
        m_worldAssets->residency.textures->beginResidencyFrame(m_d3d12.frameOrdinal());
        m_worldAssets->residency.textures->pumpCpuCompletions();
        const bool activePlay = m_worldAssets->lifetime.started &&
            !m_worldAssets->lifetime.loading;
        const render::RenderAssetReadyBudget textureUploadBudget{
            .maxItems = activePlay
                ? render::performance_limits::kTextureUploadsPerFrame
                : render::performance_limits::kTextureUploadsPerLoadingFrame,
            .maxBytes = activePlay
                ? render::performance_limits::kTextureUploadBytesPerFrame
                : render::performance_limits::kTextureUploadBytesPerLoadingFrame,
            .maxElapsedMicroseconds = activePlay
                ? render::performance_limits::kTextureUploadMicrosecondsPerFrame
                : render::performance_limits::kTextureUploadMicrosecondsPerLoadingFrame,
        };
        m_worldAssets->residency.textures->processGpuUploads(textureUploadBudget);
        const render::W3dGpuUploadBudget modelUploadBudget{
            .maxUploads = d3d12::modelUploadBudgetForFrame(
                activePlay,
                m_worldAssets->quality.modelUploadsPerFrame,
                m_worldAssets->quality.modelUploadsPerLoadingFrame),
            .maxEstimatedBytes = activePlay
                ? m_worldAssets->quality.modelUploadBytesPerFrame
                : m_worldAssets->quality.modelUploadBytesPerLoadingFrame,
            .maxElapsedMicroseconds = activePlay
                ? m_worldAssets->quality.modelUploadMicrosecondsPerFrame
                : m_worldAssets->quality.modelUploadMicrosecondsPerLoadingFrame,
        };
        m_worldAssets->residency.assets.processGpuUploads(
            [this](const render::W3dGpuUploadRequest& request) {
                render::W3dGpuUploadResult result;
                bool deferred = false;
                const d3d12::GpuRetirementIdentity retirementIdentity{
                    .identityHash =
                        (static_cast<uint64_t>(request.handle.generation) << 32u) |
                        (static_cast<uint64_t>(request.handle.index) + 1u),
                    .generation = request.handle.generation,
                    .revision = request.revision,
                };
                auto model = render::D3D12W3dModel::upload(
                    m_d3d12, m_worldAssets->residency.textures, *request.cpuModel,
                    retirementIdentity, request.priority, &result.error,
                    &deferred);
                result.deferred = deferred;
                if (model) {
                    const container::String sourcePath = m_worldAssets->residency.assets.sourcePath(request.handle);
                    const auto bindings = model->materialTextureBindings();
                    size_t baseBound = 0;
                    size_t baseFallback = 0;
                    size_t detailBound = 0;
                    size_t detailFallback = 0;
                    for (const auto& binding : bindings) {
                        if (!binding.textureName.empty()) {
                            if (binding.textureSrvIndex == 0) ++baseFallback;
                            else ++baseBound;
                            m_worldAssets->residency.assets.recordTextureDependency(
                                request.handle, binding.textureName,
                                binding.textureSrvIndex != 0u);
                        }
                        if (!binding.detailTextureName.empty()) {
                            if (binding.detailTextureSrvIndex == 0) ++detailFallback;
                            else ++detailBound;
                            m_worldAssets->residency.assets.recordTextureDependency(
                                request.handle, binding.detailTextureName,
                                binding.detailTextureSrvIndex != 0u);
                        }
                    }
                    TD_LOG_INFO("[DX12Renderer] W3D GPU ready: handle={}:{} revision={} meshes={} skinnedMeshes={} "
                                "primitives={} textures={}",
                        request.handle.index, request.handle.generation, request.revision,
                        model->meshCount(), model->skinnedMeshCount(), model->primitiveCount(),
                        m_worldAssets->residency.textures->residentTextureCount());
                    // Emit this exactly when an immutable W3D becomes GPU-ready.
                    // It lets a real-map capture distinguish a source material
                    // with no texture stage from a named texture that fell back
                    // to the white SRV due to VFS/decoder resolution.
                    TD_LOG_INFO("[DX12Renderer] W3D texture bindings: source='{}' model='{}' materials={} baseBound={} "
                                "baseFallback={} detailBound={} detailFallback={}",
                        sourcePath, request.cpuModel->name, bindings.size(), baseBound, baseFallback,
                        detailBound, detailFallback);
                    for (const auto& binding : bindings) {
                        TD_LOG_INFO("[DX12Renderer] W3D texture binding: source='{}' material={} base='{}' srv={} "
                                    "detail='{}' detailSrv={}",
                            sourcePath, binding.materialIndex, binding.textureName,
                            binding.textureSrvIndex, binding.detailTextureName,
                            binding.detailTextureSrvIndex);
                    }
                }
                result.model = std::move(model);
                return result;
            },
            modelUploadBudget);
        m_worldAssets->residency.assets.trimGpuResidency(
            render::performance_limits::kW3dResidentModelLimit,
            render::performance_limits::kW3dResidentModelBytes,
            render::performance_limits::kRenderAssetResidencyGraceFrames);
        m_worldAssets->residency.textures->trimResidency(
            render::performance_limits::kWorldTextureResidentLimit,
            render::performance_limits::kWorldTextureResidentBytes,
            render::performance_limits::kRenderAssetResidencyGraceFrames);
    }
}

void DX12Renderer::endFrame() {
    const bool submitted = m_d3d12.endFrame();
    if (m_worldAssets) m_worldAssets->residency.assets.notifyFrameSubmitted();
    if (submitted && m_worldAssets &&
        m_worldAssets->stats.submissionPending()) {
        render::WorldFrameRenderStats& frameStats =
            m_worldAssets->stats.frame();
        frameStats.frameUpload = m_d3d12.lastFrameUploadStats();
        // Seal command/binding and retirement observations only after the
        // device has flushed UI batches, closed the list and attributed the
        // submission fence. finalizeWorldFrameStats() runs before normal UI
        // submission and therefore cannot publish a complete frame itself.
        frameStats.renderBindings =
            m_d3d12.renderBindingStats();
        frameStats.worldResources =
            m_d3d12.currentWorldResourceStateStats();
        frameStats.srvDescriptors =
            m_d3d12.srvDescriptorStats();
        frameStats.gpuRetirement =
            m_d3d12.gpuRetirementStats();
        const d3d12::StaticBufferRenderStats staticBuffers =
            m_d3d12.staticBufferStats();
        frameStats.staticBufferPageCount =
            staticBuffers.pageCount;
        frameStats.staticBufferOversizedPageCount =
            staticBuffers.oversizedPageCount;
        frameStats.staticBufferActiveSliceCount =
            staticBuffers.activeSliceCount;
        frameStats.staticBufferRetiringSliceCount =
            staticBuffers.retiringSliceCount;
        frameStats.staticBufferPageCapacityBytes =
            staticBuffers.pageCapacityBytes;
        frameStats.staticBufferLiveLogicalBytes =
            staticBuffers.liveLogicalBytes;
        frameStats.staticBufferPendingCopyCount =
            staticBuffers.pendingCopyCount;
        frameStats.staticBufferPendingCopyBytes =
            staticBuffers.pendingCopyBytes;
        m_worldAssets->stats.markSubmitted();

        const uint64_t simulationFrame = frameStats.simulationFrame;
        if (m_worldAssets->stats.reportDue(simulationFrame)) {
            const render::WorldFrameRenderStats& stats = frameStats;
            TD_LOG_INFO(
                "[InterpolationQuality] frame={} samples={} measured={:.1f}fps gpu={}us",
                simulationFrame,
                m_worldAssets->stats.interpolationIntermediateSamples(),
                m_worldAssets->stats.measuredPresentationFramesPerSecond(),
                m_worldAssets->stats.measuredGpuFrameMicroseconds());
            TD_LOG_INFO(
                "[RendererStats] frame={} passes[particleUpdate={}us,particlePrepare={}us,terrainResource={}us:{},skybox={}us:{},terrainPacket={}us:{},terrainBridge={}us:{},objectPacket={}us:{},worldOverlayPacket={}us:{},opaqueWorld={}us:{},projectorPrepare={}us:{},worldEffects={}us:{},worldPost={}us:{},statsPublication={}us:{}] "
                "resources[samples={},begin={}/{},resolve={}/{},noop={}/{},fail={},barriers={},restored={},active={}] "
                "display[rev={},requested={}x{}:{}@{},effective={}x{}:{}@{},applied={}x{}:{}@{},pixel={}x{},appliedRev={},attemptRev={},attempts={}/{}/{},lastOk={},state={}/{},match={},fallback=0x{:X},changes=0x{:X}] "
                "entities={}/{} culled(distance={},frustum={},hidden={}) prep={}us tasks={}/{}/{} workers={}/{}:{} grain={} pose(eval={},reuse={},joints={},palette={}KB,visibility={}KB) "
                "poseDetail(eval[ordinary={},camera={},emitter={},track={}],reuse[camera={},emitter={}],fallback[completion={},camera={},emitter={}],channels={},controls={},time={}us[ordinary={},camera={},emitter={},track={}],arena[requested={},allocated={},rejected={},grow={}/{},capacity={}KB/{}KB,high={}KB/{}KB]) "
                "prepStorage={}KB(high={}KB,grow={}) "
                "packets={} draws={} instancedDraws={} instances={} triangles={} instanceScratch={}(high={},grow={}) skinPaletteScratch={}(high={},grow={}) skinUpload={}/{}/{}KB restPalette={}/{}/{}j bindingReject={} "
                "particles={}/{} smudges={} particleReject(src={},invalid={},visibility={}[color={}],sourceBudget={},budget={}) particleRoute(drawable={}) gpuRef={}/{} particleOutputCap={}/{}/{} "
                "particleScratch(cap={}/{}/{}/{},high={}/{}/{}/{},grow={},reuse={},reject={}) "
                "particleCpu(sample={},frames={},parallel={},blocks={},tasks={},integrated={},dead={},emitter={}us,integration={}us,compact={}us) "
                "particleGpu(ready={},gate[effective={},reject=0x{:X},count={}/{}],dispatch[reset={},command={},integration={},aliveReset={},alive={},visibleReset={},visible={},material={}/{}/{}/{}],indirect[builds={},argTransitions={},drawTransitions={},shadowReady={},shadowExec={}],ab[count[readbacks={},samples={},match={},mismatch={},stale={},last={}/{}:{}],state[readbacks={},copies={},samples={},match={},mismatch={},stale={},last={}]],commands[birth={},retire={},rejected={}],barriers[transition={},uav={}],normalize[slot={}/{} birth={} retire={} grow={}],alive[capacity={},count={},overflow={},valid={}],visible[capacity={},count={},overflow={},valid={}],material[templates={},bins={}/{}],capacity={},epoch={}) "
                "fxaa(enabled={},passes={}) terrainChunks={}/{} textures={}/{}KB texUse={}/{} cache(hits={},misses={},uploads={},fallbacks={},failures={},cpu={}/{}/{}/{}/{},done={},prepared={}KB,worker={}us,cancel={}/{}/{},stale={},maxAge={},gc={}KB/{}KB/{}KB/{}) textureGpuQueue(queued={},attempts={},deferred={},forced={},bytes={}KB,deferredBytes={}KB,time={}us,cancel={},maxAge={}) textureLru(evicted={}/{}KB,reject={}/{},pins={}) trails={}/{} tracks={}/{} projectors={}/{} decalTextures={}/{} decalRejected={} lights={}/{} "
                "modelUpload(attempted={},ok={},failed={},deferred={},forced={},bytes={}KB,deferredBytes={}KB,time={}us) "
                "modelResident(owners={},bytes={}KB,lastUse={}/{},inFlight={},completedOld={},lru={}/{}KB,reject={}/{},pins={}) "
                "modelCpuLoad(queued={},pending={},inFlight={},published={},failed={},cancelled={}/{},stale={},maxAge={},ready={}KB/{}us/{}us,deferred={},forced={},readyAge={}) "
                "animationReady(bytes={}KB,worker={}us,publish={}us,deferred={},forced={},maxAge={}) "
                "cpuRetention(model={}KB,sorting={}KB,animation={}KB) "
                "assetLife[model={}/{}/{}/{},animation={}/{}/{}/{},textureSource={}/{}/{}KB,texture={}/{}/{}/{}KB,ui={}/{},deps={}/{} policy={}/{}/{} missing={} fallback={} reject={}/{} shared={}/{}] "
                "shadows={}/{}/{} shadowState(available={},valid={},fallback={}) "
                "shadowRejected(policy={},invalid={}) "
                "visibility(rev={},enabled={},fallback={},texture={}x{},uploadedCells={},uploadedBytes={}) "
                "visibilityHidden(entities={},projectiles={},fxObjects={},fxInvocations={}) "
                "descriptors={}/{} retiring={} highWater={} failures={} warnings={} "
                "retire(resources={}/{},bytes={}KB/{}KB,srvs={}/{},tag={}/{},resUse={}/{}/{},srvUse={}/{},fence={}/{},oldest={},reclaimed={}/{}KB/{},reject={}) "
                "upload={}/{}KB spill={}KB/{}KB pages={} grow={} reuse={} "
                "rejected={}KB alloc={} failed={} cpu={}us(copy={}us) "
                "highWater={}KB+{}KB",
                stats.simulationFrame,
                stats.passTimings.particleUpdateMicroseconds,
                stats.passTimings.particlePrepareMicroseconds,
                stats.passTimings.terrainResourceMicroseconds,
                stats.passTimings.terrainResourceSkipped,
                stats.passTimings.skyboxMicroseconds,
                stats.passTimings.skyboxSkipped,
                stats.passTimings.terrainPacketMicroseconds,
                stats.passTimings.terrainPacketSkipped,
                stats.passTimings.terrainBridgeMicroseconds,
                stats.passTimings.terrainBridgeSkipped,
                stats.passTimings.objectPacketMicroseconds,
                stats.passTimings.objectPacketSkipped,
                stats.passTimings.worldOverlayPacketMicroseconds,
                stats.passTimings.worldOverlayPacketSkipped,
                stats.passTimings.opaqueWorldMicroseconds,
                stats.passTimings.opaqueWorldSkipped,
                stats.passTimings.projectorPrepareMicroseconds,
                stats.passTimings.projectorPrepareSkipped,
                stats.passTimings.worldEffectsMicroseconds,
                stats.passTimings.worldEffectsSkipped,
                stats.passTimings.worldPostMicroseconds,
                stats.passTimings.worldPostSkipped,
                stats.passTimings.statsPublicationMicroseconds,
                stats.passTimings.statsPublicationSkipped,
                stats.worldResources.sampleCount,
                stats.worldResources.beginCalls,
                stats.worldResources.beginFailures,
                stats.worldResources.resolveCalls,
                stats.worldResources.resolveExecutions,
                stats.worldResources.resolveNoopSingleSample,
                stats.worldResources.resolveNoopInactive,
                stats.worldResources.resolveFailures,
                stats.worldResources.transitionBarriers,
                stats.worldResources.presentationTargetRestored,
                stats.worldResources.multisamplePassActive,
                stats.displayOutput.revision,
                stats.displayOutput.requestedWidth,
                stats.displayOutput.requestedHeight,
                stats.displayOutput.requestedMode,
                stats.displayOutput.requestedRefreshRateHz,
                stats.displayOutput.effectiveWidth,
                stats.displayOutput.effectiveHeight,
                stats.displayOutput.effectiveMode,
                stats.displayOutput.effectiveRefreshRateHz,
                stats.displayOutput.appliedWidth,
                stats.displayOutput.appliedHeight,
                stats.displayOutput.appliedMode,
                stats.displayOutput.appliedRefreshRateHz,
                stats.displayOutput.pixelWidth,
                stats.displayOutput.pixelHeight,
                stats.displayOutput.appliedOutputRevision,
                stats.displayOutput.lastOutputAttemptRevision,
                stats.displayOutput.outputApplyAttempts,
                stats.displayOutput.outputApplySucceeded,
                stats.displayOutput.outputApplyFailed,
                stats.displayOutput.lastOutputApplySucceeded,
                stats.displayOutput.hasAppliedOutput,
                stats.displayOutput.pixelExtentValid,
                stats.displayOutput.appliedMatchesEffective,
                stats.displayOutput.capabilityFallbackMask,
                stats.displayOutput.changeMask,
                stats.preparation.preparedInstances, stats.preparation.inputEntities,
                stats.preparation.distanceCulledInstances,
                stats.preparation.frustumCulledInstances,
                stats.preparation.hiddenInstances,
                stats.preparation.elapsedMicroseconds,
                stats.preparation.chunkTaskCount,
                stats.preparation.scheduledTaskCount,
                stats.preparation.completedTaskCount,
                stats.preparation.activeWorkerCount,
                stats.preparation.executorWorkerCapacity,
                stats.preparation.activeWorkerObservationClamped,
                stats.preparation.entityGrain,
                stats.preparation.poseEvaluations,
                stats.preparation.poseReuses,
                stats.preparation.poseJointsEvaluated,
                stats.preparation.preparedPaletteBytes / 1024u,
                stats.preparation.preparedVisibilityBytes / 1024u,
                stats.preparation.ordinaryPoseEvaluations,
                stats.preparation.cameraPoseEvaluations,
                stats.preparation.emitterPoseEvaluations,
                stats.preparation.trackMarkPoseEvaluations,
                stats.preparation.cameraPoseReuses,
                stats.preparation.emitterPoseReuses,
                stats.preparation.completionFallbacks,
                stats.preparation.cameraPoseFallbacks,
                stats.preparation.emitterRootFallbacks,
                stats.preparation.poseAnimationChannelsSampled,
                stats.preparation.poseControlsApplied,
                stats.preparation.poseEvaluationMicroseconds,
                stats.preparation.ordinaryPoseMicroseconds,
                stats.preparation.cameraPoseMicroseconds,
                stats.preparation.emitterPoseMicroseconds,
                stats.preparation.trackMarkPoseMicroseconds,
                stats.preparation.poseArenaRequestedJoints,
                stats.preparation.poseArenaAllocatedJoints,
                stats.preparation.poseArenaRejectedInstances,
                stats.preparation.poseArenaGrowths,
                stats.preparation.visibilityArenaGrowths,
                stats.preparation.poseArenaCapacityBytes / 1024u,
                stats.preparation.visibilityArenaCapacityBytes / 1024u,
                stats.preparation.poseArenaCapacityHighWaterBytes / 1024u,
                stats.preparation.visibilityArenaCapacityHighWaterBytes /
                    1024u,
                stats.preparation.retainedContainerCapacityBytes / 1024u,
                stats.preparation.retainedContainerCapacityHighWaterBytes /
                    1024u,
                stats.preparation.containerCapacityGrowths,
                stats.staticMeshes.submittedPackets,
                stats.staticMeshes.drawCalls,
                stats.staticMeshes.instancedDrawCalls,
                stats.staticMeshes.renderedInstances,
                stats.staticMeshes.renderedTriangles,
                stats.staticMeshes.instanceScratchCapacity,
                stats.staticMeshes.instanceScratchCapacityHighWater,
                stats.staticMeshes.instanceScratchCapacityGrowths,
                stats.staticMeshes.skinPaletteScratchCapacity,
                stats.staticMeshes.skinPaletteScratchCapacityHighWater,
                stats.staticMeshes.skinPaletteScratchCapacityGrowths,
                stats.staticMeshes.skinPaletteUploadHits,
                stats.staticMeshes.skinPaletteUploadMisses,
                stats.staticMeshes.skinPaletteUploadBytes / 1024u,
                stats.staticMeshes.restPalettesBuilt,
                stats.staticMeshes.restPalettesReused,
                stats.staticMeshes.restPaletteJointsMaterialized,
                stats.staticMeshes.poseBindingGenerationRejects,
                stats.particleInstances, stats.particleBatches,
                stats.smudgeInstances,
                stats.particleSourceCount,
                stats.particleRejectedInvalid,
                stats.particleRejectedVisibility,
                stats.particleRejectedColor,
                stats.particleRejectedSourceBudget,
                stats.particleRejectedBudget,
                stats.particleRoutedDrawable,
                stats.particleGpuReferenceEligibleSources,
                stats.particleGpuReferenceSelectedInstances,
                stats.particleInstanceCapacity,
                stats.particleBatchCapacity,
                stats.smudgeInstanceCapacity,
                stats.particleSourcePriorityScratchCapacity,
                stats.particleSourceOrdinalScratchCapacity,
                stats.particleCandidateScratchCapacity,
                stats.particleStreakPointScratchCapacity,
                stats.particleSourcePriorityScratchHighWater,
                stats.particleSourceOrdinalScratchHighWater,
                stats.particleCandidateScratchHighWater,
                stats.particleStreakPointScratchHighWater,
                stats.particleScratchCapacityGrowths,
                stats.particleScratchContainersReused,
                stats.particleScratchHardCapRejected,
                stats.particlePhaseSampleOrdinal,
                stats.particleAuthoredFrames,
                stats.particleParallelIntegration,
                stats.particleIntegrationBlocks,
                stats.particleIntegrationTasks,
                stats.particleIntegrated,
                stats.particleCompacted,
                stats.particleEmitterUpdateMicroseconds,
                stats.particleIntegrationMicroseconds,
                stats.particleCompactMicroseconds,
                stats.gpuParticles.infrastructureReady,
                stats.gpuParticles.effectiveGpuPresentation,
                stats.gpuParticles.presentationRejectionMask,
                stats.gpuParticles.presentationParticleCount,
                stats.gpuParticles.presentationParticleBudget,
                stats.gpuParticles.resetDispatches,
                stats.gpuParticles.commandDispatches,
                stats.gpuParticles.integrationDispatches,
                stats.gpuParticles.aliveCompactCounterResetDispatches,
                stats.gpuParticles.aliveCompactDispatches,
                stats.gpuParticles.visibleCompactCounterResetDispatches,
                stats.gpuParticles.visibleCompactDispatches,
                stats.gpuParticles.materialBinResetDispatches,
                stats.gpuParticles.materialBinCountDispatches,
                stats.gpuParticles.materialBinPrefixDispatches,
                stats.gpuParticles.materialBinScatterDispatches,
                stats.gpuParticles.indirectArgumentBuilds,
                stats.gpuParticles.indirectArgumentTransitions,
                stats.gpuParticles.indirectDrawResourceTransitions,
                stats.gpuParticles.indirectShadowDrawReady,
                stats.gpuParticles.indirectShadowExecuteCalls,
                stats.gpuParticles.abCountReadbacks,
                stats.gpuParticles.abCountSamples,
                stats.gpuParticles.abCountMatches,
                stats.gpuParticles.abCountMismatches,
                stats.gpuParticles.abCountStaleSkipped,
                stats.gpuParticles.abLastCpuCount,
                stats.gpuParticles.abLastGpuCount,
                stats.gpuParticles.abLastCountValid,
                stats.gpuParticles.abStateReadbacks,
                stats.gpuParticles.abStateCopies,
                stats.gpuParticles.abStateSamples,
                stats.gpuParticles.abStateMatches,
                stats.gpuParticles.abStateMismatches,
                stats.gpuParticles.abStateStaleSkipped,
                stats.gpuParticles.abLastStateValid,
                stats.gpuParticles.submittedBirthCommands,
                stats.gpuParticles.submittedRetireCommands,
                stats.gpuParticles.rejectedCommands,
                stats.gpuParticles.transitionBarriers,
                stats.gpuParticles.uavBarriers,
                stats.gpuParticles.normalizationSlotScratchCapacity,
                stats.gpuParticles.normalizationSlotScratchHighWater,
                stats.gpuParticles.normalizationBirthScratchCapacity,
                stats.gpuParticles.normalizationRetireScratchCapacity,
                stats.gpuParticles.normalizationScratchCapacityGrowths,
                stats.gpuParticles.aliveIndexCapacity,
                stats.gpuParticles.diagnosticAliveCount,
                stats.gpuParticles.diagnosticAliveOverflow,
                stats.gpuParticles.diagnosticAliveCountsValid,
                stats.gpuParticles.visibleIndexCapacity,
                stats.gpuParticles.diagnosticVisibleCount,
                stats.gpuParticles.diagnosticVisibleOverflow,
                stats.gpuParticles.diagnosticVisibleCountsValid,
                stats.gpuParticles.materialTemplateMapCount,
                stats.gpuParticles.materialBinCount,
                stats.gpuParticles.materialBinCapacity,
                stats.gpuParticles.capacity,
                stats.gpuParticles.authorityEpoch,
                stats.fxaaEnabled, stats.fxaaPasses,
                stats.terrainVisibleChunks, stats.terrainCulledChunks,
                stats.residentTextures, stats.residentTextureBytes / 1024u,
                stats.textureLatestUsedFrame,
                stats.textureLatestUsedFence,
                stats.textureCacheHits, stats.textureCacheMisses,
                stats.textureGpuUploads, stats.textureFallbackResolutions,
                stats.textureFailedAcquisitions,
                stats.textureCpuQueuedJobs, stats.textureCpuActiveJobs,
                stats.textureCpuPendingVariants,
                stats.textureCpuPreparedVariants,
                stats.textureCpuFailedVariants,
                stats.textureCpuCompletedJobs,
                stats.textureCpuPreparedBytes / 1024u,
                stats.textureCpuWorkerNanoseconds / 1000u,
                stats.textureCpuCancelledVariants,
                stats.textureCpuCancelledReady,
                stats.textureCpuCancelRequestedActive,
                stats.textureCpuStaleCompletions,
                stats.textureCpuMaximumQueueAge,
                stats.textureCpuRetainedPreparedBytes / 1024u,
                stats.textureCpuReclaimedPreparedBytes / 1024u,
                stats.textureCpuReclaimedSourceBytes / 1024u,
                stats.textureCpuReclaimedSources,
                stats.textureGpuQueuedUploads,
                stats.textureGpuUploadAttempts,
                stats.textureGpuUploadDeferred,
                stats.textureGpuUploadForcedOversized,
                stats.textureGpuUploadAttemptedBytes / 1024u,
                stats.textureGpuUploadDeferredBytes / 1024u,
                stats.textureGpuUploadNanoseconds / 1000u,
                stats.textureGpuUploadCancelled,
                stats.textureGpuUploadMaximumAge,
                stats.textureResidencyEvictions,
                stats.textureResidencyEvictedBytes / 1024u,
                stats.textureResidencyOwnerRejects,
                stats.textureResidencyPinnedRejects,
                stats.textureResidencyPins,
                stats.projectileTrails, stats.projectileTrailSegments,
                stats.trackMarkStreams, stats.trackMarkSegments,
                stats.projectedShadows, stats.projectedShadowDrawCalls,
                stats.groundProjectorTextureBatches,
                stats.groundProjectorResidentTextures,
                stats.groundProjectorBudgetRejected,
                stats.staticMeshes.dynamicPointLights,
                stats.staticMeshes.dynamicLightReceivingPackets,
                stats.modelUploadAttempts,
                stats.modelUploadSucceeded,
                stats.modelUploadFailed,
                stats.modelUploadDeferred,
                stats.modelUploadForcedOversized,
                stats.modelUploadEstimatedBytes / 1024u,
                stats.modelUploadDeferredBytes / 1024u,
                stats.modelUploadMicroseconds,
                stats.assets[render::RenderAssetKind::Model].ownerReferences,
                stats.assets[render::RenderAssetKind::Model].gpuBytes / 1024u,
                stats.modelGpuLatestUsedFrame,
                stats.modelGpuLatestUsedFence,
                stats.modelGpuUseFencesInFlight,
                stats.modelGpuCompletedUsesWithoutExactFence,
                stats.modelResidencyEvictions,
                stats.modelResidencyEvictedBytes / 1024u,
                stats.modelResidencyPinnedRejects,
                stats.modelResidencyReferencedRejects,
                stats.modelResidencyPins,
                stats.modelCpuLoadQueuedJobs,
                stats.modelCpuLoadPendingCompletions,
                stats.modelCpuLoadsInFlight,
                stats.modelCpuLoadPublishedCompletions,
                stats.modelCpuLoadFailedCompletions,
                stats.modelCpuLoadCancelledJobs,
                stats.modelCpuLoadCancelledCompletions,
                stats.modelCpuLoadDiscardedStaleCompletions,
                stats.modelCpuLoadMaximumQueueAge,
                stats.modelCpuReadyBytes / 1024u,
                stats.modelCpuReadyWorkerNanoseconds / 1000u,
                stats.modelCpuReadyPublishMicroseconds,
                stats.modelCpuReadyDeferred,
                stats.modelCpuReadyForcedOversized,
                stats.modelCpuReadyMaximumAge,
                stats.animationReadyBytes / 1024u,
                stats.animationReadyWorkerNanoseconds / 1000u,
                stats.animationReadyPublishMicroseconds,
                stats.animationReadyDeferred,
                stats.animationReadyForcedOversized,
                stats.animationMaximumReadyAge,
                stats.assets[render::RenderAssetKind::Model].cpuBytes / 1024u,
                stats.modelRetainedSortingBytes / 1024u,
                stats.assets[render::RenderAssetKind::Animation].cpuBytes / 1024u,
                stats.assets[render::RenderAssetKind::Model].tracked,
                stats.assets[render::RenderAssetKind::Model].currentStates[
                    static_cast<size_t>(
                        render::RenderAssetLifecycleState::IoQueued)],
                stats.assets[render::RenderAssetKind::Model].currentStates[
                    static_cast<size_t>(
                        render::RenderAssetLifecycleState::CpuReady)],
                stats.assets[render::RenderAssetKind::Model].currentStates[
                    static_cast<size_t>(
                        render::RenderAssetLifecycleState::GpuResident)],
                stats.assets[render::RenderAssetKind::Animation].tracked,
                stats.assets[render::RenderAssetKind::Animation]
                    .currentStates[static_cast<size_t>(
                        render::RenderAssetLifecycleState::IoQueued)],
                stats.assets[render::RenderAssetKind::Animation]
                    .currentStates[static_cast<size_t>(
                        render::RenderAssetLifecycleState::IoInFlight)],
                stats.assets[render::RenderAssetKind::Animation]
                    .currentStates[static_cast<size_t>(
                        render::RenderAssetLifecycleState::CpuReady)],
                stats.assets[render::RenderAssetKind::TextureSource].tracked,
                stats.assets[render::RenderAssetKind::TextureSource]
                    .currentStates[static_cast<size_t>(
                        render::RenderAssetLifecycleState::Failed)],
                stats.assets[render::RenderAssetKind::TextureSource]
                    .cpuBytes / 1024u,
                stats.assets[render::RenderAssetKind::TextureVariant].tracked,
                stats.assets[render::RenderAssetKind::TextureVariant]
                    .currentStates[static_cast<size_t>(
                        render::RenderAssetLifecycleState::GpuResident)],
                stats.assets[render::RenderAssetKind::TextureVariant]
                    .currentStates[static_cast<size_t>(
                        render::RenderAssetLifecycleState::Fallback)],
                stats.assets[render::RenderAssetKind::TextureVariant]
                    .gpuBytes / 1024u,
                stats.assets[render::RenderAssetKind::UiTexture].tracked,
                stats.assets[render::RenderAssetKind::Glyph].tracked,
                stats.assets.dependencies.nodes,
                stats.assets.dependencies.edges,
                stats.assets.dependencies.requiredEdges,
                stats.assets.dependencies.optionalEdges,
                stats.assets.dependencies.fallbackAllowedEdges,
                stats.assets.dependencies.missingRequired,
                stats.assets.dependencies.fallbackResolved,
                stats.assets.dependencies.cycleRejected,
                stats.assets.dependencies.depthRejected,
                stats.assets.dependencies.sharedTargets,
                stats.assets.dependencies.incomingReferences,
                stats.staticMeshes.shadowCasterPackets,
                stats.staticMeshes.shadowDrawCalls,
                stats.staticMeshes.shadowTriangles,
                stats.staticMeshes.shadowAvailable,
                stats.staticMeshes.shadowValid,
                stats.staticMeshes.shadowFallbackBound,
                stats.staticMeshes.shadowPolicyRejectedPackets,
                stats.staticMeshes.shadowInvalidPackets,
                stats.staticMeshes.visibilityRevision,
                stats.staticMeshes.visibilityEnabled,
                stats.staticMeshes.visibilityFallbackBound,
                stats.staticMeshes.visibilityTextureWidth,
                stats.staticMeshes.visibilityTextureHeight,
                stats.staticMeshes.visibilityUploadedCells,
                stats.staticMeshes.visibilityUploadedBytes,
                stats.visibilityHiddenEntities,
                stats.visibilityHiddenProjectiles,
                stats.visibilityRejectedFxObjects,
                stats.visibilityRejectedFxInvocations,
                stats.srvDescriptors.allocated,
                stats.srvDescriptors.capacity,
                stats.srvDescriptors.retiring,
                stats.srvDescriptors.lifetimeHighWater,
                stats.srvDescriptors.allocationFailures,
                stats.srvDescriptors.pressureWarnings,
                stats.gpuRetirement.pendingResources,
                stats.gpuRetirement.fencedResources,
                stats.gpuRetirement.pendingResourceBytes / 1024u,
                stats.gpuRetirement.fencedResourceBytes / 1024u,
                stats.gpuRetirement.pendingSrvDescriptors,
                stats.gpuRetirement.fencedSrvDescriptors,
                stats.gpuRetirement.attributedResources,
                stats.gpuRetirement.attributedSrvDescriptors,
                stats.gpuRetirement.latestResourceUseFrame,
                stats.gpuRetirement.latestResourceUseFence,
                stats.gpuRetirement.completedResourceUsesWithoutExactFence,
                stats.gpuRetirement.latestSrvUseFrame,
                stats.gpuRetirement.latestSrvUseFence,
                stats.gpuRetirement.completedFence,
                stats.gpuRetirement.lastSealedRetirementFence,
                stats.gpuRetirement.oldestRetireRequestFrame,
                stats.gpuRetirement.reclaimedResources,
                stats.gpuRetirement.reclaimedResourceBytes / 1024u,
                stats.gpuRetirement.reclaimedSrvDescriptors,
                stats.gpuRetirement.rejectedRetirements,
                stats.frameUpload.uploadedBytes / 1024,
                stats.frameUpload.requestedBytes / 1024,
                stats.frameUpload.spillBytesUsed / 1024,
                stats.frameUpload.spillCapacityBytes / 1024,
                stats.frameUpload.spillPageCount,
                stats.frameUpload.spillPageGrowthCount,
                stats.frameUpload.spillPageReuseAllocationCount,
                stats.frameUpload.rejectedBytes / 1024,
                stats.frameUpload.allocationCount,
                stats.frameUpload.failedAllocationCount,
                stats.frameUpload.allocationCpuNanoseconds / 1000,
                stats.frameUpload.copyCpuNanoseconds / 1000,
                stats.frameUpload.lifetimePrimaryHighWaterBytes / 1024,
                stats.frameUpload.lifetimeSpillHighWaterBytes / 1024);
            const auto gpuTime = [&stats](
                render::GpuTimestampRange range) noexcept {
                return stats.gpuTimestamps.rangeMicroseconds[
                    static_cast<size_t>(range)];
            };
            TD_LOG_INFO(
                "[RendererStats.Detail] particleStages[select={}us,expand={}us,sort={}us,pack={}us,texture={}us,upload={}KB/{}us,draw={}us,calls={}/{},smudge={}KB/{}us/{}us/{}] "
                "particleGate[visibilityPublish={}/{}KB/reject={},consecutive={}/{}/{},visibleExec={},fallback={},qualification={} output={} profile={}:min={}] "
                "gpuTime[sourceFrame={},valid=0x{:X},frame={}us,opaque={}us,compute={}us,particleDraw={}us,sceneCopy={}us,fxaa={}us]",
                stats.particleSourceSelectionMicroseconds,
                stats.particleExpansionMicroseconds,
                stats.particleSortMicroseconds,
                stats.particlePackMicroseconds,
                stats.particleTextureBindingMicroseconds,
                stats.particleInstanceUploadBytes / 1024u,
                stats.particleInstanceUploadMicroseconds,
                stats.particleDrawRecordMicroseconds,
                stats.particleCpuDrawCalls,
                stats.particleGpuIndirectDrawCalls,
                stats.particleSmudgeUploadBytes / 1024u,
                stats.particleSmudgeUploadMicroseconds,
                stats.particleSmudgeDrawRecordMicroseconds,
                stats.particleSmudgeDrawCalls,
                stats.gpuParticles.visibilityAuthorityPublishes,
                stats.gpuParticles.visibilityAuthorityBytes / 1024u,
                stats.gpuParticles.visibilityAuthorityRejects,
                stats.gpuParticles.abCountConsecutiveFrames,
                stats.gpuParticles.abVisibilityConsecutiveFrames,
                stats.gpuParticles.abStateConsecutiveFrames,
                stats.gpuParticles.indirectVisibleExecuteCalls,
                stats.gpuParticles.presentationFallbackFrames,
                stats.gpuParticles.qualificationRevision,
                stats.gpuParticles.outputParityApproved,
                stats.gpuParticles.profileApproved,
                stats.gpuParticles.profileMinimumParticleCount,
                stats.gpuTimestamps.sourceFrameOrdinal,
                stats.gpuTimestamps.validRangeMask,
                gpuTime(render::GpuTimestampRange::Frame),
                gpuTime(render::GpuTimestampRange::OpaqueWorld),
                gpuTime(render::GpuTimestampRange::ParticleCompute),
                gpuTime(render::GpuTimestampRange::ParticleDraw),
                gpuTime(render::GpuTimestampRange::SceneColorCopy),
                gpuTime(render::GpuTimestampRange::Fxaa));
            TD_LOG_INFO(
                "[RendererStats.R9.SceneColor] resident={}KB peak={}KB alloc={}/{} copy={}/{}KB release={}/{}KB retire={}/{}KB",
                stats.sceneColor.residentAllocationBytes / 1024u,
                stats.sceneColor.lifetimeResidentHighWaterBytes / 1024u,
                stats.sceneColor.allocationAttempts,
                stats.sceneColor.allocationFailures,
                stats.sceneColor.copyCalls,
                stats.sceneColor.copiedBytes / 1024u,
                stats.sceneColor.releaseCalls,
                stats.sceneColor.releasedAllocationBytes / 1024u,
                stats.sceneColor.retirementRequests,
                stats.sceneColor.retirementRequestedBytes / 1024u);
            TD_LOG_INFO(
                "[RendererStats.R9.Bind] frame={} pso={} gfxRoot={} computeRoot={} gfxTable={} computeTable={} vb={} ib={} draw={} dispatch={} indirect={}",
                stats.renderBindings.frameOrdinal,
                stats.renderBindings.pipelineStateCalls,
                stats.renderBindings.graphicsRootSignatureCalls,
                stats.renderBindings.computeRootSignatureCalls,
                stats.renderBindings.graphicsDescriptorTableCalls,
                stats.renderBindings.computeDescriptorTableCalls,
                stats.renderBindings.vertexBufferCalls,
                stats.renderBindings.indexBufferCalls,
                stats.renderBindings.drawCalls,
                stats.renderBindings.dispatchCalls,
                stats.renderBindings.executeIndirectCalls);
            TD_LOG_INFO(
                "[RendererStats.R9.Container] preparation={}KB/high={}KB/growth={} nested={}KB/high={}KB/growthFrames={} owner={}KB/high={}KB/growthFrames={}",
                stats.preparation.retainedContainerCapacityBytes / 1024u,
                stats.preparation.retainedContainerCapacityHighWaterBytes /
                    1024u,
                stats.preparation.containerCapacityGrowths,
                stats.preparation.retainedNestedCapacityBytes / 1024u,
                stats.preparation.retainedNestedCapacityHighWaterBytes /
                    1024u,
                stats.preparation.nestedCapacityGrowthFrames,
                stats.retainedScratch.capacityBytes / 1024u,
                stats.retainedScratch.lifetimeHighWaterBytes / 1024u,
                stats.retainedScratch.capacityGrowthFrames);
            TD_LOG_INFO(
                "[RendererStats.R9.Reclaim] pending={}/{}KB fenced={}/{}KB reclaimed={}/{}KB/srv={} reject={}",
                stats.gpuRetirement.pendingResources,
                stats.gpuRetirement.pendingResourceBytes / 1024u,
                stats.gpuRetirement.fencedResources,
                stats.gpuRetirement.fencedResourceBytes / 1024u,
                stats.gpuRetirement.reclaimedResources,
                stats.gpuRetirement.reclaimedResourceBytes / 1024u,
                stats.gpuRetirement.reclaimedSrvDescriptors,
                stats.gpuRetirement.rejectedRetirements);
            m_worldAssets->stats.markReported(simulationFrame);
        }
    }
    // A failed close/present/fence is terminal for this renderer instance.
    // In particular, no GPU model recorded in that frame may be reused by a
    // later frame under an unproven submission state.
    if (!submitted) {
        throw std::runtime_error("D3D12 endFrame submission failed");
    }
    if (!m_pendingScreenshotFilename.empty()) {
        const container::String filename = std::move(m_pendingScreenshotFilename);
        m_pendingScreenshotFilename.clear();
        d3d12::RenderTargetReadback readback;
        if (!m_d3d12.readbackLastPresentedFrame(readback)) {
            TD_LOG_ERROR(
                "[DX12Renderer] Screenshot readback failed for '{}'", filename);
        } else if (!writeReadbackBmp(
                       readback, std::filesystem::path(filename))) {
            TD_LOG_ERROR(
                "[DX12Renderer] Screenshot BMP write failed for '{}'", filename);
        } else {
            TD_LOG_INFO(
                "[DX12Renderer] Screenshot captured: '{}' ({}x{})",
                filename, readback.width, readback.height);
        }
    }
}

} // namespace engine
