#include "LocalPlacementAnchorInput.h"

#include <cmath>
#include <numbers>

namespace engine::selection {
namespace {

[[nodiscard]] bool finite(LocalPlacementScreenPoint point) noexcept {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

} // namespace

bool LocalPlacementAnchorInput::begin(
    LocalPlacementScreenPoint point) noexcept {
    if (m_active || !finite(point)) return false;
    m_start = point;
    m_end = point;
    m_active = true;
    m_dragged = false;
    return true;
}

bool LocalPlacementAnchorInput::admitEnd(
    LocalPlacementScreenPoint point) noexcept {
    if (!m_active || !finite(point)) return false;
    const float deltaX = point.x - m_start.x;
    const float deltaY = point.y - m_start.y;
    constexpr float thresholdSquared =
        DragThresholdPixels * DragThresholdPixels;
    const float distanceSquared = deltaX * deltaX + deltaY * deltaY;
    if (!std::isfinite(distanceSquared) ||
        distanceSquared < thresholdSquared) {
        return false;
    }
    m_end = point;
    m_dragged = true;
    return true;
}

bool LocalPlacementAnchorInput::update(
    LocalPlacementScreenPoint point) noexcept {
    return admitEnd(point);
}

std::optional<LocalPlacementAnchorConfirmation>
LocalPlacementAnchorInput::release(
    LocalPlacementScreenPoint point) noexcept {
    if (!m_active || !finite(point)) return std::nullopt;
    static_cast<void>(admitEnd(point));
    const LocalPlacementAnchorConfirmation confirmation{
        .start = m_start,
        .end = m_end,
        .dragged = m_dragged,
    };
    cancel();
    return confirmation;
}

void LocalPlacementAnchorInput::cancel() noexcept {
    m_start = {};
    m_end = {};
    m_active = false;
    m_dragged = false;
}

math::q32_32 snapLocalPlacementYaw45Fixed(
    math::q32_32 yawRadians, bool forceAttackMode) noexcept {
    if (!forceAttackMode) return yawRadians;
    const math::q32_32 step{std::numbers::pi_v<double> / 4.0};
    const int64_t ratioRaw = (yawRadians / step).raw();
    constexpr uint64_t half = UINT64_C(1) << 31u;
    const bool negative = ratioRaw < 0;
    const uint64_t magnitude = negative
        ? static_cast<uint64_t>(-(ratioRaw + 1)) + 1u
        : static_cast<uint64_t>(ratioRaw);
    const uint64_t roundedMagnitude = (magnitude + half) >> 32u;
    const int32_t rounded = negative
        ? -static_cast<int32_t>(roundedMagnitude)
        : static_cast<int32_t>(roundedMagnitude);
    return step * math::q32_32{rounded};
}

} // namespace engine::selection
