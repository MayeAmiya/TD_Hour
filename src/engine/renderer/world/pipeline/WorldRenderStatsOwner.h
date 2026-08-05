#pragma once

#include "engine/renderer/runtime/RendererStats.h"
#include "engine/renderer/world/model/W3dAnimationCache.h"
#include "engine/renderer/world/model/W3dAssetCache.h"
#include "engine/renderer/world/resource/WorldTextureCache.h"

#include <chrono>
#include <cstdint>

namespace engine::fx {
struct ParticleRuntimePhaseProfile;
}

namespace engine::render {

struct ParticleRenderDrawList;
struct ParticleRenderExecutionStats;
struct W3dGpuUploadBatchStats;

enum class WorldFirstFrameDiagnostic : uint8_t {
    Prepared,
    Particle,
    Smudge,
    ProjectileTrail,
    TrackMark,
    TypedFx,
    DynamicLight,
    GroundDecal,
    DirectionalShadow,
    Count,
};

// Render-thread statistics authority. Sampling cadence, retained-capacity
// high-water tracking, report cadence and one-shot diagnostics are policy,
// not loose fields on the world resource aggregate.
class WorldRenderStatsOwner final {
public:
    void reset() noexcept;

    [[nodiscard]] WorldFrameRenderStats& frame() noexcept {
        return m_frame;
    }
    [[nodiscard]] const WorldFrameRenderStats& frame() const noexcept {
        return m_frame;
    }
    void markSubmissionPending() noexcept { m_submissionPending = true; }
    [[nodiscard]] bool submissionPending() const noexcept {
        return m_submissionPending;
    }
    void markSubmitted() noexcept {
        m_submissionPending = false;
        m_hasFrame = true;
    }
    [[nodiscard]] bool hasFrame() const noexcept { return m_hasFrame; }

    [[nodiscard]] bool assetSampleDue(uint64_t simulationFrame) const
        noexcept;
    void publishAssetSample(
        uint64_t simulationFrame,
        WorldTextureCache::Stats textures,
        W3dAssetCacheStats models,
        W3dAnimationCacheStats animations) noexcept;
    [[nodiscard]] const WorldTextureCache::Stats& textureSample()
        const noexcept { return m_textureSample; }
    [[nodiscard]] const W3dAssetCacheStats& modelSample() const noexcept {
        return m_modelSample;
    }
    [[nodiscard]] const W3dAnimationCacheStats& animationSample()
        const noexcept { return m_animationSample; }
    [[nodiscard]] RenderAssetLifecycleStats projectAssetLifecycle(
        const RenderAssetKindLifecycleStats& uiTextures,
        const RenderAssetKindLifecycleStats& glyphs,
        size_t traversalCycleRejects,
        size_t traversalDepthRejects) const noexcept;
    void projectParticleFrameStats(
        const ParticleRenderDrawList& drawList,
        size_t particleDrawCount,
        size_t smudgeDrawCount,
        const fx::ParticleRuntimePhaseProfile& phase,
        const GpuParticleSimulationRenderStats& gpu,
        const ParticleRenderExecutionStats& execution) noexcept;
    void projectAssetCacheFrameStats(
        const W3dGpuUploadBatchStats& upload) noexcept;

    void publishRetainedScratchCapacity(uint64_t bytes) noexcept;
    [[nodiscard]] uint64_t retainedScratchCapacity() const noexcept {
        return m_retainedScratchCapacity;
    }
    [[nodiscard]] uint64_t retainedScratchHighWater() const noexcept {
        return m_retainedScratchHighWater;
    }
    [[nodiscard]] uint32_t retainedScratchGrowthFrames() const noexcept {
        return m_retainedScratchGrowthFrames;
    }

    [[nodiscard]] float beginPresentationFrame(
        std::chrono::steady_clock::time_point now,
        uint64_t gpuFrameMicroseconds = 0u) noexcept;
    [[nodiscard]] uint32_t interpolationIntermediateSamples() const noexcept {
        return m_interpolationIntermediateSamples;
    }
    [[nodiscard]] float measuredPresentationFramesPerSecond() const noexcept {
        return m_presentationDeltaEmaSeconds > 0.0
            ? static_cast<float>(1.0 / m_presentationDeltaEmaSeconds)
            : 0.0f;
    }
    [[nodiscard]] uint64_t measuredGpuFrameMicroseconds() const noexcept {
        return static_cast<uint64_t>(m_gpuFrameMicrosecondsEma);
    }
    [[nodiscard]] bool reportDue(uint64_t simulationFrame) const noexcept;
    void markReported(uint64_t simulationFrame) noexcept;

    [[nodiscard]] bool claimFirstFrame(
        WorldFirstFrameDiagnostic diagnostic) noexcept;
    void resetFirstFrame(WorldFirstFrameDiagnostic diagnostic) noexcept;

    void setVisibilityRejectedFx(
        uint32_t objects,
        uint32_t invocations) noexcept {
        m_visibilityRejectedFxObjects = objects;
        m_visibilityRejectedFxInvocations = invocations;
    }
    [[nodiscard]] uint32_t visibilityRejectedFxObjects() const noexcept {
        return m_visibilityRejectedFxObjects;
    }
    [[nodiscard]] uint32_t visibilityRejectedFxInvocations() const noexcept {
        return m_visibilityRejectedFxInvocations;
    }

private:
    static constexpr uint64_t kAssetSampleIntervalFrames = 60;
    static constexpr uint64_t kReportIntervalFrames = 300;

    WorldFrameRenderStats m_frame;
    WorldTextureCache::Stats m_textureSample;
    W3dAssetCacheStats m_modelSample;
    W3dAnimationCacheStats m_animationSample;
    uint64_t m_assetSampleFrame = 0;
    uint64_t m_retainedScratchCapacity = 0;
    uint64_t m_retainedScratchHighWater = 0;
    uint64_t m_lastReportSimulationFrame = 0;
    std::chrono::steady_clock::time_point m_lastPresentationTime{};
    double m_presentationDeltaEmaSeconds = 0.0;
    double m_gpuFrameMicrosecondsEma = 0.0;
    uint32_t m_interpolationIntermediateSamples = 1;
    uint32_t m_interpolationCandidateSamples = 1;
    uint32_t m_interpolationCandidateFrames = 0;
    uint32_t m_retainedScratchGrowthFrames = 0;
    uint32_t m_visibilityRejectedFxObjects = 0;
    uint32_t m_visibilityRejectedFxInvocations = 0;
    uint16_t m_firstFrameMask = 0;
    bool m_assetSampleValid = false;
    bool m_hasReport = false;
    bool m_submissionPending = false;
    bool m_hasFrame = false;
};

} // namespace engine::render
