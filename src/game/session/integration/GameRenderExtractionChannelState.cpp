#include "GameRenderExtractionChannelState.h"

namespace engine::render_extraction_detail {

RenderChannelRuntimeProjection projectRenderChannelRuntime(
    const RenderModelComponent& visual,
    const game::ModelDrawVisualChannel* recipe,
    const game::ThingTemplate* templateData,
    const game::ModelConditionMask& presentationConditions,
    size_t channelIndex) noexcept {
    const RenderModelChannelState* state = channelIndex < visual.channels.size()
        ? &visual.channels[channelIndex] : nullptr;
    const bool initialized = state
        ? state->animationStateInitialized
        : visual.animationStateInitialized;
    RenderChannelRuntimeProjection result;
    result.visualRules = recipe ? &recipe->conditionVisuals
        : templateData ? &templateData->modelConditionVisuals : nullptr;
    result.transitionRules = recipe ? &recipe->transitions
        : templateData ? &templateData->modelConditionTransitions : nullptr;
    result.presentedConditions = initialized
        ? (state ? &state->animationConditionSnapshot
                 : &visual.animationConditionSnapshot)
        : &presentationConditions;
    result.animationState = state
        ? (initialized ? &state->animationStateSnapshot : &state->animationState)
        : (initialized ? &visual.animationStateSnapshot : &visual.animationState);
    result.waitingSourceConditions = state
        ? &state->waitingSourceConditionSnapshot
        : &visual.waitingSourceConditionSnapshot;
    result.animationStartSourceConditions = state
        ? &state->animationStartSourceConditionSnapshot
        : &visual.animationStartSourceConditionSnapshot;
    result.animationTimeSeconds = state ? state->animationTimeSeconds : visual.animationTimeSeconds;
    result.animationRate = state ? state->animationRate : visual.animationRate;
    result.animationMode = state ? state->animationMode : visual.animationMode;
    result.animationManualFrame = state ? state->animationManualFrame : visual.animationManualFrame;
    result.animationPaused = state
        ? state->animationPaused || state->animationPausedByObjectState
        : visual.animationPaused || visual.animationPausedByObjectState;
    result.animationStateEnterTick = state ? state->animationStateEnterTick : visual.animationStateEnterTick;
    result.resolvedVisualRuleIndex = state ? state->resolvedVisualRuleIndex : visual.resolvedVisualRuleIndex;
    result.activeTransitionRuleIndex = state ? state->activeTransitionRuleIndex : visual.activeTransitionRuleIndex;
    result.waitingSourceVisualRuleIndex = state ? state->waitingSourceVisualRuleIndex : visual.waitingSourceVisualRuleIndex;
    result.animationStateGeneration = state ? state->animationStateGeneration : visual.animationStateGeneration;
    result.animationCompletionMask = state ? state->animationCompletionMask : visual.animationCompletionMask;
    result.animationResourcePendingGeneration = state ? state->animationResourcePendingGeneration : visual.animationResourcePendingGeneration;
    result.animationResourcePendingPhase = state ? state->animationResourcePendingPhase : visual.animationResourcePendingPhase;
    result.animationCandidateOverrideIndex = state ? state->animationCandidateOverrideIndex : visual.animationCandidateOverrideIndex;
    result.animationCandidateOverrideGeneration = state ? state->animationCandidateOverrideGeneration : visual.animationCandidateOverrideGeneration;
    result.animationStartKind = state ? state->animationStartKind : visual.animationStartKind;
    result.animationRandomStartFraction = state ? state->animationRandomStartFraction : visual.animationRandomStartFraction;
    result.animationStartSourceVisualRuleIndex = state ? state->animationStartSourceVisualRuleIndex : visual.animationStartSourceVisualRuleIndex;
    result.animationStartSourceTimeSeconds = state ? state->animationStartSourceTimeSeconds : visual.animationStartSourceTimeSeconds;
    result.animationStartSourceGeneration = state ? state->animationStartSourceGeneration : visual.animationStartSourceGeneration;
    return result;
}

} // namespace engine::render_extraction_detail
