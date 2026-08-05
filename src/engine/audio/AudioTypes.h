#pragma once

#include "core/container/container_types.h"

#include "core/math/wwmath/base/wwmath.h"

#include <cstddef>
#include <cstdint>
namespace engine::audio {

// The original 4 MiB cache was sized for late-1990s memory budgets. Keep a
// comfortably sized retained encoded-clip cache on modern machines; a mod can
// still request a larger budget through AudioFootprintInBytes.
inline constexpr size_t kDefaultClipCacheBudgetBytes = 64u * 1024u * 1024u;
inline constexpr size_t kDefaultStreamBufferFrames = 48'000;
inline constexpr size_t kDefaultStreamStartWatermarkFrames = 12'000;
inline constexpr size_t kDefaultStreamBufferBudgetBytes = 4u * 1024u * 1024u;

// Stable value-boundary limits shared by authored event resolution and the
// backend sanitizer. The authored pitch range is intentionally wider: legacy
// data may request it, while the backend clamps to the range miniaudio can
// reproduce reliably.
inline constexpr float kMinimumAuthoredPitch = 0.01f;
inline constexpr float kMinimumBackendPitch = 0.125f;
inline constexpr float kMaximumAudioPitch = 8.0f;
inline constexpr float kDefaultAudioMinimumDistance = 20.0f;
inline constexpr float kDefaultAudioMaximumDistance = 1000.0f;
inline constexpr uint32_t kOfflineMixChannelCount = 2;
inline constexpr uint32_t kOfflineMixSampleRate = 48'000;

// These four buses preserve the observable categories exposed by Generals'
// audio settings without importing its Miles device/provider abstraction.
enum class AudioBus : uint8_t {
    Music,
    Sound,
    Sound3D,
    // Ambient is intentionally distinct from ordinary world Sound3D so a
    // script can pause/resume environmental loops without muting weapons or
    // UI feedback. It is still a normal device bus, not an ECS component.
    Ambient,
    Speech,
    Count,
};

enum class AudioPriority : uint8_t {
    Lowest,
    Low,
    Normal,
    High,
    Critical,
};

enum class AudioSpatialMode : uint8_t {
    TwoDimensional,
    ThreeDimensional,
};

struct AudioVoiceHandle final {
    uint64_t value = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }
    [[nodiscard]] constexpr bool operator==(const AudioVoiceHandle&) const noexcept = default;
};

// A presentation-visible lifecycle result. It is intentionally not gameplay
// authority: audio can be suppressed on a headless client or end at a device
// dependent time, neither of which may affect simulation.
enum class AudioVoiceState : uint8_t {
    Unknown,
    Pending,
    Playing,
    Completed,
    Stopped,
    Preempted,
    Failed,
    Suppressed,
};

// Detached update for a persistent 3D emitter. Future ECS/render extraction
// publishes this value each presentation frame; AudioSystem never stores an
// Object, Drawable, Entity registry pointer or renderer resource.
struct AudioVoiceTransform final {
    math::vec3 position{};
    math::vec3 velocity{};
};

// A logic/client request contains only stable value data. It deliberately has
// no Object, Drawable, renderer, VFS-file or miniaudio handle. The platform
// audio service owns decode memory and active voice lifetime after consuming
// this command.
struct AudioPlayRequest final {
    container::String assetPath;
    AudioBus bus = AudioBus::Sound;
    AudioPriority priority = AudioPriority::Normal;
    AudioSpatialMode spatialMode = AudioSpatialMode::TwoDimensional;
    math::vec3 position{};
    // Linear gain. Keep the public request capable of representing original
    // INI events above 100% (for example `Volume = 120`), rather than
    // silently clamping those authored values to unity at the backend edge.
    float volume = 1.0f;
    // AudioManager performs its first MinSampleVolume rejection before
    // VolumeShift is applied. Keep that authored/override gain separately so
    // the generic device service can reproduce the admission rule without an
    // AudioEventDefinition pointer.
    float admissionVolume = 1.0f;
    float minDistance = kDefaultAudioMinimumDistance;
    float maxDistance = kDefaultAudioMaximumDistance;
    float rolloff = 1.0f;
    // RefCode's AudioEventRTS pitch shift.  The event/INI parser is a later
    // phase, but preserve the runtime capability at the stable value API now.
    float pitch = 1.0f;
    // Global and critical 3D events are allowed to survive the legacy
    // MinSampleVolume distance cull. The flag is authored-event metadata
    // flattened at the catalog boundary; AudioSystem never needs to know an
    // AudioEvent name or INI definition.
    bool bypassSpatialVolumeCull = false;
    bool loop = false;
};

// This is the listener state consumed by the audio device.  It is separate
// from RenderCameraSnapshot on purpose: both are value projections of the
// same game camera, but neither subsystem depends on the other's runtime.
struct AudioListenerSnapshot final {
    math::vec3 position{};
    math::vec3 forward{0.0f, 1.0f, 0.0f};
    math::vec3 up{0.0f, 0.0f, 1.0f};
    // RefCode attenuates 3D sounds as the tactical camera zooms out. This is
    // a per-listener multiplier and remains independent from user bus volume.
    float zoomVolume = 1.0f;
};

// Modernized, explicit form of the listener constants held in original
// AudioSettings. Audio.ini support can populate these later without changing
// the camera-to-listener or audio-device boundary.
struct AudioListenerSettings final {
    // Defaults recovered from Generals AudioSettings.ini. They are explicit
    // values rather than hidden backend constants so later Audio.ini parsing
    // can replace them without changing listener behavior.
    float desiredHeightAbovePivot = 50.0f;
    float maxCameraFraction = 0.333f;
    float zoomMinDistance = 130.0f;
    float zoomMaxDistance = 425.0f;
    float zoomVolumeBoost = 0.20f;
};

struct AudioBusState final {
    bool enabled = true;
    // Pausing is distinct from muting: the playback cursor must stop and
    // later resume from the same sample.  Script volume/enable policy and
    // local GameLogic pause therefore remain independently composable.
    bool paused = false;
    float volume = 1.0f;
};

struct AudioSystemConfig final {
    // Audio output is a presentation service. An unavailable device must not
    // make headless sessions, probes or gameplay startup fail.
    bool enablePlaybackDevice = true;
    // Builds miniaudio's graph without an OS device. It is intentionally
    // separate from headless suppression: tools can pull deterministic PCM
    // through AudioSystem::renderOfflineFrames(), while a dedicated server
    // still consumes requests as Suppressed without decoding/mixing them.
    bool enableOfflineMix = false;
    // Original AudioSettings defaults to 4 2D + 25 3D + 3 stream slots.
    // Phase A has one modern voice pool, so retain that 32-voice baseline.
    size_t maxActiveVoices = 32;
    size_t maxPendingCommands = 256;
    // Zero means use the shared pool behavior for that class. When populated
    // from AudioSettings these restore the original independent 2D/3D/stream
    // reservations (4 / 25 / 3) without reintroducing Miles sample objects.
    size_t maxTwoDimensionalVoices = 0;
    size_t maxThreeDimensionalVoices = 0;
    size_t maxStreamingVoices = 0;
    // Retained encoded-VFS clip budget. Active decoders keep their own shared
    // ownership until playback finishes.
    size_t clipCacheBudgetBytes = kDefaultClipCacheBudgetBytes;
    // Music and speech are decoded by resource workers into bounded float PCM
    // rings. These limits prevent long tracks or many queued voices from
    // turning streaming into an unbounded decoded-audio cache.
    size_t streamBufferFrames = kDefaultStreamBufferFrames;
    size_t streamStartWatermarkFrames = kDefaultStreamStartWatermarkFrames;
    size_t streamBufferBudgetBytes = kDefaultStreamBufferBudgetBytes;
    // These AudioSettings values are read while voices are running in ZH.
    // Unlike sample-pool and cache sizes, a map.ini/solo.ini override must be
    // applicable without recreating the output device.
    float minSampleVolume = 0.02f;
    bool use3DRangeVolumeFade = true;
    float rangeVolumeFadeExponent = 4.0f;
};

struct AudioRuntimeEventPolicy final {
    float minSampleVolume = 0.02f;
    bool use3DRangeVolumeFade = true;
    float rangeVolumeFadeExponent = 4.0f;
};

struct AudioSystemStats final {
    uint64_t acceptedCommands = 0;
    uint64_t droppedCommands = 0;
    uint64_t processedCommands = 0;
    uint64_t failedPlayRequests = 0;
    uint64_t completedVoices = 0;
    uint64_t preemptedVoices = 0;
    size_t pendingCommands = 0;
    size_t activeVoices = 0;
    size_t cachedClips = 0;
    size_t cachedClipBytes = 0;
    bool playbackDeviceAvailable = false;
    bool offlineMixerAvailable = false;
};

[[nodiscard]] constexpr size_t audioBusIndex(AudioBus bus) noexcept {
    return static_cast<size_t>(bus);
}

} // namespace engine::audio
