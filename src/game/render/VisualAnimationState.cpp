#include "core/container/container_types.h"
#include "VisualAnimationState.h"

#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/contracts/ObjectDisabledTypes.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine {

namespace {

[[nodiscard]] game::ModelAnimationFlags animationFlagsForRule(
    const container::Vector<game::ModelConditionVisualRule>* rules,
    uint32_t index) noexcept {
    return rules && index < rules->size()
        ? (*rules)[index].animationFlags : 0;
}

[[nodiscard]] uint64_t nextAnimationGeneration(uint64_t current) noexcept {
    ++current;
    return current == 0 ? 1 : current;
}

[[nodiscard]] float deterministicAnimationUnit(
    uint64_t objectId, uint32_t channelIndex,
    uint64_t generation) noexcept {
    uint64_t value = objectId ^
        (static_cast<uint64_t>(channelIndex) << 32u) ^ generation;
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30u)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27u)) * 0x94D049BB133111EBull;
    value ^= value >> 31u;
    return static_cast<float>(value >> 40u) * (1.0f / 16777216.0f);
}

constexpr game::ModelAnimationFlags kMaintainFrameFlags =
    game::modelAnimationFlagBit(
        game::ModelAnimationFlag::MaintainFrameAcrossStates) |
    game::modelAnimationFlagBit(
        game::ModelAnimationFlag::MaintainFrameAcrossStates2) |
    game::modelAnimationFlagBit(
        game::ModelAnimationFlag::MaintainFrameAcrossStates3) |
    game::modelAnimationFlagBit(
        game::ModelAnimationFlag::MaintainFrameAcrossStates4);

template <typename State>
void beginAnimationGeneration(
    State& state, game::ModelAnimationFlags destinationFlags,
    uint64_t objectId, uint32_t channelIndex,
    uint32_t sourceRuleIndex,
    const game::ModelConditionMask& sourceConditions,
    float sourceTimeSeconds, uint64_t sourceGeneration,
    game::ModelAnimationFlags sourceFlags) noexcept {
    state.animationStateGeneration = nextAnimationGeneration(
        state.animationStateGeneration);
    state.animationCompletionMask = 0;
    state.animationResourcePendingGeneration = 0;
    state.animationResourcePendingPhase = UINT8_MAX;
    state.animationCandidateOverrideIndex = UINT32_MAX;
    state.animationCandidateOverrideGeneration = 0;
    state.activeAnimationFlags = destinationFlags;
    state.animationStartKind = VisualAnimationStartKind::Default;
    state.animationStartSourceVisualRuleIndex = UINT32_MAX;
    state.animationStartSourceConditionSnapshot = {};
    state.animationStartSourceTimeSeconds = 0.0f;
    state.animationStartSourceGeneration = 0;

    // adjustAnimation's ordering is observable: RANDOMSTART overrides both
    // explicit endpoints, which in turn override all maintain-frame lanes.
    if ((destinationFlags & game::modelAnimationFlagBit(
            game::ModelAnimationFlag::RandomStart)) != 0) {
        state.animationStartKind = VisualAnimationStartKind::RandomFrame;
        state.animationRandomStartFraction = deterministicAnimationUnit(
            objectId, channelIndex, state.animationStateGeneration);
    } else if ((destinationFlags & game::modelAnimationFlagBit(
                   game::ModelAnimationFlag::StartFrameFirst)) != 0) {
        state.animationStartKind = VisualAnimationStartKind::FirstFrame;
    } else if ((destinationFlags & game::modelAnimationFlagBit(
                   game::ModelAnimationFlag::StartFrameLast)) != 0) {
        state.animationStartKind = VisualAnimationStartKind::LastFrame;
    } else if (sourceRuleIndex != UINT32_MAX &&
               (destinationFlags & sourceFlags & kMaintainFrameFlags) != 0) {
        state.animationStartKind =
            VisualAnimationStartKind::MaintainFraction;
        state.animationStartSourceVisualRuleIndex = sourceRuleIndex;
        state.animationStartSourceConditionSnapshot = sourceConditions;
        state.animationStartSourceTimeSeconds = std::max(
            0.0f, sourceTimeSeconds);
        state.animationStartSourceGeneration = sourceGeneration;
    }
}

template <typename State>
void updateChannelAnimationState(
    State& state,
    const game::ModelConditionMask& modelConditionFlags,
    uint64_t conditionAnimationRevision,
    float fixedDeltaSeconds,
    uint64_t confirmedTick,
    const container::Vector<game::ModelConditionVisualRule>* visualRules,
    const container::Vector<game::ModelConditionTransitionRule>* transitions,
    uint32_t selectedRuleIndex,
    uint32_t initialRuleIndex,
    bool endpointAdmissionEnabled,
    bool pausedByObjectState, uint64_t objectId,
    uint32_t channelIndex) noexcept {
    state.animationPausedByObjectState = pausedByObjectState;
    if (!state.animationStateInitialized) {
        // W3DModelDraw constructs every Draw module in its empty/default
        // ConditionState before Object publishes any model-condition flags.
        // A newly placed structure therefore enters AWAITING_CONSTRUCTION
        // from DOWN_DEFAULT and is allowed to select the authored
        // DOWN_DEFAULT -> UP_* transition.  Initializing directly to the
        // already-mutated object mask skips that edge. A later construction
        // state can then be coalesced with completion, making the rise and
        // teardown transitions appear back-to-back.
        state.animationConditionSnapshot = {};
        state.conditionAnimationRevisionSnapshot =
            conditionAnimationRevision;
        state.animationStateSnapshot = state.animationState;
        state.animationStateEnterTick = confirmedTick;
        state.animationStateInitialized = true;
        state.resolvedVisualRuleIndex = initialRuleIndex;
        state.activeTransitionRuleIndex = UINT32_MAX;
        state.waitingSourceVisualRuleIndex = UINT32_MAX;
        beginAnimationGeneration(
            state, animationFlagsForRule(visualRules, initialRuleIndex),
            objectId, channelIndex, UINT32_MAX, {}, 0.0f, 0, 0);
    }
    {
        const game::ModelConditionMask previousConditions =
            state.waitingSourceVisualRuleIndex != UINT32_MAX
                ? state.waitingSourceConditionSnapshot
                : state.animationConditionSnapshot;
        const uint32_t presentedSourceRuleIndex =
            state.waitingSourceVisualRuleIndex != UINT32_MAX
                ? state.waitingSourceVisualRuleIndex
                : state.resolvedVisualRuleIndex;
        const float presentedSourceTimeSeconds =
            state.animationTimeSeconds;
        const uint64_t presentedSourceGeneration =
            state.animationStateGeneration;
        const game::ModelAnimationFlags presentedSourceFlags =
            animationFlagsForRule(
                visualRules, presentedSourceRuleIndex);
        const bool explicitStateChanged =
            state.animationStateSnapshot != state.animationState;
        const bool selectedRuleChanged = visualRules &&
            state.resolvedVisualRuleIndex != selectedRuleIndex;
        const bool fallbackConditionChanged = !visualRules &&
            state.animationConditionSnapshot.words !=
                modelConditionFlags.words;
        const bool stateChanged = explicitStateChanged ||
            selectedRuleChanged || fallbackConditionChanged;
        const bool authoredTimedConditionChange =
            state.conditionAnimationRevisionSnapshot !=
                conditionAnimationRevision;
        // RefCode switches to a newly selected ConditionState immediately.
        // The initial channel generation therefore never gates its first
        // state change. Once a later state has actually been entered, however,
        // newest-only world snapshots must not replace that presentation edge
        // before the renderer has published it once. This latch is confined
        // to the Draw-channel state; gameplay conditions and module phase
        // machines continue advancing at their confirmed cadence. Authored
        // WaitForStateToFinishIfPossible remains the separate natural-
        // completion rule handled below.
        if (stateChanged &&
            state.animationEndpointAdmissionRequired &&
            state.animationEndpointPublishedGeneration !=
                state.animationStateGeneration) {
            // Full world snapshots are newest-only, but an entered animation
            // generation is an edge. Retain the already-entered presentation
            // state only until the renderer publishes that endpoint once.
            // Resource readiness is renderer-owned and must never hold the
            // confirmed Draw state machine behind gameplay: doing so bunches
            // construction rise, steady-state and teardown together when an
            // asynchronously loaded scaffold clip becomes ready late.
            return;
        }
        if (stateChanged) {
            state.animationConditionSnapshot = modelConditionFlags;
            state.conditionAnimationRevisionSnapshot =
                conditionAnimationRevision;
            state.animationStateSnapshot = state.animationState;
            state.activeTransitionRuleIndex = UINT32_MAX;
            state.waitingSourceVisualRuleIndex = UINT32_MAX;
            if (!explicitStateChanged && selectedRuleChanged && visualRules &&
                presentedSourceRuleIndex < visualRules->size() &&
                selectedRuleIndex < visualRules->size()) {
                const game::ModelConditionVisualRule& sourceRule =
                    (*visualRules)[presentedSourceRuleIndex];
                const game::ModelConditionVisualRule& destinationRule =
                    (*visualRules)[selectedRuleIndex];
                const container::String& sourceKey = sourceRule.transitionKey;
                const container::String& destinationKey = destinationRule.transitionKey;
                const bool waitsForPresentedSource =
                    !sourceKey.empty() &&
                    !destinationRule.waitForStateToFinishKey.empty() &&
                    destinationRule.waitForStateToFinishKey == sourceKey;
                if (waitsForPresentedSource) {
                    state.waitingSourceVisualRuleIndex =
                        presentedSourceRuleIndex;
                    state.waitingSourceConditionSnapshot = previousConditions;
                }
                const size_t transitionCount = transitions
                    ? transitions->size() : 0u;
                for (uint32_t index = 0; index < transitionCount; ++index) {
                    const game::ModelConditionTransitionRule& transition =
                        (*transitions)[index];
                    if (!sourceKey.empty() && !destinationKey.empty() &&
                        transition.sourceKey == sourceKey &&
                        transition.destinationKey == destinationKey) {
                        state.activeTransitionRuleIndex = index;
                        break;
                    }
                }
            }
            state.resolvedVisualRuleIndex = selectedRuleIndex;
            state.animationStateEnterTick = confirmedTick;
            beginAnimationGeneration(
                state,
                animationFlagsForRule(visualRules, selectedRuleIndex),
                objectId, channelIndex, presentedSourceRuleIndex,
                previousConditions, presentedSourceTimeSeconds,
                presentedSourceGeneration, presentedSourceFlags);
            // The object-level scalar state is only a compatibility source
            // when typed Draw channels exist; no renderer endpoint is ever
            // published for it. Only a state that crosses the real render
            // contract may wait for endpoint admission.
            state.animationEndpointAdmissionRequired =
                endpointAdmissionEnabled;
            if (state.waitingSourceVisualRuleIndex == UINT32_MAX) {
                state.animationTimeSeconds = 0.0f;
            }
            return;
        }
        if (!stateChanged && authoredTimedConditionChange) {
            // A producer revision is object-wide, while each W3D Draw channel
            // selects independently. Channels whose selected rule did not
            // change acknowledge the revision without restarting their clip.
            state.conditionAnimationRevisionSnapshot =
                conditionAnimationRevision;
        }
    }

    game::ModelAnimationMode playbackMode = state.animationMode;
    if (state.animationState.empty()) {
        // W3DModelDraw advances the animation installed in m_curState.  While
        // a normal-state change is in flight that is, in order, the retained
        // source state, the explicit TransitionState, and only then the
        // selected destination ConditionState.  Looking only at the
        // destination mode freezes an ONCE transition whenever its endpoint
        // is MANUAL. Construction fence/scaffold channels use exactly that
        // authored shape (DOWN -> ONCE rise -> MANUAL final pose), which left
        // them below ground until the completion edge selected the backwards
        // teardown transition.
        if (state.waitingSourceVisualRuleIndex != UINT32_MAX && visualRules &&
            state.waitingSourceVisualRuleIndex < visualRules->size()) {
            playbackMode = (*visualRules)[
                state.waitingSourceVisualRuleIndex].animationMode;
        } else if (state.activeTransitionRuleIndex != UINT32_MAX &&
                   transitions &&
                   state.activeTransitionRuleIndex < transitions->size()) {
            playbackMode = (*transitions)[
                state.activeTransitionRuleIndex].animationMode;
        } else if (visualRules &&
                   state.resolvedVisualRuleIndex < visualRules->size()) {
            playbackMode = (*visualRules)[
                state.resolvedVisualRuleIndex].animationMode;
        }
    }
    if (!std::isfinite(fixedDeltaSeconds) || fixedDeltaSeconds <= 0.0f ||
        state.animationPaused || state.animationPausedByObjectState ||
        state.animationResourcePendingGeneration ==
            state.animationStateGeneration ||
        playbackMode == game::ModelAnimationMode::Manual) {
        return;
    }
    state.animationTimeSeconds += fixedDeltaSeconds;
}

[[nodiscard]] uint32_t narrowRuleIndex(size_t ruleIndex) noexcept {
    return ruleIndex <= UINT32_MAX ? static_cast<uint32_t>(ruleIndex)
                                   : UINT32_MAX;
}

} // namespace

bool shouldPauseVisualAnimation(
    bool animationsRequirePower,
    VisualAnimationObjectState objectState) noexcept {
    const ObjectDisabledMask disabled = objectState.disabledReasons;
    constexpr ObjectDisabledMask kMechanicalPauseReasons =
        objectDisabledBit(ObjectDisabledReason::Hacked) |
        objectDisabledBit(ObjectDisabledReason::Paralyzed) |
        objectDisabledBit(ObjectDisabledReason::Emp) |
        objectDisabledBit(ObjectDisabledReason::Subdued) |
        objectDisabledBit(ObjectDisabledReason::Unmanned);
    constexpr ObjectDisabledMask kPowerPauseReasons =
        objectDisabledBit(ObjectDisabledReason::Underpowered) |
        objectDisabledBit(ObjectDisabledReason::ScriptUnderpowered);

    // RefCode deliberately keeps helipad-produced helicopters animating
    // through EMP/HACKED/PARALYZED/SUBDUED/UNMANNED. Power remains a separate
    // Draw-module policy and therefore is not covered by that exception.
    if (!objectState.producedAtHelipad &&
        (disabled & kMechanicalPauseReasons) != 0) {
        return true;
    }
    return animationsRequirePower && (disabled & kPowerPauseReasons) != 0;
}

void setVisualAnimationFrame(
    RenderModelComponent& visual, uint32_t frame) noexcept {
    visual.animationManualFrame = frame;
    for (RenderModelChannelState& channel : visual.channels) {
        channel.animationManualFrame = frame;
    }
}

bool updateVisualAnimationState(
    RenderModelComponent& visual,
    float fixedDeltaSeconds,
    uint64_t confirmedTick,
    const game::ThingTemplate* templateData,
    VisualAnimationObjectState objectState) noexcept {
    bool descriptorChanged = false;
    const uint64_t scalarGeneration = visual.animationStateGeneration;
    const bool scalarPaused = visual.animationPausedByObjectState;
    const size_t selectedRule = templateData
        ? game::selectModelConditionVisualRuleIndex(
              *templateData, visual.modelConditionFlags)
        : std::numeric_limits<size_t>::max();
    const size_t initialRule = templateData
        ? game::selectModelConditionVisualRuleIndex(
              *templateData, {})
        : std::numeric_limits<size_t>::max();
    updateChannelAnimationState(
        visual, visual.modelConditionFlags,
        visual.conditionAnimationRevision, fixedDeltaSeconds, confirmedTick,
        templateData ? &templateData->modelConditionVisuals : nullptr,
        templateData ? &templateData->modelConditionTransitions : nullptr,
        narrowRuleIndex(selectedRule), narrowRuleIndex(initialRule),
        !templateData || templateData->drawVisualChannels.empty(),
        shouldPauseVisualAnimation(true, objectState), objectState.objectId,
        UINT32_MAX);
    descriptorChanged =
        visual.animationStateGeneration != scalarGeneration ||
        visual.animationPausedByObjectState != scalarPaused;

    if (!templateData || templateData->drawVisualChannels.empty()) {
        return descriptorChanged;
    }

    // Draw modules own independent state machines. Keep their clocks and
    // transition bookkeeping separate even while the legacy scalar fields
    // remain as a compatibility source for explicit animation commands.
    if (visual.channels.size() < templateData->drawVisualChannels.size()) {
        descriptorChanged = true;
        visual.channels.reserve(templateData->drawVisualChannels.size());
        while (visual.channels.size() < templateData->drawVisualChannels.size()) {
            visual.channels.push_back(RenderModelChannelState{
                .channelIndex = static_cast<uint32_t>(visual.channels.size()),
            });
        }
    } else if (visual.channels.size() > templateData->drawVisualChannels.size()) {
        descriptorChanged = true;
        visual.channels.resize(templateData->drawVisualChannels.size());
    }

    for (size_t index = 0; index < templateData->drawVisualChannels.size(); ++index) {
        const game::ModelDrawVisualChannel& recipe =
            templateData->drawVisualChannels[index];
        RenderModelChannelState& channel = visual.channels[index];
        const uint64_t previousGeneration = channel.animationStateGeneration;
        const bool previousPausedByObject =
            channel.animationPausedByObjectState;
        const bool sourceDescriptorChanged =
            channel.animationState != visual.animationState ||
            channel.animationRate != visual.animationRate ||
            channel.animationMode != visual.animationMode ||
            channel.animationManualFrame != visual.animationManualFrame ||
            channel.animationPaused != visual.animationPaused;
        channel.channelIndex = static_cast<uint32_t>(index);
        channel.animationState = visual.animationState;
        channel.animationRate = visual.animationRate;
        channel.animationMode = visual.animationMode;
        channel.animationManualFrame = visual.animationManualFrame;
        channel.animationPaused = visual.animationPaused;
        const size_t channelRule = game::selectModelConditionVisualRuleIndex(
            recipe, visual.modelConditionFlags);
        const size_t initialChannelRule =
            game::selectModelConditionVisualRuleIndex(recipe, {});
        updateChannelAnimationState(
            channel, visual.modelConditionFlags,
            visual.conditionAnimationRevision, fixedDeltaSeconds,
            confirmedTick, &recipe.conditionVisuals, &recipe.transitions,
            narrowRuleIndex(channelRule), narrowRuleIndex(initialChannelRule),
            true,
            shouldPauseVisualAnimation(
                recipe.animationsRequirePower, objectState),
            objectState.objectId, static_cast<uint32_t>(index));
        descriptorChanged = descriptorChanged || sourceDescriptorChanged ||
            channel.animationStateGeneration != previousGeneration ||
            channel.animationPausedByObjectState != previousPausedByObject;
    }
    return descriptorChanged;
}

bool applyVisualAnimationEndpointAdmission(
    RenderModelComponent& visual,
    uint32_t channelIndex,
    uint64_t generation) noexcept {
    RenderModelComponent* scalarState = nullptr;
    RenderModelChannelState* channelState = nullptr;
    if (!visual.channels.empty()) {
        if (channelIndex >= visual.channels.size()) return false;
        channelState = &visual.channels[channelIndex];
    } else {
        if (channelIndex != 0) return false;
        scalarState = &visual;
    }

    const auto apply = [generation](auto& state) noexcept {
        if (generation == 0 || generation != state.animationStateGeneration ||
            state.animationEndpointPublishedGeneration == generation) {
            return false;
        }
        state.animationEndpointPublishedGeneration = generation;
        state.animationEndpointAdmissionRequired = false;
        return true;
    };
    return channelState ? apply(*channelState) : apply(*scalarState);
}

bool applyVisualAnimationResourceGate(
    RenderModelComponent& visual,
    uint32_t channelIndex,
    uint64_t generation,
    VisualAnimationCompletionPhase phase,
    bool pending) noexcept {
    RenderModelComponent* scalarState = nullptr;
    RenderModelChannelState* channelState = nullptr;
    if (!visual.channels.empty()) {
        if (channelIndex >= visual.channels.size()) return false;
        channelState = &visual.channels[channelIndex];
    } else {
        if (channelIndex != 0) return false;
        scalarState = &visual;
    }

    const auto apply = [&](auto& state) {
        if (generation == 0 || generation != state.animationStateGeneration)
            return false;
        const VisualAnimationCompletionPhase expected =
            state.waitingSourceVisualRuleIndex != UINT32_MAX
                ? VisualAnimationCompletionPhase::PresentedSource
                : state.activeTransitionRuleIndex != UINT32_MAX
                    ? VisualAnimationCompletionPhase::Transition
                    : VisualAnimationCompletionPhase::ActiveState;
        if (phase != expected) return false;
        const uint8_t phaseValue = static_cast<uint8_t>(phase);
        if (pending) {
            if (state.animationResourcePendingGeneration == generation &&
                state.animationResourcePendingPhase == phaseValue) {
                return true;
            }
            state.animationResourcePendingGeneration = generation;
            state.animationResourcePendingPhase = phaseValue;
            // The renderer observes residency after a confirmed snapshot, so
            // at most one fixed tick may already have elapsed. Pending clips
            // begin at their authored start once resident, never at that
            // speculative elapsed time.
            state.animationTimeSeconds = 0.0f;
            return true;
        }
        if (state.animationResourcePendingGeneration != generation ||
            state.animationResourcePendingPhase != phaseValue) {
            return false;
        }
        state.animationResourcePendingGeneration = 0;
        state.animationResourcePendingPhase = UINT8_MAX;
        state.animationTimeSeconds = 0.0f;
        return true;
    };

    return channelState ? apply(*channelState) : apply(*scalarState);
}

bool applyVisualAnimationCompletion(
    RenderModelComponent& visual,
    const VisualAnimationCompletion& completion,
    uint64_t confirmedTick,
    const game::ThingTemplate* templateData,
    uint64_t objectId) noexcept {
    RenderModelComponent* scalarState = nullptr;
    RenderModelChannelState* channelState = nullptr;
    const container::Vector<game::ModelConditionVisualRule>* visualRules =
        nullptr;
    uint32_t generationChannel = UINT32_MAX;
    if (templateData && !templateData->drawVisualChannels.empty()) {
        if (completion.channelIndex >= visual.channels.size() ||
            completion.channelIndex >=
                templateData->drawVisualChannels.size()) {
            return false;
        }
        channelState = &visual.channels[completion.channelIndex];
        visualRules = &templateData
            ->drawVisualChannels[completion.channelIndex].conditionVisuals;
        generationChannel = completion.channelIndex;
    } else {
        if (completion.channelIndex != 0) return false;
        scalarState = &visual;
        visualRules = templateData
            ? &templateData->modelConditionVisuals : nullptr;
    }

    const auto apply = [&](auto& state) {
        if (completion.generation == 0 ||
            completion.generation != state.animationStateGeneration) {
            return false;
        }
        const uint8_t phaseBit = static_cast<uint8_t>(
            1u << static_cast<uint8_t>(completion.phase));
        if ((state.animationCompletionMask & phaseBit) != 0) return false;
        const float duration = std::isfinite(
                completion.completedDurationSeconds)
            ? std::max(0.0f, completion.completedDurationSeconds) : 0.0f;
        if (state.animationResourcePendingGeneration ==
                completion.generation &&
            state.animationResourcePendingPhase ==
                static_cast<uint8_t>(completion.phase)) {
            state.animationResourcePendingGeneration = 0;
            state.animationResourcePendingPhase = UINT8_MAX;
        }

        switch (completion.phase) {
        case VisualAnimationCompletionPhase::PresentedSource:
            if (state.waitingSourceVisualRuleIndex == UINT32_MAX) {
                return false;
            }
            state.waitingSourceVisualRuleIndex = UINT32_MAX;
            state.animationTimeSeconds = std::max(
                0.0f, state.animationTimeSeconds - duration);
            break;
        case VisualAnimationCompletionPhase::Transition:
            if (state.activeTransitionRuleIndex == UINT32_MAX) {
                return false;
            }
            state.activeTransitionRuleIndex = UINT32_MAX;
            state.waitingSourceVisualRuleIndex = UINT32_MAX;
            state.animationTimeSeconds = std::max(
                0.0f, state.animationTimeSeconds - duration);
            break;
        case VisualAnimationCompletionPhase::ActiveState: {
            if (state.waitingSourceVisualRuleIndex != UINT32_MAX ||
                state.activeTransitionRuleIndex != UINT32_MAX) {
                return false;
            }
            bool selectedCandidateIsIdle = false;
            size_t selectedCandidateIndex =
                std::numeric_limits<size_t>::max();
            const game::ModelConditionVisualRule* selectedRule = nullptr;
            if (visualRules &&
                state.resolvedVisualRuleIndex < visualRules->size()) {
                const game::ModelConditionVisualRule& rule =
                    (*visualRules)[state.resolvedVisualRuleIndex];
                selectedRule = &rule;
                const game::ModelAnimationSelection selection =
                    game::selectModelAnimation(
                        rule, objectId, state.animationConditionSnapshot,
                        state.animationStateGeneration);
                selectedCandidateIndex =
                    state.animationCandidateOverrideGeneration ==
                            state.animationStateGeneration &&
                        state.animationCandidateOverrideIndex <
                            rule.animationCandidates.size()
                    ? state.animationCandidateOverrideIndex
                    : selection.candidateIndex;
                selectedCandidateIsIdle =
                    selectedCandidateIndex <
                        rule.animationCandidates.size() &&
                    rule.animationCandidates[
                        selectedCandidateIndex].idle;
            }
            const bool restart = selectedCandidateIsIdle ||
                (state.activeAnimationFlags &
                 game::modelAnimationFlagBit(
                     game::ModelAnimationFlag::
                         RestartAnimationWhenComplete)) != 0;
            state.animationCompletionMask |= phaseBit;
            if (!restart) return true;
            beginAnimationGeneration(
                state, state.activeAnimationFlags, objectId,
                generationChannel, UINT32_MAX, {}, 0.0f, 0, 0);
            state.animationEndpointAdmissionRequired = true;
            if (selectedCandidateIsIdle && selectedRule &&
                selectedRule->animationCandidates.size() > 1u &&
                selectedCandidateIndex <
                    selectedRule->animationCandidates.size()) {
                // RefCode repeatedly samples until an IdleAnimation other
                // than the currently playing vector entry is chosen. The
                // compressed modern candidate list has no duplicate vector
                // entries, so advance to the next authored candidate to make
                // the no-immediate-repeat contract deterministic.
                state.animationCandidateOverrideIndex =
                    static_cast<uint32_t>(
                        (selectedCandidateIndex + 1u) %
                        selectedRule->animationCandidates.size());
                state.animationCandidateOverrideGeneration =
                    state.animationStateGeneration;
            }
            state.animationStateEnterTick = confirmedTick;
            state.animationTimeSeconds = 0.0f;
            return true;
        }
        }
        state.animationCompletionMask |= phaseBit;
        return true;
    };

    return channelState ? apply(*channelState) : apply(*scalarState);
}

} // namespace engine
