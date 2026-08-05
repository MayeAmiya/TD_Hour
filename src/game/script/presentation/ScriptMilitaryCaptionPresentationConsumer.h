#pragma once

#include "core/container/container_types.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
namespace engine::script {

// Client-local state for SHOW_MILITARY_CAPTION.  The legacy UI advances this
// particular typewriter while script time is frozen, so it intentionally uses
// presentation wall time rather than becoming another simulation timer.  The
// originating confirmed tick is used only to avoid reviving a stale caption
// when a UI activates late.
class ScriptMilitaryCaptionPresentationConsumer final {
public:
    using Clock = std::chrono::steady_clock;

    // RefCode defaults: first glyph after 750ms, Courier text at roughly one
    // logic frame per glyph, and a ten-frame cursor blink.  Milliseconds make
    // the modern presentation stable for non-30fps simulations and during a
    // script-driven time freeze.
    static constexpr std::chrono::milliseconds kInitialTypingDelay{750};
    static constexpr std::chrono::milliseconds kCharacterInterval{33};
    // RefCode's cumulative per-frame alpha subtraction reaches zero after
    // roughly 2.5 seconds at its fixed 30Hz.  Use one smooth local fade of
    // the same overall duration instead of making client visual timing depend
    // on how many render updates occurred in a frame.
    static constexpr std::chrono::milliseconds kFadeDuration{2500};
    static constexpr std::chrono::milliseconds kCursorBlinkPeriod{333};

    void clear() noexcept;

    // Replaces the active caption exactly as RefCode's militarySubtitle()
    // first removes the prior subtitle. `durationMilliseconds == 0` and an
    // empty resolved string therefore clear any existing caption.  The
    // returned value reports whether the visible request/state changed.
    [[nodiscard]] bool present(container::String resolvedText,
                               uint32_t durationMilliseconds,
                               uint64_t confirmedTick,
                               uint64_t currentConfirmedTick,
                               uint32_t logicFramesPerSecond,
                               Clock::time_point now);

    [[nodiscard]] bool active(Clock::time_point now) const noexcept;
    [[nodiscard]] float opacity(Clock::time_point now) const noexcept;
    [[nodiscard]] container::String visibleText(Clock::time_point now) const;
    [[nodiscard]] bool cursorVisible(Clock::time_point now) const noexcept;
    [[nodiscard]] size_t visibleCharacterCount(Clock::time_point now) const noexcept;
    [[nodiscard]] size_t characterCount() const noexcept { return m_characterCount; }

private:
    [[nodiscard]] static uint64_t elapsedMilliseconds(uint64_t elapsedTicks,
                                                       uint32_t framesPerSecond) noexcept;
    [[nodiscard]] static size_t countUtf8CodePoints(const container::String& text) noexcept;
    [[nodiscard]] static size_t utf8PrefixBytes(const container::String& text,
                                                 size_t codePointCount) noexcept;

    bool m_hasCaption = false;
    container::String m_text;
    size_t m_characterCount = 0;
    Clock::time_point m_start{};
    Clock::time_point m_expire{};
    Clock::time_point m_fadeEnd{};
};

} // namespace engine::script
