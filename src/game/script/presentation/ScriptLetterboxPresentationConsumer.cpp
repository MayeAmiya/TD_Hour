#include "ScriptLetterboxPresentationConsumer.h"

#include <algorithm>

namespace engine::script {
namespace {

[[nodiscard]] bool sameRevision(const ScriptPresentationControlStamp& left,
                                const ScriptPresentationControlStamp& right) noexcept {
    // Sequence is unique only within an epoch.  The remaining stamp fields
    // describe provenance, not an independent visual revision.
    return left.presentationEpoch == right.presentationEpoch &&
           left.sequence == right.sequence;
}

[[nodiscard]] float clampUnit(float value) noexcept {
    return std::clamp(value, 0.0f, 1.0f);
}

} // namespace

void ScriptLetterboxPresentationConsumer::clear() noexcept {
    m_hasRevision = false;
    m_targetEnabled = false;
    m_revision = {};
    m_fadeStartOpacity = 0.0f;
    m_fadeStart = {};
}

bool ScriptLetterboxPresentationConsumer::synchronize(
    const ScriptLetterboxPresentationState& state, Clock::time_point now) noexcept {
    const bool epochChanged = !m_hasRevision ||
        state.stamp.presentationEpoch != m_revision.presentationEpoch;
    if (!epochChanged && sameRevision(state.stamp, m_revision)) return false;

    // A session epoch is a hard presentation boundary.  A reused GameSession
    // must never fade from a prior match's letterbox alpha; otherwise an old
    // enabled state can briefly cover the next map before its first script
    // update.  Within one epoch, reversals are smooth and begin at the alpha
    // already visible on this client.
    m_fadeStartOpacity = epochChanged ? 0.0f : opacity(now);
    m_targetEnabled = state.enabled;
    m_revision = state.stamp;
    m_hasRevision = true;
    m_fadeStart = now;
    return true;
}

float ScriptLetterboxPresentationConsumer::opacity(Clock::time_point now) const noexcept {
    if (!m_hasRevision) return 0.0f;
    if (now <= m_fadeStart) return clampUnit(m_fadeStartOpacity);

    const std::chrono::duration<float> elapsed = now - m_fadeStart;
    const std::chrono::duration<float> duration = kFadeDuration;
    const float progress = duration.count() > 0.0f
        ? clampUnit(elapsed.count() / duration.count())
        : 1.0f;
    const float target = m_targetEnabled ? 1.0f : 0.0f;
    return clampUnit(m_fadeStartOpacity + (target - m_fadeStartOpacity) * progress);
}

} // namespace engine::script
