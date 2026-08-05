#pragma once

#include "core/container/hash_containers.h"

#include "engine/audio/AudioEventCatalog.h"
#include "engine/audio/AudioEventSequencer.h"
#include "engine/audio/AudioListener.h"
#include "engine/audio/AudioSystem.h"
#include "engine/fx/runtime/FxPresentationCommands.h"
#include "presentation/audio/AudioContentLayer.h"
#include "core/platform/runtime_mailbox.h"
#include "system/SubsystemInterface.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
namespace engine {

// Presentation/platform service for VFS-backed sound. It intentionally owns
// neither GameSession nor renderer state: the application publishes a pure
// camera-derived listener after presentation input is applied.
class AudioSubsystem final : public SubsystemInterface {
public:
    AudioSubsystem();
    ~AudioSubsystem() override;

    void init() override;
    void reset() override;
    void update() override;
    void shutdown() override;

    void publishListener(const GameCameraState& camera) noexcept;
    // Renderer-local camera-slave playback supplies this detached pose after
    // W3D skeleton evaluation. It is intentionally an audio-only observer;
    // publishing it cannot write GameSession, GameCameraDirector or replay.
    void publishPresentationCameraListener(math::vec3 position, math::vec3 target,
                                           math::vec3 up) noexcept;
    // Local pause is presentation policy, not script/audio-content state.
    // Script popups pause non-music buses; the in-game menu also pauses
    // music, matching ZH's setGamePaused(pauseMusic) split.
    void setLocalPausePolicy(bool pauseNonMusic, bool pauseMusic) noexcept;
    [[nodiscard]] audio::AudioSystem& audio() noexcept { return m_audio; }
    [[nodiscard]] const audio::AudioSystem& audio() const noexcept { return m_audio; }
    [[nodiscard]] const audio::AudioEventCatalog& events() const noexcept {
        return m_activeEvents ? *m_activeEvents : m_baseEvents;
    }
    [[nodiscard]] bool activatePresentationSession(
        uint64_t presentationEpoch,
        container::Span<const audio::AudioContentLayer> contentLayers);

    // Independent from the render-frame queue: one-shot events are submitted
    // in confirmed logic order, while emitter transforms are latest-value
    // snapshots. The subsystem owns the presentation-side sequencer.
    void submitPresentationSnapshot(audio::AudioPresentationSnapshot snapshot);
    void appendFxSoundCommands(
        audio::AudioPresentationSnapshot& snapshot,
        container::Vector<fx::FxSoundCommand> commands);
    [[nodiscard]] audio::AudioEventInstanceHandle enqueueSequencedEvent(
        audio::AudioEventIntent intent);
    // Main-thread ControlBar click handoff. The audio owner drains this
    // bounded queue during update and rejects requests from retired epochs.
    [[nodiscard]] bool requestUiEvent(
        container::String eventName, uint64_t presentationEpoch);
    // Render-thread acknowledgement for the first successfully presented
    // non-Loading frame of a session. Gameplay audio may be decoded and
    // sequenced before this point, but every gameplay bus remains paused
    // until the matching epoch is actually visible.
    [[nodiscard]] bool requestPresentationPlaybackRelease(
        uint64_t presentationEpoch);
    [[nodiscard]] bool stopSequencedEvent(audio::AudioEventInstanceHandle handle) noexcept;
    [[nodiscard]] audio::AudioEventInstanceState sequencedEventState(
        audio::AudioEventInstanceHandle handle) const noexcept;
    void clearPresentationSession();
    // Drains only value-level natural EOS/loop observations. The subsystem
    // never calls GameSession or ScriptRuntime itself; app code admits these
    // on the next confirmed frame through GameSession's guarded inbox.
    [[nodiscard]] container::Vector<audio::AudioNaturalCompletion>
    takeNaturalCompletions();

    // Presentation-visible script policy state. Ambient producers opt into a
    // dedicated bus at the value-extraction boundary, so this policy pauses
    // environmental loops without leaking a device handle into GameSession
    // or ScriptRuntime.
    [[nodiscard]] float scriptMusicVolume() const noexcept { return m_scriptMusicVolume; }
    [[nodiscard]] float scriptSoundVolume() const noexcept { return m_scriptSoundVolume; }
    [[nodiscard]] float scriptSound3DVolume() const noexcept { return m_scriptSound3DVolume; }
    [[nodiscard]] float scriptSpeechVolume() const noexcept { return m_scriptSpeechVolume; }
    [[nodiscard]] bool ambientPausedByScript() const noexcept { return m_ambientPausedByScript; }
    [[nodiscard]] bool backgroundSoundsPausedByScript() const noexcept {
        return m_backgroundSoundsPausedByScript;
    }
    [[nodiscard]] bool evaEnabledByScript() const noexcept { return m_evaEnabledByScript; }
    [[nodiscard]] std::optional<float> scriptEventVolumeOverride(
        container::StringView eventName) const;
    // Diagnostics/probes can observe the value-level object ambient policy
    // without receiving an AudioEventInstanceHandle. The returned view is
    // invalidated by a later control, reset, or session replacement.
    [[nodiscard]] std::optional<container::StringView> scriptObjectAmbientEvent(
        uint64_t emitterKey) const noexcept;
    [[nodiscard]] size_t scriptObjectAmbientCount() const noexcept {
        return m_scriptObjectAmbientSounds.size();
    }

    // The ScoreScreen is main-thread presentation, while all sequencer/device
    // mutation remains logic-thread-owned. These calls publish only the latest
    // local UI request; applyPendingScoreScreenMusicRequest() is the explicit
    // owner-thread handoff used before the next game-state transition.
    [[nodiscard]] bool requestScoreScreenMusic(container::String trackName);
    [[nodiscard]] bool stopScoreScreenMusic();
    void applyPendingScoreScreenMusicRequest();

    // Explicit single-sample escape hatch for diagnostics/editor tooling.
    // It resolves Main only; confirmed gameplay must use the sequenced event
    // API or submitPresentationSnapshot() so Attack/Decay/Loop/Limit remain
    // observable. Persistent cursors belong to the caller, never a live ECS
    // object or global miniaudio handle.
    [[nodiscard]] audio::AudioVoiceHandle enqueueEvent(
        const audio::AudioEventIntent& intent,
        audio::AudioEventPlaybackCursor* cursor = nullptr);

private:
    audio::AudioSystem m_audio;
    audio::AudioEventCatalog m_baseEvents;
    container::SharedPtr<const audio::AudioEventCatalog> m_baseEventSnapshot;
    container::SharedPtr<const audio::AudioEventCatalog> m_activeEvents;
    audio::AudioEventSequencer m_sequencer;
    audio::AudioListenerSettings m_listenerSettings;
    uint64_t m_presentationSessionEpoch = 0;
    uint64_t m_lastPresentationSimulationFrame = 0;
    bool m_hasPresentationSimulationFrame = false;
    uint64_t m_nextPresentationEventSeed = 1;
    // Renderer-expanded FX sounds are a second exactly-once stream.  Keep
    // their presentation IDs in a disjoint namespace and monotonic for the
    // whole session; numbering each renderer feedback batch from one makes
    // AudioEventSequencer reject every later batch as a duplicate.
    uint64_t m_nextFxSoundEventOrdinal = 1;
    struct PendingMusicTrack final {
        audio::AudioEventIntent intent;
        bool fadeIn = false;
    };
    struct ScoreScreenMusicRequest final {
        container::String trackName;
    };
    enum class MusicTransition : uint8_t {
        None,
        FadingOut,
        FadingIn,
    };
    audio::AudioEventInstanceHandle m_activeMusic;
    platform::runtime::LatestValueMailbox<ScoreScreenMusicRequest>
        m_scoreScreenMusicRequests;
    struct UiEventRequest final {
        container::String eventName;
        uint64_t presentationEpoch = 0;
    };
    platform::runtime::BoundedMailbox<UiEventRequest, 64>
        m_uiEventRequests;
    struct PresentationPlaybackRelease final {
        uint64_t presentationEpoch = 0;
    };
    platform::runtime::LatestValueMailbox<PresentationPlaybackRelease>
        m_presentationPlaybackReleases;
    bool m_presentationPlaybackReleased = true;
    container::String m_scoreScreenMusicTrack;
    bool m_scoreScreenMusicActive = false;
    // One active Drawable-style ambient event per ObjectId. This is a
    // presentation-only ownership table: GameSession publishes just an
    // ObjectId/event-name command, while the audio service owns the actual
    // sequencer handle and clears it when the emitter disappears or reaches
    // a terminal state.
    struct ScriptObjectAmbientSound final {
        container::String eventName;
        audio::AudioEventInstanceHandle handle;
    };
    container::HashMap<uint64_t, ScriptObjectAmbientSound> m_scriptObjectAmbientSounds;
    // Retained for the whole presentation epoch even after an emitter stops.
    // A delayed older snapshot therefore cannot resurrect a retired loop.
    container::HashMap<uint64_t, uint64_t> m_objectAmbientGenerations;
    // Gameplay loops are keyed by ObjectId plus authored event name and do
    // not replace the one-per-object script/Drawable ambient channel.
    container::HashMap<container::String, ScriptObjectAmbientSound>
        m_gameplayObjectLoopSounds;
    std::optional<PendingMusicTrack> m_pendingMusic;
    MusicTransition m_musicTransition = MusicTransition::None;
    float m_musicTransitionElapsedSeconds = 0.0f;
    float m_pendingMusicSimulationSeconds = 0.0f;
    float m_scriptMusicVolume = 1.0f;
    float m_scriptSoundVolume = 1.0f;
    float m_scriptSound3DVolume = 1.0f;
    float m_scriptSpeechVolume = 1.0f;
    bool m_ambientPausedByScript = false;
    bool m_backgroundSoundsPausedByScript = false;
    bool m_evaEnabledByScript = true;
    struct PendingEvaEvent final {
        audio::AudioPresentationEvent event;
        uint64_t triggeredFrame = 0;
        uint64_t expirationFrame = 0;
        uint64_t arrivalOrdinal = 0;
    };
    container::Vector<PendingEvaEvent> m_pendingEvaEvents;
    container::Array<uint64_t, audio::kEvaEventTypeCount>
        m_evaAvailableFrames{};
    container::Array<bool, audio::kEvaEventTypeCount>
        m_evaCooldownActive{};
    audio::AudioEventInstanceHandle m_activeEvaEvent;
    std::optional<audio::EvaEventType> m_activeEvaType;
    uint64_t m_evaSimulationFrame = 0;
    uint64_t m_lastEvaArbitrationFrame =
        std::numeric_limits<uint64_t>::max();
    uint64_t m_nextEvaArrivalOrdinal = 1;
    bool m_localNonMusicPaused = false;
    bool m_localMusicPaused = false;
    container::HashMap<container::String, float> m_scriptEventVolumeOverrides;
    audio::AudioVoiceHandle m_debugVoice;
    audio::AudioEventInstanceHandle m_debugEvent;
    container::String m_debugAudioPath;
    container::String m_pendingDebugAudioEventName;
    bool m_debugVoiceReported = false;

    void consumePresentationControl(audio::AudioPresentationControlEvent control);
    void admitEvaEvent(audio::AudioPresentationEvent event);
    void advanceEvaScheduler();
    void clearEvaScheduler(bool clearCooldowns) noexcept;
    void advanceMusicTransition(float deltaSeconds);
    void beginMusicTrack(PendingMusicTrack track);
    void stopActiveMusic() noexcept;
    void applyMusicBusVolume(float normalizedFade) noexcept;
    void applySoundBusState() noexcept;
    void applySpeechBusVolume() noexcept;
    void applyLocalPausePolicy() noexcept;
    void stopScriptObjectAmbientSound(uint64_t emitterKey) noexcept;
    void pruneScriptObjectAmbientSounds() noexcept;
    void resetScriptAudioPresentationState() noexcept;
};

} // namespace engine
