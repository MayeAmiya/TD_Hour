#pragma once

#include "core/container/container_types.h"

#include "core/ecs/ObjectId.h"
#include "core/math/wwmath/base/wwmath.h"
#include "game/player/PlayerTypes.h"
#include "presentation/audio/EvaEventContracts.h"

#include <cstdint>
#include <optional>
namespace game {

// Confirmed gameplay output, intentionally expressed without a renderer,
// miniaudio handle, AudioEventInfo pointer, or ECS entity. Systems such as a
// future WeaponUpdate emit this at the same point they create the visual/FX
// consequence; GameSessionMediaPresentationPort later turns it into a
// presentation value.
struct GameAudioEvent final {
    container::String eventName;
    std::optional<engine::ObjectId> emitter;
    std::optional<engine::ObjectId> owner;
    // Frozen at the confirmed publication edge.  Object-attached producers
    // leave this empty and the publication port derives it from OwnerComponent
    // before lifecycle retirement can make that lookup impossible.  Scripts
    // and observer-local UI/EVA requests supply the addressed player directly.
    std::optional<engine::PlayerId> sourcePlayer;
    std::optional<math::vec3> position;
    float volumeScale = 1.0f;
    bool uninterruptible = false;
    // RefCode ScriptActions marks synchronized script audio logical.  This
    // preserves the deterministic/script bookkeeping identity, but it does
    // not bypass AudioManager's eventual local Player/Allies/Enemies gate.
    bool logical = false;
    // Ambient sources are explicitly marked by their producer. This keeps
    // SOUND_AMBIENT_PAUSE/RESUME from guessing based on asset names or world
    // position, while still letting audio remain independent of ECS handles.
    bool ambient = false;
    // EVA is a separate presentation category. Keeping it explicit lets
    // EVA_SET_ENABLED_DISABLED suppress only advisor playback rather than
    // globally silencing ordinary speech streams.
    bool eva = false;
    // Present only for an Eva.ini-scheduled advisor request. The detached
    // audio owner performs priority/cooldown/expiration arbitration and may
    // therefore retain this request after the producing logic frame.
    std::optional<engine::audio::EvaPresentationPolicy> evaPolicy;

    // Filled by GameSession when the event is committed. A producer can leave
    // both as zero; the session derives a deterministic value from confirmed
    // tick, event ordinal, map/session seed and emitter key.
    uint64_t eventId = 0;
    uint64_t confirmedFrame = 0;
    uint64_t variationSeed = 0;
};

// Global audio controls are separate from one-shot GameAudioEvent values:
// they replace a persistent presentation policy (music track/volume or
// ambient pause) and must never be interpreted as a spatial object sound.
// Script bridge code stamps these at the confirmed-session boundary; the
// audio extraction/subsystem owns all device-specific lifetime and fades.
enum class GameAudioControlKind : uint8_t {
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
    // A script can start/stop an ambient event attached to one live object.
    // The payload stays value-only: ObjectId is translated to a presentation
    // emitter key by extraction, and the audio subsystem owns every handle.
    SetObjectAmbientSoundEnabled,
    // Gameplay-owned loop distinct from Drawable/script ambient audio. The
    // presentation key is (ObjectId,eventName), so a burning loop cannot
    // replace the object's normal damage-state ambient event.
    SetObjectLoopingSoundEnabled,
};

// Per-instance copy of the map Object Panel's DynamicAudioEventInfo fields.
// It is presentation data only and never mutates the shared AudioEvent
// catalog or another object using the same authored event name.
struct GameAudioInstanceOverrides final {
    std::optional<bool> looping;
    std::optional<int32_t> loopCount;
    std::optional<float> minimumVolume;
    std::optional<float> volume;
    std::optional<float> minimumRange;
    std::optional<float> maximumRange;
    std::optional<uint8_t> priority;
};

struct GameAudioControlEvent final {
    GameAudioControlKind kind = GameAudioControlKind::SetMusicTrack;
    container::String trackName;
    bool fadeOut = false;
    bool fadeIn = false;
    bool paused = false;
    bool enabled = true;
    bool automaticEnabled = false;
    // SetEventVolumeOverride/RestoreEventVolumeOverride/RemoveEvent use the
    // original AudioEvent name. It is intentionally not an asset path or an
    // AudioEvent pointer: session output stays device-independent.
    container::String eventName;
    // SetObjectAmbientSoundEnabled and gameplay loops identify their owning
    // gameplay object here. Presentation-only moving emitters may override
    // the transform key below without leaking an audio handle into ECS.
    std::optional<engine::ObjectId> object;
    // Object loop/ambient controls require the same authored audience gate as
    // a one-shot.  This remains optional for global music/volume controls.
    std::optional<engine::PlayerId> sourcePlayer;
    uint64_t emitterKeyOverride = 0;
    // Monotonic per-object generation for Drawable-style ambient ownership.
    // Zero means the control family does not use generation ordering.
    uint64_t generation = 0;
    GameAudioInstanceOverrides instanceOverrides;
    float volume = 1.0f;
    uint64_t eventId = 0;
    uint64_t confirmedFrame = 0;
};

} // namespace game
