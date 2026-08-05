#include "AudioListener.h"

#include <algorithm>
#include <cmath>

namespace engine::audio {
namespace {

const math::vec3 kWorldUp{0.0f, 0.0f, 1.0f};
const math::vec3 kFallbackForward{0.0f, 1.0f, 0.0f};

bool finite(float value) noexcept {
    return std::isfinite(value);
}

bool usable(const math::vec3& value) noexcept {
    const float lengthSq = value.length_sq();
    return finite(value.x()) && finite(value.y()) && finite(value.z()) &&
        finite(lengthSq) && lengthSq > math::EPSILON * math::EPSILON;
}

float finiteOr(float value, float fallback) noexcept {
    return finite(value) ? value : fallback;
}

} // namespace

math::vec3 AudioListenerBuilder::toAudioSpace(math::vec3 worldValue) noexcept {
    // W3D (x, y, z) -> miniaudio/OpenGL (x, z, -y).
    return {worldValue.x(), worldValue.z(), -worldValue.y()};
}

AudioListenerSnapshot AudioListenerBuilder::fromCamera(
    const GameCameraState& camera,
    const AudioListenerSettings& rawSettings) noexcept {
    const GameCameraState state = camera.sanitized();
    AudioListenerSnapshot result;

    const float desiredHeight = std::max(finiteOr(rawSettings.desiredHeightAbovePivot, 50.0f),
                                         0.0f);
    const float maximumFraction = std::clamp(
        finiteOr(rawSettings.maxCameraFraction, 0.333f), 0.0f, 1.0f);
    const float zoomMinDistance = std::max(
        finiteOr(rawSettings.zoomMinDistance, 130.0f), 0.0f);
    const float zoomMaxDistance = std::max(
        finiteOr(rawSettings.zoomMaxDistance, 425.0f), zoomMinDistance + 0.0001f);
    const float zoomBoost = std::clamp(
        finiteOr(rawSettings.zoomVolumeBoost, 0.20f), 0.0f, 1.0f);

    // RefCode places the microphone between the tactical ground pivot and
    // physical camera: stop at the desired height when possible, but never
    // move farther than the configured camera fraction. Target is the modern
    // detached equivalent of that pivot; terrain ownership remains outside
    // the audio backend.
    const math::vec3 pivot = state.target;
    const math::vec3 cameraDelta = state.position - pivot;
    float microphoneFraction = maximumFraction;
    if (cameraDelta.z() > math::EPSILON && state.position.z() > pivot.z() + desiredHeight) {
        microphoneFraction = std::min(maximumFraction, desiredHeight / cameraDelta.z());
    }
    microphoneFraction = std::clamp(microphoneFraction, 0.0f, 1.0f);
    result.position = pivot + cameraDelta * microphoneFraction;

    // Generals' listener orientation follows tactical yaw, not camera pitch.
    // Preserve that for RTS panning while keeping the project-wide Z-up axis.
    math::vec3 forward = state.target - state.position;
    forward[2] = 0.0f;
    result.forward = usable(forward) ? forward.normalized() : kFallbackForward;
    result.up = kWorldUp;

    const float cameraDistance = (state.position - result.position).length();
    result.zoomVolume = 1.0f - zoomBoost;
    if (finite(cameraDistance) && zoomBoost > 0.0f) {
        if (cameraDistance < zoomMinDistance) {
            result.zoomVolume = 1.0f;
        } else if (cameraDistance < zoomMaxDistance) {
            const float t = (cameraDistance - zoomMinDistance) /
                (zoomMaxDistance - zoomMinDistance);
            result.zoomVolume = 1.0f - t * zoomBoost;
        }
    }
    result.zoomVolume = std::clamp(result.zoomVolume, 0.0f, 1.0f);
    return result;
}

AudioListenerSnapshot AudioListenerBuilder::fromPresentationCamera(
    math::vec3 position, math::vec3 target, math::vec3 up,
    const AudioListenerSettings& settings) noexcept {
    // Reuse GameCameraState's finite/direction sanitization as a pure value
    // guard, without installing this pose in GameCameraDirector. The regular
    // builder still supplies the established zoom-volume policy, while slave
    // playback uses the actual camera-bone position rather than its RTS
    // ground-pivot microphone interpolation.
    GameCameraState presentation;
    presentation.position = position;
    presentation.target = target;
    presentation.up = up;
    const GameCameraState state = presentation.sanitized();
    AudioListenerSnapshot result = fromCamera(state, settings);
    result.position = state.position;

    const math::vec3 forward = state.target - state.position;
    const float forwardLength = forward.length();
    if (usable(forward) && finite(forwardLength) && forwardLength > math::EPSILON) {
        result.forward = forward / forwardLength;
    }
    const float upLength = state.up.length();
    if (usable(state.up) && finite(upLength) && upLength > math::EPSILON) {
        result.up = state.up / upLength;
    }
    return result;
}

} // namespace engine::audio
