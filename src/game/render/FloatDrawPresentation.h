#pragma once

#include "core/math/wwmath/base/wwmath.h"

#include <cstdint>

namespace engine {

// FloatUpdate 的 Drawable 姿态仅属于表现域。即使水面吸附被禁用，原版
// 船只仍保留摇摆，因此这里消费已确认 tick，而不回写模拟组件。
[[nodiscard]] math::quat objectFloatVisualOrientation(
    float zYawRadians, uint64_t confirmedTick) noexcept;

} // namespace engine
