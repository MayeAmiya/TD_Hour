#include "core/container/container_types.h"
#include "AudioSubsystem.h"

#include "CommandLine.h"
#include "GlobalData.h"
#include "debug/debug.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>

namespace engine {
namespace {

// AudioEventRTS owns a fixed client-side fade rather than a script-authored
// duration. Keep a short, deterministic presentation fade driven by
// confirmed simulation deltas; it is deliberately not a wall-clock timer.
constexpr float kScriptMusicFadeSeconds = 0.75f;
constexpr uint64_t kFxSoundEventNamespace = uint64_t{1} << 63u;
constexpr uint64_t kFxSoundEventOrdinalMask =
    kFxSoundEventNamespace - uint64_t{1};

[[nodiscard]] container::String canonicalEventKey(container::StringView value) {
    container::String key;
    key.reserve(value.size());
    for (const unsigned char character : value) {
        key.push_back(static_cast<char>(std::tolower(character)));
    }
    return key;
}

[[nodiscard]] bool isLiveSequencedEvent(audio::AudioEventInstanceState state) noexcept {
    return state == audio::AudioEventInstanceState::Pending ||
           state == audio::AudioEventInstanceState::Playing ||
           state == audio::AudioEventInstanceState::Waiting;
}

// RefCode AudioManager::shouldPlayLocally() evaluates the authored recipient
// bits in this exact precedence order.  `AudioAudience` was frozen by the
// GameSession media extractor, so this presentation owner can preserve the
// policy without reading PlayerRegistry, ECS, or any simulation object.
[[nodiscard]] bool shouldPlayForAudience(
    const audio::AudioEventCatalog& catalog,
    const audio::AudioEventIntent& intent) noexcept {
    const audio::AudioEventDefinition* definition =
        catalog.find(intent.eventName);
    // Let the normal resolver diagnose unknown event names. Music and
    // uninterruptible audio deliberately bypass the original local-audience
    // rejection path.  Logical script audio does not: the original retains
    // its deterministic bookkeeping for a logical event, then still retires
    // a non-local request before it reaches the device.
    if (!definition || definition->kind == audio::AudioEventKind::Music ||
        intent.uninterruptible) {
        return true;
    }

    const uint16_t type = definition->typeFlags;
    const bool player = audio::hasAudioEventType(
        type, audio::AudioEventType::Player);
    const bool allies = audio::hasAudioEventType(
        type, audio::AudioEventType::Allies);
    const bool enemies = audio::hasAudioEventType(
        type, audio::AudioEventType::Enemies);
    const bool everyone = audio::hasAudioEventType(
        type, audio::AudioEventType::Everyone);
    if (everyone || (!player && !allies && !enemies)) return true;

    // Original AudioManager permits an ownerless Player+UI event outside a
    // game update (menu/control-bar clicks).  It does not relax the check for
    // world/speech events, where a missing owning player is malformed input.
    if (player) {
        if (intent.audience.relation == audio::AudioAudienceRelation::Self) {
            return true;
        }
        return !intent.audience.hasSourcePlayer() &&
            audio::hasAudioEventType(type, audio::AudioEventType::Ui);
    }
    if (allies) {
        return intent.audience.relation == audio::AudioAudienceRelation::Ally;
    }
    return intent.audience.relation == audio::AudioAudienceRelation::Enemy;
}

} // namespace

AudioSubsystem::AudioSubsystem() {
    setName("Audio");
}

AudioSubsystem::~AudioSubsystem() {
    shutdown();
}

void AudioSubsystem::init() {
    m_scoreScreenMusicRequests.reset();
    m_presentationPlaybackReleases.reset();
    container::String catalogError;
    if (!m_baseEvents.loadFromVfs(&catalogError)) {
        // Audio device startup remains available for direct diagnostic paths,
        // but normal game events must not pretend they are defined when the
        // source INI set was unavailable.
        TD_LOG_WARN("[Audio] AudioEvent catalog unavailable: {}", catalogError);
    } else {
        m_listenerSettings = m_baseEvents.settings().listener;
    }
    m_baseEventSnapshot =
        std::make_shared<const audio::AudioEventCatalog>(m_baseEvents);
    m_activeEvents.reset();

    audio::AudioSystemConfig config;
    const bool globalAudioEnabled = config::TheGlobalData.isAudioOn();
    config.enablePlaybackDevice = globalAudioEnabled &&
        !CommandLine::instance().hasParam("nosound");
    if (m_baseEvents.eventCount() > 0) {
        const audio::AudioEventSettings& settings = m_baseEvents.settings();
        // Retain the original 4 + 25 + 3 total as the modern backend's hard
        // pool limit until class reservations are introduced with the active
        // event sequencer.
        config.maxActiveVoices = settings.sampleCount2D + settings.sampleCount3D +
            settings.streamCount;
        config.maxTwoDimensionalVoices = settings.sampleCount2D;
        config.maxThreeDimensionalVoices = settings.sampleCount3D;
        config.maxStreamingVoices = settings.streamCount;
        // Generals' shipped 4 MiB setting is a historical memory constraint,
        // not an appropriate modern runtime ceiling. Preserve mod overrides
        // when they are larger while giving legacy/default content 64 MiB.
        config.clipCacheBudgetBytes = std::max(settings.cacheBudgetBytes,
                                                audio::kDefaultClipCacheBudgetBytes);
        config.minSampleVolume = settings.minSampleVolume;
        config.use3DRangeVolumeFade = settings.use3DRangeVolumeFade;
        config.rangeVolumeFadeExponent = settings.rangeVolumeFadeExponent;
    }
    if (!m_audio.init(config)) {
        TD_LOG_ERROR("[Audio] Audio service initialization was requested twice");
        return;
    }
    if (m_audio.isPlaybackDeviceAvailable()) {
        TD_LOG_INFO("[Audio] Initialized miniaudio playback device (VFS 2D/3D foundation)");
    } else if (config.enablePlaybackDevice) {
        TD_LOG_WARN("[Audio] Running without a playback device; commands remain safe no-ops");
    } else {
        TD_LOG_INFO("[Audio] Playback disabled (--nosound or GameOptions); command boundary remains active");
    }

    if (m_baseEvents.eventCount() > 0) {
        const audio::AudioEventSettings& settings = m_baseEvents.settings();
        m_scriptMusicVolume = settings.defaultMusicVolume;
        m_scriptSoundVolume = settings.defaultSoundVolume;
        m_scriptSound3DVolume = settings.defaultSound3DVolume;
        m_scriptSpeechVolume = settings.defaultSpeechVolume;
        applyMusicBusVolume(1.0f);
        applySoundBusState();
        applySpeechBusVolume();
        m_audio.setBusEnabled(audio::AudioBus::Ambient, true);
    }

    // Repeatable real-asset diagnostic. This deliberately accepts a VFS path
    // only; it cannot bypass archive/mod precedence through a host path.
    const container::String debugAudio = CommandLine::instance().getParam("debug-audio");
    const container::String debugAudioEvent = CommandLine::instance().getParam("debug-audio-event");
    if (!debugAudio.empty()) {
        audio::AudioPlayRequest request;
        request.assetPath = debugAudio;
        request.bus = audio::AudioBus::Sound3D;
        request.spatialMode = audio::AudioSpatialMode::ThreeDimensional;
        request.position = {};
        request.minDistance = 75.0f;
        request.maxDistance = 2500.0f;
        m_debugVoice = m_audio.enqueuePlay(std::move(request));
        m_debugAudioPath = debugAudio;
        m_debugVoiceReported = false;
        if (!m_debugVoice) {
            TD_LOG_WARN("[Audio] Could not enqueue --debug-audio '{}'", debugAudio);
        } else {
            TD_LOG_INFO("[Audio] Queued VFS 3D diagnostic '{}'", debugAudio);
        }
    } else if (!debugAudioEvent.empty()) {
        m_debugAudioPath = "AudioEvent " + debugAudioEvent;
        m_pendingDebugAudioEventName = debugAudioEvent;
        m_debugVoiceReported = false;
        // A production direct-start creates the presentation epoch after the
        // subsystem is initialized.  Queueing here lets the intervening
        // session reset stop the diagnostic before any authored portion is
        // heard, so arm it now and enqueue after Loading releases playback.
        TD_LOG_INFO(
            "[Audio] Armed sequenced VFS AudioEvent diagnostic '{}'",
            debugAudioEvent);
    }
}

void AudioSubsystem::reset() {
    m_scoreScreenMusicRequests.reset();
    m_uiEventRequests.reset();
    m_presentationPlaybackReleases.reset();
    m_sequencer.reset();
    m_audio.reset();
    m_activeEvents.reset();
    const audio::AudioEventSettings& settings = m_baseEvents.settings();
    m_audio.setRuntimeEventPolicy({
        .minSampleVolume = settings.minSampleVolume,
        .use3DRangeVolumeFade = settings.use3DRangeVolumeFade,
        .rangeVolumeFadeExponent = settings.rangeVolumeFadeExponent,
    });
    resetScriptAudioPresentationState();
    m_presentationSessionEpoch = 0;
    m_presentationPlaybackReleased = true;
    m_lastPresentationSimulationFrame = 0;
    m_hasPresentationSimulationFrame = false;
    m_debugVoice = {};
    m_debugEvent = {};
    m_debugAudioPath.clear();
    m_pendingDebugAudioEventName.clear();
    m_debugVoiceReported = false;
    m_nextPresentationEventSeed = 1;
    m_nextFxSoundEventOrdinal = 1;
}

void AudioSubsystem::update() {
    PresentationPlaybackRelease playbackRelease;
    if (m_presentationPlaybackReleases.tryTake(playbackRelease) &&
        playbackRelease.presentationEpoch != 0u &&
        playbackRelease.presentationEpoch == m_presentationSessionEpoch &&
        !m_presentationPlaybackReleased) {
        m_presentationPlaybackReleased = true;
        applyLocalPausePolicy();
    }
    if (!m_pendingDebugAudioEventName.empty() &&
        m_presentationSessionEpoch != 0u &&
        m_presentationPlaybackReleased && !m_debugEvent) {
        const container::String eventName =
            std::exchange(m_pendingDebugAudioEventName, {});
        audio::AudioEventIntent intent;
        intent.eventName = eventName;
        // This switch proves catalog selection, VFS decode and authored
        // Attack/Main/Decay sequencing. Keep it non-positional so an authored
        // world range or a simultaneous cinematic camera cut cannot turn the
        // asset test into an unrelated distance-culling test.
        m_debugEvent = enqueueSequencedEvent(std::move(intent));
        if (!m_debugEvent) {
            m_debugVoiceReported = true;
            TD_LOG_WARN(
                "[Audio] Could not enqueue --debug-audio-event '{}'",
                eventName);
        } else {
            TD_LOG_INFO(
                "[Audio] Queued sequenced VFS AudioEvent diagnostic '{}' after presentation release",
                eventName);
        }
    }
    while (std::optional<UiEventRequest> request =
               m_uiEventRequests.tryPop()) {
        if (request->eventName.empty() ||
            request->presentationEpoch == 0u ||
            request->presentationEpoch != m_presentationSessionEpoch) {
            continue;
        }
        audio::AudioEventIntent intent;
        intent.eventName = std::move(request->eventName);
        static_cast<void>(enqueueSequencedEvent(std::move(intent)));
    }
    advanceEvaScheduler();
    // Sequencer first preserves the confirmed event ordering and gives new
    // requests a chance to enter AudioSystem's bounded queue this frame. The
    // next presentation update observes EOS and advances Attack/Main/Decay.
    m_sequencer.update(m_audio, events());
    pruneScriptObjectAmbientSounds();
    m_audio.update();
    advanceMusicTransition(m_pendingMusicSimulationSeconds);
    m_pendingMusicSimulationSeconds = 0.0f;
    if (m_debugVoice && !m_debugVoiceReported) {
        const audio::AudioVoiceState state = m_audio.voiceState(m_debugVoice);
        if (state == audio::AudioVoiceState::Pending || state == audio::AudioVoiceState::Unknown) return;
        m_debugVoiceReported = true;
        if (state == audio::AudioVoiceState::Suppressed) {
            TD_LOG_INFO("[Audio] --debug-audio '{}' consumed without a playback device",
                        m_debugAudioPath);
        } else if (state == audio::AudioVoiceState::Failed) {
            TD_LOG_ERROR("[Audio] --debug-audio '{}' failed to decode/start", m_debugAudioPath);
        } else {
            TD_LOG_INFO("[Audio] --debug-audio '{}' decoded from VFS and submitted (state={})",
                        m_debugAudioPath, static_cast<int>(state));
        }
    }
    if (m_debugEvent && !m_debugVoiceReported) {
        const audio::AudioEventInstanceState state = m_sequencer.state(m_debugEvent);
        if (state == audio::AudioEventInstanceState::Pending ||
            state == audio::AudioEventInstanceState::Playing ||
            state == audio::AudioEventInstanceState::Waiting ||
            state == audio::AudioEventInstanceState::Unknown) {
            return;
        }
        m_debugVoiceReported = true;
        if (state == audio::AudioEventInstanceState::Suppressed) {
            TD_LOG_INFO("[Audio] --debug-audio-event '{}' consumed without a playback device",
                        m_debugAudioPath);
        } else if (state == audio::AudioEventInstanceState::Completed) {
            TD_LOG_INFO("[Audio] --debug-audio-event '{}' completed its authored sequence",
                        m_debugAudioPath);
        } else {
            TD_LOG_ERROR("[Audio] --debug-audio-event '{}' terminated (state={})",
                         m_debugAudioPath, static_cast<int>(state));
        }
    }
}

void AudioSubsystem::shutdown() {
    m_scoreScreenMusicRequests.close();
    m_uiEventRequests.close();
    m_presentationPlaybackReleases.close();
    m_sequencer.reset();
    m_audio.shutdown();
    resetScriptAudioPresentationState();
    m_presentationSessionEpoch = 0;
    m_presentationPlaybackReleased = true;
    m_lastPresentationSimulationFrame = 0;
    m_hasPresentationSimulationFrame = false;
    m_debugVoice = {};
    m_debugEvent = {};
    m_debugAudioPath.clear();
    m_pendingDebugAudioEventName.clear();
    m_debugVoiceReported = false;
    m_activeEvents.reset();
    m_baseEventSnapshot.reset();
    m_baseEvents.clear();
    m_nextPresentationEventSeed = 1;
    m_nextFxSoundEventOrdinal = 1;
}

void AudioSubsystem::publishListener(const GameCameraState& camera) noexcept {
    m_audio.publishListener(audio::AudioListenerBuilder::fromCamera(camera, m_listenerSettings));
}

void AudioSubsystem::publishPresentationCameraListener(
    math::vec3 position, math::vec3 target, math::vec3 up) noexcept {
    m_audio.publishListener(audio::AudioListenerBuilder::fromPresentationCamera(
        position, target, up, m_listenerSettings));
}

void AudioSubsystem::setLocalPausePolicy(
    bool pauseNonMusic, bool pauseMusic) noexcept {
    if (m_localNonMusicPaused == pauseNonMusic &&
        m_localMusicPaused == pauseMusic) {
        return;
    }
    m_localNonMusicPaused = pauseNonMusic;
    m_localMusicPaused = pauseMusic;
    applyLocalPausePolicy();
}

bool AudioSubsystem::activatePresentationSession(
    uint64_t presentationEpoch,
    container::Span<const audio::AudioContentLayer> contentLayers) {
    if (presentationEpoch == 0) return false;
    if (presentationEpoch == m_presentationSessionEpoch) return true;
    if (!m_baseEventSnapshot) return false;

    container::SharedPtr<const audio::AudioEventCatalog> catalog =
        m_baseEventSnapshot;
    if (!contentLayers.empty()) {
        auto sessionCatalog =
            std::make_shared<audio::AudioEventCatalog>(m_baseEvents);
        for (const audio::AudioContentLayer& layer : contentLayers) {
            container::String layerError;
            if (!sessionCatalog->applyOverrides(
                    layer.content, layer.sourcePath, &layerError)) {
                TD_LOG_WARN(
                    "[Audio] Ignored map-local audio layer '{}': {}",
                    layer.sourcePath, layerError);
            }
        }
        catalog = std::move(sessionCatalog);
    }

    // Retire the complete old sequence before publishing the new immutable
    // catalog. An old Attack/Main/Decay continuation must never resolve its
    // next portion through a map override from another presentation epoch.
    if (m_presentationSessionEpoch != 0) {
        m_sequencer.reset();
        m_audio.reset();
    }
    m_activeEvents = std::move(catalog);
    const audio::AudioEventSettings& settings = events().settings();
    m_listenerSettings = settings.listener;
    m_audio.setRuntimeEventPolicy({
        .minSampleVolume = settings.minSampleVolume,
        .use3DRangeVolumeFade = settings.use3DRangeVolumeFade,
        .rangeVolumeFadeExponent = settings.rangeVolumeFadeExponent,
    });
    resetScriptAudioPresentationState();
    m_presentationSessionEpoch = presentationEpoch;
    m_presentationPlaybackReleased = false;
    applyLocalPausePolicy();
    m_lastPresentationSimulationFrame = 0;
    m_hasPresentationSimulationFrame = false;
    m_nextPresentationEventSeed = 1;
    m_nextFxSoundEventOrdinal = 1;
    return true;
}

void AudioSubsystem::submitPresentationSnapshot(audio::AudioPresentationSnapshot snapshot) {
    if (snapshot.sessionEpoch != 0 && snapshot.sessionEpoch != m_presentationSessionEpoch) {
        // Direct tooling can submit without a GameSession catalog handoff.
        // Production activates epoch+catalog before listener publication;
        // this fallback deliberately uses only the immutable base catalog.
        if (!activatePresentationSession(snapshot.sessionEpoch, {})) return;
    }
    if (snapshot.sessionEpoch != 0) {
        m_evaSimulationFrame = std::max(
            m_evaSimulationFrame, snapshot.simulationFrame);
        if (m_hasPresentationSimulationFrame) {
            if (snapshot.simulationFrame <= m_lastPresentationSimulationFrame) {
                snapshot.simulationDeltaSeconds = 0.0f;
            } else {
                const uint64_t gap = snapshot.simulationFrame - m_lastPresentationSimulationFrame;
                // A lockstep client can receive several confirmed frames at
                // once. Advance authored loop delay by that same confirmed
                // time, with a finite cap guarding malformed input.
                const float multiplier = static_cast<float>(std::min<uint64_t>(gap, 600));
                snapshot.simulationDeltaSeconds *= multiplier;
            }
        } else {
            snapshot.simulationDeltaSeconds = 0.0f;
            m_hasPresentationSimulationFrame = true;
        }
        m_lastPresentationSimulationFrame = snapshot.simulationFrame;
    }
    // Audio events and controls share GameSession's confirmed ordinal, but
    // arrive in two typed vectors. Merge them here instead of applying every
    // control first: PLAY(A) -> SOUND_DISABLE_TYPE(A) must enqueue then mute
    // A, while DISABLE(A) -> PLAY(A) must enqueue A with a zero override.
    // This is a presentation ordering rule only; no audio completion flows
    // back into simulation.
    container::Vector<audio::AudioPresentationEvent> events = std::move(snapshot.events);
    container::Vector<audio::AudioPresentationControlEvent> controls = std::move(snapshot.controls);
    const float simulationDeltaSeconds = snapshot.simulationDeltaSeconds;
    snapshot.events.clear();
    snapshot.controls.clear();
    m_sequencer.submit(std::move(snapshot));

    const auto submitEvent = [this](audio::AudioPresentationEvent event) {
        if (!shouldPlayForAudience(this->events(), event.intent)) return;
        if (event.eva && !m_evaEnabledByScript) return;
        if (event.evaPolicy) {
            admitEvaEvent(std::move(event));
            return;
        }
        const auto overrideValue = m_scriptEventVolumeOverrides.find(
            canonicalEventKey(event.intent.eventName));
        if (overrideValue != m_scriptEventVolumeOverrides.end()) {
            event.eventVolumeOverride = overrideValue->second;
        }
        static_cast<void>(m_sequencer.enqueue(
            std::move(event.intent), std::move(event.eventVolumeOverride),
            event.ambient, event.eva));
    };

    size_t eventIndex = 0;
    size_t controlIndex = 0;
    while (eventIndex < events.size() || controlIndex < controls.size()) {
        const bool hasEvent = eventIndex < events.size();
        const bool hasControl = controlIndex < controls.size();
        bool takeControl = !hasEvent;
        if (hasEvent && hasControl) {
            const uint64_t eventId = events[eventIndex].intent.eventId;
            const uint64_t controlId = controls[controlIndex].eventId;
            // Session-originated entries always have nonzero IDs. The zero-ID
            // fallback keeps legacy/direct tool batches deterministic by
            // retaining the historical control-first behavior for a tie.
            takeControl = controlId == 0 || (eventId != 0 && controlId <= eventId);
        }
        if (takeControl) {
            consumePresentationControl(std::move(controls[controlIndex++]));
        } else {
            submitEvent(std::move(events[eventIndex++]));
        }
    }

    // `m_sequencer.submit` consumed the fixed-frame delta before the merge.
    // Music's client-side transition owns a separate accumulator so track
    // replacement remains in source order while its fade advances once per
    // presentation update.
    if (std::isfinite(simulationDeltaSeconds) && simulationDeltaSeconds > 0.0f) {
        m_pendingMusicSimulationSeconds = std::min(
            10.0f, m_pendingMusicSimulationSeconds + simulationDeltaSeconds);
    }
}

void AudioSubsystem::appendFxSoundCommands(
    audio::AudioPresentationSnapshot& snapshot,
    container::Vector<fx::FxSoundCommand> commands) {
    snapshot.events.reserve(snapshot.events.size() + commands.size());
    for (fx::FxSoundCommand& command : commands) {
        if (command.eventName.empty()) continue;
        audio::AudioEventIntent intent;
        intent.eventName = std::move(command.eventName);
        // GameAudioJournal IDs occupy the ordinary confirmed-frame namespace.
        // Renderer-expanded Sound nuggets use the high-bit namespace and a
        // session-monotonic ordinal.  The renderer already guarantees each FX
        // invocation is expanded exactly once; this ID preserves that fact at
        // the AudioEventSequencer de-duplication boundary across feedback
        // batches, rather than restarting from one every presentation frame.
        uint64_t ordinal = m_nextFxSoundEventOrdinal &
            kFxSoundEventOrdinalMask;
        if (ordinal == 0) ordinal = 1;
        intent.eventId = kFxSoundEventNamespace | ordinal;
        m_nextFxSoundEventOrdinal = ordinal == kFxSoundEventOrdinalMask
            ? 1 : ordinal + 1;
        intent.confirmedFrame = command.identity.confirmedFrame;
        intent.variationSeed = command.identity.variationSeed;
        const fx::FxPresentationAnchor& world =
            fx::worldTransform(command.anchor);
        intent.position = {
            world.position.x, world.position.y, world.position.z};
        intent.audience = world.audience;
        std::visit([&intent](const auto& anchor) {
            using Anchor = std::decay_t<decltype(anchor)>;
            if constexpr (std::is_same_v<Anchor, fx::FxObjectAnchor>) {
                intent.emitterKey = anchor.objectKey;
                intent.ownerKey = anchor.objectKey;
                intent.fallbackToPositionIfEmitterMissing = true;
            } else if constexpr (std::is_same_v<Anchor, fx::FxBoneAnchor>) {
                // Preserve the renderer-resolved bone world position for the
                // one-shot while still assigning object ownership. Binding
                // emitterKey here would replace that pose with the root
                // transform from the ordinary audio emitter snapshot.
                intent.ownerKey = anchor.objectKey;
            }
        }, command.anchor);
        snapshot.events.push_back({.intent = std::move(intent)});
    }
}

audio::AudioEventInstanceHandle AudioSubsystem::enqueueSequencedEvent(
    audio::AudioEventIntent intent) {
    if (!shouldPlayForAudience(events(), intent)) return {};
    return m_sequencer.enqueue(std::move(intent));
}

bool AudioSubsystem::requestUiEvent(
    container::String eventName, uint64_t presentationEpoch) {
    if (eventName.empty() || presentationEpoch == 0u) return false;
    return m_uiEventRequests.tryPush({
        .eventName = std::move(eventName),
        .presentationEpoch = presentationEpoch,
    });
}

bool AudioSubsystem::requestPresentationPlaybackRelease(
    uint64_t presentationEpoch) {
    if (presentationEpoch == 0u) return false;
    return m_presentationPlaybackReleases.publish({
        .presentationEpoch = presentationEpoch,
    });
}

bool AudioSubsystem::requestScoreScreenMusic(container::String trackName) {
    if (trackName.empty()) return stopScoreScreenMusic();
    return m_scoreScreenMusicRequests.publish(
        ScoreScreenMusicRequest{.trackName = std::move(trackName)});
}

bool AudioSubsystem::stopScoreScreenMusic() {
    return m_scoreScreenMusicRequests.publish(ScoreScreenMusicRequest{});
}

void AudioSubsystem::applyPendingScoreScreenMusicRequest() {
    ScoreScreenMusicRequest request;
    if (!m_scoreScreenMusicRequests.tryTake(request)) return;

    if (request.trackName.empty()) {
        // Do not stop a new map's script-selected music when a delayed UI
        // close arrives after session replacement. Any ordinary SetMusicTrack
        // control clears this ownership bit below.
        if (!m_scoreScreenMusicActive) return;
        m_pendingMusic.reset();
        m_musicTransition = MusicTransition::None;
        m_musicTransitionElapsedSeconds = 0.0f;
        stopActiveMusic();
        m_scoreScreenMusicTrack.clear();
        m_scoreScreenMusicActive = false;
        applyMusicBusVolume(1.0f);
        return;
    }

    if (m_scoreScreenMusicActive &&
        m_scoreScreenMusicTrack == request.trackName) {
        return;
    }

    audio::AudioPresentationControlEvent control;
    control.kind = audio::AudioPresentationControlKind::SetMusicTrack;
    control.trackName = request.trackName;
    // Result state may no longer advance confirmed simulation time. Replace
    // immediately so a simulation-driven script fade cannot strand the score
    // track in Pending forever.
    control.fadeOut = false;
    control.fadeIn = false;
    consumePresentationControl(std::move(control));
    m_scoreScreenMusicTrack = std::move(request.trackName);
    m_scoreScreenMusicActive = true;
}

bool AudioSubsystem::stopSequencedEvent(audio::AudioEventInstanceHandle handle) noexcept {
    return m_sequencer.stop(handle);
}

audio::AudioEventInstanceState AudioSubsystem::sequencedEventState(
    audio::AudioEventInstanceHandle handle) const noexcept {
    return m_sequencer.state(handle);
}

void AudioSubsystem::clearPresentationSession() {
    if (m_presentationSessionEpoch == 0) return;
    m_sequencer.reset();
    m_audio.reset();
    m_activeEvents.reset();
    const audio::AudioEventSettings& settings = m_baseEvents.settings();
    m_audio.setRuntimeEventPolicy({
        .minSampleVolume = settings.minSampleVolume,
        .use3DRangeVolumeFade = settings.use3DRangeVolumeFade,
        .rangeVolumeFadeExponent = settings.rangeVolumeFadeExponent,
    });
    resetScriptAudioPresentationState();
    m_presentationSessionEpoch = 0;
    m_presentationPlaybackReleased = true;
    applyLocalPausePolicy();
    m_lastPresentationSimulationFrame = 0;
    m_hasPresentationSimulationFrame = false;
    m_nextPresentationEventSeed = 1;
    m_nextFxSoundEventOrdinal = 1;
}

container::Vector<audio::AudioNaturalCompletion> AudioSubsystem::takeNaturalCompletions() {
    return m_sequencer.takeNaturalCompletions();
}

void AudioSubsystem::consumePresentationControl(audio::AudioPresentationControlEvent control) {
    switch (control.kind) {
    case audio::AudioPresentationControlKind::SetMusicVolume:
        if (!std::isfinite(control.volume)) return;
        m_scriptMusicVolume = std::clamp(control.volume, 0.0f, 1.0f);
        if (m_musicTransition == MusicTransition::FadingOut) {
            const float fade = std::clamp(1.0f -
                m_musicTransitionElapsedSeconds / kScriptMusicFadeSeconds, 0.0f, 1.0f);
            applyMusicBusVolume(fade);
        } else if (m_musicTransition == MusicTransition::FadingIn) {
            const float fade = std::clamp(
                m_musicTransitionElapsedSeconds / kScriptMusicFadeSeconds, 0.0f, 1.0f);
            applyMusicBusVolume(fade);
        } else {
            applyMusicBusVolume(1.0f);
        }
        return;
    case audio::AudioPresentationControlKind::SetAmbientPaused:
        m_ambientPausedByScript = control.paused;
        m_audio.setBusEnabled(audio::AudioBus::Ambient, !m_ambientPausedByScript);
        return;
    case audio::AudioPresentationControlKind::SetBackgroundSoundsPaused:
        m_backgroundSoundsPausedByScript = control.paused;
        applySoundBusState();
        applyLocalPausePolicy();
        return;
    case audio::AudioPresentationControlKind::SetSoundVolume:
        if (!std::isfinite(control.volume)) return;
        m_scriptSoundVolume = std::clamp(control.volume, 0.0f, 1.0f);
        m_scriptSound3DVolume = m_scriptSoundVolume;
        applySoundBusState();
        return;
    case audio::AudioPresentationControlKind::SetSpeechVolume:
        if (!std::isfinite(control.volume)) return;
        m_scriptSpeechVolume = std::clamp(control.volume, 0.0f, 1.0f);
        applySpeechBusVolume();
        return;
    case audio::AudioPresentationControlKind::SetEventVolumeOverride: {
        const container::String key = canonicalEventKey(control.eventName);
        if (key.empty() || !std::isfinite(control.volume)) return;
        const float volume = std::clamp(control.volume, 0.0f, 4.0f);
        m_scriptEventVolumeOverrides.insert_or_assign(key, volume);
        m_sequencer.setEventVolumeOverride(key, volume, m_audio);
        return;
    }
    case audio::AudioPresentationControlKind::RestoreEventVolumeOverride: {
        const container::String key = canonicalEventKey(control.eventName);
        if (key.empty()) return;
        m_scriptEventVolumeOverrides.erase(key);
        m_sequencer.setEventVolumeOverride(key, std::nullopt, m_audio);
        return;
    }
    case audio::AudioPresentationControlKind::RestoreAllEventVolumeOverrides:
        m_scriptEventVolumeOverrides.clear();
        m_sequencer.clearEventVolumeOverrides(m_audio);
        return;
    case audio::AudioPresentationControlKind::RemoveEvent:
        m_sequencer.removeEventsNamed(control.eventName);
        return;
    case audio::AudioPresentationControlKind::RemoveDisabledEvents:
        for (const auto& [eventName, volume] : m_scriptEventVolumeOverrides) {
            if (volume <= 0.0f) m_sequencer.removeEventsNamed(eventName);
        }
        return;
    case audio::AudioPresentationControlKind::SetEvaEnabled:
        m_evaEnabledByScript = control.enabled;
        if (!m_evaEnabledByScript) {
            m_sequencer.removeEvaEvents();
            clearEvaScheduler(false);
        }
        return;
    case audio::AudioPresentationControlKind::SetObjectAmbientSoundEnabled: {
        // Drawable::enableAmbientSoundFromScript intentionally does not
        // coalesce repeated enables: startAmbientSound first stops the old
        // handle, then starts the current damage-state event again. Preserve
        // that observable one-shot/restart behavior here.
        if (control.emitterKey == 0) return;
        if (control.generation != 0) {
            const auto observed =
                m_objectAmbientGenerations.find(control.emitterKey);
            if (observed != m_objectAmbientGenerations.end() &&
                control.generation <= observed->second) {
                return;
            }
            m_objectAmbientGenerations.insert_or_assign(
                control.emitterKey, control.generation);
        }
        stopScriptObjectAmbientSound(control.emitterKey);
        const audio::AudioEventDefinition* definition =
            events().find(control.eventName);
        bool enabled = control.enabled;
        if (control.automaticEnabled && definition) {
            const bool looping = control.instanceOverrides.looping.value_or(
                audio::hasAudioEventControl(
                    definition->controlFlags,
                    audio::AudioEventControl::Loop));
            const int32_t loopCount = looping &&
                    control.instanceOverrides.loopCount
                ? *control.instanceOverrides.loopCount
                : definition->loopCount;
            enabled = looping && loopCount == 0;
        }
        if (!enabled || control.eventName.empty() || !definition) {
            return;
        }
        const container::String eventName = control.eventName;
        audio::AudioEventIntent intent{
            .eventName = eventName,
            .eventId = control.eventId,
            .confirmedFrame = control.confirmedFrame,
            .emitterKey = control.emitterKey,
            .ownerKey = control.emitterKey,
            .variationSeed = control.eventId == 0
                ? m_nextPresentationEventSeed++
                : control.eventId,
            .audience = control.audience,
        };
        if (m_nextPresentationEventSeed == 0) m_nextPresentationEventSeed = 1;
        std::optional<float> eventVolumeOverride;
        if (const auto overrideValue = m_scriptEventVolumeOverrides.find(
                canonicalEventKey(intent.eventName));
            overrideValue != m_scriptEventVolumeOverrides.end()) {
            eventVolumeOverride = overrideValue->second;
        }
        if (!shouldPlayForAudience(events(), intent)) return;
        const audio::AudioEventInstanceHandle handle = m_sequencer.enqueue(
            std::move(intent), std::move(eventVolumeOverride), true, false,
            std::move(control.instanceOverrides));
        if (!handle) return;
        m_scriptObjectAmbientSounds.insert_or_assign(control.emitterKey,
            ScriptObjectAmbientSound{.eventName = eventName, .handle = handle});
        return;
    }
    case audio::AudioPresentationControlKind::SetObjectLoopingSoundEnabled: {
        if (control.emitterKey == 0 || control.eventName.empty()) return;
        container::String loopKey = std::to_string(control.emitterKey);
        loopKey.push_back('\x1f');
        loopKey += canonicalEventKey(control.eventName);
        if (const auto found = m_gameplayObjectLoopSounds.find(loopKey);
            found != m_gameplayObjectLoopSounds.end()) {
            if (found->second.handle) {
                static_cast<void>(m_sequencer.stop(found->second.handle));
            }
            m_gameplayObjectLoopSounds.erase(found);
        }
        if (!control.enabled || !events().find(control.eventName)) return;

        const container::String eventName = control.eventName;
        audio::AudioEventIntent intent{
            .eventName = eventName,
            .eventId = control.eventId,
            .confirmedFrame = control.confirmedFrame,
            .emitterKey = control.emitterKey,
            .ownerKey = control.emitterKey,
            .variationSeed = control.eventId == 0
                ? m_nextPresentationEventSeed++ : control.eventId,
            .audience = control.audience,
        };
        if (m_nextPresentationEventSeed == 0) m_nextPresentationEventSeed = 1;
        std::optional<float> eventVolumeOverride;
        if (const auto overrideValue = m_scriptEventVolumeOverrides.find(
                canonicalEventKey(intent.eventName));
            overrideValue != m_scriptEventVolumeOverrides.end()) {
            eventVolumeOverride = overrideValue->second;
        }
        if (!shouldPlayForAudience(events(), intent)) return;
        const audio::AudioEventInstanceHandle handle = m_sequencer.enqueue(
            std::move(intent), std::move(eventVolumeOverride), false, false);
        if (!handle) return;
        m_gameplayObjectLoopSounds.insert_or_assign(
            std::move(loopKey),
            ScriptObjectAmbientSound{.eventName = eventName, .handle = handle});
        return;
    }
    case audio::AudioPresentationControlKind::SetMusicTrack:
        // Script/new-session music supersedes a local ScoreScreen override.
        // A subsequently delayed UI stop must not silence that newer track.
        m_scoreScreenMusicTrack.clear();
        m_scoreScreenMusicActive = false;
        break;
    }

    if (control.trackName.empty()) return;
    PendingMusicTrack requested{
        .intent = {
            .eventName = std::move(control.trackName),
            .eventId = control.eventId,
            .confirmedFrame = control.confirmedFrame,
            .variationSeed = control.eventId == 0 ? 1 : control.eventId,
        },
        .fadeIn = control.fadeIn,
    };
    // A replacement arriving while the prior fade is in flight supersedes
    // the pending track; keep the current voice only long enough to fade it
    // out once. This is the modern equivalent of removeAudioEvent followed
    // by a new AudioEventRTS in the same script-action order.
    m_pendingMusic = std::move(requested);
    if (control.fadeOut && m_activeMusic) {
        m_musicTransition = MusicTransition::FadingOut;
        m_musicTransitionElapsedSeconds = 0.0f;
        applyMusicBusVolume(1.0f);
    } else {
        stopActiveMusic();
        PendingMusicTrack next = std::move(*m_pendingMusic);
        m_pendingMusic.reset();
        beginMusicTrack(std::move(next));
    }
}

void AudioSubsystem::advanceMusicTransition(float deltaSeconds) {
    if (m_musicTransition == MusicTransition::None || !std::isfinite(deltaSeconds) ||
        deltaSeconds <= 0.0f) {
        return;
    }
    m_musicTransitionElapsedSeconds = std::min(
        kScriptMusicFadeSeconds, m_musicTransitionElapsedSeconds + deltaSeconds);
    const float fraction = std::clamp(
        m_musicTransitionElapsedSeconds / kScriptMusicFadeSeconds, 0.0f, 1.0f);
    if (m_musicTransition == MusicTransition::FadingOut) {
        applyMusicBusVolume(1.0f - fraction);
        if (fraction < 1.0f) return;
        stopActiveMusic();
        if (!m_pendingMusic) {
            m_musicTransition = MusicTransition::None;
            applyMusicBusVolume(1.0f);
            return;
        }
        PendingMusicTrack next = std::move(*m_pendingMusic);
        m_pendingMusic.reset();
        beginMusicTrack(std::move(next));
        return;
    }

    applyMusicBusVolume(fraction);
    if (fraction >= 1.0f) {
        m_musicTransition = MusicTransition::None;
        m_musicTransitionElapsedSeconds = 0.0f;
        applyMusicBusVolume(1.0f);
    }
}

void AudioSubsystem::beginMusicTrack(PendingMusicTrack track) {
    m_activeMusic = m_sequencer.enqueue(std::move(track.intent));
    if (track.fadeIn && m_activeMusic) {
        m_musicTransition = MusicTransition::FadingIn;
        m_musicTransitionElapsedSeconds = 0.0f;
        applyMusicBusVolume(0.0f);
    } else {
        m_musicTransition = MusicTransition::None;
        m_musicTransitionElapsedSeconds = 0.0f;
        applyMusicBusVolume(1.0f);
    }
}

void AudioSubsystem::stopActiveMusic() noexcept {
    if (m_activeMusic) {
        static_cast<void>(m_sequencer.stop(m_activeMusic));
        m_activeMusic = {};
    }
}

void AudioSubsystem::applyMusicBusVolume(float normalizedFade) noexcept {
    m_audio.setBusVolume(audio::AudioBus::Music,
                          std::clamp(m_scriptMusicVolume * normalizedFade, 0.0f, 1.0f));
}

void AudioSubsystem::applySoundBusState() noexcept {
    m_audio.setBusVolume(audio::AudioBus::Sound, m_scriptSoundVolume);
    m_audio.setBusVolume(audio::AudioBus::Sound3D, m_scriptSound3DVolume);
    m_audio.setBusVolume(audio::AudioBus::Ambient, m_scriptSound3DVolume);
    // Script SUSPEND_BACKGROUND_SOUNDS maps to RefCode's
    // pauseAudio(AudioAffect_Sound): preserve every sample cursor and resume
    // it later.  Bus enable is a mute/disable policy and lets a one-shot
    // weapon or detonation sound expire silently while the script is paused.
    m_audio.setBusEnabled(audio::AudioBus::Sound, true);
    m_audio.setBusEnabled(audio::AudioBus::Sound3D, true);
}

void AudioSubsystem::applySpeechBusVolume() noexcept {
    m_audio.setBusVolume(audio::AudioBus::Speech, m_scriptSpeechVolume);
}

void AudioSubsystem::applyLocalPausePolicy() noexcept {
    const bool presentationHeld = m_presentationSessionEpoch != 0u &&
        !m_presentationPlaybackReleased;
    m_audio.setBusPaused(
        audio::AudioBus::Music, m_localMusicPaused || presentationHeld);
    const bool pauseNonMusic = m_localNonMusicPaused || presentationHeld;
    const bool pauseSound = pauseNonMusic ||
        m_backgroundSoundsPausedByScript;
    m_audio.setBusPaused(audio::AudioBus::Sound, pauseSound);
    m_audio.setBusPaused(audio::AudioBus::Sound3D, pauseSound);
    m_audio.setBusPaused(audio::AudioBus::Ambient, pauseNonMusic);
    m_audio.setBusPaused(audio::AudioBus::Speech, pauseNonMusic);
}

void AudioSubsystem::stopScriptObjectAmbientSound(uint64_t emitterKey) noexcept {
    if (emitterKey == 0) return;
    const auto found = m_scriptObjectAmbientSounds.find(emitterKey);
    if (found == m_scriptObjectAmbientSounds.end()) return;
    if (found->second.handle) static_cast<void>(m_sequencer.stop(found->second.handle));
    m_scriptObjectAmbientSounds.erase(found);
}

void AudioSubsystem::pruneScriptObjectAmbientSounds() noexcept {
    std::erase_if(m_scriptObjectAmbientSounds, [this](const auto& entry) {
        return !entry.second.handle || !isLiveSequencedEvent(m_sequencer.state(entry.second.handle));
    });
    std::erase_if(m_gameplayObjectLoopSounds, [this](const auto& entry) {
        return !entry.second.handle ||
               !isLiveSequencedEvent(m_sequencer.state(entry.second.handle));
    });
}

void AudioSubsystem::resetScriptAudioPresentationState() noexcept {
    m_activeMusic = {};
    m_scoreScreenMusicTrack.clear();
    m_scoreScreenMusicActive = false;
    m_pendingMusic.reset();
    m_musicTransition = MusicTransition::None;
    m_musicTransitionElapsedSeconds = 0.0f;
    m_pendingMusicSimulationSeconds = 0.0f;
    m_scriptObjectAmbientSounds.clear();
    m_objectAmbientGenerations.clear();
    m_gameplayObjectLoopSounds.clear();
    const audio::AudioEventSettings& settings = events().settings();
    m_scriptMusicVolume = settings.defaultMusicVolume;
    m_scriptSoundVolume = settings.defaultSoundVolume;
    m_scriptSound3DVolume = settings.defaultSound3DVolume;
    m_scriptSpeechVolume = settings.defaultSpeechVolume;
    m_ambientPausedByScript = false;
    m_backgroundSoundsPausedByScript = false;
    m_evaEnabledByScript = true;
    clearEvaScheduler(true);
    m_scriptEventVolumeOverrides.clear();
    applyMusicBusVolume(1.0f);
    applySoundBusState();
    applySpeechBusVolume();
    m_audio.setBusEnabled(audio::AudioBus::Ambient, true);
}

void AudioSubsystem::admitEvaEvent(audio::AudioPresentationEvent event) {
    // submitPresentationSnapshot already performs this check, but keep the
    // scheduler ingress self-contained: an EVA event must never become an
    // audible pending request merely because a future caller bypassed the
    // ordinary snapshot loop.
    if (!m_evaEnabledByScript || !event.evaPolicy ||
        !shouldPlayForAudience(events(), event.intent)) {
        return;
    }
    const audio::EvaPresentationPolicy policy = *event.evaPolicy;
    const size_t typeIndex = static_cast<size_t>(policy.type);
    if (typeIndex >= audio::kEvaEventTypeCount || policy.priority == 0)
        return;
    if (m_evaCooldownActive[typeIndex] ||
        (m_activeEvaType && *m_activeEvaType == policy.type) ||
        std::any_of(
            m_pendingEvaEvents.begin(), m_pendingEvaEvents.end(),
            [type = policy.type](const PendingEvaEvent& pending) {
                return pending.event.evaPolicy &&
                    pending.event.evaPolicy->type == type;
            })) {
        return;
    }
    const uint64_t triggeredFrame = event.intent.confirmedFrame;
    const uint64_t expirationFrame = policy.expirationFrames >
            std::numeric_limits<uint64_t>::max() - triggeredFrame
        ? std::numeric_limits<uint64_t>::max()
        : triggeredFrame + policy.expirationFrames;
    m_pendingEvaEvents.push_back({
        .event = std::move(event),
        .triggeredFrame = triggeredFrame,
        .expirationFrame = expirationFrame,
        .arrivalOrdinal = m_nextEvaArrivalOrdinal++,
    });
    if (m_nextEvaArrivalOrdinal == 0) m_nextEvaArrivalOrdinal = 1;
}

void AudioSubsystem::advanceEvaScheduler() {
    if (m_activeEvaEvent &&
        !isLiveSequencedEvent(m_sequencer.state(m_activeEvaEvent))) {
        m_activeEvaEvent = {};
        m_activeEvaType.reset();
    }
    for (size_t index = 0; index < m_evaCooldownActive.size(); ++index) {
        if (m_evaCooldownActive[index] &&
            m_evaAvailableFrames[index] <= m_evaSimulationFrame +
                (m_evaSimulationFrame != std::numeric_limits<uint64_t>::max()
                    ? 1u : 0u)) {
            m_evaCooldownActive[index] = false;
        }
    }
    std::erase_if(m_pendingEvaEvents, [this](const PendingEvaEvent& pending) {
        return pending.expirationFrame <= m_evaSimulationFrame;
    });
    if (!m_evaEnabledByScript || m_activeEvaEvent ||
        m_pendingEvaEvents.empty() ||
        m_lastEvaArbitrationFrame == m_evaSimulationFrame) {
        return;
    }
    m_lastEvaArbitrationFrame = m_evaSimulationFrame;
    auto selected = m_pendingEvaEvents.begin();
    for (auto candidate = selected + 1; candidate != m_pendingEvaEvents.end();
         ++candidate) {
        const uint32_t candidatePriority = candidate->event.evaPolicy
            ? candidate->event.evaPolicy->priority : 0u;
        const uint32_t selectedPriority = selected->event.evaPolicy
            ? selected->event.evaPolicy->priority : 0u;
        if (candidatePriority > selectedPriority ||
            (candidatePriority == selectedPriority &&
             candidate->arrivalOrdinal < selected->arrivalOrdinal)) {
            selected = candidate;
        }
    }
    audio::AudioPresentationEvent event = std::move(selected->event);
    m_pendingEvaEvents.erase(selected);
    if (!event.evaPolicy) return;
    const audio::EvaPresentationPolicy policy = *event.evaPolicy;
    const size_t typeIndex = static_cast<size_t>(policy.type);
    m_evaCooldownActive[typeIndex] = true;
    m_evaAvailableFrames[typeIndex] = policy.cooldownFrames >
            std::numeric_limits<uint64_t>::max() - m_evaSimulationFrame
        ? std::numeric_limits<uint64_t>::max()
        : m_evaSimulationFrame + policy.cooldownFrames;
    if (event.intent.eventName.empty()) return;

    const auto overrideValue = m_scriptEventVolumeOverrides.find(
        canonicalEventKey(event.intent.eventName));
    if (overrideValue != m_scriptEventVolumeOverrides.end()) {
        event.eventVolumeOverride = overrideValue->second;
    }
    m_activeEvaType = policy.type;
    m_activeEvaEvent = m_sequencer.enqueue(
        std::move(event.intent), std::move(event.eventVolumeOverride),
        event.ambient, true);
    if (!m_activeEvaEvent) m_activeEvaType.reset();
}

void AudioSubsystem::clearEvaScheduler(bool clearCooldowns) noexcept {
    m_pendingEvaEvents.clear();
    m_activeEvaEvent = {};
    m_activeEvaType.reset();
    m_evaSimulationFrame = 0;
    m_lastEvaArbitrationFrame = std::numeric_limits<uint64_t>::max();
    m_nextEvaArrivalOrdinal = 1;
    if (clearCooldowns) {
        m_evaAvailableFrames = {};
        m_evaCooldownActive = {};
    }
}

std::optional<float> AudioSubsystem::scriptEventVolumeOverride(
    container::StringView eventName) const {
    const auto found = m_scriptEventVolumeOverrides.find(canonicalEventKey(eventName));
    return found == m_scriptEventVolumeOverrides.end()
        ? std::nullopt
        : std::optional<float>{found->second};
}

std::optional<container::StringView> AudioSubsystem::scriptObjectAmbientEvent(
    uint64_t emitterKey) const noexcept {
    const auto found = m_scriptObjectAmbientSounds.find(emitterKey);
    return found == m_scriptObjectAmbientSounds.end()
        ? std::nullopt
        : std::optional<container::StringView>{found->second.eventName};
}

audio::AudioVoiceHandle AudioSubsystem::enqueueEvent(
    const audio::AudioEventIntent& suppliedIntent,
    audio::AudioEventPlaybackCursor* suppliedCursor) {
    audio::AudioEventIntent intent = suppliedIntent;
    if (intent.variationSeed == 0) {
        intent.variationSeed = m_nextPresentationEventSeed++;
        if (m_nextPresentationEventSeed == 0) m_nextPresentationEventSeed = 1;
    }

    audio::AudioEventPlaybackCursor localCursor;
    audio::AudioEventPlaybackCursor& cursor = suppliedCursor ? *suppliedCursor : localCursor;
    container::String error;
    const auto resolved = events().resolve(
        intent, cursor, audio::AudioEventPortion::Main, &error);
    if (!resolved) {
        TD_LOG_WARN("[Audio] Could not resolve AudioEvent '{}': {}", intent.eventName, error);
        return {};
    }
    return m_audio.enqueuePlay(resolved->request);
}

} // namespace engine
