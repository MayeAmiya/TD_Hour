#pragma once

#include "presentation/render/RenderWorldDescriptorContracts.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace engine::render {

[[nodiscard]] uint32_t supplyBonesToShow(
    uint32_t totalBones, uint32_t currentSupply,
    uint32_t maximumSupply) noexcept;

[[nodiscard]] float policeCarInitialFrame(
    RenderEntityId objectId, uint32_t channelIndex) noexcept;

[[nodiscard]] float policeCarAnimationFrame(
    RenderEntityId objectId, uint32_t channelIndex,
    uint64_t simulationFrame, float animationFrameCount = 15.0f) noexcept;

[[nodiscard]] float policeCarAdvanceFrame(
    float initialFrame, uint64_t drawCount,
    float animationFrameCount = 15.0f) noexcept;

[[nodiscard]] constexpr uint64_t policeCarAgeTick(
    uint64_t simulationFrame, uint64_t createdAtTick) noexcept {
    return simulationFrame >= createdAtTick
        ? simulationFrame - createdAtTick : 0u;
}

[[nodiscard]] RenderVector policeCarLightColor(float animationFrame) noexcept;

[[nodiscard]] constexpr float policeCarLightFade(
    uint64_t simulationFrame, uint64_t lastSeenFrame,
    uint32_t fadeFrames = 5u) noexcept {
    if (simulationFrame <= lastSeenFrame) return 1.0f;
    const uint64_t elapsed = simulationFrame - lastSeenFrame;
    if (fadeFrames == 0u || elapsed >= fadeFrames) return 0.0f;
    return 1.0f - static_cast<float>(elapsed) /
        static_cast<float>(fadeFrames);
}

struct ResolvedDebrisAnimation final {
    container::String animation;
    float timeSeconds = 0.0f;
    RenderAnimationMode mode = RenderAnimationMode::Loop;
    uint32_t manualFrame = 0;
};

[[nodiscard]] ResolvedDebrisAnimation resolveDebrisAnimation(
    const RenderDebrisState& state,
    std::optional<float> initialDurationSeconds) noexcept;

[[nodiscard]] uint64_t beaconAttachmentGroup(
    uint64_t objectId, uint32_t authoredOrder) noexcept;

[[nodiscard]] constexpr bool beaconVisibleToObserver(
    bool hasLocalObserver, bool alliedOrSpectating,
    bool drawableEffectivelyHidden) noexcept {
    return hasLocalObserver && alliedOrSpectating &&
        !drawableEffectivelyHidden;
}

} // namespace engine::render
