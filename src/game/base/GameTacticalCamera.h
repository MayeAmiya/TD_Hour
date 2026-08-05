#pragma once

#include "presentation/camera/GameCameraInput.h"
#include "presentation/render/RenderGameDataSettings.h"

namespace game::terrain
{
class TerrainMap;
}

namespace engine
{

// Original tactical-view geometry expressed as logic-side value operations.
// The policy consumes only the frozen session settings and TerrainMap value
// queries; no renderer, SDL object, ECS entity, or live preferences object is
// retained across a confirmed tick.
class GameTacticalCamera final
{
public:
    [[nodiscard]] static GameCameraState makeInitial(math::vec3 authoredPivot,
                                                     const game::terrain::TerrainMap& terrain,
                                                     const RenderCameraGameData& settings,
                                                     float tacticalAspectRatio) noexcept;

    // Applies middle-click home/reset, terrain following, zoom-height limits,
    // playable-map constraints, source-above-terrain safety and dynamic far
    // projection after GameCameraManipulator has consumed one manual sample.
    static void constrainManualCamera(GameCameraState& camera,
                                      const GameCameraInput& input,
                                      float fixedDeltaSeconds,
                                      const game::terrain::TerrainMap& terrain,
                                      const RenderCameraGameData& settings) noexcept;

    // Value-only XY constraint shared by the confirmed camera and the
    // presentation-thread prediction. Returning the applied translation lets
    // the logic owner perform its terrain-height resample without making the
    // presentation thread retain or query TerrainMap.
    [[nodiscard]] static math::vec3 constrainToPlayableExtent(
        GameCameraState& camera,
        math::vec3 playableMinimum,
        math::vec3 playableMaximum,
        const RenderCameraGameData& settings,
        float tacticalAspectRatio) noexcept;

    [[nodiscard]] static float verticalFovRadians(float horizontalFovDegrees, float tacticalAspectRatio) noexcept;
    [[nodiscard]] static float projectedTerrainFarClip(const GameCameraState& camera,
                                                       const game::terrain::TerrainMap& terrain) noexcept;

private:
    [[nodiscard]] static float averagedGroundHeight(const game::terrain::TerrainMap& terrain,
                                                    float x,
                                                    float y) noexcept;
    static void setHomePose(GameCameraState& camera,
                            math::vec3 pivot,
                            float height,
                            const RenderCameraGameData& settings) noexcept;
};

} // namespace engine
