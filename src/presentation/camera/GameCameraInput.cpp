#include "presentation/camera/GameCameraInput.h"

#include <algorithm>
#include <cmath>

namespace engine
{
namespace
{

const math::vec3 kWorldUp{0.0f, 0.0f, 1.0f};
constexpr float kMaximumPendingPixels = 16384.0f;
constexpr float kMaximumPendingWheelUnits = 32.0f;
constexpr float kMaximumPendingOrbitRadians = 3.14159265358979323846f;
constexpr float kZoomFactorPerWheelUnit = 0.85f;
constexpr float kConfiguredScrollUnitsPerSecond = 200.0f;
constexpr float kDragPanDistanceScale = 0.0015f;
constexpr float kOrbitRadiansPerPixel = 0.008f;
constexpr float kMinOrbitPitchRadians = math::deg_to_rad(5.0f);
constexpr float kMaxOrbitPitchRadians = math::deg_to_rad(85.0f);

bool finite(float value) noexcept
{
    return std::isfinite(value);
}

bool usable(const math::vec3& value) noexcept
{
    const float lengthSq = value.length_sq();
    return finite(value.x()) && finite(value.y()) && finite(value.z()) && finite(lengthSq) &&
           lengthSq > math::EPSILON * math::EPSILON;
}

float clampAccumulated(float current, float addition, float limit) noexcept
{
    if (!finite(current))
        current = 0.0f;
    if (!finite(addition))
        return current;
    return std::clamp(current + addition, -limit, limit);
}

math::vec3 groundForward(const GameCameraState& camera) noexcept
{
    math::vec3 result = camera.target - camera.position;
    result[2] = 0.0f;
    if (!usable(result))
        return {0.0f, 1.0f, 0.0f};
    return result.normalized();
}

float cameraDistance(const GameCameraState& camera) noexcept
{
    const float distance = (camera.position - camera.target).length();
    return finite(distance) && distance > math::EPSILON ? distance : 1.0f;
}

void panWorld(GameCameraState& camera, math::vec3 direction) noexcept
{
    if (!usable(direction))
        return;
    camera.position += direction;
    camera.target += direction;
}

void zoom(GameCameraState& camera, float wheelUnits) noexcept
{
    if (!finite(wheelUnits) || std::abs(wheelUnits) <= math::EPSILON)
        return;
    math::vec3 radial = camera.position - camera.target;
    if (!usable(radial))
        radial = {0.0f, -1.0f, 1.0f};
    const float oldDistance = cameraDistance(camera);
    const float newDistance =
        oldDistance * std::pow(kZoomFactorPerWheelUnit, wheelUnits);
    // There is no authored gameplay zoom range. Only reject values which
    // cannot form a finite, non-degenerate view transform.
    if (!finite(newDistance) || newDistance <= math::EPSILON)
        return;
    camera.position = camera.target + radial.normalized() * newDistance;
    camera.farClip = std::max(camera.farClip, newDistance * 2.0f);
}

void panByPixels(GameCameraState& camera, float xPixels, float yPixels) noexcept
{
    xPixels = finite(xPixels) ? xPixels : 0.0f;
    yPixels = finite(yPixels) ? yPixels : 0.0f;
    if (std::abs(xPixels) <= math::EPSILON && std::abs(yPixels) <= math::EPSILON)
    {
        return;
    }
    const math::vec3 forward = groundForward(camera);
    const math::vec3 right = forward.cross(kWorldUp).normalized();
    const float scale = std::clamp(cameraDistance(camera) * kDragPanDistanceScale, 0.05f, 32.0f);
    // Grab-to-pan: moving the pointer right makes the terrain follow it, so
    // the camera/target translate left in world coordinates.
    panWorld(camera, (right * -xPixels + forward * yPixels) * scale);
}

void orbitByPixels(GameCameraState& camera, float xPixels, float yPixels) noexcept
{
    xPixels = finite(xPixels) ? xPixels : 0.0f;
    yPixels = finite(yPixels) ? yPixels : 0.0f;
    if (std::abs(xPixels) <= math::EPSILON && std::abs(yPixels) <= math::EPSILON)
    {
        return;
    }
    math::vec3 radial = camera.position - camera.target;
    if (!usable(radial))
        radial = {0.0f, -1.0f, 1.0f};
    const float distance = cameraDistance(camera);
    radial = radial.normalized() * distance;

    float yaw = std::atan2(radial.y(), radial.x());
    const float horizontalDistance = std::sqrt(radial.x() * radial.x() + radial.y() * radial.y());
    float pitch = std::atan2(radial.z(), horizontalDistance);
    yaw -= xPixels * kOrbitRadiansPerPixel;
    pitch = std::clamp(pitch + yPixels * kOrbitRadiansPerPixel, kMinOrbitPitchRadians, kMaxOrbitPitchRadians);

    const float horizontal = distance * std::cos(pitch);
    camera.position =
        camera.target + math::vec3{horizontal * std::cos(yaw), horizontal * std::sin(yaw), distance * std::sin(pitch)};
    camera.up = kWorldUp;
}

} // namespace

bool GameCameraInput::hasManualInput() const noexcept
{
    const auto nonZeroFinite = [](float value) noexcept { return finite(value) && std::abs(value) > math::EPSILON; };
    return manualIntent || resetToHome || nonZeroFinite(panForwardAxis) || nonZeroFinite(panRightAxis) ||
           nonZeroFinite(screenEdgeForwardAxis) || nonZeroFinite(screenEdgeRightAxis) ||
           nonZeroFinite(zoomWheelUnits) || nonZeroFinite(anchorScrollPixelsX) || nonZeroFinite(anchorScrollPixelsY) ||
           nonZeroFinite(panPixelsX) || nonZeroFinite(panPixelsY) || nonZeroFinite(orbitPixelsX) ||
           nonZeroFinite(orbitPixelsY) || nonZeroFinite(orbitPitchStepRadians) ||
           nonZeroFinite(orbitPitchAxis) || nonZeroFinite(orbitYawAxis) ||
           nonZeroFinite(zoomAxis) ||
           hasAbsoluteTarget || hasAbsoluteState;
}

void GameCameraInput::clear() noexcept
{
    *this = {};
}

void GameCameraInput::accumulate(const GameCameraInput& input) noexcept
{
    // Axes describe current held state, not a displacement. Always replace
    // them with the newest sample so a network/lockstep wait cannot retain a
    // released key and cannot accumulate camera movement while no game tick
    // is being confirmed.
    panForwardAxis = finite(input.panForwardAxis) ? std::clamp(input.panForwardAxis, -1.0f, 1.0f) : 0.0f;
    panRightAxis = finite(input.panRightAxis) ? std::clamp(input.panRightAxis, -1.0f, 1.0f) : 0.0f;
    screenEdgeForwardAxis =
        finite(input.screenEdgeForwardAxis) ? std::clamp(input.screenEdgeForwardAxis, -1.0f, 1.0f) : 0.0f;
    screenEdgeRightAxis = finite(input.screenEdgeRightAxis) ? std::clamp(input.screenEdgeRightAxis, -1.0f, 1.0f) : 0.0f;
    anchorScrollPixelsX = finite(input.anchorScrollPixelsX)
                              ? std::clamp(input.anchorScrollPixelsX, -kMaximumPendingPixels, kMaximumPendingPixels)
                              : 0.0f;
    anchorScrollPixelsY = finite(input.anchorScrollPixelsY)
                              ? std::clamp(input.anchorScrollPixelsY, -kMaximumPendingPixels, kMaximumPendingPixels)
                              : 0.0f;
    zoomWheelUnits = clampAccumulated(zoomWheelUnits, input.zoomWheelUnits, kMaximumPendingWheelUnits);
    panPixelsX = clampAccumulated(panPixelsX, input.panPixelsX, kMaximumPendingPixels);
    panPixelsY = clampAccumulated(panPixelsY, input.panPixelsY, kMaximumPendingPixels);
    orbitPixelsX = clampAccumulated(orbitPixelsX, input.orbitPixelsX, kMaximumPendingPixels);
    orbitPixelsY = clampAccumulated(orbitPixelsY, input.orbitPixelsY, kMaximumPendingPixels);
    orbitPitchStepRadians = clampAccumulated(
        orbitPitchStepRadians, input.orbitPitchStepRadians,
        kMaximumPendingOrbitRadians);
    orbitPitchAxis = finite(input.orbitPitchAxis)
        ? std::clamp(input.orbitPitchAxis, -1.0f, 1.0f) : 0.0f;
    orbitYawAxis = finite(input.orbitYawAxis)
        ? std::clamp(input.orbitYawAxis, -1.0f, 1.0f) : 0.0f;
    zoomAxis = finite(input.zoomAxis)
        ? std::clamp(input.zoomAxis, -1.0f, 1.0f) : 0.0f;
    horizontalScrollSpeedFactor =
        finite(input.horizontalScrollSpeedFactor) ? std::max(0.0f, input.horizontalScrollSpeedFactor) : 1.6f;
    verticalScrollSpeedFactor =
        finite(input.verticalScrollSpeedFactor) ? std::max(0.0f, input.verticalScrollSpeedFactor) : 2.0f;
    keyboardScrollSpeedFactor =
        finite(input.keyboardScrollSpeedFactor) ? std::max(0.0f, input.keyboardScrollSpeedFactor) : 0.5f;
    keyboardRotateSpeed = finite(input.keyboardRotateSpeed)
        ? std::max(0.0f, input.keyboardRotateSpeed) : 0.1f;
    tacticalViewportAspectRatio =
        finite(input.tacticalViewportAspectRatio) && input.tacticalViewportAspectRatio > math::EPSILON
            ? input.tacticalViewportAspectRatio
            : 5.0f / 3.0f;
    if (input.hasAbsoluteTarget && finite(input.absoluteTarget.x()) &&
        finite(input.absoluteTarget.y()) && finite(input.absoluteTarget.z())) {
        absoluteTarget = input.absoluteTarget;
        hasAbsoluteTarget = true;
    }
    if (input.hasAbsoluteState) {
        absoluteState = input.absoluteState.sanitized();
        hasAbsoluteState = true;
    }
    resetToHome = resetToHome || input.resetToHome;
    manualIntent = manualIntent || input.manualIntent;
}

void GameCameraManipulator::apply(GameCameraState& camera,
                                  const GameCameraInput& input,
                                  float fixedDeltaSeconds) noexcept
{
    // This is the point at which transient local input becomes durable
    // session state. Keep malformed event data from crossing that boundary.
    camera = camera.sanitized();

    if (input.hasAbsoluteState) {
        camera = input.absoluteState.sanitized();
    }

    if (input.hasAbsoluteTarget && finite(input.absoluteTarget.x()) &&
        finite(input.absoluteTarget.y()) && finite(input.absoluteTarget.z())) {
        const math::vec3 delta = input.absoluteTarget - camera.target;
        camera.position += delta;
        camera.target = input.absoluteTarget;
    }

    zoom(camera, input.zoomWheelUnits);
    panByPixels(camera, input.panPixelsX, input.panPixelsY);
    orbitByPixels(camera, input.orbitPixelsX, input.orbitPixelsY);
    if (finite(input.orbitPitchStepRadians) &&
        std::abs(input.orbitPitchStepRadians) > math::EPSILON) {
        orbitByPixels(camera, 0.0f,
                      input.orbitPitchStepRadians / kOrbitRadiansPerPixel);
    }
    const float pitchAxis = finite(input.orbitPitchAxis)
        ? std::clamp(input.orbitPitchAxis, -1.0f, 1.0f) : 0.0f;
    const float yawAxis = finite(input.orbitYawAxis)
        ? std::clamp(input.orbitYawAxis, -1.0f, 1.0f) : 0.0f;
    const float rotateSpeed = finite(input.keyboardRotateSpeed)
        ? std::max(0.0f, input.keyboardRotateSpeed) : 0.1f;
    if (finite(fixedDeltaSeconds) && fixedDeltaSeconds > 0.0f &&
        std::abs(pitchAxis) > math::EPSILON && rotateSpeed > 0.0f) {
        orbitByPixels(
            camera, 0.0f,
            pitchAxis * rotateSpeed * fixedDeltaSeconds /
                kOrbitRadiansPerPixel);
    }
    if (finite(fixedDeltaSeconds) && fixedDeltaSeconds > 0.0f &&
        std::abs(yawAxis) > math::EPSILON && rotateSpeed > 0.0f) {
        orbitByPixels(
            camera, yawAxis * rotateSpeed * fixedDeltaSeconds /
                kOrbitRadiansPerPixel, 0.0f);
    }
    if (finite(fixedDeltaSeconds) && fixedDeltaSeconds > 0.0f &&
        std::abs(input.zoomAxis) > math::EPSILON) {
        zoom(camera, std::clamp(input.zoomAxis, -1.0f, 1.0f) *
            fixedDeltaSeconds * 3.0f);
    }

    const float forwardAxis = finite(input.panForwardAxis) ? std::clamp(input.panForwardAxis, -1.0f, 1.0f) : 0.0f;
    const float rightAxis = finite(input.panRightAxis) ? std::clamp(input.panRightAxis, -1.0f, 1.0f) : 0.0f;
    const float edgeForwardAxis =
        finite(input.screenEdgeForwardAxis) ? std::clamp(input.screenEdgeForwardAxis, -1.0f, 1.0f) : 0.0f;
    const float edgeRightAxis =
        finite(input.screenEdgeRightAxis) ? std::clamp(input.screenEdgeRightAxis, -1.0f, 1.0f) : 0.0f;
    if (!finite(fixedDeltaSeconds) || fixedDeltaSeconds <= 0.0f ||
        (std::abs(forwardAxis) <= math::EPSILON && std::abs(rightAxis) <= math::EPSILON &&
         std::abs(edgeForwardAxis) <= math::EPSILON && std::abs(edgeRightAxis) <= math::EPSILON &&
         std::abs(input.anchorScrollPixelsX) <= math::EPSILON && std::abs(input.anchorScrollPixelsY) <= math::EPSILON))
    {
        return;
    }

    const math::vec3 forward = groundForward(camera);
    const math::vec3 right = forward.cross(kWorldUp).normalized();
    const float horizontalFactor =
        finite(input.horizontalScrollSpeedFactor) ? std::max(0.0f, input.horizontalScrollSpeedFactor) : 1.6f;
    const float verticalFactor =
        finite(input.verticalScrollSpeedFactor) ? std::max(0.0f, input.verticalScrollSpeedFactor) : 2.0f;
    const float userFactor =
        finite(input.keyboardScrollSpeedFactor) ? std::max(0.0f, input.keyboardScrollSpeedFactor) : 0.5f;

    math::vec3 configuredMotion = forward * ((forwardAxis + edgeForwardAxis) * verticalFactor) +
                                  right * ((rightAxis + edgeRightAxis) * horizontalFactor);
    if (usable(configuredMotion))
    {
        panWorld(camera, configuredMotion * (kConfiguredScrollUnitsPerSecond * userFactor * fixedDeltaSeconds));
    }

    // Anchor scroll is displacement-proportional and persistent.  Scale the
    // former grab-pan world/pixel relation to a 30 Hz authored tick so the
    // cursor may stop moving while the camera continues to scroll.
    const float anchorX = finite(input.anchorScrollPixelsX) ? input.anchorScrollPixelsX : 0.0f;
    const float anchorY = finite(input.anchorScrollPixelsY) ? input.anchorScrollPixelsY : 0.0f;
    if (std::abs(anchorX) > math::EPSILON || std::abs(anchorY) > math::EPSILON)
    {
        const float worldPerPixel = std::clamp(cameraDistance(camera) * kDragPanDistanceScale, 0.05f, 32.0f);
        const float authoredTickScale = fixedDeltaSeconds * 30.0f;
        panWorld(camera,
                 (right * (anchorX * horizontalFactor) - forward * (anchorY * verticalFactor)) *
                     (worldPerPixel * authoredTickScale * userFactor));
    }
}

} // namespace engine
