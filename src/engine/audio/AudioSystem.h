#pragma once

#include "core/container/container_types.h"

#include "AudioTypes.h"
#include <optional>
namespace engine::audio {

// Platform-owned audio service. Logic/client code publishes pure listener
// values and commands; miniaudio device objects, decoded VFS bytes, mixers and
// active voice lifetime stay entirely behind this boundary.
class AudioSystem final {
public:
    AudioSystem();
    ~AudioSystem();

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    // Returns false only for an invalid repeated setup. A missing/default
    // output device is a supported no-op presentation mode, not a startup
    // failure for gameplay, dedicated servers or probes.
    [[nodiscard]] bool init(const AudioSystemConfig& config = {});
    void reset() noexcept;
    void shutdown() noexcept;

    [[nodiscard]] bool isInitialized() const noexcept;
    [[nodiscard]] bool isPlaybackDeviceAvailable() const noexcept;
    [[nodiscard]] bool isOfflineMixerAvailable() const noexcept;

    // Available only when AudioSystemConfig::enableOfflineMix was selected.
    // Pulling frames advances miniaudio deterministically without opening a
    // WASAPI device, which makes EOS/sequence regression tests reliable.
    [[nodiscard]] bool renderOfflineFrames(uint64_t frameCount) noexcept;

    // Decodes only from bytes supplied by VFS. This is useful for explicit
    // preload and makes the source-format failure visible before an event is
    // submitted; it never opens a host filesystem path directly.
    [[nodiscard]] bool preload(container::StringView assetPath, container::String* error = nullptr);

    // Commands are bounded and thread-safe between a logic/client producer
    // and the presentation-thread update consumer. An invalid return value
    // means the queue was full or the request was malformed.
    [[nodiscard]] AudioVoiceHandle enqueuePlay(AudioPlayRequest request);
    [[nodiscard]] bool enqueueStop(AudioVoiceHandle handle);
    [[nodiscard]] bool enqueueStopAll(std::optional<AudioBus> bus = std::nullopt);
    // Event-level script overrides must also affect an already-playing
    // AudioEvent.  Keep this as a value command on the presentation queue;
    // callers never receive or mutate a miniaudio sound directly.
    [[nodiscard]] bool enqueueSetVolume(AudioVoiceHandle handle, float volume);
    [[nodiscard]] bool enqueueUpdateTransform(AudioVoiceHandle handle,
                                               AudioVoiceTransform transform);

    // Thread-safe status observation for a submitted handle. Terminal states
    // are retained in a small bounded history so diagnostics/event sequencers
    // can distinguish actual decode failure from headless suppression.
    [[nodiscard]] AudioVoiceState voiceState(AudioVoiceHandle handle) const noexcept;

    // The methods below belong to the serial audio owner currently driven by
    // the Logic client loop. Only enqueuePlay/Stop are cross-thread command
    // producers; listener/bus/cache mutation remains serial with miniaudio
    // voice lifetime and the device recovery boundary.
    // Applied immediately to the device listener when playback is available;
    // retaining the latest pure value makes device recovery deterministic.
    void publishListener(AudioListenerSnapshot listener) noexcept;
    [[nodiscard]] AudioListenerSnapshot listener() const noexcept;

    void setBusVolume(AudioBus bus, float volume) noexcept;
    void setBusEnabled(AudioBus bus, bool enabled) noexcept;
    // Owner-thread playback pause. Unlike setBusEnabled(), this stops the
    // category clock and resumes existing voices from their prior cursor.
    void setBusPaused(AudioBus bus, bool paused) noexcept;
    [[nodiscard]] AudioBusState busState(AudioBus bus) const noexcept;
    // Serial presentation-owner update for the subset of AudioSettings that
    // ZH consults after device/sample-pool initialization. It does not rebuild
    // WASAPI/miniaudio and is therefore safe at a GameSession epoch switch.
    void setRuntimeEventPolicy(AudioRuntimeEventPolicy policy) noexcept;

    // Call once per client/presentation frame after listener publication.
    // It consumes events, starts/preempts voices, refreshes listener-dependent
    // gain and retires completed one-shots without a game-object callback.
    void update();

    [[nodiscard]] AudioSystemStats stats() const noexcept;

private:
    struct Impl;
    container::UniquePtr<Impl> m_impl;
};

} // namespace engine::audio
