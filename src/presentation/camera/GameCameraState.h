#pragma once

#include <cmath>
#include <cstdint>

#include "core/math/wwmath/base/wwmath.h"

namespace engine
{

// Logic-owned camera value state.  It deliberately contains no ECS handle,
// renderer object, or GPU resource: a completed logic frame can copy this
// data into a renderer snapshot without retaining game-domain state.
//
// Generals/W3D worlds are Z-up and use a right-handed view space.  These
// defaults match the long-standing world diagnostic camera so the normal game
// path and the debug path start from the same view convention.
struct GameCameraState final
{
    math::vec3 position{5.0f, -7.0f, 4.0f};
    math::vec3 target{0.0f, 0.0f, 0.0f};
    math::vec3 up{0.0f, 0.0f, 1.0f};
    float verticalFovRadians = math::deg_to_rad(60.0f);
    // A positive horizontal value is authoritative for the tactical camera;
    // verticalFovRadians remains the compatibility representation for debug
    // and scripted callers which predate the original W3D FOV convention.
    float horizontalFovRadians = 0.0f;
    float tacticalViewportHeightScale = 1.0f;
    float nearClip = 0.1f;
    float farClip = 2000.0f;
    // Non-positive remains an explicit "no distance culling" request.
    float visibilityDistance = 2000.0f;

    bool fogEnabled = false;
    math::vec3 fogColor{};
    float fogStartDistance = 0.0f;
    float fogEndDistance = 0.0f;

    // Monotonic presentation boundary for authored camera cuts. Ordinary
    // manual orbit/pan and timed camera tracks keep this value unchanged;
    // SETUP_CAMERA, zero-duration transitions and snap camera locks advance
    // it so the renderer never blends across a discontinuity.
    uint64_t cameraCutRevision = 0;

    // Snapshot publication is a trust boundary.  Make malformed user/input
    // state deterministic before it reaches a view/projection calculation;
    // only malformed or unsafe view/fog bounds are normalized.
    [[nodiscard]] GameCameraState sanitized() const noexcept
    {
        const GameCameraState defaults{};
        GameCameraState result = *this;

        if (!isFinite(result.position))
            result.position = defaults.position;
        if (!isFinite(result.target))
            result.target = defaults.target;

        math::vec3 forward = result.target - result.position;
        if (!isUsableDirection(forward))
        {
            result.target = result.position + (defaults.target - defaults.position);
            forward = result.target - result.position;
            // Finite but extreme coordinates can lose the small default
            // direction during addition; return to a known camera in that
            // case rather than feeding a zero direction to DirectXMath.
            if (!isUsableDirection(forward))
            {
                result.position = defaults.position;
                result.target = defaults.target;
                forward = result.target - result.position;
            }
        }

        if (!isUsableDirection(result.up))
        {
            result.up = defaults.up;
        }
        const math::vec3 normalizedForward = forward.normalized();
        if (std::abs(normalizedForward.dot(result.up.normalized())) > 0.999f)
        {
            result.up = std::abs(normalizedForward.z()) < 0.999f ? defaults.up : math::vec3{0.0f, 1.0f, 0.0f};
        }

        result.verticalFovRadians = finiteOr(result.verticalFovRadians, defaults.verticalFovRadians);
        result.verticalFovRadians = math::clamp(result.verticalFovRadians, 0.01f, math::PI - 0.01f);
        result.horizontalFovRadians = finiteOr(result.horizontalFovRadians, 0.0f);
        if (result.horizontalFovRadians > 0.0f)
        {
            result.horizontalFovRadians = math::clamp(result.horizontalFovRadians, 0.01f, math::PI - 0.01f);
        }
        result.tacticalViewportHeightScale =
            math::clamp(finiteOr(result.tacticalViewportHeightScale, 1.0f), 0.1f, 1.0f);
        result.nearClip = math::max(finiteOr(result.nearClip, defaults.nearClip), 0.001f);
        result.farClip = math::max(finiteOr(result.farClip, defaults.farClip), result.nearClip + 0.001f);
        result.visibilityDistance = finiteOr(result.visibilityDistance, defaults.visibilityDistance);

        if (!isFinite(result.fogColor))
            result.fogColor = defaults.fogColor;
        result.fogStartDistance = finiteOr(result.fogStartDistance, defaults.fogStartDistance);
        result.fogEndDistance = finiteOr(result.fogEndDistance, defaults.fogEndDistance);
        if (result.fogEnabled)
        {
            result.fogStartDistance = math::max(result.fogStartDistance, 0.0f);
            result.fogEndDistance = math::max(result.fogEndDistance, result.fogStartDistance + 0.0001f);
        }
        return result;
    }

private:
    [[nodiscard]] static bool isFinite(const math::vec3& value) noexcept
    {
        return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
    }

    [[nodiscard]] static bool isUsableDirection(const math::vec3& value) noexcept
    {
        if (!isFinite(value))
            return false;
        const float lengthSq = value.length_sq();
        return std::isfinite(lengthSq) && lengthSq > math::EPSILON * math::EPSILON;
    }

    [[nodiscard]] static float finiteOr(float value, float fallback) noexcept
    {
        return std::isfinite(value) ? value : fallback;
    }
};

} // namespace engine
