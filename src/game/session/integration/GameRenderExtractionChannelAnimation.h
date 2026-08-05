#pragma once

#include "GameRenderExtractionChannelState.h"
#include "GameRenderExtractionDetail.h"

#include <cstddef>
#include <cstdint>

namespace engine::script {
class ScriptObjectPresentationState;
}

namespace engine::render_extraction_detail {

struct ChannelAnimationSource final {
    const ecs::registry& registry;
    const WeaponPresentationSource& weaponPresentation;
    const script::ScriptObjectPresentationState& objectPresentation;
    const RenderFeatureQualitySettings& featureQuality;
    uint64_t simulationFrame = 0;
    int32_t logicFramesPerSecond = 1;
};

struct ChannelAnimationInput final {
    ecs::entity entity = ecs::null;
    size_t channelIndex = 0;
    const ObjectIdentityComponent& identity;
    const RenderModelComponent& visual;
    const game::ModelDrawVisualChannel* recipe = nullptr;
    const game::ThingTemplate* templateData = nullptr;
    const ObjectWeaponComponent* weapons = nullptr;
    const RenderChannelRuntimeProjection& runtime;
    const game::ModelConditionMask& presentationConditions;
    container::StringView archetypeName;
    size_t normalPoseRuleCount = 0;
    size_t visualRuleOffset = 0;
    size_t transitionRuleOffset = 0;
    uint64_t supplyCurrent = 0;
    uint64_t supplyMaximum = 0;
};

struct ChannelAnimationResult final {
    float ruleRateFactor = 1.0f;
    float authoredAssetScale = 1.0f;
    bool drawable = false;
    // RefCode W3DModelDraw::adjustTransformMtx tests the *current* condition
    // state's ADJUST_HEIGHT_BY_CONSTRUCTION_PERCENT flag before sinking the
    // model into the ground. Publish the resolved rule's authored flag so the
    // post-process (which owns the final world transform) does not have to
    // re-select the visual rule.
    bool adjustHeightByConstructionPercent = false;
};

[[nodiscard]] ChannelAnimationResult applyRenderChannelAnimation(
    const ChannelAnimationSource& source,
    const ChannelAnimationInput& input,
    render::RenderEntitySnapshot& output);

} // namespace engine::render_extraction_detail
