#pragma once

#include <algorithm>
#include <cmath>

#include "presentation/render/RenderViewSnapshot.h"
#include "core/math/wwmath/base/wwmath.h"

namespace engine::render
{

// WW3D uses a right-handed view space (forward is -Z). Keep that convention
// through the D3D12 world pass so W3D mesh orientation and texture U direction
// remain intact. The camera deliberately exposes only wwmath types.
class WorldCamera
{
public:
    WorldCamera() = default;

    WorldCamera(math::vec3 position,
                math::vec3 target,
                float verticalFovRadians = math::deg_to_rad(60.0f),
                float nearClip = 0.1f,
                float farClip = 2000.0f) noexcept
        : m_position(position)
        , m_target(target)
    {
        setPerspective(verticalFovRadians, nearClip, farClip);
    }

    // Rendering must derive its view state from the sealed frame, rather
    // than from a mutable renderer-side camera.  This helper intentionally
    // copies only value data from the backend-neutral render snapshot.
    [[nodiscard]] static WorldCamera fromSnapshot(const RenderCameraSnapshot& snapshot) noexcept
    {
        WorldCamera camera{
            snapshot.position, snapshot.target, snapshot.verticalFovRadians, snapshot.nearClip, snapshot.farClip};
        camera.setUp(snapshot.up);
        camera.m_horizontalFovRadians = snapshot.horizontalFovRadians;
        camera.m_tacticalViewportHeightScale = snapshot.tacticalViewportHeightScale;
        return camera;
    }

    // The diagnostic path owns a separate camera, but it feeds that camera
    // through the exact same immutable snapshot format as a game frame.
    [[nodiscard]] RenderCameraSnapshot toSnapshot(float visibilityDistance = 2000.0f) const noexcept
    {
        RenderCameraSnapshot snapshot;
        snapshot.position = m_position;
        snapshot.visibilityDistance = visibilityDistance;
        snapshot.target = m_target;
        snapshot.up = m_up;
        snapshot.verticalFovRadians = m_verticalFovRadians;
        snapshot.horizontalFovRadians = m_horizontalFovRadians;
        snapshot.tacticalViewportHeightScale = m_tacticalViewportHeightScale;
        snapshot.nearClip = m_nearClip;
        snapshot.farClip = m_farClip;
        return snapshot;
    }

    // Generals/W3D assets are Z-up; handedness is handled by the view and
    // projection matrices, not by altering asset vertices or UVs.
    static math::vec3 worldUp() noexcept
    {
        return {0.0f, 0.0f, 1.0f};
    }

    const math::vec3& position() const noexcept
    {
        return m_position;
    }
    const math::vec3& target() const noexcept
    {
        return m_target;
    }
    const math::vec3& up() const noexcept
    {
        return m_up;
    }
    float verticalFovRadians() const noexcept
    {
        return m_verticalFovRadians;
    }
    float horizontalFovRadians() const noexcept
    {
        return m_horizontalFovRadians;
    }
    float tacticalViewportHeightScale() const noexcept
    {
        return m_tacticalViewportHeightScale;
    }
    [[nodiscard]] uint32_t tacticalViewportHeight(uint32_t fullHeight) const noexcept
    {
        const float scaled = static_cast<float>(fullHeight) * std::clamp(m_tacticalViewportHeightScale, 0.1f, 1.0f);
        return std::clamp(static_cast<uint32_t>(scaled + 0.5f), 1u, std::max(fullHeight, 1u));
    }
    float nearClip() const noexcept
    {
        return m_nearClip;
    }
    float farClip() const noexcept
    {
        return m_farClip;
    }

    void setPosition(math::vec3 position) noexcept
    {
        m_position = position;
    }
    void setTarget(math::vec3 target) noexcept
    {
        m_target = target;
    }
    void lookAt(math::vec3 position, math::vec3 target, math::vec3 up = worldUp()) noexcept
    {
        m_position = position;
        m_target = target;
        m_up = up;
    }
    void setUp(math::vec3 up) noexcept
    {
        m_up = up;
    }

    void setPerspective(float verticalFovRadians, float nearClip, float farClip) noexcept
    {
        // Avoid invalid projection matrices while keeping the caller's intent.
        m_verticalFovRadians = math::clamp(verticalFovRadians, 0.01f, math::PI - 0.01f);
        m_nearClip = math::max(nearClip, 0.001f);
        m_farClip = math::max(farClip, m_nearClip + 0.001f);
    }

    [[nodiscard]] math::float4x4 viewMatrix() const noexcept
    {
        const math::vec3 forward = m_target - m_position;
        if (forward.length_sq() <= math::EPSILON * math::EPSILON)
        {
            return math::float4x4::identity();
        }

        math::vec3 cameraUp = m_up;
        if (cameraUp.length_sq() <= math::EPSILON * math::EPSILON)
        {
            cameraUp = worldUp();
        }

        const math::vec3 normalizedForward = forward.normalized();
        const math::vec3 normalizedUp = cameraUp.normalized();
        if (math::abs(normalizedForward.dot(normalizedUp)) > 0.999f)
        {
            cameraUp = math::abs(normalizedForward.z()) < 0.999f ? worldUp() : math::vec3{0.0f, 1.0f, 0.0f};
        }

        return math::float4x4::look_at_rh(m_position, m_target, cameraUp);
    }

    [[nodiscard]] math::float4x4 projectionMatrix(float aspectRatio) const noexcept
    {
        const float fullAspect = aspectRatio > math::EPSILON ? aspectRatio : 1.0f;
        const float heightScale = std::clamp(m_tacticalViewportHeightScale, 0.1f, 1.0f);
        const float safeAspect = fullAspect / heightScale;
        const float verticalFov = m_horizontalFovRadians > 0.0f
                                      ? 2.0f * std::atan(std::tan(m_horizontalFovRadians * 0.5f) / safeAspect)
                                      : m_verticalFovRadians;
        return math::float4x4::perspective_fov_rh(verticalFov, safeAspect, m_nearClip, m_farClip);
    }

    // wwmath and the world shaders both use DirectX's row-vector convention:
    // a position is transformed with mul(position, viewProjection).
    [[nodiscard]] math::float4x4 viewProjectionMatrix(float aspectRatio) const noexcept
    {
        return viewMatrix() * projectionMatrix(aspectRatio);
    }

private:
    math::vec3 m_position{5.0f, -7.0f, 4.0f};
    math::vec3 m_target{0.0f, 0.0f, 0.0f};
    math::vec3 m_up = worldUp();
    float m_verticalFovRadians = math::deg_to_rad(60.0f);
    float m_horizontalFovRadians = 0.0f;
    float m_tacticalViewportHeightScale = 1.0f;
    float m_nearClip = 0.1f;
    float m_farClip = 2000.0f;
};

} // namespace engine::render
