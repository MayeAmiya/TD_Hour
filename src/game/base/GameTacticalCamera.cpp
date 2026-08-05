#include "game/base/GameTacticalCamera.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "game/terrain/TerrainMap.h"

namespace engine
{
namespace
{

constexpr float kTerrainSampleDistance = 10.0f;

float finiteOr(float value, float fallback) noexcept
{
    return std::isfinite(value) ? value : fallback;
}

float cameraElevation(const GameCameraState& camera) noexcept
{
    return std::max(camera.position.z() - camera.target.z(), math::EPSILON);
}

} // namespace

float GameTacticalCamera::verticalFovRadians(float horizontalFovDegrees, float tacticalAspectRatio) noexcept
{
    const float horizontal = math::deg_to_rad(std::clamp(finiteOr(horizontalFovDegrees, 50.0f), 1.0f, 175.0f));
    const float aspect = std::max(finiteOr(tacticalAspectRatio, 5.0f / 3.0f), 0.1f);
    return 2.0f * std::atan(std::tan(horizontal * 0.5f) / aspect);
}

float GameTacticalCamera::averagedGroundHeight(const game::terrain::TerrainMap& terrain, float x, float y) noexcept
{
    float sum = terrain.groundHeight(x, y);
    sum += terrain.groundHeight(x + kTerrainSampleDistance, y - kTerrainSampleDistance);
    sum += terrain.groundHeight(x - kTerrainSampleDistance, y - kTerrainSampleDistance);
    sum += terrain.groundHeight(x + kTerrainSampleDistance, y + kTerrainSampleDistance);
    sum += terrain.groundHeight(x - kTerrainSampleDistance, y + kTerrainSampleDistance);
    return sum * 0.2f;
}

void GameTacticalCamera::setHomePose(GameCameraState& camera,
                                     math::vec3 pivot,
                                     float height,
                                     const RenderCameraGameData& settings) noexcept
{
    const float pitch = math::deg_to_rad(std::clamp(finiteOr(settings.pitchDegrees, 37.5f), 0.1f, 89.9f));
    const float yaw = math::deg_to_rad(finiteOr(settings.yawDegrees, 0.0f));
    const float safeHeight = std::max(finiteOr(height, 232.0f), 1.0f);
    const float horizontal = safeHeight / std::tan(pitch);
    camera.target = pivot;
    camera.position = pivot + math::vec3{
                                  std::sin(yaw) * horizontal,
                                  -std::cos(yaw) * horizontal,
                                  safeHeight,
                              };
    camera.up = {0.0f, 0.0f, 1.0f};
}

GameCameraState GameTacticalCamera::makeInitial(math::vec3 authoredPivot,
                                                const game::terrain::TerrainMap& terrain,
                                                const RenderCameraGameData& settings,
                                                float tacticalAspectRatio) noexcept
{
    GameCameraState camera;
    authoredPivot[2] = averagedGroundHeight(terrain, authoredPivot.x(), authoredPivot.y());
    setHomePose(camera, authoredPivot, settings.initialHeight, settings);
    camera.verticalFovRadians = verticalFovRadians(settings.horizontalFieldOfViewDegrees, tacticalAspectRatio);
    camera.horizontalFovRadians = math::deg_to_rad(settings.horizontalFieldOfViewDegrees);
    // GeneralsTD is an in-game runtime: the command bar is composited over the
    // tactical world and never reserves a bottom strip in the world viewport.
    camera.tacticalViewportHeightScale = 1.0f;
    camera.nearClip = std::max(finiteOr(settings.nearClipDistance, 10.0f), 0.001f);
    camera.farClip = projectedTerrainFarClip(camera, terrain);
    camera.visibilityDistance = camera.farClip;
    GameCameraInput initialConstraint;
    initialConstraint.tacticalViewportAspectRatio = tacticalAspectRatio;
    constrainManualCamera(camera, initialConstraint, 0.0f, terrain, settings);
    return camera.sanitized();
}

float GameTacticalCamera::projectedTerrainFarClip(const GameCameraState& camera,
                                                  const game::terrain::TerrainMap& terrain) noexcept
{
    const game::terrain::TerrainExtent extent = terrain.extentIncludingBorder();
    math::vec3 forward = camera.target - camera.position;
    if (!std::isfinite(forward.length_sq()) || forward.length_sq() <= math::EPSILON * math::EPSILON)
    {
        return std::max(camera.nearClip + 1.0f, 2000.0f);
    }
    forward = forward.normalized();
    float farClip = camera.nearClip + 1.0f;
    for (uint32_t corner = 0; corner < 8; ++corner)
    {
        const math::vec3 point{
            (corner & 1u) ? extent.maximum.x() : extent.minimum.x(),
            (corner & 2u) ? extent.maximum.y() : extent.minimum.y(),
            (corner & 4u) ? extent.maximum.z() : extent.minimum.z(),
        };
        farClip = std::max(farClip, (point - camera.position).dot(forward));
    }
    return std::max(farClip + camera.nearClip, camera.nearClip + 1.0f);
}

math::vec3 GameTacticalCamera::constrainToPlayableExtent(
    GameCameraState& camera,
    math::vec3 playableMinimum,
    math::vec3 playableMaximum,
    const RenderCameraGameData& settings,
    float tacticalAspectRatio) noexcept
{
    if (!std::isfinite(playableMinimum.x()) ||
        !std::isfinite(playableMinimum.y()) ||
        !std::isfinite(playableMaximum.x()) ||
        !std::isfinite(playableMaximum.y()) ||
        playableMinimum.x() > playableMaximum.x() ||
        playableMinimum.y() > playableMaximum.y()) {
        return {};
    }
    (void)settings;
    (void)tacticalAspectRatio;
    // The tactical pivot, not every projected screen corner, is the map
    // constraint. At a 30/60-degree pitch the lower half of the viewport can
    // extend beyond the active boundary; the renderer owns that exterior and
    // fills it with border black. Insetting by the camera footprint prevented
    // the screen centre from ever reaching the bottom/corners and made valid
    // edge units unreachable at ordinary angles.
    const float clampedX = std::clamp(
        camera.target.x(), playableMinimum.x(), playableMaximum.x());
    const float clampedY = std::clamp(
        camera.target.y(), playableMinimum.y(), playableMaximum.y());
    const math::vec3 shift{
        clampedX - camera.target.x(), clampedY - camera.target.y(), 0.0f};
    camera.target += shift;
    camera.position += shift;
    return shift;
}

void GameTacticalCamera::constrainManualCamera(GameCameraState& camera,
                                               const GameCameraInput& input,
                                               float fixedDeltaSeconds,
                                               const game::terrain::TerrainMap& terrain,
                                               const RenderCameraGameData& settings) noexcept
{
    if (!terrain.isLoaded())
        return;
    camera = camera.sanitized();
    const float aspect =
        std::isfinite(input.tacticalViewportAspectRatio) && input.tacticalViewportAspectRatio > math::EPSILON
            ? input.tacticalViewportAspectRatio
            : 5.0f / 3.0f;

    if (input.resetToHome)
    {
        math::vec3 pivot = camera.target;
        pivot[2] = averagedGroundHeight(terrain, pivot.x(), pivot.y());
        setHomePose(camera, pivot, settings.maximumHeight, settings);
    }

    // Move the pivot toward the averaged terrain sample at the original
    // authored per-30-Hz adjustment rate, translating the eye by the same
    // amount so pitch and zoom remain stable on rolling ground.
    const float desiredGround = averagedGroundHeight(terrain, camera.target.x(), camera.target.y());
    const float deltaGround = desiredGround - camera.target.z();
    const float sampledScrollAmount = std::max({
        std::abs(input.anchorScrollPixelsX),
        std::abs(input.anchorScrollPixelsY),
        std::abs(input.panPixelsX),
        std::abs(input.panPixelsY)});
    const bool settleTerrainHeight =
        sampledScrollAmount <=
            std::max(finiteOr(settings.scrollAmountCutoff, 50.0f), 0.0f);
    const float adjust = std::clamp(
        finiteOr(settings.adjustSpeed, 0.3f) * std::max(finiteOr(fixedDeltaSeconds, 0.0f), 0.0f) * 30.0f, 0.0f, 1.0f);
    const float groundStep = settleTerrainHeight
        ? deltaGround * adjust : 0.0f;
    camera.target[2] += groundStep;
    camera.position[2] += groundStep;

    // Preserve yaw/pitch while enforcing the authored vertical zoom range.
    const float minimumHeight = std::max(finiteOr(settings.minimumHeight, 120.0f), 1.0f);
    const float maximumHeight = std::max(finiteOr(settings.maximumHeight, 310.0f), minimumHeight);
    const float currentHeight = cameraElevation(camera);
    const float constrainedHeight = settings.enforceMaximumHeight
        ? std::clamp(currentHeight, minimumHeight, maximumHeight)
        : std::max(currentHeight, minimumHeight);
    if (std::abs(constrainedHeight - currentHeight) > math::EPSILON)
    {
        const math::vec3 radial = camera.position - camera.target;
        const float scale = constrainedHeight / currentHeight;
        camera.position = camera.target + radial * scale;
    }

    // The physical exterior is retained and darkened by the renderer, so the
    // screen may cross an edge; only the tactical pivot remains inside the
    // authored map area.
    const game::terrain::TerrainExtent extent = terrain.playableExtent();
    const math::vec3 mapShift = constrainToPlayableExtent(
        camera, extent.minimum, extent.maximum, settings, aspect);
    if (std::abs(mapShift.x()) > math::EPSILON || std::abs(mapShift.y()) > math::EPSILON)
    {
        const float constrainedGround =
            averagedGroundHeight(terrain, camera.target.x(), camera.target.y());
        const float terrainShift = constrainedGround - camera.target.z();
        camera.target[2] += terrainShift;
        camera.position[2] += terrainShift;
    }

    // The near plane itself is the original source-above-terrain clearance.
    const float sourceGround = averagedGroundHeight(terrain, camera.position.x(), camera.position.y());
    const float minimumSourceZ = sourceGround + std::max(finiteOr(settings.nearClipDistance, 10.0f), 0.001f);
    if (camera.position.z() < minimumSourceZ)
    {
        const float lift = minimumSourceZ - camera.position.z();
        camera.position[2] += lift;
        camera.target[2] += lift;
    }

    const float horizontalFovDegrees = std::clamp(
        finiteOr(settings.horizontalFieldOfViewDegrees, 50.0f),
        1.0f, 175.0f);
    camera.verticalFovRadians = verticalFovRadians(
        horizontalFovDegrees, aspect);
    camera.horizontalFovRadians = math::deg_to_rad(horizontalFovDegrees);
    camera.tacticalViewportHeightScale = 1.0f;
    camera.nearClip = std::max(finiteOr(settings.nearClipDistance, 10.0f), 0.001f);
    camera.farClip = projectedTerrainFarClip(camera, terrain);
    camera.visibilityDistance = camera.farClip;
    camera = camera.sanitized();
}

} // namespace engine
