#pragma once

#include "ScriptCinematicPresentationControls.h"

#include <chrono>
#include <cstdint>

namespace engine::script {

// Client-local projection of the durable letterbox command published by a
// confirmed GameSession.  It intentionally owns no simulation clock or
// renderer state: a sequence change starts a presentation-only wall-clock
// fade, while duplicate Begin/End commands retain their existing revision and
// therefore do not restart the transition.
class ScriptLetterboxPresentationConsumer final {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr std::chrono::milliseconds kFadeDuration{1000};

    void clear() noexcept;

    // Returns true only when the session's (epoch, sequence) revision
    // changed.  An epoch change begins from transparent, preventing a reused
    // GameSession instance from carrying a previous match's visual state.
    [[nodiscard]] bool synchronize(const ScriptLetterboxPresentationState& state,
                                   Clock::time_point now) noexcept;

    [[nodiscard]] float opacity(Clock::time_point now) const noexcept;
    [[nodiscard]] bool suppressesGameplayHud() const noexcept { return m_targetEnabled; }
    [[nodiscard]] bool hasRevision() const noexcept { return m_hasRevision; }
    [[nodiscard]] ScriptPresentationControlStamp revision() const noexcept {
        return m_revision;
    }

private:
    bool m_hasRevision = false;
    bool m_targetEnabled = false;
    ScriptPresentationControlStamp m_revision{};
    float m_fadeStartOpacity = 0.0f;
    Clock::time_point m_fadeStart{};
};

} // namespace engine::script
