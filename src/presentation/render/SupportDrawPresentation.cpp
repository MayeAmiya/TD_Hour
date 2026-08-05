#include "presentation/render/SupportDrawPresentation.h"

#include <limits>

namespace engine::render {

uint32_t supplyBonesToShow(uint32_t totalBones, uint32_t currentSupply,
                           uint32_t maximumSupply) noexcept {
    if (totalBones == 0 || maximumSupply == 0 || currentSupply == 0) return 0;
    if (currentSupply >= maximumSupply) return totalBones;
    const uint64_t numerator = static_cast<uint64_t>(totalBones) * currentSupply;
    return static_cast<uint32_t>(std::min<uint64_t>(
        totalBones, (numerator + maximumSupply - 1u) / maximumSupply));
}

float policeCarInitialFrame(RenderEntityId objectId,
                            uint32_t channelIndex) noexcept {
    uint64_t value = objectId ^
        (static_cast<uint64_t>(channelIndex) + 0x9e3779b97f4a7c15ull);
    value ^= value >> 30u;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27u;
    value *= 0x94d049bb133111ebull;
    value ^= value >> 31u;
    constexpr float inverse = 1.0f / 16777215.0f;
    return static_cast<float>(value & 0x00ffffffu) * inverse * 10.0f;
}

float policeCarAnimationFrame(RenderEntityId objectId, uint32_t channelIndex,
                              uint64_t simulationFrame,
                              float animationFrameCount) noexcept {
    return policeCarAdvanceFrame(
        policeCarInitialFrame(objectId, channelIndex),
        simulationFrame == std::numeric_limits<uint64_t>::max()
            ? simulationFrame : simulationFrame + 1u,
        animationFrameCount);
}

float policeCarAdvanceFrame(float initialFrame, uint64_t drawCount,
                            float animationFrameCount) noexcept {
    const float frames = std::isfinite(animationFrameCount)
        ? std::max(1.0f, animationFrameCount) : 15.0f;
    const double maximumFrame = static_cast<double>(frames) - 1.0;
    const double initial = std::isfinite(initialFrame)
        ? std::clamp(static_cast<double>(initialFrame), 0.0, maximumFrame)
        : 0.0;
    constexpr double increment = 0.25;
    const uint64_t drawsUntilReset = static_cast<uint64_t>(std::floor(
        std::max(0.0, maximumFrame - initial) / increment)) + 1u;
    if (drawCount < drawsUntilReset) {
        return static_cast<float>(initial +
            static_cast<double>(drawCount) * increment);
    }
    const uint64_t cycleDraws = static_cast<uint64_t>(std::floor(
        maximumFrame / increment)) + 1u;
    const uint64_t afterFirstReset = drawCount - drawsUntilReset;
    return static_cast<float>(afterFirstReset % cycleDraws) *
        static_cast<float>(increment);
}

RenderVector policeCarLightColor(float frame) noexcept {
    if (!std::isfinite(frame)) return {};
    RenderVector color{};
    if (frame < 3.0f) {
        color = {1.0f, 0.5f, 0.0f};
    } else if (frame < 6.0f) {
        color = {1.0f, 0.0f, 0.0f};
    } else if (frame < 7.0f) {
        color = {1.0f, 0.5f, 0.0f};
    } else if (frame < 9.0f) {
        color = {0.5f + (9.0f - frame) / 4.0f,
                 0.0f, (frame - 5.0f) / 6.0f};
    } else if (frame < 12.0f) {
        color = {0.0f, 0.0f, 1.0f};
    } else if (frame <= 14.0f) {
        color = {(frame - 11.0f) / 3.0f,
                 (frame - 11.0f) / 3.0f,
                 (14.0f - frame) / 2.0f};
    }
    return {
        std::clamp(color.x(), 0.0f, 1.0f),
        std::clamp(color.y(), 0.0f, 1.0f),
        std::clamp(color.z(), 0.0f, 1.0f),
    };
}

ResolvedDebrisAnimation resolveDebrisAnimation(
    const RenderDebrisState& state,
    std::optional<float> initialDurationSeconds) noexcept {
    const float framesPerSecond = static_cast<float>(
        std::max(1u, state.logicFramesPerSecond));
    if (state.finalState) {
        return {
            .animation = state.finalAnimation,
            .timeSeconds = static_cast<float>(state.finalAgeFrames) /
                framesPerSecond,
            .mode = state.finalStop
                ? RenderAnimationMode::Manual : RenderAnimationMode::Once,
            .manualFrame = 0,
        };
    }
    float ageSeconds = static_cast<float>(state.ageFrames) / framesPerSecond;
    if (!state.initialAnimation.empty() && !initialDurationSeconds) {
        // External HAnim metadata may arrive one frame after the model. Do
        // not flash the flying clip and then jump backwards into Initial;
        // name the authored initial phase and hold its bind/first pose until
        // the duration becomes available.
        return {
            .animation = state.initialAnimation,
            .timeSeconds = 0.0f,
            .mode = RenderAnimationMode::Once,
        };
    }
    if (!state.initialAnimation.empty() && initialDurationSeconds &&
        std::isfinite(*initialDurationSeconds) && *initialDurationSeconds > 0.0f &&
        ageSeconds < *initialDurationSeconds) {
        return {
            .animation = state.initialAnimation,
            .timeSeconds = ageSeconds,
            .mode = RenderAnimationMode::Once,
        };
    }
    if (initialDurationSeconds && std::isfinite(*initialDurationSeconds) &&
        *initialDurationSeconds > 0.0f) {
        ageSeconds = std::max(0.0f, ageSeconds - *initialDurationSeconds);
    }
    return {
        .animation = state.flyingAnimation,
        .timeSeconds = ageSeconds,
        .mode = RenderAnimationMode::Loop,
    };
}

uint64_t beaconAttachmentGroup(uint64_t objectId,
                               uint32_t authoredOrder) noexcept {
    uint64_t value = objectId ^ 0x424541434f4e0000ull ^
        (static_cast<uint64_t>(authoredOrder) << 1u);
    if (value == 0) value = 1;
    return value;
}

} // namespace engine::render
