#include "FramePacer.h"
#include "runtime/GameUiProjection.h"

#include "core/config/GlobalData.h"
#include "game/ini/GameDataLoader.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <thread>

namespace app {

FramePacer::FramePacer()
    : m_previousFrameStart(std::chrono::steady_clock::now()),
      m_frameStart(m_previousFrameStart) {}

float FramePacer::beginFrame() {
    m_frameStart = std::chrono::steady_clock::now();
    const float deltaSeconds = std::clamp(
        std::chrono::duration<float>(m_frameStart - m_previousFrameStart).count(),
        0.0f, 0.1f);
    m_previousFrameStart = m_frameStart;
    return deltaSeconds;
}

void FramePacer::pace(const runtime::GameUiProjection& projection) const {
    // SET_FPS_LIMIT is client-only and is applied after world/UI submission.
    int32_t frameRateLimit = 0;
    if (config::TheWritableGlobalData &&
        config::TheWritableGlobalData->useFpsLimit()) {
        frameRateLimit =
            config::TheWritableGlobalData->framesPerSecondLimit();
    }
    if (projection.scriptFrameRateLimit) {
        frameRateLimit = *projection.scriptFrameRateLimit;
    }
    if (const auto quality = game::GameDataLoader::instance()
                                 .renderQualitySettingsSnapshot();
        quality && !quality->display.effective.fpsLimitEnabled) {
        frameRateLimit = 0;
    }
    // RefCode disables its limiter during local scripted fast-forward.
    if (projection.localFastForwardActive) {
        frameRateLimit = 0;
    }

    if (frameRateLimit > 0) {
        const auto framePeriod = std::chrono::duration_cast<
            std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(
                1.0 / static_cast<double>(frameRateLimit)));
        const auto deadline = m_frameStart + framePeriod;
        constexpr auto schedulerMargin = std::chrono::microseconds(500);
        auto now = std::chrono::steady_clock::now();
        if (now + schedulerMargin < deadline) {
            std::this_thread::sleep_until(deadline - schedulerMargin);
        }
        while ((now = std::chrono::steady_clock::now()) < deadline) {
            std::this_thread::yield();
        }
    } else {
        SDL_Delay(0);
    }
}

} // namespace app
