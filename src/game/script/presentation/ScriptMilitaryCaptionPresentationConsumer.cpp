#include "core/container/container_types.h"
#include "ScriptMilitaryCaptionPresentationConsumer.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace engine::script {
namespace {

[[nodiscard]] bool isContinuationByte(unsigned char byte) noexcept {
    return (byte & 0xC0u) == 0x80u;
}

[[nodiscard]] size_t utf8SequenceLength(unsigned char lead) noexcept {
    if ((lead & 0x80u) == 0u) return 1;
    if ((lead & 0xE0u) == 0xC0u) return 2;
    if ((lead & 0xF0u) == 0xE0u) return 3;
    if ((lead & 0xF8u) == 0xF0u) return 4;
    return 1;
}

[[nodiscard]] size_t completeUtf8CharacterBytes(const container::String& text, size_t offset) noexcept {
    if (offset >= text.size()) return 0;
    const size_t length = utf8SequenceLength(static_cast<unsigned char>(text[offset]));
    if (length == 1 || offset + length > text.size()) return 1;
    for (size_t index = 1; index < length; ++index) {
        if (!isContinuationByte(static_cast<unsigned char>(text[offset + index]))) return 1;
    }
    return length;
}

[[nodiscard]] float clampUnit(float value) noexcept {
    return std::clamp(value, 0.0f, 1.0f);
}

} // namespace

void ScriptMilitaryCaptionPresentationConsumer::clear() noexcept {
    m_hasCaption = false;
    m_text.clear();
    m_characterCount = 0;
    m_start = {};
    m_expire = {};
    m_fadeEnd = {};
}

bool ScriptMilitaryCaptionPresentationConsumer::present(
    container::String resolvedText, uint32_t durationMilliseconds, uint64_t confirmedTick,
    uint64_t currentConfirmedTick, uint32_t logicFramesPerSecond, Clock::time_point now) {
    const bool hadCaption = m_hasCaption;
    // RefCode removes the old subtitle before it checks the new label and
    // duration.  Preserve that replacement rule, but make malformed/missing
    // localized text a harmless clear instead of a debug crash.
    if (resolvedText.empty() || durationMilliseconds == 0) {
        clear();
        return hadCaption;
    }

    const uint64_t elapsedTicks = currentConfirmedTick > confirmedTick
        ? currentConfirmedTick - confirmedTick
        : 0;
    const uint64_t elapsedMs = elapsedMilliseconds(elapsedTicks, logicFramesPerSecond);
    const uint64_t visibleLifetimeMs = static_cast<uint64_t>(durationMilliseconds) +
        static_cast<uint64_t>(kFadeDuration.count());
    // A delayed presentation activation must not make a confirmed caption
    // immortal.  It is already fully gone by the time UI observes it.
    if (elapsedMs >= visibleLifetimeMs) {
        clear();
        return hadCaption;
    }

    const auto elapsed = std::chrono::milliseconds{
        static_cast<std::chrono::milliseconds::rep>(elapsedMs)};
    m_hasCaption = true;
    m_text = std::move(resolvedText);
    m_characterCount = countUtf8CodePoints(m_text);
    m_start = now - elapsed;
    m_expire = m_start + std::chrono::milliseconds{durationMilliseconds};
    m_fadeEnd = m_expire + kFadeDuration;
    return true;
}

bool ScriptMilitaryCaptionPresentationConsumer::active(Clock::time_point now) const noexcept {
    return m_hasCaption && now < m_fadeEnd;
}

float ScriptMilitaryCaptionPresentationConsumer::opacity(Clock::time_point now) const noexcept {
    if (!active(now)) return 0.0f;
    if (now <= m_expire) return 1.0f;

    const std::chrono::duration<float> elapsed = now - m_expire;
    const std::chrono::duration<float> duration = kFadeDuration;
    if (duration.count() <= 0.0f) return 0.0f;
    return clampUnit(1.0f - elapsed.count() / duration.count());
}

container::String ScriptMilitaryCaptionPresentationConsumer::visibleText(Clock::time_point now) const {
    const size_t count = visibleCharacterCount(now);
    return m_text.substr(0, utf8PrefixBytes(m_text, count));
}

bool ScriptMilitaryCaptionPresentationConsumer::cursorVisible(Clock::time_point now) const noexcept {
    if (!active(now) || now >= m_expire) return false;
    const std::chrono::duration<float, std::milli> elapsed = now - m_start;
    const float blink = static_cast<float>(kCursorBlinkPeriod.count());
    if (blink <= 0.0f) return true;
    const uint64_t phase = static_cast<uint64_t>(std::max(0.0f, elapsed.count()) / blink);
    return (phase & 1u) == 0u;
}

size_t ScriptMilitaryCaptionPresentationConsumer::visibleCharacterCount(
    Clock::time_point now) const noexcept {
    if (!active(now) || now <= m_start + kInitialTypingDelay) return 0;

    // The legacy update stops adding glyphs once the requested lifetime has
    // elapsed.  Keep that rule even while the final alpha fade is visible.
    const Clock::time_point revealEnd = std::min(now, m_expire);
    if (revealEnd <= m_start + kInitialTypingDelay) return 0;
    const std::chrono::duration<float, std::milli> elapsed =
        revealEnd - (m_start + kInitialTypingDelay);
    const float interval = static_cast<float>(kCharacterInterval.count());
    const uint64_t additional = interval > 0.0f
        ? static_cast<uint64_t>(std::max(0.0f, elapsed.count()) / interval)
        : static_cast<uint64_t>(m_characterCount);
    const uint64_t visible = std::min<uint64_t>(
        static_cast<uint64_t>(m_characterCount), additional + 1u);
    return static_cast<size_t>(visible);
}

uint64_t ScriptMilitaryCaptionPresentationConsumer::elapsedMilliseconds(
    uint64_t elapsedTicks, uint32_t framesPerSecond) noexcept {
    const uint64_t fps = std::max<uint64_t>(1, framesPerSecond);
    const uint64_t wholeSeconds = elapsedTicks / fps;
    const uint64_t remainderTicks = elapsedTicks % fps;
    constexpr uint64_t kMaximum = std::numeric_limits<uint64_t>::max();
    if (wholeSeconds > kMaximum / 1000u) return kMaximum;
    const uint64_t secondsMs = wholeSeconds * 1000u;
    const uint64_t remainderMs = (remainderTicks * 1000u) / fps;
    return secondsMs > kMaximum - remainderMs ? kMaximum : secondsMs + remainderMs;
}

size_t ScriptMilitaryCaptionPresentationConsumer::countUtf8CodePoints(
    const container::String& text) noexcept {
    size_t count = 0;
    for (size_t offset = 0; offset < text.size();) {
        offset += completeUtf8CharacterBytes(text, offset);
        ++count;
    }
    return count;
}

size_t ScriptMilitaryCaptionPresentationConsumer::utf8PrefixBytes(
    const container::String& text, size_t codePointCount) noexcept {
    size_t offset = 0;
    while (offset < text.size() && codePointCount > 0) {
        offset += completeUtf8CharacterBytes(text, offset);
        --codePointCount;
    }
    return offset;
}

} // namespace engine::script
