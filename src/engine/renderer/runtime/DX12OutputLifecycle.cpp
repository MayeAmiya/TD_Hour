#include "DX12OutputLifecycle.h"

#include "RendererStats.h"

namespace engine::render {

DisplayOutputRenderStats DX12OutputLifecycle::projectStats() const noexcept {
    return {
        .revision = hasResolvedDisplay ? lastResolvedDisplay.revision : 0u,
        .requestedWidth = hasResolvedDisplay
            ? lastResolvedDisplay.requested.width : 0u,
        .requestedHeight = hasResolvedDisplay
            ? lastResolvedDisplay.requested.height : 0u,
        .requestedMode = hasResolvedDisplay
            ? static_cast<uint32_t>(
                  lastResolvedDisplay.requested.displayMode)
            : 0u,
        .requestedRefreshRateHz = hasResolvedDisplay
            ? lastResolvedDisplay.requested.refreshRateHz : 0u,
        .effectiveWidth = hasResolvedDisplay
            ? lastResolvedDisplay.effective.width : 0u,
        .effectiveHeight = hasResolvedDisplay
            ? lastResolvedDisplay.effective.height : 0u,
        .effectiveMode = hasResolvedDisplay
            ? static_cast<uint32_t>(
                  lastResolvedDisplay.effective.displayMode)
            : 0u,
        .effectiveRefreshRateHz = hasResolvedDisplay
            ? lastResolvedDisplay.effective.refreshRateHz : 0u,
        .appliedWidth = appliedOutputWidth,
        .appliedHeight = appliedOutputHeight,
        .appliedMode = static_cast<uint32_t>(appliedOutputMode),
        .appliedRefreshRateHz = appliedOutputRefreshRateHz,
        .pixelWidth = displayPixelWidth,
        .pixelHeight = displayPixelHeight,
        .changeMask = hasResolvedDisplay
            ? static_cast<uint32_t>(lastResolvedDisplay.changeMask) : 0u,
        .capabilityFallbackMask = hasResolvedDisplay
            ? static_cast<uint32_t>(lastResolvedDisplay.fallbackMask) : 0u,
        .appliedOutputRevision = appliedOutputRevision,
        .lastOutputAttemptRevision = lastOutputAttemptRevision,
        .outputApplyAttempts = outputApplyAttempts,
        .outputApplySucceeded = outputApplySucceeded,
        .outputApplyFailed = outputApplyFailed,
        .lastOutputApplySucceeded = lastOutputApplySucceeded,
        .hasAppliedOutput = hasAppliedOutput,
        .pixelExtentValid = displayPixelExtentValid,
        .appliedMatchesEffective = hasResolvedDisplay && hasAppliedOutput &&
            appliedOutputMode == lastResolvedDisplay.effective.displayMode &&
            appliedOutputWidth == lastResolvedDisplay.effective.width &&
            appliedOutputHeight == lastResolvedDisplay.effective.height &&
            (lastResolvedDisplay.effective.displayMode ==
                 RenderDisplayMode::Windowed ||
             lastResolvedDisplay.effective.refreshRateHz == 0u ||
             appliedOutputRefreshRateHz ==
                 lastResolvedDisplay.effective.refreshRateHz),
    };
}

} // namespace engine::render
