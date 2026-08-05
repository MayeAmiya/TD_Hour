#include "game/render/FloatDrawPresentation.h"

#include <cmath>

namespace engine {

math::quat objectFloatVisualOrientation(float zYawRadians,
                                        uint64_t confirmedTick) noexcept {
    // 原版把 32 位 GameLogic frame 转为 Real；保留其回绕边界。
    const float frame = static_cast<float>(static_cast<uint32_t>(confirmedTick));
    const float swayYaw = std::sin(frame * 0.0291f) * 0.05f;
    const float swayPitch = std::sin(frame * 0.0515f) * 0.05f;
    const float stableYaw = std::isfinite(zYawRadians) ? zYawRadians : 0.0f;
    const math::quat pitch = math::quat::from_axis_angle(
        {1.0f, 0.0f, 0.0f}, swayPitch);
    const math::quat yaw = math::quat::from_axis_angle(
        {0.0f, 1.0f, 0.0f}, swayYaw);
    const math::quat z = math::quat::from_axis_angle(
        {0.0f, 0.0f, 1.0f}, stableYaw);
    return ((pitch * yaw) * z).normalized();
}

} // namespace engine
