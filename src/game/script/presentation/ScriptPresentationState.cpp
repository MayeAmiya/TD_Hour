#include "core/container/container_types.h"
#include "ScriptPresentationState.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace engine::script {
namespace {

[[nodiscard]] uint64_t eventStartTick(const ScriptSessionEvent& event,
                                       uint64_t currentConfirmedTick) noexcept {
    // A presentation subsystem can first activate a few confirmed ticks after
    // the logic event was emitted.  Do not make an old event immortal by
    // restarting its lifetime at activation, but do tolerate malformed/future
    // event stamps without underflow.
    return std::min(event.confirmedTick, currentConfirmedTick);
}

} // namespace

void ScriptPresentationState::clear() noexcept {
    m_messages.clear();
    m_cinematic.reset();
    m_subtitle.reset();
}

void ScriptPresentationState::consume(container::Vector<ScriptSessionEvent> events,
                                      uint64_t currentConfirmedTick,
                                      uint32_t ordinaryMessageLifetimeTicks,
                                      uint32_t ordinaryMessageFadeTicks) {
    for (ScriptSessionEvent& event : events) {
        if (event.text.empty()) continue;

        const uint64_t startTick = eventStartTick(event, currentConfirmedTick);
        switch (event.kind) {
        case ScriptSessionEventKind::Text: {
            const uint64_t lifetime = ordinaryMessageLifetimeTicks;
            if (lifetime == 0) continue;
            const uint64_t fade = std::min<uint64_t>(ordinaryMessageFadeTicks, lifetime);
            ScriptPresentationMessage message{
                .event = std::move(event),
                .fadeBeginTick = saturatedAdd(startTick, lifetime - fade),
                .expireTick = saturatedAdd(startTick, lifetime),
            };
            // InGameUI shifts the old stack down and places each new message
            // at index zero.  `insert(begin)` retains that visual order while
            // the fixed cap releases the oldest entry.
            m_messages.insert(m_messages.begin(), std::move(message));
            if (m_messages.size() > kMaximumMessages) m_messages.pop_back();
            break;
        }
        case ScriptSessionEventKind::CinematicText:
            // RefCode's Display owns one active cinematic string.  A zero
            // frame count writes the value but draws nothing, so model it as
            // a clear rather than retaining an invisible stale overlay.
            if (event.durationTicks == 0) {
                m_cinematic.reset();
            } else {
                m_cinematic = ScriptPresentationOverlay{
                    .event = std::move(event),
                    .expireTick = saturatedAdd(startTick, event.durationTicks),
                };
            }
            break;
        case ScriptSessionEventKind::Subtitle:
            if (event.durationTicks == 0) {
                m_subtitle.reset();
            } else {
                // Retained for value-level callers. The production InGameUI
                // routes SPEECH_PLAY through its shared military-caption
                // consumer, matching the original single subtitle owner.
                m_subtitle = ScriptPresentationOverlay{
                    .event = std::move(event),
                    .expireTick = saturatedAdd(startTick, event.durationTicks),
                };
            }
            break;
        case ScriptSessionEventKind::MilitaryCaption:
            // SHOW_MILITARY_CAPTION is consumed by its own wall-clock
            // typewriter. It must not fall through to ordinary message or
            // subtitle lifetime rules if a future presentation caller sends
            // a mixed batch directly to this state object.
            break;
        case ScriptSessionEventKind::Diagnostic:
            // Diagnostics are delivered through the same bounded session
            // queue so no producer can leak memory, but they remain log/UI
            // policy rather than a player-facing script message.
            break;
        }
    }
    advance(currentConfirmedTick);
}

void ScriptPresentationState::advance(uint64_t currentConfirmedTick) noexcept {
    std::erase_if(m_messages, [currentConfirmedTick](const ScriptPresentationMessage& message) {
        return currentConfirmedTick >= message.expireTick;
    });
    if (m_cinematic && currentConfirmedTick >= m_cinematic->expireTick) {
        m_cinematic.reset();
    }
    if (m_subtitle && currentConfirmedTick >= m_subtitle->expireTick) {
        m_subtitle.reset();
    }
}

uint8_t ScriptPresentationState::messageOpacity(const ScriptPresentationMessage& message,
                                                uint64_t currentConfirmedTick) noexcept {
    if (currentConfirmedTick >= message.expireTick) return 0;
    if (currentConfirmedTick <= message.fadeBeginTick ||
        message.fadeBeginTick >= message.expireTick) {
        return 255;
    }

    const uint64_t fadeDuration = message.expireTick - message.fadeBeginTick;
    const uint64_t elapsed = currentConfirmedTick - message.fadeBeginTick;
    const uint64_t remaining = fadeDuration > elapsed ? fadeDuration - elapsed : 0;
    return static_cast<uint8_t>((remaining * 255u) / fadeDuration);
}

uint64_t ScriptPresentationState::saturatedAdd(uint64_t value, uint64_t delta) noexcept {
    return value > std::numeric_limits<uint64_t>::max() - delta
        ? std::numeric_limits<uint64_t>::max()
        : value + delta;
}

} // namespace engine::script
