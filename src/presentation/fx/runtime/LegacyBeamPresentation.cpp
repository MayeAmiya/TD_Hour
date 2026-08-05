#include "presentation/fx/runtime/LegacyBeamPresentation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace engine::fx {
namespace {

[[nodiscard]] uint64_t mix64(uint64_t value) noexcept {
    value ^= value >> 30u;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27u;
    value *= 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

} // namespace

std::optional<LegacyModelRayState> makeLegacyModelRay(
    const LegacyBeamTemplate& descriptor, ParticleVector3 start,
    ParticleVector3 end, uint64_t deterministicIdentity) {
    if (descriptor.kind != LegacyBeamTemplateKind::ModelRay ||
        descriptor.modelAsset.empty() || !std::isfinite(start.x) ||
        !std::isfinite(start.y) || !std::isfinite(start.z) ||
        !std::isfinite(end.x) || !std::isfinite(end.y) ||
        !std::isfinite(end.z) ||
        !std::isfinite(descriptor.modelRay.assetScale)) {
        return std::nullopt;
    }

    LegacyModelRayState output{
        .modelAsset = descriptor.modelAsset,
        // Multiplying before addition avoids overflowing a finite authored
        // endpoint pair while preserving W3DGameClient's exact midpoint rule.
        .position = {
            start.x * 0.5f + end.x * 0.5f,
            start.y * 0.5f + end.y * 0.5f,
            start.z * 0.5f + end.z * 0.5f,
        },
        .assetScale = descriptor.modelRay.assetScale,
        .castsDirectionalShadow =
            descriptor.modelRay.castsDirectionalShadow,
    };
    if (!std::isfinite(output.position.x) ||
        !std::isfinite(output.position.y) ||
        !std::isfinite(output.position.z)) {
        return std::nullopt;
    }
    if (!descriptor.modelRay.lifetimeAuthored) return output;

    const uint32_t minimum = descriptor.modelRay.minimumLifetimeMilliseconds;
    const uint32_t maximum = descriptor.modelRay.maximumLifetimeMilliseconds;
    // RefCode's GameLogicRandomValue returns the second endpoint for equal or
    // inverted ranges.  Preserve that unusual Mod-visible rule here.
    uint32_t milliseconds = maximum;
    if (minimum < maximum) {
        const uint64_t width = static_cast<uint64_t>(maximum) - minimum + 1u;
        milliseconds = minimum + static_cast<uint32_t>(
            mix64(deterministicIdentity) % width);
    }
    constexpr uint64_t authoredFramesPerSecond = 30u;
    const uint64_t frames = milliseconds == 0u ? 0u :
        (static_cast<uint64_t>(milliseconds) * authoredFramesPerSecond +
         999u) / 1000u;
    // A zero-duration LifetimeUpdate/DeletionUpdate wakes on the next logic
    // frame; zero is reserved above for a template with no timer at all.
    output.lifetimeFrames = static_cast<uint32_t>(std::min<uint64_t>(
        std::max<uint64_t>(1u, frames),
        std::numeric_limits<uint32_t>::max()));
    return output;
}

bool legacyModelRayAlive(const LegacyModelRayState& state,
                         uint64_t admittedFrame,
                         uint64_t simulationFrame) noexcept {
    if (state.lifetimeFrames == 0 || simulationFrame < admittedFrame) {
        return true;
    }
    return simulationFrame - admittedFrame < state.lifetimeFrames;
}

container::Vector<LegacyLaserSegment> buildLegacyLaserSegments(
    const LegacyLaserTemplate& descriptor,
    ParticleVector3 start, ParticleVector3 end) {
    container::Vector<LegacyLaserSegment> result;
    buildLegacyLaserSegmentsInto(result, descriptor, start, end);
    return result;
}

void buildLegacyLaserSegmentsInto(
    container::Vector<LegacyLaserSegment>& result,
    const LegacyLaserTemplate& descriptor,
    ParticleVector3 start, ParticleVector3 end) {
    result.clear();
    const uint32_t count = std::max(1u, descriptor.segments);
    result.reserve(count);
    const ParticleVector3 delta{end.x - start.x, end.y - start.y, end.z - start.z};
    const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y +
                                   delta.z * delta.z);
    if (!std::isfinite(length) || length <= 1.0e-6f) return;
    for (uint32_t index = 0; index < count; ++index) {
        float startRatio = static_cast<float>(index) / static_cast<float>(count);
        float endRatio = static_cast<float>(index + 1u) / static_cast<float>(count);
        if (index > 0u) startRatio -= descriptor.segmentOverlapRatio;
        if (index + 1u < count) endRatio += descriptor.segmentOverlapRatio;
        const auto point = [&](float ratio) {
            ParticleVector3 output{
                start.x + delta.x * ratio,
                start.y + delta.y * ratio,
                start.z + delta.z * ratio,
            };
            if (descriptor.arcHeight > 0.0f && count > 1u) {
                const float distanceFromMiddle = std::abs(ratio - 0.5f) * length;
                const float halfLength = length * 0.5f;
                const float radians = distanceFromMiddle / halfLength *
                    std::numbers::pi_v<float> * 0.5f;
                output.z += std::cos(radians) * descriptor.arcHeight;
            }
            return output;
        };
        result.push_back({.start = point(startRatio), .end = point(endRatio)});
    }
}

float legacyLaserTextureTileFactor(
    const LegacyLaserTemplate& descriptor, float segmentLength,
    float beamWidth, float textureAspectRatio) noexcept {
    if (!descriptor.tileTexture || !std::isfinite(segmentLength) ||
        !std::isfinite(beamWidth) || !std::isfinite(textureAspectRatio) ||
        !(segmentLength > 0.0f) || !(beamWidth > 0.0f) ||
        !(textureAspectRatio > 0.0f)) {
        return 1.0f;
    }
    const float result = segmentLength / beamWidth * textureAspectRatio *
        descriptor.tilingScalar;
    // Do not clamp authored TilingScalar: retail left the old positive clamp
    // behind a disabled build flag, so zero/negative Mod values remain
    // observable as a collapsed/flipped V axis.
    return std::isfinite(result) ? result : 1.0f;
}

std::optional<LegacyRopeState> makeLegacyRope(
    ParticleVector3 origin, float length, float width,
    LegacyBeamColor color, float wobbleLength, float wobbleAmplitude,
    float wobbleRatePerFrame, uint64_t deterministicIdentity) {
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) ||
        !std::isfinite(origin.z) || !std::isfinite(length) ||
        !std::isfinite(width) || !std::isfinite(wobbleLength) ||
        !std::isfinite(wobbleAmplitude) || !std::isfinite(wobbleRatePerFrame) ||
        width <= 0.0f || wobbleLength <= 0.0f) return std::nullopt;
    LegacyRopeState result;
    result.origin = origin;
    result.maximumLength = std::max(1.0f, length);
    result.width = width;
    result.color = color;
    result.wobbleLength = std::min(result.maximumLength, wobbleLength);
    result.wobbleAmplitude = wobbleAmplitude;
    result.wobbleRatePerFrame = wobbleRatePerFrame;
    const uint32_t count = std::max(1u, static_cast<uint32_t>(
        std::ceil(result.maximumLength / result.wobbleLength)));
    result.wobbleAxisRadians.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        const uint64_t bits = mix64(deterministicIdentity ^
                                    (static_cast<uint64_t>(index) + 1u));
        const float unit = static_cast<float>(bits >> 40u) /
            static_cast<float>(1u << 24u);
        result.wobbleAxisRadians.push_back(
            unit * std::numbers::pi_v<float> * 2.0f);
    }
    return result;
}

void setLegacyRopeLength(LegacyRopeState& state, float length) noexcept {
    if (std::isfinite(length)) state.currentLength = length;
}

void setLegacyRopeSpeed(LegacyRopeState& state, float currentSpeedPerFrame,
                        float maximumSpeedPerFrame,
                        float accelerationPerFrame) noexcept {
    if (std::isfinite(currentSpeedPerFrame)) {
        state.currentSpeedPerFrame = currentSpeedPerFrame;
    }
    if (std::isfinite(maximumSpeedPerFrame)) {
        state.maximumSpeedPerFrame = std::abs(maximumSpeedPerFrame);
    }
    if (std::isfinite(accelerationPerFrame)) {
        state.accelerationPerFrame = accelerationPerFrame;
    }
}

void advanceLegacyRope(LegacyRopeState& state, uint32_t authoredFrames) noexcept {
    for (uint32_t frame = 0; frame < authoredFrames; ++frame) {
        state.wobblePhase += state.wobbleRatePerFrame;
        if (state.wobblePhase > std::numbers::pi_v<float> * 2.0f) {
            state.wobblePhase -= std::numbers::pi_v<float> * 2.0f;
        }
        state.verticalOffset += state.currentSpeedPerFrame;
        state.currentSpeedPerFrame += state.accelerationPerFrame;
        state.currentSpeedPerFrame = std::clamp(
            state.currentSpeedPerFrame, -state.maximumSpeedPerFrame,
            state.maximumSpeedPerFrame);
    }
}

container::Vector<LegacyLaserSegment> buildLegacyRopeSegments(
    const LegacyRopeState& state) {
    container::Vector<LegacyLaserSegment> result;
    buildLegacyRopeSegmentsInto(result, state);
    return result;
}

void buildLegacyRopeSegmentsInto(
    container::Vector<LegacyLaserSegment>& result,
    const LegacyRopeState& state) {
    result.clear();
    if (state.wobbleAxisRadians.empty()) return;
    result.reserve(state.wobbleAxisRadians.size());
    const float deflection = std::sin(state.wobblePhase) * state.wobbleAmplitude;
    const float eachLength = state.currentLength /
        static_cast<float>(state.wobbleAxisRadians.size());
    ParticleVector3 start{state.origin.x, state.origin.y,
                          state.origin.z + state.verticalOffset};
    for (const float axis : state.wobbleAxisRadians) {
        ParticleVector3 end{
            state.origin.x + deflection * std::cos(axis),
            state.origin.y + deflection * std::sin(axis),
            start.z - eachLength,
        };
        result.push_back({.start = start, .end = end});
        start = end;
    }
}

} // namespace engine::fx
