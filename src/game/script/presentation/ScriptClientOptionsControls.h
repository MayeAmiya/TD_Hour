#pragma once

#include "game/script/presentation/ScriptCinematicPresentationControls.h"
#include "game/script/contracts/ScriptPresentationValueTypes.h"

#include <cstdint>

namespace engine::script {

// These are confirmed map-script requests, but all four target local client
// policy rather than lockstep state. Keeping them in a distinct typed family
// prevents FPS/renderer settings from being mistaken for ECS or replay
// commands.

// Session-owned local policy. `requestedFrameRateLimit` preserves the
// original authored integer, while effectiveFrameRateLimit is resolved once
// at the GameSession boundary (`0 -> GlobalData default`) so main's render
// loop never reaches back into mutable configuration while a session runs.
class ScriptClientOptionsState final {
public:
    void reset(uint64_t presentationEpoch = 0) noexcept;
    void rebindPresentationEpoch(uint64_t presentationEpoch) noexcept;

    [[nodiscard]] bool setFrameRateLimit(int32_t requested, int32_t effective,
                                         ScriptPresentationControlStamp stamp) noexcept;
    [[nodiscard]] bool setOcclusionEnabled(bool enabled,
                                           ScriptPresentationControlStamp stamp) noexcept;
    [[nodiscard]] bool setDrawIconUiEnabled(bool enabled,
                                            ScriptPresentationControlStamp stamp) noexcept;
    [[nodiscard]] bool setDynamicParticleLodEnabled(
        bool enabled, ScriptPresentationControlStamp stamp) noexcept;

    [[nodiscard]] bool hasFrameRateLimitOverride() const noexcept {
        return m_hasFrameRateLimitOverride;
    }
    [[nodiscard]] int32_t requestedFrameRateLimit() const noexcept {
        return m_requestedFrameRateLimit;
    }
    [[nodiscard]] int32_t effectiveFrameRateLimit() const noexcept {
        return m_effectiveFrameRateLimit;
    }
    [[nodiscard]] bool occlusionEnabled() const noexcept { return m_occlusionEnabled; }
    [[nodiscard]] bool drawIconUiEnabled() const noexcept { return m_drawIconUiEnabled; }
    [[nodiscard]] const ScriptPresentationControlStamp& lastMutation() const noexcept {
        return m_lastMutation;
    }

private:
    bool m_hasFrameRateLimitOverride = false;
    int32_t m_requestedFrameRateLimit = 0;
    int32_t m_effectiveFrameRateLimit = 0;
    bool m_occlusionEnabled = true;
    bool m_drawIconUiEnabled = true;
    ScriptPresentationControlStamp m_lastMutation{};
};

} // namespace engine::script
