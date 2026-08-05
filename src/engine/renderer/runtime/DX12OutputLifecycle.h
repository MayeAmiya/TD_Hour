#pragma once

#include "presentation/render/RenderGameDataSettings.h"

#include <cstdint>

namespace engine::render {

struct DisplayOutputRenderStats;

// Render-thread-owned state for applying window/output revisions. Keeping the
// requested, applied and observed pixel extents together prevents resource
// recreation policy from leaking into the world asset/cache aggregate.
struct DX12OutputLifecycle final {
    [[nodiscard]] DisplayOutputRenderStats projectStats() const noexcept;

    uint32_t displayWidth = 0;
    uint32_t displayHeight = 0;
    RenderDisplayMode displayMode = RenderDisplayMode::Windowed;
    uint32_t displayRefreshRateHz = 0;
    uint32_t appliedOutputWidth = 0;
    uint32_t appliedOutputHeight = 0;
    RenderDisplayMode appliedOutputMode = RenderDisplayMode::Windowed;
    uint32_t appliedOutputRefreshRateHz = 0;
    uint32_t displayPixelWidth = 0;
    uint32_t displayPixelHeight = 0;
    ResolvedRenderDisplaySnapshot lastResolvedDisplay;
    uint64_t lastOutputAttemptRevision = 0;
    uint64_t appliedOutputRevision = 0;
    uint64_t outputApplyAttempts = 0;
    uint64_t outputApplySucceeded = 0;
    uint64_t outputApplyFailed = 0;
    bool hasResolvedDisplay = false;
    bool hasAppliedOutput = false;
    bool displayPixelExtentValid = false;
    bool lastOutputApplySucceeded = false;
    bool verticalSync = false;
};

} // namespace engine::render
