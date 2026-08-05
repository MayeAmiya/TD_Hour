#include "engine/renderer/world/effects/TypedFxBeamPresentation.h"

#include <algorithm>
#include <cmath>

namespace engine::render {
namespace {

constexpr float kLegacyLogicFrameSeconds = 1.0f / 30.0f;

[[nodiscard]] bool finiteVector(const RenderVector& value) noexcept {
    return std::isfinite(value.x()) && std::isfinite(value.y()) &&
        std::isfinite(value.z());
}

} // namespace

RenderVector TypedFxTracerState::start() const noexcept {
    return origin + direction * (speedPerSecond * ageSeconds);
}

RenderVector TypedFxTracerState::end() const noexcept {
    return start() + direction * length;
}

float TypedFxTracerState::opacity() const noexcept {
    if (!(lifetimeSeconds > 0.0f) || !std::isfinite(lifetimeSeconds)) {
        return 0.0f;
    }
    return 1.0f - std::clamp(ageSeconds / lifetimeSeconds, 0.0f, 1.0f);
}

bool TypedFxTracerState::expired() const noexcept {
    return ageSeconds >= lifetimeSeconds;
}

void TypedFxTracerState::advance(float deltaSeconds) noexcept {
    if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f) return;
    ageSeconds = std::min(ageSeconds + deltaSeconds, lifetimeSeconds);
}

std::optional<TypedFxTracerState> makeTypedFxTracer(
    RenderVector source, RenderVector target, RenderVector color,
    float speedPerSecond, float decayAt, float length, float width) noexcept {
    const RenderVector delta = target - source;
    const float distanceSquared = delta.length_sq();
    if (!finiteVector(source) || !finiteVector(target) || !finiteVector(color) ||
        !std::isfinite(distanceSquared) ||
        distanceSquared <= math::EPSILON * math::EPSILON ||
        !std::isfinite(speedPerSecond) || speedPerSecond < 0.0f ||
        !std::isfinite(decayAt) || decayAt <= 0.0f ||
        !std::isfinite(length) || length <= 0.0f ||
        !std::isfinite(width) || width <= 0.0f) {
        return std::nullopt;
    }

    const float distance = std::sqrt(distanceSquared);
    const float travelDistance = distance - length;
    // RefCode uses one frame when the tracer already spans the destination.
    // Its speed==0/dist>=0 division is undefined; keep the visible one-frame
    // fallback instead of manufacturing a multi-second stationary ribbon.
    const float travelSeconds = travelDistance >= 0.0f && speedPerSecond > 0.0f
        ? travelDistance / speedPerSecond
        : kLegacyLogicFrameSeconds;
    const float lifetime = std::max(
        travelSeconds * decayAt, kLegacyLogicFrameSeconds);
    if (!std::isfinite(lifetime)) return std::nullopt;

    return TypedFxTracerState{
        .origin = source,
        .direction = delta / distance,
        .color = color,
        .length = length,
        .width = width,
        .speedPerSecond = speedPerSecond,
        .lifetimeSeconds = lifetime,
    };
}

} // namespace engine::render
