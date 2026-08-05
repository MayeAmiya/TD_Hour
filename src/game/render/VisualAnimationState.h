#pragma once

#include <cstdint>

namespace game { struct ThingTemplate; }

namespace engine {

struct RenderModelComponent;

enum class VisualAnimationCompletionPhase : uint8_t {
    PresentedSource,
    Transition,
    ActiveState,
};

struct VisualAnimationCompletion final {
    uint32_t channelIndex = 0;
    uint64_t generation = 0;
    VisualAnimationCompletionPhase phase =
        VisualAnimationCompletionPhase::ActiveState;
    float completedDurationSeconds = 0.0f;
};

// Applies renderer resource admission without transferring clock ownership.
// Pending freezes only the exact generation/phase and rewinds the at-most-one
// feedback-latency tick; Ready starts that same phase from its authored start.
[[nodiscard]] bool applyVisualAnimationResourceGate(
    RenderModelComponent& visual,
    uint32_t channelIndex,
    uint64_t generation,
    VisualAnimationCompletionPhase phase,
    bool pending) noexcept;

// Records that the exact channel generation reached a completed renderer
// endpoint. This is distinct from resource readiness and natural completion:
// hidden/off-screen channels are admitted too, so returning to view cannot
// replay an old transient state.
[[nodiscard]] bool applyVisualAnimationEndpointAdmission(
    RenderModelComponent& visual,
    uint32_t channelIndex,
    uint64_t generation) noexcept;

// Confirmed object facts needed by RefCode Drawable::getShouldAnimate. The
// disabled mask uses ObjectDisabledReason bit positions without making this
// small render-state interface depend on ECS registry ownership.
struct VisualAnimationObjectState final {
    uint64_t objectId = 0;
    uint32_t disabledReasons = 0;
    bool producedAtHelipad = false;
};

[[nodiscard]] bool shouldPauseVisualAnimation(
    bool animationsRequirePower,
    VisualAnimationObjectState objectState) noexcept;

// Drawable::setAnimationFrame forwards one manual frame to every Draw module.
// Preserve that producer-facing contract without requiring gameplay to know
// the selected W3D clip's frame rate.
void setVisualAnimationFrame(
    RenderModelComponent& visual, uint32_t frame) noexcept;

// Advances one logic-owned visual clock. A changed model-condition or
// explicit animation intent starts a new deterministic state at exactly the
// confirmed tick supplied by GameSession; rendering only samples the result.
[[nodiscard]] bool updateVisualAnimationState(
    RenderModelComponent& visual,
    float fixedDeltaSeconds,
    uint64_t confirmedTick,
    const game::ThingTemplate* templateData = nullptr,
    VisualAnimationObjectState objectState = {}) noexcept;

// Admits one renderer-observed natural completion at a confirmed-tick
// boundary. Exact generation/channel matching rejects delayed frames. The
// game owns phase cleanup and idle/restart reselection; renderer clip metadata
// contributes only the completed phase duration.
[[nodiscard]] bool applyVisualAnimationCompletion(
    RenderModelComponent& visual,
    const VisualAnimationCompletion& completion,
    uint64_t confirmedTick,
    const game::ThingTemplate* templateData = nullptr,
    uint64_t objectId = 0) noexcept;

} // namespace engine
