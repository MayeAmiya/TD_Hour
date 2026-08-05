#pragma once

#include "AudioTypes.h"
#include "presentation/camera/GameCameraState.h"

namespace engine::audio {

// Converts the client/logic-owned RTS camera into the listener convention
// used by spatial audio. The operation is pure and deterministic; it can be
// tested without an audio device, terrain renderer, ECS object or VFS mount.
class AudioListenerBuilder final {
public:
    // miniaudio's spatializer is OpenGL-style (X right, Y up, -Z forward),
    // while Generals/W3D world data is X right, Z up, +Y forward. Keep this
    // conversion at the audio boundary so neither game values nor rendering
    // conventions are mutated for a backend detail.
    [[nodiscard]] static math::vec3 toAudioSpace(math::vec3 worldValue) noexcept;

    [[nodiscard]] static AudioListenerSnapshot fromCamera(
        const GameCameraState& camera,
        const AudioListenerSettings& settings = {}) noexcept;

    // A renderer-local camera slave has no GameCameraDirector backing it. Its
    // fully sealed pose still supplies a legitimate presentation listener:
    // use the bone position directly (matching W3DView's setPosition2D
    // listener update) and preserve its valid orientation without mutating
    // any logic-camera or simulation state.
    [[nodiscard]] static AudioListenerSnapshot fromPresentationCamera(
        math::vec3 position, math::vec3 target, math::vec3 up,
        const AudioListenerSettings& settings = {}) noexcept;
};

} // namespace engine::audio
