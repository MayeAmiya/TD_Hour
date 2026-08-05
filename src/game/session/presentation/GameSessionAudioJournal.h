#pragma once

#include "game/audio/GameAudioEvents.h"

#include <cmath>
#include <cstdint>
#include <utility>

namespace engine {

// Single owner of the confirmed audio command stream. Producers submit
// detached values; device/backend state never enters the session.
class GameSessionAudioJournal final {
public:
    void reset(uint64_t presentationEpoch = 0) noexcept {
        m_presentationEpoch = presentationEpoch;
        m_confirmedFrame = 0;
        m_eventOrdinal = 0;
        m_events.clear();
        m_controlEvents.clear();
    }

    void rebindEpoch(uint64_t presentationEpoch) noexcept {
        m_presentationEpoch = presentationEpoch;
        m_events.clear();
        m_controlEvents.clear();
    }

    void beginConfirmedFrame(
        uint64_t confirmedFrame, bool resetOrdinal) noexcept {
        m_confirmedFrame = confirmedFrame;
        if (resetOrdinal) m_eventOrdinal = 0;
    }

    [[nodiscard]] bool emit(
        game::GameAudioEvent event, int32_t sessionSeed) {
        if (event.eventName.empty() && !event.evaPolicy) return false;
        const uint64_t ordinal = ++m_eventOrdinal;
        event.confirmedFrame = m_confirmedFrame;
        if (event.eventId == 0) {
            event.eventId = (m_confirmedFrame << 32u) | ordinal;
            if (event.eventId == 0) event.eventId = ordinal == 0 ? 1 : ordinal;
        }
        if (event.variationSeed == 0) {
            uint64_t key = static_cast<uint32_t>(sessionSeed);
            key ^= m_confirmedFrame * 0x9e3779b97f4a7c15ull;
            key ^= ordinal * 0xbf58476d1ce4e5b9ull;
            if (event.emitter && *event.emitter) {
                key ^= static_cast<uint64_t>(event.emitter->value) << 17u;
            }
            event.variationSeed = mixSeed(key);
            if (event.variationSeed == 0) event.variationSeed = 1;
        }
        m_events.push_back(std::move(event));
        return true;
    }

    [[nodiscard]] bool emit(game::GameAudioControlEvent event) {
        if (!std::isfinite(event.volume)) return false;
        switch (event.kind) {
        case game::GameAudioControlKind::SetMusicTrack:
            if (event.trackName.empty()) return false;
            break;
        case game::GameAudioControlKind::SetEventVolumeOverride:
        case game::GameAudioControlKind::RestoreEventVolumeOverride:
        case game::GameAudioControlKind::RemoveEvent:
            if (event.eventName.empty()) return false;
            break;
        case game::GameAudioControlKind::SetObjectAmbientSoundEnabled:
        case game::GameAudioControlKind::SetObjectLoopingSoundEnabled:
            if (!event.object || !*event.object ||
                (event.enabled && event.eventName.empty())) {
                return false;
            }
            break;
        case game::GameAudioControlKind::SetMusicVolume:
        case game::GameAudioControlKind::SetAmbientPaused:
        case game::GameAudioControlKind::SetBackgroundSoundsPaused:
        case game::GameAudioControlKind::SetSoundVolume:
        case game::GameAudioControlKind::SetSpeechVolume:
        case game::GameAudioControlKind::RestoreAllEventVolumeOverrides:
        case game::GameAudioControlKind::RemoveDisabledEvents:
        case game::GameAudioControlKind::SetEvaEnabled:
            break;
        }
        const uint64_t ordinal = ++m_eventOrdinal;
        event.confirmedFrame = m_confirmedFrame;
        if (event.eventId == 0) {
            event.eventId = (m_confirmedFrame << 32u) | ordinal;
            if (event.eventId == 0) event.eventId = ordinal == 0 ? 1 : ordinal;
        }
        m_controlEvents.push_back(std::move(event));
        return true;
    }

    [[nodiscard]] container::Vector<game::GameAudioEvent> takeEvents() {
        container::Vector<game::GameAudioEvent> result = std::move(m_events);
        m_events.clear();
        return result;
    }

    [[nodiscard]] container::Vector<game::GameAudioControlEvent>
    takeControlEvents() {
        container::Vector<game::GameAudioControlEvent> result =
            std::move(m_controlEvents);
        m_controlEvents.clear();
        return result;
    }

    [[nodiscard]] uint64_t presentationEpoch() const noexcept {
        return m_presentationEpoch;
    }
    [[nodiscard]] uint64_t confirmedFrame() const noexcept {
        return m_confirmedFrame;
    }
    [[nodiscard]] uint32_t eventOrdinal() const noexcept {
        return m_eventOrdinal;
    }
    [[nodiscard]] bool empty() const noexcept {
        return m_presentationEpoch == 0 && m_confirmedFrame == 0 &&
            m_eventOrdinal == 0 && m_events.empty() &&
            m_controlEvents.empty();
    }

private:
    [[nodiscard]] static uint64_t mixSeed(uint64_t value) noexcept {
        value += 0x9e3779b97f4a7c15ull;
        value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
        value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
        return value ^ (value >> 31u);
    }

    uint64_t m_presentationEpoch = 0;
    uint64_t m_confirmedFrame = 0;
    uint32_t m_eventOrdinal = 0;
    container::Vector<game::GameAudioEvent> m_events;
    container::Vector<game::GameAudioControlEvent> m_controlEvents;
};

} // namespace engine
