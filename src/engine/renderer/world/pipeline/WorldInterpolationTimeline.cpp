#include "engine/renderer/world/pipeline/WorldInterpolationTimeline.h"
#include "core/debug/debug.h"

#include <algorithm>
#include <cmath>

namespace engine::render {

void WorldInterpolationTimeline::setEnabled(bool enabled) noexcept {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    if (!m_enabled) {
        // Disabling interpolation is an immediate renderer-local policy
        // change. Collapse the published logical pair to B/B as well as
        // returning alpha=1 so picking/UI never keep looking for a retired A
        // endpoint that the world pass no longer displays.
        m_started = {};
        m_duration = {};
        m_worldARevision = m_worldBRevision;
        m_active = false;
    }
}

void WorldInterpolationTimeline::reset() noexcept {
    m_started = {};
    m_duration = {};
    m_worldARevision = 0;
    m_worldBRevision = 0;
    m_active = false;
}

void WorldInterpolationTimeline::beginEndpoint(
    const WorldPreparationStamp* previous,
    const WorldPreparationStamp& current,
    uint32_t logicFramesPerSecond,
    Clock::time_point now) noexcept {
    const uint64_t tickDelta = previous &&
            current.simulationFrame > previous->simulationFrame
        ? current.simulationFrame - previous->simulationFrame
        : 0u;
    constexpr uint64_t kMaximumCatchUpInterpolationTicks = 3u;
    const bool compatiblePair = previous &&
        m_enabled &&
        previous->presentationEpoch == current.presentationEpoch &&
        previous->sessionRevision == current.sessionRevision &&
        previous->loadingRevision == current.loadingRevision &&
        previous->simulationFrame != UINT64_MAX &&
        tickDelta != 0u &&
        tickDelta <= kMaximumCatchUpInterpolationTicks;
    if (!compatiblePair) {
        m_started = {};
        m_duration = {};
        m_worldARevision = current.worldRevision;
        m_worldBRevision = current.worldRevision;
        m_active = false;
        return;
    }

    const uint32_t tickRate = std::max(1u, logicFramesPerSecond);
    // World state is newest-biased. If one or two intermediate logic samples
    // were superseded, catch the visible state up within one ordinary logic
    // interval instead of spending tickDelta intervals replaying history.
    // Larger discontinuities were rejected above and collapse to B/B.
    m_started = now;
    m_duration = std::chrono::duration<float>(
        1.0f / static_cast<float>(tickRate));
    m_worldARevision = previous->worldRevision;
    m_worldBRevision = current.worldRevision;
    m_active = m_duration.count() > 0.0f;
    if (current.simulationFrame % 300u == 0u) {
        TD_LOG_INFO(
            "[InterpolationClock] frame={} previous={} gap={} duration={}us",
            current.simulationFrame,
            previous ? previous->simulationFrame : UINT64_MAX,
            tickDelta,
            std::chrono::duration_cast<std::chrono::microseconds>(
                m_duration).count());
    }
}

float WorldInterpolationTimeline::alpha(Clock::time_point now) const noexcept {
    const float continuous = continuousAlpha(now);
    if (!m_quantizeSamples || continuous <= 0.0f || continuous >= 1.0f) {
        return continuous;
    }
    const float steps = static_cast<float>(
        m_maximumIntermediateSamples + 1u);
    return std::clamp(std::round(continuous * steps) / steps, 0.0f, 1.0f);
}

float WorldInterpolationTimeline::continuousAlpha(
    Clock::time_point now) const noexcept {
    if (!m_active || m_duration.count() <= 0.0f) return 1.0f;
    const float elapsed = std::chrono::duration<float>(now - m_started).count();
    return std::clamp(elapsed / m_duration.count(), 0.0f, 1.0f);
}

bool WorldInterpolationTimeline::endpointRotationReady(
    bool hasCompletedEndpoint, Clock::time_point now) const noexcept {
    return !m_enabled || !hasCompletedEndpoint ||
        continuousAlpha(now) >= 1.0f;
}

} // namespace engine::render
