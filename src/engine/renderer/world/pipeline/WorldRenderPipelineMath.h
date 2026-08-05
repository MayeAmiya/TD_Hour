#pragma once

#include "presentation/render/RenderSceneSnapshot.h"

#include <cmath>
#include <cstdint>

namespace engine::render::world_pipeline_detail {

[[nodiscard]] inline RenderMatrix makeEntityTransform(
    const RenderTransform& transform) noexcept {
    return RenderMatrix::from_trs(
        transform.scale, transform.orientation.normalized(),
        transform.position);
}

[[nodiscard]] inline RenderQuaternion floatSwayOrientation(
    float zYawRadians, uint64_t sampleTick) noexcept {
    const float frame = static_cast<float>(static_cast<uint32_t>(sampleTick));
    const float swayYaw = std::sin(frame * 0.0291f) * 0.05f;
    const float swayPitch = std::sin(frame * 0.0515f) * 0.05f;
    const float stableYaw = std::isfinite(zYawRadians)
        ? zYawRadians : 0.0f;
    const RenderQuaternion pitch = RenderQuaternion::from_axis_angle(
        {1.0f, 0.0f, 0.0f}, swayPitch);
    const RenderQuaternion yaw = RenderQuaternion::from_axis_angle(
        {0.0f, 1.0f, 0.0f}, swayYaw);
    const RenderQuaternion z = RenderQuaternion::from_axis_angle(
        {0.0f, 0.0f, 1.0f}, stableYaw);
    return ((pitch * yaw) * z).normalized();
}

[[nodiscard]] inline bool finiteVector(
    const RenderVector& value) noexcept {
    return std::isfinite(value.x()) && std::isfinite(value.y()) &&
        std::isfinite(value.z());
}

} // namespace engine::render::world_pipeline_detail
