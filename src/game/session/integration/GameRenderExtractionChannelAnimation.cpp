#include "GameRenderExtractionChannelAnimation.h"

#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/script/presentation/ScriptObjectPresentationControls.h"
#include "presentation/render/SupportDrawPresentation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace engine::render_extraction_detail {
namespace {

// RefCode adjustAnimSpeedToMovementSpeed() reads
// PhysicsBehavior::getVelocityMagnitude(), which is world units per logic
// frame. Ground locomotion in this port owns its own authoritative forward
// speed and does not publish a free-body velocity, so prefer the locomotor
// value and fall back to the physics body exactly like
// VehicleDrawPresentation's planar speed helper. The result is world units per
// second, which is why the derived duration below needs no frame conversion:
// dist / (units per frame) * msecPerFrame == dist / (units per second) * 1000.
[[nodiscard]] float confirmedSpeedUnitsPerSecond(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    if (const ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(registry, entity)) {
        const float forward = locomotion->forwardSpeed.to_float();
        return std::isfinite(forward) ? std::abs(forward) : 0.0f;
    }
    if (const ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, entity)) {
        const float x = physics->velocityUnitsPerSecond.x.to_float();
        const float y = physics->velocityUnitsPerSecond.y.to_float();
        const float z = physics->velocityUnitsPerSecond.z.to_float();
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
            return 0.0f;
        }
        const float magnitude = std::sqrt(x * x + y * y + z * z);
        return std::isfinite(magnitude) ? magnitude : 0.0f;
    }
    return 0.0f;
}

// Smallest speed that still counts as movement. Below this the authored
// condition state is an idle/stopped pose anyway, and dividing by it would
// produce a duration long enough to lose all float precision.
constexpr float kMinimumSpeedSyncSpeedUnitsPerSecond = 0.01f;
// A clip is never throttled to more than this. Shipped distanceCovered values
// are 10..50 world units against speeds of a few tens of units per second, so
// this only fences off absurd or hand-edited content; it also keeps the
// published value inside a range the renderer can invert without overflow.
constexpr float kMaximumSpeedSyncDurationSeconds = 60.0f;

// distanceCovered / speed, or zero when the pair cannot produce a usable
// duration. An authored distance of zero means "this clip does not translate",
// which must read as "do not adjust" rather than as a division by zero.
[[nodiscard]] float animationSpeedSyncDurationSeconds(
    float distanceCovered, float speedUnitsPerSecond) noexcept {
    if (!std::isfinite(distanceCovered) || distanceCovered <= 0.0f ||
        distanceCovered > kMaximumSpeedSyncDurationSeconds * 1.0e6f) {
        return 0.0f;
    }
    if (!std::isfinite(speedUnitsPerSecond) ||
        speedUnitsPerSecond < kMinimumSpeedSyncSpeedUnitsPerSecond) {
        return 0.0f;
    }
    const float duration = distanceCovered / speedUnitsPerSecond;
    if (!std::isfinite(duration) || duration <= 0.0f ||
        duration > kMaximumSpeedSyncDurationSeconds) {
        return 0.0f;
    }
    return duration;
}

} // namespace

ChannelAnimationResult applyRenderChannelAnimation(
    const ChannelAnimationSource& source,
    const ChannelAnimationInput& input,
    render::RenderEntitySnapshot& output) {
    const ecs::registry& registry = source.registry;
    const ecs::entity entity = input.entity;
    const size_t channelIndex = input.channelIndex;
    const ObjectIdentityComponent& identity = input.identity;
    const RenderModelComponent& visual = input.visual;
    const game::ModelDrawVisualChannel* channelRecipe = input.recipe;
    const game::ThingTemplate* templateData = input.templateData;
    const ObjectWeaponComponent* weapons = input.weapons;
    const auto* visualRules = input.runtime.visualRules;
    const auto* transitionRules = input.runtime.transitionRules;
    const game::ModelConditionMask& channelPresentedConditions =
        *input.runtime.presentedConditions;
    const container::String& channelAnimationState =
        *input.runtime.animationState;
    const game::ModelConditionMask& channelWaitingSourceConditionSnapshot =
        *input.runtime.waitingSourceConditions;
    const game::ModelConditionMask&
        channelAnimationStartSourceConditionSnapshot =
            *input.runtime.animationStartSourceConditions;
    const float channelAnimationRate = input.runtime.animationRate;
    const uint32_t channelResolvedVisualRuleIndex =
        input.runtime.resolvedVisualRuleIndex;
    const uint32_t channelActiveTransitionRuleIndex =
        input.runtime.activeTransitionRuleIndex;
    const uint32_t channelWaitingSourceVisualRuleIndex =
        input.runtime.waitingSourceVisualRuleIndex;
    const uint64_t channelAnimationStateGeneration =
        input.runtime.animationStateGeneration;
    const uint32_t channelAnimationCandidateOverrideIndex =
        input.runtime.animationCandidateOverrideIndex;
    const uint64_t channelAnimationCandidateOverrideGeneration =
        input.runtime.animationCandidateOverrideGeneration;
    const VisualAnimationStartKind channelAnimationStartKind =
        input.runtime.animationStartKind;
    const float channelAnimationRandomStartFraction =
        input.runtime.animationRandomStartFraction;
    const uint32_t channelAnimationStartSourceVisualRuleIndex =
        input.runtime.animationStartSourceVisualRuleIndex;
    const float channelAnimationStartSourceTimeSeconds =
        input.runtime.animationStartSourceTimeSeconds;
    const uint64_t channelAnimationStartSourceGeneration =
        input.runtime.animationStartSourceGeneration;
    const game::ModelConditionMask& presentationModelConditions =
        input.presentationConditions;
    const container::StringView archetypeName = input.archetypeName;
    const size_t normalPoseRuleCount = input.normalPoseRuleCount;
    const size_t currentChannelVisualRuleOffset = input.visualRuleOffset;
    const size_t currentChannelTransitionRuleOffset =
        input.transitionRuleOffset;
    const uint64_t supplyCurrent = input.supplyCurrent;
    const uint64_t supplyMaximum = input.supplyMaximum;
    const uint64_t simulationFrame = source.simulationFrame;
    // One confirmed sample per channel: RefCode reads the object's physics
    // once per doDrawModule() and applies it to whichever animation the
    // current state selected.
    const float channelSpeedUnitsPerSecond =
        confirmedSpeedUnitsPerSecond(registry, entity);

    ChannelAnimationResult result;
    float& animationRuleRateFactor = result.ruleRateFactor;
    float& authoredAssetScale = result.authoredAssetScale;
        if (templateData) {
            authoredAssetScale = templateData->assetScale.to_float();
            size_t visualRuleIndex = channelResolvedVisualRuleIndex;
            if (visualRules && visualRuleIndex >= visualRules->size()) {
                visualRuleIndex = channelRecipe
                    ? game::selectModelConditionVisualRuleIndex(
                          *channelRecipe, channelPresentedConditions)
                    : game::selectModelConditionVisualRuleIndex(
                          *templateData, channelPresentedConditions);
            }
            if (visualRules && visualRuleIndex < visualRules->size()) {
                const game::ModelConditionVisualRule* rule =
                    &(*visualRules)[visualRuleIndex];
                // Model=None is a valid destination endpoint.  Construction
                // fence/scaffold Draw modules use it together with an
                // authored UP_* -> DOWN_DEFAULT TransitionState so their
                // teardown clip remains visible until it finishes.  Let the
                // phase resolver below select that transition; when no source
                // or transition is active, the final empty model still makes
                // this channel naturally non-drawable.
                // RefCode reads this from m_curState, i.e. the selected
                // ConditionState for this Draw module, not from a transition
                // clip. Mirror that by sampling the resolved normal rule.
                result.adjustHeightByConstructionPercent =
                    (rule->animationFlags & game::modelAnimationFlagBit(
                        game::ModelAnimationFlag::
                            AdjustHeightByConstructionPercent)) != 0;
                const auto applyVisibility = [&output](
                        const container::Vector<game::ModelSubObjectVisibility>& values) {
                    output.visual.subObjectVisibility.clear();
                    output.visual.subObjectVisibility.reserve(values.size());
                    for (const game::ModelSubObjectVisibility& visibility : values) {
                        output.visual.subObjectVisibility.push_back({
                            .name = visibility.name,
                            .visible = visibility.visible,
                        });
                    }
                };
                const auto applyConditionPhase = [&source, &output, &applyVisibility,
                        &animationRuleRateFactor, &identity, weapons, &visual, simulationFrame,
                        channelRecipe, archetypeName,
                        currentChannelVisualRuleOffset,
                        channelAnimationStateGeneration,
                        channelAnimationCandidateOverrideIndex,
                        channelAnimationCandidateOverrideGeneration,
                        channelSpeedUnitsPerSecond](
                        const game::ModelConditionVisualRule& phase,
                        const game::ModelConditionMask& selectionConditions,
                        uint32_t phaseIdentity) {
                    output.modelAsset = phase.model;
                    const game::ModelAnimationSelection selection =
                        game::selectModelAnimation(
                            phase, identity.id.value, selectionConditions,
                            channelAnimationStateGeneration);
                    const size_t candidateIndex =
                        channelAnimationCandidateOverrideGeneration ==
                                channelAnimationStateGeneration &&
                            channelAnimationCandidateOverrideIndex <
                                phase.animationCandidates.size()
                        ? channelAnimationCandidateOverrideIndex
                        : selection.candidateIndex;
                    output.visual.animationState =
                        candidateIndex < phase.animationCandidates.size()
                            ? phase.animationCandidates[
                                  candidateIndex].resource
                            : phase.animation;
                    output.visual.animationMode =
                        toRenderAnimationMode(phase.animationMode);
                    animationRuleRateFactor = selection.speedFactor;
                    // RefCode sets the random AnimationSpeedFactorRange
                    // multiplier once when the clip changes, then
                    // adjustAnimSpeedToMovementSpeed() overwrites it every
                    // frame for any clip with an authored distanceCovered.
                    // Publishing the duration beside the factor keeps that
                    // precedence in the renderer instead of multiplying both.
                    output.visual.animationSpeedSyncDurationSeconds =
                        animationSpeedSyncDurationSeconds(
                            candidateIndex < phase.animationCandidates.size()
                                ? phase.animationCandidates[candidateIndex]
                                      .distanceCovered
                                : 0.0f,
                            channelSpeedUnitsPerSecond);
                    applyVisibility(phase.subObjectVisibility);
                    output.visual.boneControls = extractTurretControls(
                        weapons, phase.turrets);
                    const auto* barrelTables = resolveWeaponBarrelTables(
                        source.weaponPresentation, archetypeName,
                        currentChannelVisualRuleOffset + phaseIdentity,
                        phase.weaponBones);
                    appendWeaponPresentationControls(
                        source.weaponPresentation, weapons, visual,
                        phase.weaponBones,
                        barrelTables,
                        channelRecipe
                            ? channelRecipe->projectileBoneFeedbackEnabledSlots
                            : 0u,
                        phase.recoil, simulationFrame,
                        output.visual.subObjectVisibility,
                        output.visual.weaponImpulses);
                    output.visual.particleSystemBones.clear();
                    output.visual.particleSystemBones.reserve(
                        phase.particleSystemBones.size());
                    for (size_t emitterIndex = 0;
                         emitterIndex < phase.particleSystemBones.size();
                         ++emitterIndex) {
                        const game::ModelParticleSystemBoneDefinition& emitter =
                            phase.particleSystemBones[emitterIndex];
                        output.visual.particleSystemBones.push_back({
                            .identity = modelParticleEmitterIdentity(
                                output.id, phaseIdentity, emitterIndex),
                            .boneName = emitter.boneName,
                            .particleSystem = emitter.particleSystem,
                            .followsAnimatedBone = channelRecipe &&
                                channelRecipe->particlesAttachedToAnimatedBones,
                        });
                    }
                };
                const auto applyTransitionPhase = [&source, &output, &applyVisibility,
                        &animationRuleRateFactor, &identity, &visual, &channelPresentedConditions,
                        weapons, simulationFrame, channelRecipe,
                        archetypeName, normalPoseRuleCount,
                        currentChannelTransitionRuleOffset,
                        channelAnimationStateGeneration,
                        channelSpeedUnitsPerSecond](
                        const game::ModelConditionTransitionRule& phase,
                        uint32_t phaseIdentity) {
                    const game::ModelAnimationSelection selection =
                        game::selectModelAnimation(
                            phase, identity.id.value,
                            channelPresentedConditions,
                            channelAnimationStateGeneration);
                    output.modelAsset = phase.model;
                    output.visual.animationState =
                        selection.candidateIndex < phase.animationCandidates.size()
                            ? phase.animationCandidates[
                                  selection.candidateIndex].resource
                            : phase.animation;
                    output.visual.animationMode =
                        toRenderAnimationMode(phase.animationMode);
                    animationRuleRateFactor = selection.speedFactor;
                    output.visual.animationSpeedSyncDurationSeconds =
                        animationSpeedSyncDurationSeconds(
                            selection.candidateIndex <
                                    phase.animationCandidates.size()
                                ? phase.animationCandidates[
                                      selection.candidateIndex].distanceCovered
                                : 0.0f,
                            channelSpeedUnitsPerSecond);
                    applyVisibility(phase.subObjectVisibility);
                    output.visual.boneControls = extractTurretControls(
                        weapons, phase.turrets);
                    const auto* barrelTables = resolveWeaponBarrelTables(
                        source.weaponPresentation, archetypeName,
                        normalPoseRuleCount + currentChannelTransitionRuleOffset +
                            (phaseIdentity & 0x7fffffffu),
                        phase.weaponBones);
                    appendWeaponPresentationControls(
                        source.weaponPresentation, weapons, visual,
                        phase.weaponBones,
                        barrelTables,
                        channelRecipe
                            ? channelRecipe->projectileBoneFeedbackEnabledSlots
                            : 0u,
                        phase.recoil, simulationFrame,
                        output.visual.subObjectVisibility,
                        output.visual.weaponImpulses);
                    output.visual.particleSystemBones.clear();
                    output.visual.particleSystemBones.reserve(
                        phase.particleSystemBones.size());
                    for (size_t emitterIndex = 0;
                         emitterIndex < phase.particleSystemBones.size();
                         ++emitterIndex) {
                        const game::ModelParticleSystemBoneDefinition& emitter =
                            phase.particleSystemBones[emitterIndex];
                        output.visual.particleSystemBones.push_back({
                            .identity = modelParticleEmitterIdentity(
                                output.id, phaseIdentity, emitterIndex),
                            .boneName = emitter.boneName,
                            .particleSystem = emitter.particleSystem,
                            .followsAnimatedBone = channelRecipe &&
                                channelRecipe->particlesAttachedToAnimatedBones,
                        });
                    }
                };
                const auto configureConditionAnimationStart = [
                        &output, &identity, &visual, visualRules,
                        &channelPresentedConditions,
                        &channelAnimationRate,
                        channelAnimationStartKind,
                        channelAnimationRandomStartFraction,
                        channelAnimationStateGeneration,
                        channelAnimationStartSourceVisualRuleIndex,
                        &channelAnimationStartSourceConditionSnapshot,
                        channelAnimationStartSourceTimeSeconds,
                        channelAnimationStartSourceGeneration,
                        channelAnimationCandidateOverrideIndex,
                        channelAnimationCandidateOverrideGeneration](
                        const game::ModelConditionVisualRule& phase,
                        bool enteringDestination) {
                    output.visual.animationStart = {};
                    output.visual.animationStart.generation =
                        channelAnimationStateGeneration;
                    const game::ModelAnimationSelection selection =
                        game::selectModelAnimation(
                            phase, identity.id.value,
                            channelPresentedConditions,
                            channelAnimationStateGeneration);
                    const size_t candidateIndex =
                        channelAnimationCandidateOverrideGeneration ==
                                channelAnimationStateGeneration &&
                            channelAnimationCandidateOverrideIndex <
                                phase.animationCandidates.size()
                        ? channelAnimationCandidateOverrideIndex
                        : selection.candidateIndex;
                    const bool selectedCandidateIsIdle =
                        candidateIndex <
                            phase.animationCandidates.size() &&
                        phase.animationCandidates[
                            candidateIndex].idle;
                    output.visual.animationStart.restartWhenComplete =
                        selectedCandidateIsIdle ||
                        (phase.animationFlags & game::modelAnimationFlagBit(
                            game::ModelAnimationFlag::
                                RestartAnimationWhenComplete)) != 0;
                    if (!enteringDestination) return;
                    output.visual.animationStart.kind =
                        toRenderAnimationStartKind(
                            channelAnimationStartKind);
                    output.visual.animationStart.randomFraction = std::clamp(
                        channelAnimationRandomStartFraction, 0.0f, 1.0f);
                    if (channelAnimationStartKind !=
                            VisualAnimationStartKind::MaintainFraction ||
                        !visualRules ||
                        channelAnimationStartSourceVisualRuleIndex >=
                            visualRules->size()) {
                        return;
                    }
                    const game::ModelConditionVisualRule& sourceRule =
                        (*visualRules)[
                            channelAnimationStartSourceVisualRuleIndex];
                    const game::ModelAnimationSelection sourceSelection =
                        game::selectModelAnimation(
                            sourceRule, identity.id.value,
                            channelAnimationStartSourceConditionSnapshot,
                            channelAnimationStartSourceGeneration);
                    output.visual.animationStart.sourceModelAsset =
                        sourceRule.model;
                    output.visual.animationStart.sourceAnimationState =
                        sourceSelection.candidateIndex <
                                sourceRule.animationCandidates.size()
                            ? sourceRule.animationCandidates[
                                  sourceSelection.candidateIndex].resource
                            : sourceRule.animation;
                    output.visual.animationStart.sourceTimeSeconds =
                        channelAnimationStartSourceTimeSeconds;
                    output.visual.animationStart.sourceRate = std::max(
                        0.0f, channelAnimationRate *
                            sourceSelection.speedFactor);
                    output.visual.animationStart.sourceMode =
                        toRenderAnimationMode(sourceRule.animationMode);
                };
                const auto sealCurrentPhase = [&output,
                        &channelAnimationRate,
                        &animationRuleRateFactor]() {
                    return render::RenderAnimationCompletionTarget{
                        .modelAsset = output.modelAsset,
                        .animationState = output.visual.animationState,
                        .animationRate = std::max(
                            0.0f, channelAnimationRate * animationRuleRateFactor),
                        .animationSpeedSyncDurationSeconds =
                            output.visual.animationSpeedSyncDurationSeconds,
                        .animationMode = output.visual.animationMode,
                        .animationManualFrame =
                            output.visual.animationManualFrame,
                        .animationStart = output.visual.animationStart,
                        .subObjectVisibility = output.visual.subObjectVisibility,
                        .boneControls = output.visual.boneControls,
                        .particleSystemBones =
                            output.visual.particleSystemBones,
                        .weaponLaunchBones = output.weaponLaunchBones,
                        .weaponLaunchBoneSequenceOrdinals =
                            output.weaponLaunchBoneSequenceOrdinals,
                    };
                };

                output.modelAsset = rule->model;
                if (channelAnimationState.empty()) {
                    applyConditionPhase(
                        *rule, channelPresentedConditions,
                        static_cast<uint32_t>(visualRuleIndex));
                    configureConditionAnimationStart(*rule, true);
                    render::RenderAnimationCompletionTarget requestedTarget =
                        sealCurrentPhase();
                    const bool hasTransition =
                        transitionRules && channelActiveTransitionRuleIndex <
                            transitionRules->size();
                    const bool waitsForSource =
                        visualRules && channelWaitingSourceVisualRuleIndex <
                            visualRules->size();
                    if (waitsForSource) {
                        output.animationCompletionPhase =
                            render::RenderAnimationCompletionPhase::
                                PresentedSource;
                        if (hasTransition) {
                            applyTransitionPhase(
                                (*transitionRules)[
                                    channelActiveTransitionRuleIndex],
                                0x80000000u |
                                    channelActiveTransitionRuleIndex);
                            output.visual.animationStart = {};
                            output.visual.animationStart.generation =
                                channelAnimationStateGeneration;
                            output.visual.animationStart.restartWhenComplete =
                                ((*transitionRules)[
                                    channelActiveTransitionRuleIndex]
                                     .animationFlags &
                                 game::modelAnimationFlagBit(
                                     game::ModelAnimationFlag::
                                         RestartAnimationWhenComplete)) != 0;
                            output.animationCompletionTarget = sealCurrentPhase();
                            output.animationFinalTarget =
                                std::move(requestedTarget);
                        } else {
                            output.animationCompletionTarget =
                                std::move(requestedTarget);
                        }
                        applyConditionPhase(
                            (*visualRules)[
                                channelWaitingSourceVisualRuleIndex],
                            channelWaitingSourceConditionSnapshot,
                            channelWaitingSourceVisualRuleIndex);
                        configureConditionAnimationStart(
                            (*visualRules)[
                                channelWaitingSourceVisualRuleIndex],
                            false);
                    } else if (hasTransition) {
                        output.animationCompletionTarget =
                            std::move(requestedTarget);
                        applyTransitionPhase(
                            (*transitionRules)[
                                channelActiveTransitionRuleIndex],
                            0x80000000u |
                                channelActiveTransitionRuleIndex);
                        output.visual.animationStart = {};
                        output.visual.animationStart.generation =
                            channelAnimationStateGeneration;
                        output.visual.animationStart.restartWhenComplete =
                            ((*transitionRules)[
                                channelActiveTransitionRuleIndex]
                                 .animationFlags &
                             game::modelAnimationFlagBit(
                                 game::ModelAnimationFlag::
                                     RestartAnimationWhenComplete)) != 0;
                    }
                } else {
                    applyVisibility(rule->subObjectVisibility);
                    output.visual.boneControls = extractTurretControls(
                        weapons, rule->turrets);
                    const auto* barrelTables = resolveWeaponBarrelTables(
                        source.weaponPresentation, archetypeName,
                        currentChannelVisualRuleOffset + visualRuleIndex,
                        rule->weaponBones);
                    appendWeaponPresentationControls(
                        source.weaponPresentation, weapons, visual,
                        rule->weaponBones,
                        barrelTables,
                        channelRecipe
                            ? channelRecipe->projectileBoneFeedbackEnabledSlots
                            : 0u,
                        rule->recoil, simulationFrame,
                        output.visual.subObjectVisibility,
                        output.visual.weaponImpulses);
                    output.visual.particleSystemBones.clear();
                    output.visual.particleSystemBones.reserve(
                        rule->particleSystemBones.size());
                    for (size_t emitterIndex = 0;
                         emitterIndex < rule->particleSystemBones.size();
                         ++emitterIndex) {
                        const game::ModelParticleSystemBoneDefinition& emitter =
                            rule->particleSystemBones[emitterIndex];
                        output.visual.particleSystemBones.push_back({
                            .identity = modelParticleEmitterIdentity(
                                output.id, static_cast<uint32_t>(visualRuleIndex),
                                emitterIndex),
                            .boneName = emitter.boneName,
                            .particleSystem = emitter.particleSystem,
                            .followsAnimatedBone = channelRecipe &&
                                channelRecipe->particlesAttachedToAnimatedBones,
                        });
                    }
                }
                // Object::setCustomIndicatorColor() reaches a W3DModelDraw
                // only when its Draw module explicitly authorizes model
                // recolouring.  Copy the durable, value-only script colour
                // into this sealed render instance rather than touching the
                // shared W3D model/texture cache. A normal script flash uses
                // the same source state separately below and does not imply
                // a persistent house-colour override.
                if (rule->allowsModelColorChange) {
                    if (const std::optional<math::vec3> indicator =
                            source.objectPresentation.customIndicatorColor(identity.id);
                        indicator && std::isfinite(indicator->x()) &&
                            std::isfinite(indicator->y()) &&
                            std::isfinite(indicator->z())) {
                        output.visual.hasScriptIndicatorColor = true;
                        output.visual.scriptIndicatorColor = *indicator;
                    }
                }
            }
        }
        if (channelRecipe && channelRecipe->vehicleDraw.enabled()) {
            if (const VehicleDrawPresentationComponent* vehicle =
                    ecs::try_get<VehicleDrawPresentationComponent>(
                        registry, entity)) {
                const auto state = std::find_if(
                    vehicle->channels.begin(), vehicle->channels.end(),
                    [channelIndex](
                        const VehicleDrawChannelPresentationState& value) {
                        return value.channelIndex == channelIndex;
                    });
                if (state != vehicle->channels.end()) {
                    appendVehicleDrawControls(
                        channelRecipe->vehicleDraw, *state,
                        output.visual.boneControls,
                        output.visual.vehicleTreads);
                    render::RenderVehicleTreadState ignoredTreads;
                    if (output.animationCompletionTarget) {
                        appendVehicleDrawControls(
                            channelRecipe->vehicleDraw, *state,
                            output.animationCompletionTarget->boneControls,
                            ignoredTreads);
                    }
                    if (output.animationFinalTarget) {
                        appendVehicleDrawControls(
                            channelRecipe->vehicleDraw, *state,
                            output.animationFinalTarget->boneControls,
                            ignoredTreads);
                    }
                }
            }
        }
        if (channelRecipe && channelRecipe->supplyDraw.enabled()) {
            output.visual.supplyBones = {
                .prefix = channelRecipe->supplyDraw.bonePrefix,
                .currentSupply = static_cast<uint32_t>(std::min<uint64_t>(
                    supplyCurrent, std::numeric_limits<uint32_t>::max())),
                .maximumSupply = static_cast<uint32_t>(std::min<uint64_t>(
                    supplyMaximum, std::numeric_limits<uint32_t>::max())),
                .enabled = supplyMaximum != 0,
            };
        }
        if (channelRecipe && channelRecipe->policeCarDraw.active) {
            const ObjectLifecycleComponent* lifecycle =
                ecs::try_get<ObjectLifecycleComponent>(
                    registry, entity);
            const uint64_t ageTick = render::policeCarAgeTick(
                simulationFrame, lifecycle ? lifecycle->createdAtTick : 0u);
            const float frame = render::policeCarAnimationFrame(
                identity.id.value, static_cast<uint32_t>(channelIndex),
                ageTick);
            const render::RenderVector color =
                render::policeCarLightColor(frame);
            output.visual.policeCar = {
                .animationFrame = frame,
                .simulationFrame = ageTick,
                .diffuseColor = color,
                .ambientColor = color * 0.5f,
                .heightOffset = 8.0f,
                .innerRadius = 3.0f,
                .outerRadius = 20.0f,
                .enabled = true,
            };
            output.boundingRadius = std::max(
                output.boundingRadius,
                output.visual.policeCar.outerRadius +
                    output.cullingCenterOffset.length());
        }
        if (const DebrisDrawPresentationComponent* debris =
                ecs::try_get<DebrisDrawPresentationComponent>(
                    registry, entity);
            debris && channelRecipe && equalsInsensitive(
                channelRecipe->sourceModuleClass, "W3DDebrisDraw")) {
            output.modelAsset = visual.modelAsset;
            output.shadow.typeMask = debris->shadowTypeMask;
            output.visual.hasScriptIndicatorColor =
                debris->okToChangeModelColor &&
                output.visual.hasScriptIndicatorColor;
            output.visual.debris = {
                .initialAnimation = debris->initialAnimation,
                .flyingAnimation = debris->flyingAnimation,
                .finalAnimation = debris->finalAnimation,
                .ageFrames = simulationFrame >= debris->spawnedTick
                    ? simulationFrame - debris->spawnedTick : 0u,
                .finalAgeFrames =
                    debris->phase == DebrisDrawPresentationPhase::Final &&
                        simulationFrame >= debris->finalStateTick
                    ? simulationFrame - debris->finalStateTick : 0u,
                .logicFramesPerSecond = static_cast<uint32_t>(
                    std::max(1, source.logicFramesPerSecond)),
                .finalState =
                    debris->phase == DebrisDrawPresentationPhase::Final,
                .finalStop = debris->finalStop,
                .enabled = true,
            };
        }
        // GameLOD/Options shadow switches are session-frozen presentation
        // policy. Apply them after every channel-specific override (including
        // W3DDebrisDraw) so an authored descriptor cannot bypass Low/Custom
        // quality settings.
        output.shadow.typeMask = render::filterRenderShadowTypeMask(
            output.shadow.typeMask,
            source.featureQuality.useShadowVolumes,
            source.featureQuality.useShadowDecals);


    result.drawable = !output.modelAsset.empty();
    return result;
}

} // namespace engine::render_extraction_detail
