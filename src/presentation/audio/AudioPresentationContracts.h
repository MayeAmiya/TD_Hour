#pragma once

#include "core/container/container_types.h"
#include "core/math/wwmath/base/wwmath.h"
#include "presentation/contracts/PresentationPlayerAudience.h"
#include "presentation/audio/EvaEventContracts.h"

#include <cstdint>
#include <optional>

namespace engine::audio {

// Audio keeps these aliases for its public contract, while FX and any future
// presentation stream use the same neutral value type without depending on
// audio implementation details.
using AudioAudience = presentation::PlayerAudience;
using AudioAudienceRelation = presentation::PlayerAudienceRelation;
inline constexpr uint8_t kInvalidAudioAudiencePlayer =
    presentation::kInvalidPlayerAudience;

// Stable game/presentation intent. It carries no backend voice, device or ECS
// identity and is safe to cross the detached confirmed-frame boundary.
struct AudioEventIntent final {
    container::String eventName;
    uint64_t eventId = 0;
    uint64_t confirmedFrame = 0;
    std::optional<math::vec3> position;
    std::optional<uint64_t> emitterKey;
    std::optional<uint64_t> ownerKey;
    bool uninterruptible = false;
    bool logical = false;
    float volumeScale = 1.0f;
    uint64_t variationSeed = 0;
    bool fallbackToPositionIfEmitterMissing = false;
    AudioAudience audience;
};

enum class AudioNaturalCompletionKind : uint8_t {
    Audio,
    Speech,
    MusicLoop,
};

struct AudioNaturalCompletion final {
    uint64_t sessionEpoch = 0;
    container::String eventName;
    AudioNaturalCompletionKind kind = AudioNaturalCompletionKind::Audio;
    uint64_t eventId = 0;
    uint64_t confirmedFrame = 0;
};

struct AudioEmitterTransform final {
    math::vec3 position{};
    math::vec3 velocity{};
};

struct AudioEmitterSnapshot final {
    uint64_t emitterKey = 0;
    AudioEmitterTransform transform;
    AudioAudience audience;
};

struct AudioPresentationEvent final {
    AudioEventIntent intent;
    std::optional<float> eventVolumeOverride;
    bool ambient = false;
    bool eva = false;
    std::optional<EvaPresentationPolicy> evaPolicy;
};

struct AudioEventInstanceOverrides final {
    std::optional<bool> looping;
    std::optional<int32_t> loopCount;
    std::optional<float> minimumVolume;
    std::optional<float> volume;
    std::optional<float> minimumRange;
    std::optional<float> maximumRange;
    std::optional<uint8_t> priority;
};

enum class AudioPresentationControlKind : uint8_t {
    SetMusicTrack,
    SetMusicVolume,
    SetAmbientPaused,
    SetBackgroundSoundsPaused,
    SetSoundVolume,
    SetSpeechVolume,
    SetEventVolumeOverride,
    RestoreEventVolumeOverride,
    RestoreAllEventVolumeOverrides,
    RemoveEvent,
    RemoveDisabledEvents,
    SetEvaEnabled,
    SetObjectAmbientSoundEnabled,
    SetObjectLoopingSoundEnabled,
};

struct AudioPresentationControlEvent final {
    AudioPresentationControlKind kind =
        AudioPresentationControlKind::SetMusicTrack;
    container::String trackName;
    bool fadeOut = false;
    bool fadeIn = false;
    bool paused = false;
    bool enabled = true;
    bool automaticEnabled = false;
    container::String eventName;
    uint64_t emitterKey = 0;
    uint64_t generation = 0;
    AudioEventInstanceOverrides instanceOverrides;
    float volume = 1.0f;
    uint64_t eventId = 0;
    uint64_t confirmedFrame = 0;
    AudioAudience audience;
};

struct AudioPresentationSnapshot final {
    uint64_t sessionEpoch = 0;
    uint64_t simulationFrame = 0;
    float simulationDeltaSeconds = 0.0f;
    container::Vector<AudioPresentationEvent> events;
    container::Vector<AudioPresentationControlEvent> controls;
    container::Vector<AudioEmitterSnapshot> emitters;
};

} // namespace engine::audio
