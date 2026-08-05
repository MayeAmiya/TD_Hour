#include "core/container/hash_containers.h"
#include "AudioEventSequencer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <utility>

namespace engine::audio {
namespace {

float saneDelta(float value) noexcept {
    return std::clamp(std::isfinite(value) ? value : 0.0f, 0.0f, 10.0f);
}

} // namespace

AudioEventInstanceHandle AudioEventSequencer::enqueue(
    AudioEventIntent intent, std::optional<float> eventVolumeOverride,
    bool ambient, bool eva,
    AudioEventInstanceOverrides instanceOverrides) {
    if (intent.eventName.empty()) return {};
    if (intent.eventId != 0) {
        if (m_seenPresentationEventIds.contains(intent.eventId)) return {};
        m_seenPresentationEventIds.insert(intent.eventId);
        m_seenPresentationEventOrder.push_back(intent.eventId);
        while (m_seenPresentationEventOrder.size() > kRetainedPresentationEventIds) {
            const uint64_t expired = m_seenPresentationEventOrder.front();
            m_seenPresentationEventOrder.erase(m_seenPresentationEventOrder.begin());
            m_seenPresentationEventIds.erase(expired);
        }
    }

    uint64_t value = m_nextHandleValue++;
    if (value == 0) value = m_nextHandleValue++;
    const AudioEventInstanceHandle handle{value};

    ActiveEvent event;
    event.handle = handle;
    event.intent = std::move(intent);
    if (eventVolumeOverride && std::isfinite(*eventVolumeOverride)) {
        event.eventVolumeOverride = std::clamp(*eventVolumeOverride, 0.0f, 4.0f);
    }
    const auto sanitizeFinite = [](std::optional<float>& value,
                                   float minimum, float maximum) {
        if (!value || !std::isfinite(*value)) {
            value.reset();
            return;
        }
        *value = std::clamp(*value, minimum, maximum);
    };
    if (instanceOverrides.loopCount && *instanceOverrides.loopCount < 0) {
        instanceOverrides.loopCount.reset();
    }
    sanitizeFinite(instanceOverrides.minimumVolume, 0.0f, 4.0f);
    sanitizeFinite(instanceOverrides.volume, 0.0f, 4.0f);
    sanitizeFinite(instanceOverrides.minimumRange, 0.0f, 1'000'000.0f);
    sanitizeFinite(instanceOverrides.maximumRange, 0.0f, 1'000'000.0f);
    if (instanceOverrides.priority && *instanceOverrides.priority > 4u) {
        instanceOverrides.priority.reset();
    }
    event.instanceOverrides = std::move(instanceOverrides);
    event.ambient = ambient;
    event.eva = eva;
    event.arrivalOrdinal = m_nextArrivalOrdinal++;
    m_active.emplace(handle.value, std::move(event));
    m_activeOrder.push_back(handle.value);
    setState(handle, AudioEventInstanceState::Pending);
    return handle;
}

void AudioEventSequencer::submit(AudioPresentationSnapshot snapshot) {
    // GameSession replacement is coordinated by AudioSubsystem, which also
    // stops backend voices. Keep this standalone sequencer defensive for
    // tools: a direct session change drops only its local event bookkeeping.
    if (snapshot.sessionEpoch != 0 && m_sessionEpoch != 0 &&
        snapshot.sessionEpoch != m_sessionEpoch) {
        reset();
    }
    if (snapshot.sessionEpoch != 0) m_sessionEpoch = snapshot.sessionEpoch;

    m_pendingSimulationDeltaSeconds += saneDelta(snapshot.simulationDeltaSeconds);
    m_emitters.clear();
    m_emitters.reserve(snapshot.emitters.size());
    for (const AudioEmitterSnapshot& emitter : snapshot.emitters) {
        if (emitter.emitterKey != 0) {
            m_emitters.insert_or_assign(emitter.emitterKey, emitter.transform);
        }
    }
    // A submitted snapshot declares the whole current liveness set. This is
    // intentionally separate from direct editor/diagnostic enqueue() calls,
    // which may supply a fixed position without a session snapshot.
    m_hasEmitterSnapshot = true;
    for (AudioPresentationEvent& event : snapshot.events) {
        static_cast<void>(enqueue(std::move(event.intent), std::move(event.eventVolumeOverride),
                                  event.ambient, event.eva));
    }
}

bool AudioEventSequencer::stop(AudioEventInstanceHandle handle) noexcept {
    ActiveEvent* event = findActive(handle);
    if (!event) return false;
    event->stopRequested = true;
    return true;
}

void AudioEventSequencer::stopAll() noexcept {
    for (const uint64_t handle : m_activeOrder) {
        if (const auto found = m_active.find(handle); found != m_active.end()) {
            found->second.stopRequested = true;
        }
    }
}

void AudioEventSequencer::setEventVolumeOverride(container::StringView eventName,
                                                  std::optional<float> volume,
                                                  AudioSystem& audio) {
    const container::String eventKey = canonicalEventKey(eventName);
    if (eventKey.empty()) return;
    if (volume && (!std::isfinite(*volume) || *volume < 0.0f)) return;
    const std::optional<float> normalized = volume
        ? std::optional<float>{std::clamp(*volume, 0.0f, 4.0f)}
        : std::nullopt;

    for (const uint64_t handle : m_activeOrder) {
        const auto found = m_active.find(handle);
        if (found == m_active.end()) continue;
        ActiveEvent& event = found->second;
        const container::StringView key = event.eventKey.empty()
            ? container::StringView{event.intent.eventName}
            : container::StringView{event.eventKey};
        if (canonicalEventKey(key) != eventKey) continue;
        event.eventVolumeOverride = normalized;
        if (event.voice) {
            const float effective = normalized.value_or(event.lastResolvedVolume);
            static_cast<void>(audio.enqueueSetVolume(event.voice, effective));
        }
    }
}

void AudioEventSequencer::clearEventVolumeOverrides(AudioSystem& audio) {
    for (const uint64_t handle : m_activeOrder) {
        const auto found = m_active.find(handle);
        if (found == m_active.end()) continue;
        ActiveEvent& event = found->second;
        if (!event.eventVolumeOverride) continue;
        event.eventVolumeOverride.reset();
        if (event.voice) {
            static_cast<void>(audio.enqueueSetVolume(event.voice, event.lastResolvedVolume));
        }
    }
}

void AudioEventSequencer::removeEventsNamed(container::StringView eventName) noexcept {
    const container::String eventKey = canonicalEventKey(eventName);
    if (eventKey.empty()) return;
    for (const uint64_t handle : m_activeOrder) {
        const auto found = m_active.find(handle);
        if (found == m_active.end()) continue;
        ActiveEvent& event = found->second;
        const container::StringView key = event.eventKey.empty()
            ? container::StringView{event.intent.eventName}
            : container::StringView{event.eventKey};
        if (canonicalEventKey(key) == eventKey) event.stopRequested = true;
    }
}

void AudioEventSequencer::removeEvaEvents() noexcept {
    for (const uint64_t handle : m_activeOrder) {
        const auto found = m_active.find(handle);
        if (found != m_active.end() && found->second.eva) {
            found->second.stopRequested = true;
        }
    }
}

void AudioEventSequencer::update(AudioSystem& audio, const AudioEventCatalog& catalog) {
    const float deltaSeconds = std::exchange(m_pendingSimulationDeltaSeconds, 0.0f);
    // Do not enumerate `m_active`: same-frame events can compete for an INI
    // Limit, a speech channel or a bounded backend command queue. The source
    // snapshot's vector order is therefore part of presentation semantics.
    const container::Vector<uint64_t> handles = m_activeOrder;

    for (const uint64_t value : handles) {
        auto found = m_active.find(value);
        if (found == m_active.end()) continue;
        ActiveEvent& event = found->second;

        refreshEmitter(event, audio);

        if (event.stopRequested) {
            // A full command queue must not turn emitter retirement into a
            // bookkeeping-only stop that leaves a looping backend voice
            // audible. Retain the event and retry on the next presentation
            // update until the stop reaches AudioSystem.
            if (!event.voice || audio.enqueueStop(event.voice)) {
                finish(found, AudioEventInstanceState::Stopped);
            }
            continue;
        }

        if (event.phase == Phase::Queued) {
            startQueued(event, audio, catalog);
            continue;
        }

        if (event.phase == Phase::WaitingForLoop) {
            event.loopDelayRemainingSeconds -= deltaSeconds;
            if (event.loopDelayRemainingSeconds <= 0.0f) {
                startPreparedLoop(event, audio, catalog);
            } else {
                setState(event.handle, AudioEventInstanceState::Waiting);
            }
            continue;
        }

        const AudioVoiceState voiceState = audio.voiceState(event.voice);
        switch (voiceState) {
        case AudioVoiceState::Pending:
            setState(event.handle, AudioEventInstanceState::Pending);
            break;
        case AudioVoiceState::Playing:
            setState(event.handle, AudioEventInstanceState::Playing);
            break;
        case AudioVoiceState::Completed:
            event.voice = {};
            completeVoice(event, audio, catalog);
            break;
        case AudioVoiceState::Stopped:
            finish(found, AudioEventInstanceState::Stopped);
            break;
        case AudioVoiceState::Preempted:
            finish(found, AudioEventInstanceState::Preempted);
            break;
        case AudioVoiceState::Failed:
            finish(found, AudioEventInstanceState::Failed);
            break;
        case AudioVoiceState::Suppressed:
            // A headless client intentionally consumes presentation events
            // without pretending that an Attack/Main/Decay sequence rendered.
            finish(found, AudioEventInstanceState::Suppressed);
            break;
        case AudioVoiceState::Unknown:
            // A retained voice state can only become unknown after its bounded
            // history expired. Treat that as a terminal backend loss rather
            // than restarting an authored effect unexpectedly.
            finish(found, AudioEventInstanceState::Failed);
            break;
        }
    }
}

void AudioEventSequencer::reset() noexcept {
    for (const uint64_t handle : m_activeOrder) {
        if (const auto found = m_active.find(handle); found != m_active.end()) {
            setState(found->second.handle, AudioEventInstanceState::Stopped);
        }
    }
    m_active.clear();
    m_activeOrder.clear();
    m_emitters.clear();
    m_naturalCompletions.clear();
    m_hasEmitterSnapshot = false;
    m_seenPresentationEventIds.clear();
    m_seenPresentationEventOrder.clear();
    m_disallowSpeech = false;
    m_pendingSimulationDeltaSeconds = 0.0f;
    m_sessionEpoch = 0;
}

AudioEventInstanceState AudioEventSequencer::state(AudioEventInstanceHandle handle) const noexcept {
    if (!handle) return AudioEventInstanceState::Unknown;
    const auto found = m_states.find(handle.value);
    return found == m_states.end() ? AudioEventInstanceState::Unknown : found->second;
}

size_t AudioEventSequencer::activeCount() const noexcept {
    return m_active.size();
}

container::Vector<AudioNaturalCompletion> AudioEventSequencer::takeNaturalCompletions() {
    container::Vector<AudioNaturalCompletion> output = std::move(m_naturalCompletions);
    m_naturalCompletions.clear();
    return output;
}

AudioEventSequencer::ActiveEvent* AudioEventSequencer::findActive(
    AudioEventInstanceHandle handle) noexcept {
    const auto found = m_active.find(handle.value);
    return found == m_active.end() ? nullptr : &found->second;
}

const AudioEventSequencer::ActiveEvent* AudioEventSequencer::findActive(
    AudioEventInstanceHandle handle) const noexcept {
    const auto found = m_active.find(handle.value);
    return found == m_active.end() ? nullptr : &found->second;
}

void AudioEventSequencer::setState(AudioEventInstanceHandle handle,
                                   AudioEventInstanceState state) noexcept {
    if (!handle) return;
    m_states.insert_or_assign(handle.value, state);
    if (!isTerminal(state)) return;

    m_terminalOrder.push_back(handle.value);
    while (m_terminalOrder.size() > kRetainedTerminalStates) {
        const uint64_t expired = m_terminalOrder.front();
        m_terminalOrder.erase(m_terminalOrder.begin());
        const auto found = m_states.find(expired);
        if (found != m_states.end() && isTerminal(found->second)) {
            m_states.erase(found);
        }
    }
}

void AudioEventSequencer::finish(container::HashMap<uint64_t, ActiveEvent>::iterator event,
                                 AudioEventInstanceState state) noexcept {
    if (state == AudioEventInstanceState::Completed) {
        switch (event->second.kind) {
        case AudioEventKind::SoundEffect:
            appendNaturalCompletion(event->second, AudioNaturalCompletionKind::Audio);
            break;
        case AudioEventKind::Streaming:
            appendNaturalCompletion(event->second, AudioNaturalCompletionKind::Speech);
            break;
        case AudioEventKind::Music:
            // A normal MusicTrack Main never reaches finish: it restarts in
            // completeVoice() and reports MusicLoop there. Keep terminal
            // malformed paths from being mistaken for a natural loop.
            break;
        }
    }
    const uint64_t handle = event->second.handle.value;
    const bool releasedUninterruptibleSpeech =
        event->second.kind == AudioEventKind::Streaming && event->second.intent.uninterruptible;
    setState(event->second.handle, state);
    m_active.erase(event);
    const auto ordered = std::find(m_activeOrder.begin(), m_activeOrder.end(), handle);
    if (ordered != m_activeOrder.end()) m_activeOrder.erase(ordered);
    if (releasedUninterruptibleSpeech) {
        m_disallowSpeech = false;
        for (const uint64_t activeHandle : m_activeOrder) {
            const auto active = m_active.find(activeHandle);
            if (active != m_active.end() && !active->second.stopRequested &&
                active->second.kind == AudioEventKind::Streaming &&
                active->second.intent.uninterruptible) {
                m_disallowSpeech = true;
                break;
            }
        }
    }
}

void AudioEventSequencer::appendNaturalCompletion(
    const ActiveEvent& event, AudioNaturalCompletionKind kind) noexcept {
    if (event.intent.eventName.empty()) return;
    try {
        // HAS_FINISHED_AUDIO/SPEECH consumes an exact one-shot fact.  The
        // presentation coordinator normally drains this vector every frame,
        // but a burst must never evict an older completion that a script is
        // still waiting to consume.  Session reset remains the only normal
        // retention boundary.
        m_naturalCompletions.push_back({
            .sessionEpoch = m_sessionEpoch,
            .eventName = event.intent.eventName,
            .kind = kind,
            .eventId = event.intent.eventId,
            .confirmedFrame = event.intent.confirmedFrame,
        });
    } catch (...) {
        // Audio completion reporting is presentation telemetry. A local OOM
        // must not terminate the sequencer or turn one failed allocation into
        // a device-thread/gameplay failure; the bounded value is simply lost.
    }
}

void AudioEventSequencer::refreshEmitter(ActiveEvent& event, AudioSystem& audio) {
    if (!event.intent.emitterKey) return;
    const auto source = m_emitters.find(*event.intent.emitterKey);
    if (source == m_emitters.end()) {
        // RefCode's AudioEventRTS deliberately changed an absent Object or
        // Drawable into OT_Dead and retained its last pose. A complete modern
        // value snapshot can express object liveness directly, so an already
        // positional, emitter-bound sound must retire rather than leave a
        // looping voice audible at a corpse's former coordinates.
        if (m_hasEmitterSnapshot && event.positional) event.stopRequested = true;
        return;
    }

    event.intent.position = source->second.position;
    const AudioVoiceTransform transform{
        .position = source->second.position,
        .velocity = source->second.velocity,
    };
    if (!event.voice || (event.hasLastSentTransform &&
        sameTransform(transform, event.lastSentTransform))) {
        return;
    }
    if (audio.enqueueUpdateTransform(event.voice, transform)) {
        event.lastSentTransform = transform;
        event.hasLastSentTransform = true;
    }
}

void AudioEventSequencer::startQueued(ActiveEvent& event, AudioSystem& audio,
                                      const AudioEventCatalog& catalog) {
    const AudioEventDefinition* definition = catalog.find(event.intent.eventName);
    if (!definition) {
        const auto found = m_active.find(event.handle.value);
        if (found != m_active.end()) finish(found, AudioEventInstanceState::Failed);
        return;
    }

    event.eventKey = canonicalEventKey(definition->name);
    event.typeFlags = definition->typeFlags;
    event.controlFlags = definition->controlFlags;
    if (event.instanceOverrides.looping) {
        if (*event.instanceOverrides.looping) {
            event.controlFlags |= audioEventFlag(AudioEventControl::Loop);
        } else {
            event.controlFlags &= static_cast<uint16_t>(
                ~audioEventFlag(AudioEventControl::Loop));
        }
    }
    event.kind = definition->kind;
    event.remainingMainPlays =
        hasAudioEventControl(event.controlFlags, AudioEventControl::Loop) &&
            event.instanceOverrides.loopCount
        ? *event.instanceOverrides.loopCount
        : definition->loopCount;
    const bool isWorldSound = definition->kind == AudioEventKind::SoundEffect &&
        hasAudioEventType(definition->typeFlags, AudioEventType::World);
    if (isWorldSound && event.intent.emitterKey && m_hasEmitterSnapshot &&
        !m_emitters.contains(*event.intent.emitterKey)) {
        if (event.intent.fallbackToPositionIfEmitterMissing &&
            event.intent.position) {
            // Confirmed FX can outlive its source object before audio sees its
            // first complete emitter snapshot. Retain the sealed 3D position,
            // but do not weaken ordinary emitter-bound audio lifetime rules.
            event.intent.emitterKey.reset();
        } else {
            // Do not let an emitter-bound world event that missed its first
            // liveness snapshot silently degrade into a 2D sound.
            const auto found = m_active.find(event.handle.value);
            if (found != m_active.end())
                finish(found, AudioEventInstanceState::Stopped);
            return;
        }
    }
    event.positional = isWorldSound && event.intent.position.has_value();

    // admit() can finish OTHER events, which erases them from the dense
    // m_active map and relocates its last element — typically this very event.
    // Capture the handle first and re-acquire afterwards; writing through the
    // stale reference would corrupt an unrelated event and leave this one
    // stuck in Queued, re-submitting its stream every frame.
    const uint64_t selfHandle = event.handle.value;
    if (!admit(event, *definition, audio)) {
        const auto found = m_active.find(selfHandle);
        if (found != m_active.end()) finish(found, AudioEventInstanceState::Rejected);
        return;
    }

    const auto admitted = m_active.find(selfHandle);
    if (admitted == m_active.end()) return;

    const AudioEventPortion initial = definition->kind == AudioEventKind::SoundEffect &&
            !definition->attackSounds.empty()
        ? AudioEventPortion::Attack
        : AudioEventPortion::Main;
    startPortion(admitted->second, initial, audio, catalog);
}

void AudioEventSequencer::startPreparedLoop(ActiveEvent& event, AudioSystem& audio,
                                            const AudioEventCatalog& catalog) {
    if (!event.preparedLoopSample) {
        const auto found = m_active.find(event.handle.value);
        if (found != m_active.end()) finish(found, AudioEventInstanceState::Failed);
        return;
    }
    std::optional<ResolvedAudioEventSample> sample = std::move(event.preparedLoopSample);
    event.preparedLoopSample.reset();
    startPortion(event, AudioEventPortion::Main, audio, catalog, std::move(sample));
}

void AudioEventSequencer::startPortion(ActiveEvent& event, AudioEventPortion portion,
                                       AudioSystem& audio, const AudioEventCatalog& catalog,
                                       std::optional<ResolvedAudioEventSample> prepared) {
    std::optional<ResolvedAudioEventSample> resolved = std::move(prepared);
    if (!resolved) {
        container::String error;
        resolved = catalog.resolve(event.intent, event.cursor, portion, &error);
        if (!resolved) {
            // Attack/Decay are optional authored lists. A missing Attack moves
            // to Main; a missing Decay is a normal completed event. A missing
            // Main is malformed data and must not silently turn into a loop.
            if (portion == AudioEventPortion::Attack) {
                startPortion(event, AudioEventPortion::Main, audio, catalog);
                return;
            }
            const auto found = m_active.find(event.handle.value);
            if (found != m_active.end()) {
                finish(found, portion == AudioEventPortion::Decay
                    ? AudioEventInstanceState::Completed
                    : AudioEventInstanceState::Failed);
            }
            return;
        }
    }

    if (resolved->request.spatialMode == AudioSpatialMode::ThreeDimensional &&
        event.intent.position) {
        resolved->request.position = *event.intent.position;
    }
    if (event.instanceOverrides.volume) {
        const float volumeScale = std::clamp(
            std::isfinite(event.intent.volumeScale)
                ? event.intent.volumeScale : 1.0f,
            0.0f, 4.0f);
        resolved->request.volume = std::clamp(
            *event.instanceOverrides.volume *
                event.cursor.resolvedVolumeShift * volumeScale,
            0.0f, 4.0f);
        resolved->request.admissionVolume = std::clamp(
            *event.instanceOverrides.volume * volumeScale,
            0.0f, 4.0f);
    }
    if (!hasAudioEventType(event.typeFlags, AudioEventType::Global)) {
        if (event.instanceOverrides.minimumRange) {
            resolved->request.minDistance =
                *event.instanceOverrides.minimumRange;
        }
        if (event.instanceOverrides.maximumRange) {
            resolved->request.maxDistance =
                *event.instanceOverrides.maximumRange;
        }
        if (resolved->request.maxDistance < resolved->request.minDistance) {
            resolved->request.maxDistance = resolved->request.minDistance;
        }
    }
    if (event.instanceOverrides.priority) {
        resolved->request.priority = static_cast<AudioPriority>(
            *event.instanceOverrides.priority);
        resolved->request.bypassSpatialVolumeCull =
            resolved->request.priority == AudioPriority::Critical ||
            hasAudioEventType(event.typeFlags, AudioEventType::Global);
    }
    event.lastResolvedVolume = resolved->request.volume;
    if (event.eventVolumeOverride) {
        // GameAudio stores script overrides as an absolute event gain. Do
        // this after the catalog has selected a sample/VolumeShift, so the
        // override wins exactly as AudioEventRTS::setVolume did.
        resolved->request.volume = *event.eventVolumeOverride;
        resolved->request.admissionVolume = *event.eventVolumeOverride;
    }
    if (event.ambient) {
        // A source must opt in to the ambient lane. Never infer it from a
        // filename or a World flag: weapons and vehicle loops are both world
        // sounds but must remain audible while scripts pause environmental
        // ambience.
        resolved->request.bus = AudioBus::Ambient;
    }
    // MusicTrack is an infinite logical stream, but do not hand that loop to
    // the device. Sequencing each completed Main sample lets the presentation
    // owner report a natural loop boundary as a value event; that event is
    // later admitted to single-player scripts only at a confirmed frame.
    // Ordinary AudioEvent LOOP still follows its authored LoopCount/Delay
    // path below, so it remains distinct from a MusicTrack.
    resolved->request.loop = false;

    const AudioVoiceHandle voice = audio.enqueuePlay(std::move(resolved->request));
    if (!voice) {
        const auto found = m_active.find(event.handle.value);
        if (found != m_active.end()) finish(found, AudioEventInstanceState::Failed);
        return;
    }

    event.portion = portion;
    event.phase = Phase::Playing;
    event.voice = voice;
    event.hasLastSentTransform = false;
    setState(event.handle, AudioEventInstanceState::Pending);
    if (event.kind == AudioEventKind::Streaming && event.intent.uninterruptible) {
        // GameAudio rejects later stream requests while a scripted
        // uninterruptible dialog owns the speech channel.
        m_disallowSpeech = true;
    }
    refreshEmitter(event, audio);
}

void AudioEventSequencer::completeVoice(ActiveEvent& event, AudioSystem& audio,
                                        const AudioEventCatalog& catalog) {
    if (event.portion == AudioEventPortion::Attack) {
        startPortion(event, AudioEventPortion::Main, audio, catalog);
        return;
    }

    if (event.portion == AudioEventPortion::Main) {
        if (event.kind == AudioEventKind::Music) {
            // MusicTrack is permanent until replaced/stopped. Restarting at
            // the sequencer boundary makes every natural decoder EOF visible
            // without exposing a miniaudio callback or handle to simulation.
            appendNaturalCompletion(event, AudioNaturalCompletionKind::MusicLoop);
            startPortion(event, AudioEventPortion::Main, audio, catalog);
            return;
        }
        if (event.kind != AudioEventKind::Music &&
            hasAudioEventControl(event.controlFlags, AudioEventControl::Loop)) {
            // AudioEventRTS::decreaseLoopCount(): N means N total Main plays;
            // zero remains zero and represents an explicitly permanent sound.
            if (event.remainingMainPlays == 1) event.remainingMainPlays = -1;
            else if (event.remainingMainPlays > 1) --event.remainingMainPlays;
            if (event.remainingMainPlays >= 0) {
                scheduleNextMainLoop(event, audio, catalog);
                return;
            }
        }

        const AudioEventDefinition* definition = catalog.find(event.intent.eventName);
        if (definition && definition->kind == AudioEventKind::SoundEffect &&
            !definition->decaySounds.empty()) {
            startPortion(event, AudioEventPortion::Decay, audio, catalog);
        } else {
            const auto found = m_active.find(event.handle.value);
            if (found != m_active.end()) finish(found, AudioEventInstanceState::Completed);
        }
        return;
    }

    const auto found = m_active.find(event.handle.value);
    if (found != m_active.end()) finish(found, AudioEventInstanceState::Completed);
}

void AudioEventSequencer::scheduleNextMainLoop(ActiveEvent& event, AudioSystem& audio,
                                               const AudioEventCatalog& catalog) {
    container::String error;
    std::optional<ResolvedAudioEventSample> resolved = catalog.resolve(
        event.intent, event.cursor, AudioEventPortion::Main, &error);
    if (!resolved) {
        const auto found = m_active.find(event.handle.value);
        if (found != m_active.end()) finish(found, AudioEventInstanceState::Failed);
        return;
    }

    // AudioEventRTS generates Delay while selecting the *next* main filename.
    // Miles puts that event back into its pending request list only when the
    // delay exceeds a logic frame. Waiting for any positive deterministic
    // value here is the modern, frame-rate independent equivalent.
    const float delaySeconds = static_cast<float>(resolved->delayBeforeNextMilliseconds) / 1000.0f;
    if (delaySeconds > 0.0f) {
        event.preparedLoopSample = std::move(resolved);
        event.loopDelayRemainingSeconds = delaySeconds;
        event.phase = Phase::WaitingForLoop;
        setState(event.handle, AudioEventInstanceState::Waiting);
        return;
    }
    startPortion(event, AudioEventPortion::Main, audio, catalog, std::move(resolved));
}

bool AudioEventSequencer::admit(ActiveEvent& event, const AudioEventDefinition& definition,
                                AudioSystem& audio) {
    // `event` names an element of m_active, and container::HashMap is a dense
    // map: erasing any element moves the last one into the vacated slot.  A
    // just-enqueued event is typically that last element, so every read after
    // a finish() below must go through a fresh lookup instead of the incoming
    // reference.  The handle value is a plain copy and stays valid.
    const uint64_t selfHandle = event.handle.value;
    const ActiveEvent* self = &event;

    if (definition.kind == AudioEventKind::Streaming) {
        if (m_disallowSpeech) return false;
        if (event.intent.uninterruptible) {
            // RefCode's MilesAudioManager::playAudioEvent() calls
            // stopAllSpeech() before starting an uninterruptible stream.
            // Work entirely on values/voice handles; no stream object escapes.
            container::Vector<uint64_t> existingSpeech;
            for (const uint64_t activeHandle : m_activeOrder) {
                const auto active = m_active.find(activeHandle);
                if (active != m_active.end() && active->second.handle != event.handle &&
                    !active->second.stopRequested &&
                    active->second.kind == AudioEventKind::Streaming) {
                    existingSpeech.push_back(activeHandle);
                }
            }
            for (const uint64_t handle : existingSpeech) {
                const auto existing = m_active.find(handle);
                if (existing == m_active.end()) continue;
                if (existing->second.voice) {
                    static_cast<void>(audio.enqueueStop(existing->second.voice));
                }
                finish(existing, AudioEventInstanceState::Stopped);
            }
            if (!existingSpeech.empty()) {
                const auto reacquired = m_active.find(selfHandle);
                if (reacquired == m_active.end()) return false;
                self = &reacquired->second;
            }
        }
    }

    if (hasAudioEventType(definition.typeFlags, AudioEventType::Voice) &&
        hasActiveVoiceForOwner(*self)) {
        return false;
    }

    if (definition.limit <= 0) return true;
    size_t matching = 0;
    for (const uint64_t activeHandle : m_activeOrder) {
        const auto active = m_active.find(activeHandle);
        if (active == m_active.end() || active->second.handle == self->handle ||
            active->second.stopRequested || active->second.eventKey != self->eventKey ||
            active->second.positional != self->positional) {
            continue;
        }
        ++matching;
    }
    if (matching < static_cast<size_t>(definition.limit)) return true;
    if (!hasAudioEventControl(definition.controlFlags, AudioEventControl::Interrupt)) {
        return false;
    }

    // RefCode refuses an interrupt when the limit is occupied entirely by
    // pending requests. Only a real oldest active voice is cannibalized.
    const std::optional<uint64_t> oldest = oldestMatchingActive(*self);
    if (!oldest) return false;
    const auto target = m_active.find(*oldest);
    if (target == m_active.end() || !target->second.voice) return false;
    static_cast<void>(audio.enqueueStop(target->second.voice));
    finish(target, AudioEventInstanceState::Preempted);
    return true;
}

bool AudioEventSequencer::hasActiveVoiceForOwner(const ActiveEvent& candidate) const noexcept {
    if (!candidate.intent.ownerKey ||
        !hasAudioEventType(candidate.typeFlags, AudioEventType::Voice)) {
        return false;
    }
    for (const uint64_t activeHandle : m_activeOrder) {
        const auto active = m_active.find(activeHandle);
        if (active == m_active.end() || active->second.handle == candidate.handle ||
            active->second.stopRequested || !active->second.voice ||
            !active->second.intent.ownerKey ||
            *active->second.intent.ownerKey != *candidate.intent.ownerKey ||
            !hasAudioEventType(active->second.typeFlags, AudioEventType::Voice)) {
            continue;
        }
        return true;
    }
    return false;
}

std::optional<uint64_t> AudioEventSequencer::oldestMatchingActive(
    const ActiveEvent& candidate) const {
    const ActiveEvent* oldest = nullptr;
    for (const uint64_t activeHandle : m_activeOrder) {
        const auto active = m_active.find(activeHandle);
        if (active == m_active.end() || active->second.handle == candidate.handle ||
            active->second.stopRequested || !active->second.voice ||
            active->second.eventKey != candidate.eventKey ||
            active->second.positional != candidate.positional) {
            continue;
        }
        if (!oldest || active->second.arrivalOrdinal < oldest->arrivalOrdinal) {
            oldest = &active->second;
        }
    }
    return oldest ? std::optional<uint64_t>{oldest->handle.value} : std::nullopt;
}

bool AudioEventSequencer::isTerminal(AudioEventInstanceState state) noexcept {
    switch (state) {
    case AudioEventInstanceState::Completed:
    case AudioEventInstanceState::Stopped:
    case AudioEventInstanceState::Preempted:
    case AudioEventInstanceState::Failed:
    case AudioEventInstanceState::Rejected:
    case AudioEventInstanceState::Suppressed:
        return true;
    case AudioEventInstanceState::Unknown:
    case AudioEventInstanceState::Pending:
    case AudioEventInstanceState::Playing:
    case AudioEventInstanceState::Waiting:
        return false;
    }
    return false;
}

container::String AudioEventSequencer::canonicalEventKey(container::StringView name) {
    container::String result;
    result.reserve(name.size());
    for (const unsigned char character : name) {
        if (!std::isspace(character)) {
            result.push_back(static_cast<char>(std::tolower(character)));
        }
    }
    return result;
}

bool AudioEventSequencer::sameTransform(const AudioVoiceTransform& lhs,
                                        const AudioVoiceTransform& rhs) noexcept {
    constexpr float epsilon = 0.0001f;
    return std::abs(lhs.position.x() - rhs.position.x()) <= epsilon &&
        std::abs(lhs.position.y() - rhs.position.y()) <= epsilon &&
        std::abs(lhs.position.z() - rhs.position.z()) <= epsilon &&
        std::abs(lhs.velocity.x() - rhs.velocity.x()) <= epsilon &&
        std::abs(lhs.velocity.y() - rhs.velocity.y()) <= epsilon &&
        std::abs(lhs.velocity.z() - rhs.velocity.z()) <= epsilon;
}

} // namespace engine::audio
