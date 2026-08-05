#pragma once

#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/definition/ThingObjectRecipe.h"

#include <cstddef>
#include <cstdint>

namespace engine::render_extraction_detail {

struct RenderChannelRuntimeProjection final {
    const container::Vector<game::ModelConditionVisualRule>* visualRules = nullptr;
    const container::Vector<game::ModelConditionTransitionRule>* transitionRules = nullptr;
    const game::ModelConditionMask* presentedConditions = nullptr;
    const container::String* animationState = nullptr;
    const game::ModelConditionMask* waitingSourceConditions = nullptr;
    const game::ModelConditionMask* animationStartSourceConditions = nullptr;
    float animationTimeSeconds = 0.0f;
    float animationRate = 0.0f;
    float animationRandomStartFraction = 0.0f;
    float animationStartSourceTimeSeconds = 0.0f;
    game::ModelAnimationMode animationMode = game::ModelAnimationMode::Loop;
    VisualAnimationStartKind animationStartKind = VisualAnimationStartKind::Default;
    uint64_t animationStateEnterTick = 0;
    uint64_t animationStateGeneration = 0;
    uint64_t animationResourcePendingGeneration = 0;
    uint64_t animationCandidateOverrideGeneration = 0;
    uint64_t animationStartSourceGeneration = 0;
    uint32_t animationManualFrame = 0;
    uint32_t resolvedVisualRuleIndex = 0;
    uint32_t activeTransitionRuleIndex = 0;
    uint32_t waitingSourceVisualRuleIndex = 0;
    uint32_t animationCandidateOverrideIndex = 0;
    uint32_t animationStartSourceVisualRuleIndex = 0;
    uint8_t animationCompletionMask = 0;
    uint8_t animationResourcePendingPhase = 0;
    bool animationPaused = false;
};

[[nodiscard]] RenderChannelRuntimeProjection projectRenderChannelRuntime(
    const RenderModelComponent& visual,
    const game::ModelDrawVisualChannel* recipe,
    const game::ThingTemplate* templateData,
    const game::ModelConditionMask& presentationConditions,
    size_t channelIndex) noexcept;

} // namespace engine::render_extraction_detail
