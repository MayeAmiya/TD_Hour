#include "TacticalRadarEventPresentation.h"

#include <algorithm>
#include <cmath>

namespace engine::render {
namespace {

constexpr float kPi = 3.14159265358979323846f;

[[nodiscard]] uint32_t withAlpha(uint32_t rgb, uint8_t alpha) noexcept {
    return (static_cast<uint32_t>(alpha) << 24u) | (rgb & 0x00ffffffu);
}

} // namespace

TacticalRadarEventColors tacticalRadarEventColors(
    int32_t eventType, uint8_t alpha) noexcept {
    // Radar.cpp's authored lookup table, kept as ARGB values here. Event 10
    // (FAKE) is intentionally fully transparent and remains useful only for
    // the "jump to last event" history contract.
    uint32_t primary = 0xffffffffu;
    uint32_t secondary = 0xffffffffu;
    switch (eventType) {
    case 1: primary = 0x008080ffu; secondary = 0x0080ffffu; break;
    case 2: primary = 0x00800040u; secondary = 0x00ffb9dcu; break;
    case 3: primary = 0x00ff0000u; secondary = 0x00ff8080u; break;
    case 4:
    case 5: primary = 0x00ffff00u; secondary = 0x00ffff80u; break;
    case 6: primary = 0x0000ffffu; secondary = 0x0080ffffu; break;
    case 7: primary = secondary = 0x00ffffffu; break;
    case 8:
    case 9: primary = 0x0000ff00u; secondary = 0x00008000u; break;
    case 10: return {.primary = 0u, .secondary = 0u};
    default: break;
    }
    return {
        .primary = withAlpha(primary, alpha),
        .secondary = withAlpha(secondary, alpha),
    };
}

std::optional<TacticalRadarEventTriangle> tacticalRadarEventTriangle(
    const TacticalRadarEventRenderSnapshot& event,
    math::vec2 center, float radarWidth,
    uint64_t simulationFrame,
    uint32_t logicFramesPerSecond) noexcept {
    if (!std::isfinite(center.x()) || !std::isfinite(center.y()) ||
        !std::isfinite(radarWidth) || radarWidth <= 0.0f ||
        logicFramesPerSecond == 0u || simulationFrame > event.dieTick ||
        event.eventType == 10) {
        return std::nullopt;
    }
    const uint64_t ageTicks = simulationFrame >= event.createTick
        ? simulationFrame - event.createTick
        : 0u;
    const float contractionTicks =
        static_cast<float>(logicFramesPerSecond) * 1.5f;
    const float progress = static_cast<float>(ageTicks) / contractionTicks;
    const bool beacon = event.eventType == 5;
    const float maximumRadius = radarWidth * (beacon ? 0.1f : 0.5f);
    const float radius = static_cast<float>(std::max(
        6, static_cast<int32_t>(maximumRadius * (1.0f - progress))));
    // W3DRadar subtracts addAngle from all three base angles. Generic
    // addAngle is positive; beacon addAngle is negative.
    const float rotation = (beacon ? 1.0f : -1.0f) *
        2.0f * kPi * progress;

    uint8_t alpha = 255u;
    if (simulationFrame > event.fadeTick) {
        if (event.dieTick <= event.fadeTick) {
            alpha = 0u;
        } else {
            alpha = static_cast<uint8_t>(std::min<uint64_t>(
                255u, (event.dieTick - simulationFrame) * 255u /
                    (event.dieTick - event.fadeTick)));
        }
    }
    const TacticalRadarEventColors colors =
        tacticalRadarEventColors(event.eventType, alpha);
    if (colors.primary == 0u && colors.secondary == 0u) return std::nullopt;

    TacticalRadarEventTriangle result;
    result.radius = radius;
    result.rotationRadians = rotation;
    result.startColor = colors.primary;
    result.endColor = colors.secondary;
    constexpr container::Array<float, 3> baseAngles{
        0.0f, 2.0f * kPi / 3.0f, -2.0f * kPi / 3.0f};
    for (size_t index = 0; index < result.vertices.size(); ++index) {
        const float angle = baseAngles[index] + rotation;
        result.vertices[index] = math::vec2{
            center.x() + std::cos(angle) * radius,
            center.y() + std::sin(angle) * radius};
    }
    return result;
}

} // namespace engine::render
