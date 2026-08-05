#pragma once

#include "core/container/hash_containers.h"

#include "AudioEventCatalog.h"
#include "AudioSystem.h"
#include "presentation/audio/AudioPresentationContracts.h"

#include <cstddef>
#include <cstdint>
#include <optional>
namespace engine::audio {

// Event instances deliberately have a different identity from backend voices.
// A single AudioEventRTS-style event can own an Attack, several Main loops and
// a Decay voice over its lifetime, while every individual sample has its own
// AudioVoiceHandle.
struct AudioEventInstanceHandle final {
    uint64_t value = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }
    [[nodiscard]] constexpr bool operator==(const AudioEventInstanceHandle&) const noexcept = default;
};

// Presentation-observable lifecycle for an authored AudioEvent, rather than
// one concrete WAV/MP3 voice. It mirrors AudioEventRTS portions while keeping
// device-dependent details out of simulation.
enum class AudioEventInstanceState : uint8_t {
    Unknown,
    Pending,
    Playing,
    Waiting,
    Completed,
    Stopped,
    Preempted,
    Failed,
    Rejected,
    Suppressed,
};

// Presentation-owned modern counterpart of original AudioEventRTS plus the
// EOS state machine in MilesAudioManager. It resolves immutable INI data via
// AudioEventCatalog, then sequences one-shot backend voices as
// Attack -> Main (with Loop/LoopCount/Delay) -> Decay. AudioSystem remains a
// generic VFS/device service and never learns authored event names or owners.
class AudioEventSequencer final {
public:
    AudioEventSequencer() = default;

    AudioEventSequencer(const AudioEventSequencer&) = delete;
    AudioEventSequencer& operator=(const AudioEventSequencer&) = delete;

    // Creates a presentation event immediately, but defers catalog lookup and
    // voice submission to update(). This lets a same-frame emitter snapshot
    // establish the initial world position before the first sample is queued.
    [[nodiscard]] AudioEventInstanceHandle enqueue(
        AudioEventIntent intent, std::optional<float> eventVolumeOverride = std::nullopt,
        bool ambient = false, bool eva = false,
        AudioEventInstanceOverrides instanceOverrides = {});

    // Preserves input order for same-frame event admission/submission and
    // replaces the complete latest emitter transform/liveness set.
    // A zero sessionEpoch is valid for standalone tools; the application
    // should publish a non-zero GameSession epoch.
    void submit(AudioPresentationSnapshot snapshot);

    // Stops an authored event and all of its remaining portions. As in the
    // original direct-stop path, this is an immediate stop; callers that need
    // an authored release should enqueue a dedicated event instead of relying
    // on the old ambiguous fade/decay side effect.
    [[nodiscard]] bool stop(AudioEventInstanceHandle handle) noexcept;
    void stopAll() noexcept;

    // Event-name controls mirror AudioManager's adjusted-volume table. They
    // affect queued/future portions and submit a bounded presentation command
    // for an already-playing voice; no game-thread object or backend pointer
    // crosses this API.
    void setEventVolumeOverride(container::StringView eventName,
                                std::optional<float> volume,
                                AudioSystem& audio);
    void clearEventVolumeOverrides(AudioSystem& audio);
    void removeEventsNamed(container::StringView eventName) noexcept;
    void removeEvaEvents() noexcept;

    // Call once after AudioSystem::update(). It consumes backend completion
    // states, submits needed voices/transforms, and advances deterministic
    // loop delays by simulationDeltaSeconds received through submit().
    void update(AudioSystem& audio, const AudioEventCatalog& catalog);

    void reset() noexcept;

    [[nodiscard]] AudioEventInstanceState state(AudioEventInstanceHandle handle) const noexcept;
    [[nodiscard]] size_t activeCount() const noexcept;
    [[nodiscard]] uint64_t sessionEpoch() const noexcept { return m_sessionEpoch; }
    // Drains terminal observations in presentation order. This does not
    // itself mutate gameplay/script state; the application owns the later
    // main-thread acknowledgement policy.
    [[nodiscard]] container::Vector<AudioNaturalCompletion> takeNaturalCompletions();

private:
    enum class Phase : uint8_t {
        Queued,
        WaitingForLoop,
        Playing,
    };

    struct ActiveEvent final {
        AudioEventInstanceHandle handle;
        AudioEventIntent intent;
        AudioEventPlaybackCursor cursor;
        container::String eventKey;
        std::optional<float> eventVolumeOverride;
        AudioEventInstanceOverrides instanceOverrides;
        bool ambient = false;
        bool eva = false;
        // The catalog-resolved volume before the legacy absolute override.
        // Retaining it lets AUDIO_RESTORE_VOLUME_TYPE restore a live voice
        // without reselecting a random sample or restarting its event.
        float lastResolvedVolume = 1.0f;
        AudioEventPortion portion = AudioEventPortion::Main;
        Phase phase = Phase::Queued;
        AudioVoiceHandle voice;
        std::optional<ResolvedAudioEventSample> preparedLoopSample;
        float loopDelayRemainingSeconds = 0.0f;
        // Mirrors AudioEventRTS::m_loopCount. Zero means permanent when the
        // LOOP control bit is present; positive values are total Main plays.
        int32_t remainingMainPlays = 1;
        uint16_t typeFlags = 0;
        uint16_t controlFlags = 0;
        AudioEventKind kind = AudioEventKind::SoundEffect;
        bool positional = false;
        bool stopRequested = false;
        uint64_t arrivalOrdinal = 0;
        AudioVoiceTransform lastSentTransform;
        bool hasLastSentTransform = false;
    };

    static constexpr size_t kRetainedTerminalStates = 1024;
    static constexpr size_t kRetainedPresentationEventIds = 4096;

    [[nodiscard]] ActiveEvent* findActive(AudioEventInstanceHandle handle) noexcept;
    [[nodiscard]] const ActiveEvent* findActive(AudioEventInstanceHandle handle) const noexcept;

    void setState(AudioEventInstanceHandle handle, AudioEventInstanceState state) noexcept;
    void finish(container::HashMap<uint64_t, ActiveEvent>::iterator event,
                AudioEventInstanceState state) noexcept;
    void refreshEmitter(ActiveEvent& event, AudioSystem& audio);
    void startQueued(ActiveEvent& event, AudioSystem& audio, const AudioEventCatalog& catalog);
    void startPreparedLoop(ActiveEvent& event, AudioSystem& audio,
                           const AudioEventCatalog& catalog);
    void startPortion(ActiveEvent& event, AudioEventPortion portion,
                      AudioSystem& audio, const AudioEventCatalog& catalog,
                      std::optional<ResolvedAudioEventSample> prepared = std::nullopt);
    void completeVoice(ActiveEvent& event, AudioSystem& audio,
                       const AudioEventCatalog& catalog);
    void scheduleNextMainLoop(ActiveEvent& event, AudioSystem& audio,
                              const AudioEventCatalog& catalog);
    void appendNaturalCompletion(const ActiveEvent& event,
                                 AudioNaturalCompletionKind kind) noexcept;
    [[nodiscard]] bool admit(ActiveEvent& event, const AudioEventDefinition& definition,
                              AudioSystem& audio);
    [[nodiscard]] bool hasActiveVoiceForOwner(const ActiveEvent& candidate) const noexcept;
    [[nodiscard]] std::optional<uint64_t> oldestMatchingActive(const ActiveEvent& candidate) const;
    [[nodiscard]] static bool isTerminal(AudioEventInstanceState state) noexcept;
    [[nodiscard]] static container::String canonicalEventKey(container::StringView name);
    [[nodiscard]] static bool sameTransform(const AudioVoiceTransform& lhs,
                                            const AudioVoiceTransform& rhs) noexcept;

    uint64_t m_nextHandleValue = 1;
    uint64_t m_nextArrivalOrdinal = 1;
    uint64_t m_sessionEpoch = 0;
    float m_pendingSimulationDeltaSeconds = 0.0f;
    // `m_active` is an O(1) identity lookup only. Every lifecycle action that
    // can affect voice command ordering walks this insertion-order list, so
    // a hash-table bucket layout can never reorder same-frame events.
    container::HashMap<uint64_t, ActiveEvent> m_active;
    container::Vector<uint64_t> m_activeOrder;
    container::HashMap<uint64_t, AudioEventInstanceState> m_states;
    container::Vector<uint64_t> m_terminalOrder;
    container::HashSet<uint64_t> m_seenPresentationEventIds;
    container::Vector<uint64_t> m_seenPresentationEventOrder;
    container::HashMap<uint64_t, AudioEmitterTransform> m_emitters;
    container::Vector<AudioNaturalCompletion> m_naturalCompletions;
    bool m_hasEmitterSnapshot = false;
    bool m_disallowSpeech = false;
};

} // namespace engine::audio
