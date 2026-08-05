#include "core/container/container_types.h"
#include "ScriptPresentationCompletionLedger.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace engine::script {

void ScriptPresentationCompletionLedger::reset(uint64_t presentationEpoch) noexcept {
    m_presentationEpoch = presentationEpoch;
    m_oneShotCompletions.clear();
    m_music = {};
}

bool ScriptPresentationCompletionLedger::recordOneShot(
    ScriptPresentationCompletion completion) {
    if (completion.kind == ScriptPresentationCompletionKind::MusicLoop ||
        !acceptsStamp(completion.stamp) || !validName(completion.name)) {
        return false;
    }

    // HAS_FINISHED_* consumes an exact completion fact. Evicting an older,
    // unrelated key makes a valid sequential script wait forever, so retain
    // every admitted fact for the lifetime of this session or until consumed.
    m_oneShotCompletions.push_back(std::move(completion));
    return true;
}

std::optional<ScriptPresentationCompletion> ScriptPresentationCompletionLedger::consumeOne(
    ScriptPresentationCompletionKind kind, container::StringView name) {
    if (kind == ScriptPresentationCompletionKind::MusicLoop || !validName(name)) {
        return std::nullopt;
    }
    const auto found = std::find_if(
        m_oneShotCompletions.begin(), m_oneShotCompletions.end(),
        [kind, name](const ScriptPresentationCompletion& completion) {
            return completion.kind == kind && completion.name == name;
        });
    if (found == m_oneShotCompletions.end()) return std::nullopt;

    ScriptPresentationCompletion result = std::move(*found);
    m_oneShotCompletions.erase(found);
    return result;
}

bool ScriptPresentationCompletionLedger::hasOne(ScriptPresentationCompletionKind kind,
                                                container::StringView name) const noexcept {
    if (kind == ScriptPresentationCompletionKind::MusicLoop || !validName(name)) return false;
    return std::any_of(
        m_oneShotCompletions.begin(), m_oneShotCompletions.end(),
        [kind, name](const ScriptPresentationCompletion& completion) {
            return completion.kind == kind && completion.name == name;
        });
}

bool ScriptPresentationCompletionLedger::beginMusicTrack(
    container::String trackName, ScriptPresentationControlStamp stamp) {
    if (!acceptsStamp(stamp) || !validName(trackName)) return false;
    m_music = {
        .trackName = std::move(trackName),
        .started = stamp,
        .completedLoops = 0,
        .active = true,
    };
    return true;
}

void ScriptPresentationCompletionLedger::stopMusicTrack() noexcept {
    m_music = {};
}

bool ScriptPresentationCompletionLedger::recordMusicLoop(container::StringView trackName,
                                                          uint64_t presentationEpoch) noexcept {
    if (presentationEpoch == 0 || presentationEpoch != m_presentationEpoch ||
        !m_music.active || !validName(trackName) || m_music.trackName != trackName) {
        return false;
    }
    if (m_music.completedLoops != std::numeric_limits<uint64_t>::max()) {
        ++m_music.completedLoops;
    }
    return true;
}

bool ScriptPresentationCompletionLedger::musicTrackHasCompleted(
    container::StringView trackName, int32_t minimumLoops) const noexcept {
    // MilesAudioManager returns false when the requested track is not the
    // active stream. For an active stream its loop counter starts at zero, so
    // a non-positive authored threshold is immediately true, exactly like
    // `completedLoops >= numberOfTimes` in the original implementation.
    if (!m_music.active || !validName(trackName) || m_music.trackName != trackName) return false;
    if (minimumLoops <= 0) return true;
    return m_music.completedLoops >= static_cast<uint64_t>(minimumLoops);
}

bool ScriptPresentationCompletionLedger::acceptsStamp(
    const ScriptPresentationControlStamp& stamp) const noexcept {
    return m_presentationEpoch != 0 && stamp.presentationEpoch == m_presentationEpoch &&
        stamp.sequence != 0;
}

bool ScriptPresentationCompletionLedger::validName(container::StringView name) noexcept {
    return !name.empty() && name.size() <= kMaximumScriptPresentationNameLength &&
        name.find('\0') == container::StringView::npos;
}

} // namespace engine::script
