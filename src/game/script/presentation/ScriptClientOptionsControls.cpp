#include "ScriptClientOptionsControls.h"

namespace engine::script {

void ScriptClientOptionsState::reset(uint64_t presentationEpoch) noexcept {
    m_hasFrameRateLimitOverride = false;
    m_requestedFrameRateLimit = 0;
    m_effectiveFrameRateLimit = 0;
    m_occlusionEnabled = true;
    m_drawIconUiEnabled = true;
    m_lastMutation = {.presentationEpoch = presentationEpoch};
}

void ScriptClientOptionsState::rebindPresentationEpoch(
    uint64_t presentationEpoch) noexcept {
    m_lastMutation.presentationEpoch = presentationEpoch;
}

bool ScriptClientOptionsState::setFrameRateLimit(
    int32_t requested, int32_t effective, ScriptPresentationControlStamp stamp) noexcept {
    if (m_hasFrameRateLimitOverride && m_requestedFrameRateLimit == requested &&
        m_effectiveFrameRateLimit == effective) {
        return false;
    }
    m_hasFrameRateLimitOverride = true;
    m_requestedFrameRateLimit = requested;
    m_effectiveFrameRateLimit = effective;
    m_lastMutation = stamp;
    return true;
}

bool ScriptClientOptionsState::setOcclusionEnabled(
    bool enabled, ScriptPresentationControlStamp stamp) noexcept {
    if (m_occlusionEnabled == enabled) return false;
    m_occlusionEnabled = enabled;
    m_lastMutation = stamp;
    return true;
}

bool ScriptClientOptionsState::setDrawIconUiEnabled(
    bool enabled, ScriptPresentationControlStamp stamp) noexcept {
    if (m_drawIconUiEnabled == enabled) return false;
    m_drawIconUiEnabled = enabled;
    m_lastMutation = stamp;
    return true;
}

bool ScriptClientOptionsState::setDynamicParticleLodEnabled(
    bool enabled, ScriptPresentationControlStamp stamp) noexcept {
    // Product policy keeps authored particle detail fixed at VeryHigh.  Keep
    // accepting the legacy script action as a harmless presentation no-op so
    // old maps need no special-case rewrite.
    static_cast<void>(enabled);
    static_cast<void>(stamp);
    return false;
}

} // namespace engine::script
