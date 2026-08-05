#pragma once

#include "core/container/hash_containers.h"

#include "AudioTypes.h"
#include "presentation/audio/AudioPresentationContracts.h"

#include <cstddef>
#include <cstdint>
#include <optional>
namespace engine::audio {

// These are the authored categories from AudioEventInfo. They describe game
// semantics, not a miniaudio implementation detail; the resolver translates
// them to the modern buses/spatial request at the presentation boundary.
enum class AudioEventKind : uint8_t {
    SoundEffect,
    Music,
    Streaming,
};

enum class AudioEventType : uint16_t {
    Ui       = 1u << 0u,
    World    = 1u << 1u,
    Shrouded = 1u << 2u,
    Global   = 1u << 3u,
    Voice    = 1u << 4u,
    Player   = 1u << 5u,
    Allies   = 1u << 6u,
    Enemies  = 1u << 7u,
    Everyone = 1u << 8u,
};

enum class AudioEventControl : uint16_t {
    Loop      = 1u << 0u,
    Random    = 1u << 1u,
    All       = 1u << 2u,
    PostDelay = 1u << 3u,
    Interrupt = 1u << 4u,
};

[[nodiscard]] constexpr uint16_t audioEventFlag(AudioEventType value) noexcept {
    return static_cast<uint16_t>(value);
}

[[nodiscard]] constexpr uint16_t audioEventFlag(AudioEventControl value) noexcept {
    return static_cast<uint16_t>(value);
}

[[nodiscard]] constexpr bool hasAudioEventType(uint16_t flags, AudioEventType value) noexcept {
    return (flags & audioEventFlag(value)) != 0;
}

[[nodiscard]] constexpr bool hasAudioEventControl(uint16_t flags,
                                                  AudioEventControl value) noexcept {
    return (flags & audioEventFlag(value)) != 0;
}

// Data-only modernization of original AudioSettings.ini. Device/provider
// choices intentionally stay out of it; only values that affect authored
// event resolution, listener construction or modern voice limits are kept.
struct AudioEventSettings final {
    container::String audioRoot = "data/audio";
    container::String soundsFolder = "sounds";
    container::String musicFolder = "tracks";
    container::String streamingFolder = "speech";
    container::String soundsExtension = "wav";

    size_t sampleCount2D = 4;
    size_t sampleCount3D = 25;
    size_t streamCount = 3;
    // AudioSettings.ini can raise this. AudioSubsystem clamps old authored
    // 4 MiB defaults to the modern shared baseline.
    size_t cacheBudgetBytes = kDefaultClipCacheBudgetBytes;
    float globalMinDistance = 5000.0f;
    float globalMaxDistance = 500000.0f;
    float minSampleVolume = 0.02f;
    bool use3DRangeVolumeFade = true;
    float rangeVolumeFadeExponent = 4.0f;

    float defaultSoundVolume = 0.80f;
    float defaultSound3DVolume = 0.80f;
    float defaultSpeechVolume = 0.70f;
    float defaultMusicVolume = 0.55f;
    // RefCode's GadgetPushButton does not name the default UI click itself:
    // it resolves MiscAudio::m_guiClickSound.  Keep that authored indirection
    // in the immutable catalog so mods can replace it without teaching WND
    // code about an AudioEvent spelling.
    container::String guiClickSound = "GUIClick";
    AudioListenerSettings listener{};
};

// Equivalent in responsibility to RefCode's AudioEventInfo, expressed as
// ordinary immutable-value friendly C++ data. Unimplemented legacy flags are
// retained rather than discarded so later EventPlayer work does not need an
// INI/API migration.
struct AudioEventDefinition final {
    container::String name;
    container::String filename;
    AudioEventKind kind = AudioEventKind::SoundEffect;
    AudioPriority priority = AudioPriority::High;
    uint16_t typeFlags = audioEventFlag(AudioEventType::Ui) |
        audioEventFlag(AudioEventType::Player);
    uint16_t controlFlags = audioEventFlag(AudioEventControl::Random);

    float volume = 1.0f;
    float minVolume = 0.40f;
    // RefCode's VolumeShift means a random gain in [1 + shift, 1].
    float volumeShift = 0.0f;
    float pitchShiftMin = 1.0f;
    float pitchShiftMax = 1.0f;
    uint32_t delayMinMilliseconds = 0;
    uint32_t delayMaxMilliseconds = 0;
    int32_t limit = 4;
    int32_t loopCount = 1;
    float minDistance = 175.0f;
    float maxDistance = 600.0f;
    float lowPassCutoff = 0.0f;

    container::Vector<container::String> sounds;
    container::Vector<container::String> soundsMorning;
    container::Vector<container::String> soundsEvening;
    container::Vector<container::String> soundsNight;
    container::Vector<container::String> attackSounds;
    container::Vector<container::String> decaySounds;
};

// Original AudioEventRTS keeps selection state on each event instance. A
// caller that needs exact per-emitter sequential/random-no-repeat behaviour
// owns one of these value cursors alongside its own event state. It never
// exposes a renderer, ECS or miniaudio pointer.
struct AudioEventPlaybackCursor final {
    int32_t mainSampleIndex = -1;
    int32_t attackSampleIndex = -1;
    int32_t decaySampleIndex = -1;
    uint64_t selectionOrdinal = 0;
    bool variationInitialized = false;
    float resolvedVolumeShift = 1.0f;
    float resolvedPitch = 1.0f;
};

enum class AudioEventPortion : uint8_t {
    Attack,
    Main,
    Decay,
};

struct ResolvedAudioEventSample final {
    AudioPlayRequest request;
    AudioEventPortion portion = AudioEventPortion::Main;
    uint32_t delayBeforeNextMilliseconds = 0;
};

// VFS-backed registry for the original AudioSettings / AudioEventInfo data
// surface. This is deliberately a pure resolver: device lifetime and active
// event sequencing remain in the presentation service, while gameplay can
// produce AudioEventIntent values without importing a platform handle.
class AudioEventCatalog final {
public:
    [[nodiscard]] bool loadFromVfs(container::String* error = nullptr);
    // Applies one CreateOverrides content layer without clearing the base
    // catalog. The operation is transactional: a malformed layer leaves this
    // catalog unchanged, which lets a GameSession degrade map-local audio
    // without affecting gameplay or a later session.
    [[nodiscard]] bool applyOverrides(
        container::StringView content,
        container::StringView sourcePath = {},
        container::String* error = nullptr);
    [[nodiscard]] bool applyOverridesFromVfs(
        container::StringView path,
        container::String* error = nullptr);
    void clear();

    [[nodiscard]] const AudioEventSettings& settings() const noexcept { return m_settings; }
    [[nodiscard]] const AudioEventDefinition* find(container::StringView eventName) const;
    [[nodiscard]] size_t eventCount() const noexcept { return m_events.size(); }

    // Resolves one concrete asset request. It purposely does not turn LOOP,
    // Attack/Decay or delay into backend looping: a later event sequencer can
    // advance those original portions after actual voice EOS without making
    // the low-level AudioSystem aware of game event names.
    [[nodiscard]] std::optional<ResolvedAudioEventSample> resolve(
        const AudioEventIntent& intent,
        AudioEventPlaybackCursor& cursor,
        AudioEventPortion portion = AudioEventPortion::Main,
        container::String* error = nullptr) const;

private:
    AudioEventSettings m_settings;
    container::HashMap<container::String, AudioEventDefinition> m_events;
};

} // namespace engine::audio
