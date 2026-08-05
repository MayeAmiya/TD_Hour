#pragma once

#include "game/script/contracts/ScriptPresentationValueTypes.h"

#include "core/container/container_types.h"

#include "ScriptCinematicPresentationControls.h"
#include "game/script/contracts/ScriptPresentationLimits.h"

#include <cstddef>
#include <cstdint>
#include <optional>
namespace engine::script {

// The original ScriptEngine has two subtly different "finished media"
// concepts. Video/speech/audio completions are one-shot facts consumed by the
// matching condition, whereas MUSIC_TRACK_HAS_COMPLETED observes the loop
// count of the currently active track without consuming it.  Keep those
// semantics in one session-owned value ledger, rather than allowing a UI or
// audio callback to mutate ScriptRuntime directly.

struct ScriptPresentationCompletion final {
    ScriptPresentationCompletionKind kind = ScriptPresentationCompletionKind::Video;
    container::String name;
    ScriptPresentationControlStamp stamp{};
};

struct ScriptMusicLoopState final {
    container::String trackName;
    ScriptPresentationControlStamp started{};
    uint64_t completedLoops = 0;
    bool active = false;
};

// A deterministic-frame ingress for speech/audio completion facts plus the
// synchronous legacy-video compatibility fact. Exact one-shot keys are
// retained until consumed; session lifetime is the outer memory bound.
//
// This type deliberately has no AudioSystem, movie decoder, renderer, ECS,
// thread primitive, or ScriptRuntime dependency.  Audio/speech clients first
// validate natural backend completion and schedule that value at confirmed
// ingress; legacy Video is written synchronously by the script bridge.
// ScriptWorldQuery may consume/query only these value facts, so an arbitrary
// wall-clock callback cannot become a lockstep condition by accident.
class ScriptPresentationCompletionLedger final {
public:
    void reset(uint64_t presentationEpoch = 0) noexcept;

    [[nodiscard]] uint64_t presentationEpoch() const noexcept { return m_presentationEpoch; }

    // Records one validated completion.  Speech/Audio arrive through
    // confirmed ingress; Video is the script bridge's direct-complete
    // compatibility fact. Music loops use recordMusicLoop() so their
    // persistent counter cannot be confused with a consumable one-shot fact.
    // Returns false for stale epochs, malformed stamps/names, or MusicLoop
    // passed to the wrong API.
    [[nodiscard]] bool recordOneShot(ScriptPresentationCompletion completion);

    // Mirrors ScriptEngine::is{Video,Speech,Audio}Complete(..., true): the
    // oldest matching fact is consumed exactly once.  Name comparison stays
    // byte-exact because the legacy completion list stores the authored
    // AsciiString label; callers must use the same resolved label that was
    // submitted at the script boundary.
    [[nodiscard]] std::optional<ScriptPresentationCompletion> consumeOne(
        ScriptPresentationCompletionKind kind, container::StringView name);
    [[nodiscard]] bool hasOne(ScriptPresentationCompletionKind kind,
                              container::StringView name) const noexcept;
    [[nodiscard]] size_t pendingOneShotCount() const noexcept {
        return m_oneShotCompletions.size();
    }

    // Music has one active stream at a time. Starting a new name replaces the
    // previous state and resets its completed-loop counter, matching the
    // legacy AudioManager lookup which only succeeds for the active track.
    [[nodiscard]] bool beginMusicTrack(container::String trackName,
                                       ScriptPresentationControlStamp stamp);
    void stopMusicTrack() noexcept;
    // A decoder/sequencer calls this only after it observes one natural loop
    // boundary for the active track. The count saturates instead of wrapping;
    // a malformed endlessly-running session cannot turn a true predicate
    // false again.
    [[nodiscard]] bool recordMusicLoop(container::StringView trackName,
                                       uint64_t presentationEpoch) noexcept;
    [[nodiscard]] bool musicTrackHasCompleted(container::StringView trackName,
                                               int32_t minimumLoops) const noexcept;
    [[nodiscard]] const ScriptMusicLoopState& musicLoopState() const noexcept {
        return m_music;
    }

private:
    [[nodiscard]] bool acceptsStamp(const ScriptPresentationControlStamp& stamp) const noexcept;
    [[nodiscard]] static bool validName(container::StringView name) noexcept;

    uint64_t m_presentationEpoch = 0;
    container::Vector<ScriptPresentationCompletion> m_oneShotCompletions;
    ScriptMusicLoopState m_music;
};

} // namespace engine::script
