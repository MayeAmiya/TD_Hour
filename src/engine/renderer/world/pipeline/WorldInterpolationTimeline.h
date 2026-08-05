#pragma once

#include "presentation/render/RenderSceneSnapshot.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace engine::render {

// Renderer-owned display clock for two authoritative world endpoints. It
// never advances simulation; it only decides how the GPU samples A/B while
// the resource domain prepares C.
class WorldInterpolationTimeline final {
public:
    using Clock = std::chrono::steady_clock;

    void setEnabled(bool enabled) noexcept;
    [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
    void setMaximumIntermediateSamples(uint32_t samples) noexcept {
        m_maximumIntermediateSamples = std::min(samples, 3u);
        m_quantizeSamples = true;
    }
    void reset() noexcept;
    void beginEndpoint(
        const WorldPreparationStamp* previous,
        const WorldPreparationStamp& current,
        uint32_t logicFramesPerSecond,
        Clock::time_point now) noexcept;

    [[nodiscard]] float alpha(Clock::time_point now) const noexcept;
    [[nodiscard]] bool endpointRotationReady(
        bool hasCompletedEndpoint, Clock::time_point now) const noexcept;
    [[nodiscard]] uint64_t worldARevision() const noexcept {
        return m_worldARevision;
    }
    [[nodiscard]] uint64_t worldBRevision() const noexcept {
        return m_worldBRevision;
    }
    [[nodiscard]] bool active() const noexcept { return m_active; }

private:
    [[nodiscard]] float continuousAlpha(
        Clock::time_point now) const noexcept;

    Clock::time_point m_started{};
    std::chrono::duration<float> m_duration{};
    uint64_t m_worldARevision = 0;
    uint64_t m_worldBRevision = 0;
    uint32_t m_maximumIntermediateSamples = 3;
    bool m_enabled = true;
    bool m_active = false;
    bool m_quantizeSamples = false;
};

} // namespace engine::render
