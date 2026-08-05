#pragma once

#include "presentation/render/RenderWorldDescriptorContracts.h"
#include "presentation/render/TrackMarksPerformanceSettings.h"
#include "presentation/render/TrackMarksVisualSettings.h"

namespace engine::render {

struct RenderEntitySnapshot {
    RenderEntityId id = 0;
    container::String modelAsset;
    RenderTransform transform;
    RenderVisualState visual;
    float boundingRadius = 0.0f;
    // RefCode swaps a separately scaled directional-light environment only
    // for KindOf INFANTRY.  This scalar is already classified at extraction
    // time, so world rendering never needs a ThingTemplate/ECS lookup.
    float directionalLightScale = 1.0f;
    // Active W3D condition-state launch bones in PRIMARY/SECONDARY/TERTIARY
    // order. Names remain backend-neutral until WorldRenderPipeline resolves
    // them against the immutable skeleton pose prepared for this frame.
    container::Array<container::String, kRenderWeaponSlotCount> weaponLaunchBones;
    container::Array<uint32_t, kRenderWeaponSlotCount>
        weaponLaunchBoneSequenceOrdinals{};
    std::optional<RenderAnimationCompletionTarget> animationCompletionTarget;
    RenderAnimationCompletionPhase animationCompletionPhase =
        RenderAnimationCompletionPhase::Transition;
    // WaitForStateToFinishIfPossible can require three sealed phases:
    // current Once state, an authored TransitionState, then the requested
    // ConditionState. Ordinary transitions use only completionTarget.
    std::optional<RenderAnimationCompletionTarget> animationFinalTarget;
    // `id` identifies one render instance. `objectId` remains the canonical
    // game object identity when an object owns several simultaneous Draw
    // modules; channelIndex is stable in the final compiled Draw order.
    // These fields stay at the end to preserve the historical aggregate
    // prefix used by focused renderer probes.
    RenderEntityId objectId = 0;
    uint32_t channelIndex = 0;
    RenderShadowDescriptor shadow;
    LocalVisibilityRenderCellState localVisibilityState =
        LocalVisibilityRenderCellState::Visible;
    RenderLocalVisibilityMemoryPolicy localVisibilityMemoryPolicy =
        RenderLocalVisibilityMemoryPolicy::None;
    // Total grace from the last clear frame. RefCode uses 2 seconds for a
    // live drawable and 5 seconds (2 + 3) when it is effectively dead.
    uint32_t localVisibilityPersistenceTicks = 0;
    bool hiddenByLocalVisibility = false;
    // Frozen explored-terrain ghosts are renderer memory, not the live
    // Drawable. They must never acknowledge a clip on behalf of a current or
    // destroyed game object that happens to share the retained ObjectId.
    bool animationCompletionFeedbackEnabled = true;
    // W3DDependencyModelDraw is cleared only after its non-enclosing
    // container has produced the current pose. Keep the relationship as
    // stable value data: the renderer resolves the named bone from this
    // frame's prepared container channel, never from ECS or a Drawable.
    // A missing bone deliberately falls back to the container root, matching
    // RefCode adjustTransformMtx(); a missing/hidden container suppresses the
    // dependent draw entirely.
    RenderEntityId containerObjectId = 0;
    container::String attachToBoneInContainer;
    // W3DModelDraw AttachToBoneInAnotherModule. The shipped RefCode enables
    // CACHE_ATTACH_BONE: the named bone is resolved from another Draw channel
    // of the SAME object, but only its pristine model-space translation is
    // applied through this channel's own object basis. It must not inherit the
    // parent bone's current rotation. Shipped content uses this for the GLA
    // Technical gunner riding the chassis module's Dum_Turret offset.
    container::String attachToBoneInAnotherModule;
    std::optional<RenderVector> attachToBoneInAnotherModuleOffset;
    // A confirmed presentation event (for example a freshly created
    // channel) may share the same model and remain below the geometric snap
    // distance. Such an endpoint is still a discontinuity and must not be
    // blended with the preceding endpoint.
    bool interpolationDisabled = false;
    // World-axis offset from the rendered root to the conservative culling
    // sphere centre. Gameplay Cylinder/Box geometry starts at position.z and
    // extends upward, so using the root with a half-height radius clips tall
    // objects at the view edge. This offset deliberately does not rotate with
    // the visual model transform.
    RenderVector cullingCenterOffset{};
};

// One logic-frame sample for renderer-owned vehicle track history. The game
// publishes no previous position, GPU handle, edge ring or fade timer; those
// remain renderer presentation state. Confirmed samples are consumed in order
// and their live anchors are interpolated with the same A/B as the model.
struct TrackMarkRenderInput final {
    RenderEntityId objectId = 0;
    RenderVector position{};
    RenderVector forward{1.0f, 0.0f, 0.0f};
    container::String textureName;
    // Width stays renderer-derived because the authored W3D hierarchy is a
    // render asset. These detached names/addition are the complete immutable
    // descriptor input; WorldRenderPipeline never needs to revisit ECS/INI.
    container::String leftWidthBone{
        game::track_marks::visual_defaults::kLeftWidthBone};
    container::String rightWidthBone{
        game::track_marks::visual_defaults::kRightWidthBone};
    float additionalTreadWidth =
        game::track_marks::visual_defaults::kAdditionalTreadWidth;
    float trackWidth = game::track_marks::visual_defaults::kFallbackWidth;
    float edgeSpacing = game::track_marks::visual_defaults::kSegmentLength;
    uint32_t maximumEdges = engine::kHighTrackMarksBudget.maximumEdges;
    uint32_t opaqueEdges = engine::kHighTrackMarksBudget.opaqueEdges;
    uint32_t fadeLifetimeFrames =
        engine::track_marks::performance_limits::fadeFramesFromMilliseconds(
            engine::kHighTrackMarksBudget.fadeDelayMilliseconds);
    bool moving = false;
};


} // namespace engine::render
