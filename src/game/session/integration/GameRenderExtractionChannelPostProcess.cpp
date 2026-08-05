#include "GameRenderExtractionChannelPostProcess.h"

#include "GameRenderExtractionDetail.h"
#include "GameRenderExtractionEntityEffects.h"
#include "game/data/presentation/TrackMarksRenderDescriptor.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/simulation/combat/ObjectNeutronMissileSlowDeath.h"
#include "game/object/simulation/combat/ObjectWeaponBonusUpdate.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/simulation/lifecycle/ObjectStructureDestruction.h"
#include "game/object/simulation/movement/ObjectFloat.h"
#include "game/object/simulation/status/ObjectEmpUpdate.h"
#include "game/object/simulation/status/ObjectStealth.h"
#include "game/render/FloatDrawPresentation.h"
#include "game/render/ObjectPresentationPose.h"
#include "game/render/VehicleDrawPresentation.h"
#include "game/script/presentation/ScriptObjectPresentationControls.h"
#include "presentation/render/HeatVisionVisualSettings.h"
#include "presentation/render/TrackMarksPerformanceSettings.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace engine::render_extraction_detail {

void appendFinalizedRenderChannel(
    const ChannelPostProcessSource& source,
    const ChannelPostProcessInput& input,
    render::RenderEntitySnapshot output,
    render::WorldRenderSnapshot& snapshot) {
    const ecs::registry& registry = source.registry;
    const ecs::entity entity = input.entity;
    const size_t channelIndex = input.channelIndex;
    const ObjectIdentityComponent& identity = input.identity;
    const TransformComponent& transform = input.transform;
    const RenderModelComponent& visual = input.visual;
    const ObjectProjectileComponent* projectile = input.projectile;
    const ObjectKindOfComponent* kinds = input.kinds;
    const ObjectHealthComponent* health = input.health;
    const ObjectStatusComponent* status = input.status;
    const game::ThingTemplate* templateData = input.templateData;
    const game::ModelConditionMask& channelPresentedConditions =
        input.presentedConditions;
    const container::String& channelAnimationState = input.animationState;
    const game::ModelAnimationMode channelAnimationMode =
        input.animationMode;
    const float channelAnimationTimeSeconds = input.animationTimeSeconds;
    const float channelAnimationRate = input.animationRate;
    const float animationRuleRateFactor = input.animationRuleRateFactor;
    const float authoredAssetScale = input.authoredAssetScale;
    const uint64_t channelAnimationStateEnterTick =
        input.animationStateEnterTick;
    const bool channelAnimationPaused = input.animationPaused;
    const bool hasSimulationObserver = input.hasSimulationObserver;
    const bool alliedToObserver = input.alliedToObserver;
    const container::Span<const ObjectId> localSelection =
        source.localSelection;
    const script::ScriptObjectPresentationState& objectPresentation =
        source.objectPresentation;
    const RenderBodyParticleGameData& bodyParticleSettings =
        source.bodyParticles;
    const uint64_t simulationFrame = source.simulationFrame;

        output.transform.position = projectObjectPresentationPose(
            registry, entity, transform).position;
        output.transform.scale = {
            authoredAssetScale, authoredAssetScale, authoredAssetScale};
        const ObjectEmpUpdateComponent* empVisual =
            ecs::try_get<ObjectEmpUpdateComponent>(registry, entity);
        if (empVisual && empVisual->visualActive) {
            const float combinedScale =
                authoredAssetScale * empVisual->visualScale.to_float();
            output.transform.scale = {
                combinedScale,
                combinedScale,
                combinedScale,
            };
        }
        const ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, entity);
        const ObjectNeutronMissileProjectileComponent* neutron = projectile
            ? ecs::try_get<ObjectNeutronMissileProjectileComponent>(
                  registry, entity)
            : nullptr;
        if (const ObjectFloatComponent* floatState =
                ecs::try_get<ObjectFloatComponent>(registry, entity)) {
            // FloatUpdate's Drawable pass intentionally discards the prior
            // X/Y attitude and rebuilds the model from its Z yaw plus buoy
            // sway. Enabled controls only water snapping, not this visual.
            output.transform.orientation = objectFloatVisualOrientation(
                transform.rotation, floatState->visualSampleTick);
            output.visual.floatSwayBaseYawRadians = transform.rotation;
            output.visual.floatSwaySampleTick = floatState->visualSampleTick;
            output.visual.floatSwayEnabled = true;
            output.visual.floatSwayRunning =
                floatState->visualSampleTick == simulationFrame;
        } else if (projectile && neutron &&
                   projectile->hasLaunchOrientation) {
            // NeutronMissileUpdate retains the launch transform verbatim while
            // DistanceToTravelBeforeTurning is positive.  This includes the
            // launch-bone roll/pitch and its authored local-X PI/2 model
            // correction; rebuilding a generic Z-up basis on the first
            // detached frame is the visible rack-exit flip.
            output.transform.orientation = projectileFlightOrientation(
                *projectile, snapshot.simulationFrame, true)
                .value_or(math::quat::from_axis_angle(
                    {0.0f, 0.0f, 1.0f}, transform.rotation));
        } else if (physics &&
                   (physics->ownsAttitude || physics->conformsToTerrain)) {
            // Physics owns a fixed-point 3D attitude. Render extraction is
            // the one-way presentation boundary that projects it to the
            // backend-neutral quaternion; Transform.rotation remains the
            // legacy yaw-only compatibility projection.
            //
            // conformsToTerrain is the narrow ground-vehicle case: locomotion
            // still owns XYZ and yaw, and Physics owns only the chassis
            // pitch/roll springing towards the terrain slope. The published
            // basis already carries the live locomotor yaw, so reading it here
            // cannot resurrect a stale Physics heading.
            output.transform.orientation = extractPhysicsOrientation(*physics);
        } else if (projectile) {
            output.transform.orientation = projectileFlightOrientation(
                *projectile, snapshot.simulationFrame)
                .value_or(math::quat::from_axis_angle(
                    {0.0f, 0.0f, 1.0f}, transform.rotation));
        } else {
            output.transform.orientation = math::quat::from_axis_angle(
                {0.0f, 0.0f, 1.0f}, transform.rotation);
        }
        const ObjectLocomotorDrawPresentationState& locomotorDraw =
            visual.locomotorDraw;
        if (locomotorDraw.outputVerticalOffset != math::q32_32{}) {
            output.transform.position[2] +=
                locomotorDraw.outputVerticalOffset.to_float();
        }
        if (locomotorDraw.outputPitch != math::q32_32{} ||
            locomotorDraw.outputRoll != math::q32_32{} ||
            locomotorDraw.outputYaw != math::q32_32{}) {
            // Drawable::applyPhysicsXform post-multiplies this client-only
            // locomotor attitude after the authoritative object transform.
            // Conversion to float happens only in this sealed snapshot path.
            const math::quat pitch = math::quat::from_axis_angle(
                {0.0f, 1.0f, 0.0f},
                locomotorDraw.outputPitch.to_float());
            const math::quat yaw = math::quat::from_axis_angle(
                {0.0f, 0.0f, 1.0f},
                locomotorDraw.outputYaw.to_float());
            const math::quat roll = math::quat::from_axis_angle(
                {1.0f, 0.0f, 0.0f},
                -locomotorDraw.outputRoll.to_float());
            output.transform.orientation =
                (output.transform.orientation * pitch * roll * yaw)
                    .normalized();
        }
        if (visual.weaponRecoilPitch != math::q32_32{} ||
            visual.weaponRecoilRoll != math::q32_32{}) {
            const math::quat pitch = math::quat::from_axis_angle(
                {0.0f, 1.0f, 0.0f},
                visual.weaponRecoilPitch.to_float());
            const math::quat roll = math::quat::from_axis_angle(
                {1.0f, 0.0f, 0.0f},
                -visual.weaponRecoilRoll.to_float());
            output.transform.orientation =
                (output.transform.orientation * pitch * roll).normalized();
        }
        if (const ObjectStructureDestructionComponent* structure =
                ecs::try_get<ObjectStructureDestructionComponent>(
                    registry, entity)) {
            uint32_t selectedOrder = 0;
            const ObjectStructureToppleRuntime* selectedTopple = nullptr;
            const ObjectStructureCollapseRuntime* selectedCollapse = nullptr;
            for (const ObjectStructureToppleRuntime& runtime :
                 structure->topples) {
                if (!runtime.plan ||
                    runtime.ruleIndex >= runtime.plan->rules.size()) continue;
                const uint32_t order =
                    runtime.plan->rules[runtime.ruleIndex].authoredOrder;
                if (!selectedTopple && !selectedCollapse ||
                    order >= selectedOrder) {
                    selectedOrder = order;
                    selectedTopple = &runtime;
                    selectedCollapse = nullptr;
                }
            }
            for (const ObjectStructureCollapseRuntime& runtime :
                 structure->collapses) {
                if (!runtime.plan ||
                    runtime.ruleIndex >= runtime.plan->rules.size()) continue;
                const uint32_t order =
                    runtime.plan->rules[runtime.ruleIndex].authoredOrder;
                if (!selectedTopple && !selectedCollapse ||
                    order >= selectedOrder) {
                    selectedOrder = order;
                    selectedTopple = nullptr;
                    selectedCollapse = &runtime;
                }
            }
            if (selectedTopple) {
                const math::vec3 axis{
                    -selectedTopple->directionY.to_float(),
                    selectedTopple->directionX.to_float(), 0.0f};
                const math::quat tilt = math::quat::from_axis_angle(
                    axis, selectedTopple->accumulatedAngle.to_float());
                output.transform.orientation =
                    (tilt * output.transform.orientation).normalized();
            } else if (selectedCollapse) {
                output.transform.position += math::vec3{
                    selectedCollapse->visualShudderX.to_float(),
                    selectedCollapse->visualShudderY.to_float(),
                    selectedCollapse->currentHeight.to_float(),
                };
            }
        }
        // RefCode W3DModelDraw::adjustTransformMtx sinks a model by its full
        // geometry height and lifts it back out as construction advances, so a
        // structure appears to rise out of the ground. Object's construction
        // percent is CONSTRUCTION_COMPLETE (-1) unless the object is actually
        // being built, which the UNDER_CONSTRUCTION status reproduces here; the
        // height source is GeometryInfo::getMaxHeightAbovePosition().
        const ObjectSaleComponent* sale =
            ecs::try_get<ObjectSaleComponent>(registry, entity);
        const bool underConstruction = status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction));
        if (input.adjustHeightByConstructionPercent && input.geometry &&
            (underConstruction || sale)) {
            float constructionPercent = 0.0f;
            if (sale) {
                constructionPercent =
                    sale->constructionPercent(simulationFrame).to_float();
            } else if (const ObjectConstructionSiteComponent* site =
                    ecs::try_get<ObjectConstructionSiteComponent>(
                        registry, entity)) {
                const uint32_t requiredFrames =
                    std::max(1u, site->requiredFrames);
                constructionPercent = std::clamp(
                    static_cast<float>(site->completedFrames) * 100.0f /
                        static_cast<float>(requiredFrames),
                    0.0f, 100.0f);
            }
            const float geometryHeight = std::max(
                0.0f,
                (input.geometry->shape == ObjectGeometryShape::Sphere
                     ? input.geometry->majorRadiusFixed
                     : input.geometry->heightFixed).to_float());
            // Matrix3D::Translate_Z is a local-axis translation. Structures
            // carry a yaw-only basis so this reduces to world -Z, but keeping
            // the rotation stays faithful for a toppling/terrain-conforming
            // basis and for the collapse offset applied just above.
            output.transform.position +=
                output.transform.orientation.rotate_vec(math::vec3{
                    0.0f, 0.0f,
                    -geometryHeight +
                        geometryHeight * constructionPercent / 100.0f});
        }
        output.visual.modelConditionFlags = channelPresentedConditions.words;
        if (output.visual.animationState.empty()) {
            output.visual.animationState = channelAnimationState;
        }
        // GameSession owns this clock. Extraction copies the completed logic
        // frame verbatim; renderer preparation only samples this detached
        // value and must never write an ECS component back through the
        // snapshot boundary.
        output.visual.animationTimeSeconds = channelAnimationTimeSeconds;
        output.visual.animationSampleTick = simulationFrame;
        output.visual.animationStateEnterTick = channelAnimationStateEnterTick;
        output.visual.animationRate = std::max(
            0.0f, channelAnimationRate * animationRuleRateFactor);
        static const game::ModelConditionMask kPreattackConditions =
            game::modelConditionMaskOf(
                game::ModelConditionFlag::PreattackA,
                game::ModelConditionFlag::PreattackB,
                game::ModelConditionFlag::PreattackC);
        const bool weaponPreattackSelected =
            channelPresentedConditions.intersectionCount(
                kPreattackConditions) != 0;
        float requestedAnimationLoopDuration = weaponPreattackSelected
            ? visual.weaponPreattackLoopDurationSeconds
            : visual.animationLoopDurationSeconds;
        if (sale) {
            // BuildAssistant::sellObject shortens the ordinary construction
            // Draw loops to TOTAL_FRAMES_TO_SELL_OBJECT / 2 (1.5 seconds),
            // once when scaffolds rise and again when SOLD makes them sink.
            // ObjectSaleComponent owns the equivalent confirmed-frame timing;
            // extraction performs the sole conversion to presentation time.
            requestedAnimationLoopDuration =
                static_cast<float>(sale->scaffoldAnimationFrames()) /
                static_cast<float>(std::max(
                    1, source.logicFramesPerSecond));
        }
        output.visual.animationLoopDurationSeconds = std::max(
            0.0f, requestedAnimationLoopDuration);
        if (!channelAnimationState.empty()) {
            output.visual.animationMode =
                toRenderAnimationMode(channelAnimationMode);
        }
        output.visual.animationPaused = channelAnimationPaused;
        output.visual.selected = std::binary_search(localSelection.begin(), localSelection.end(), identity.id);
        // Local shroud is renderer-owned temporal presentation. Preserve the
        // authored hidden bit independently so a timed visibility grace can
        // never unhide a script-hidden model.
        output.visual.hidden = visual.hidden;
        output.visual.treeSwayEnabled = visual.treeSwayEnabled;
        output.visual.objectOpacity = std::isfinite(visual.explicitOpacity)
            ? std::clamp(visual.explicitOpacity, 0.0f, 1.0f)
            : 1.0f;
        if (visual.opacityFadeMode != ObjectOpacityFadeMode::None) {
            output.visual.opacityFadeMode =
                visual.opacityFadeMode == ObjectOpacityFadeMode::In
                ? render::RenderOpacityFadeMode::In
                : render::RenderOpacityFadeMode::Out;
            output.visual.opacityFadeStartTick =
                visual.opacityFadeStartTick;
            output.visual.opacityFadeDurationFrames =
                visual.opacityFadeDurationFrames;
            const uint64_t elapsed = simulationFrame >=
                    visual.opacityFadeStartTick
                ? simulationFrame - visual.opacityFadeStartTick : 0u;
            const uint64_t duration =
                visual.opacityFadeDurationFrames;
            const float progress = duration == 0
                ? 1.0f
                : std::clamp(static_cast<float>(std::min(elapsed, duration)) /
                                 static_cast<float>(duration),
                             0.0f, 1.0f);
            output.visual.objectOpacity =
                visual.opacityFadeMode == ObjectOpacityFadeMode::In
                    ? progress : 1.0f - progress;
        }
        // Retail heat vision is an object-local second material pass driven
        // by stealth detection. Enemy detected stealth suppresses the normal
        // pass, while an allied unit keeps its base pass. Mines are the
        // explicit original exception. Keep this observer-relative decision
        // in the sealed render snapshot rather than querying ECS in DX12.
        if (const ObjectStatusComponent* status =
                ecs::try_get<ObjectStatusComponent>(registry, entity)) {
            const ObjectHealthComponent* heatVisionHealth =
                ecs::try_get<ObjectHealthComponent>(registry, entity);
            const bool effectivelyDead = heatVisionHealth &&
                heatVisionHealth->effectivelyDead;
            const bool stealthed = status->hasAny(
                game::objectStatusBit(game::ObjectStatusFlag::Stealthed));
            const bool detected = status->hasAny(
                game::objectStatusBit(game::ObjectStatusFlag::Detected));
            const bool mine = hasObjectKind(kinds, game::ObjectKindOf::Mine);
            const ObjectStealthComponent* stealthRuntime =
                ecs::try_get<ObjectStealthComponent>(
                    registry, entity);
            // Visibility of stealth is observer-relative presentation state.
            // The simulation retains STEALTHED for targeting/radar rules,
            // while an enemy client receives no ordinary model until a
            // detector extends DETECTED. Spectators/no-observer extraction
            // intentionally keeps the full diagnostic scene.
            if (hasSimulationObserver && stealthed && !detected &&
                !alliedToObserver && !effectivelyDead) {
                output.visual.hidden = true;
            }
            const render::HeatVisionRenderState heatVision =
                render::resolveHeatVisionRenderState(
                    hasSimulationObserver && stealthed,
                    detected,
                    alliedToObserver,
                    mine,
                    effectivelyDead);
            const bool localHint = hasSimulationObserver &&
                alliedToObserver && !stealthed && !effectivelyDead &&
                stealthRuntime && stealthRuntime->plan &&
                status->hasAny(stealthRuntime->plan->hintDetectableStatuses);
            const float heatOpacity = stealthRuntime
                ? std::clamp(
                      stealthRuntime->heatVisionOpacity.to_float(), 0.0f, 1.0f)
                : 0.0f;
            output.visual.heatVisionIntensity =
                (heatVision.intensity > 0.0f || localHint)
                    ? heatOpacity : 0.0f;
            output.visual.heatVisionOnly = heatVision.only;

            // The ordinary friendly stealth look remains visible and pulses
            // between the authored minimum and one.  Enemy opacity is not
            // reused here: undetected enemies are hidden above, while a
            // detected enemy uses the replacement heat-only pass. Mines are
            // the retail zero-opacity special case for their owner.
            if (hasSimulationObserver && alliedToObserver && stealthed &&
                !effectivelyDead) {
                if (mine) {
                    output.visual.objectOpacity = 0.0f;
                } else {
                    const float baseOpacity = output.visual.objectOpacity;
                    const float minimum = stealthRuntime &&
                            stealthRuntime->plan
                        ? std::clamp(
                              stealthRuntime->plan->friendlyOpacityMinimum
                                  .to_float(),
                              0.0f, 1.0f)
                        : 0.5f;
                    const float phase = stealthRuntime
                        ? stealthRuntime->friendlyPulsePhaseRadians.to_float()
                        : 0.0f;
                    output.visual.friendlyStealthBaseOpacity = baseOpacity;
                    output.visual.friendlyStealthMinimumOpacity = minimum;
                    output.visual.friendlyStealthPulsePhaseRadians = phase;
                    output.visual.friendlyStealthPulseEnabled = true;
                    output.visual.friendlyStealthPulseRunning =
                        stealthRuntime && stealthRuntime->enabled;
                    output.visual.objectOpacity =
                        render::resolveFriendlyStealthOpacity(
                            baseOpacity, minimum,
                            phase, false);
                }
            }
        }
        // Drawable tint statuses are detached presentation facts. The retail
        // poisoned branch is commented out, so PoisonedBehavior deliberately
        // contributes no invented green tint. TempWeaponBonusHelper does use
        // the signed FRENZY envelope; script flashes remain additive.
        float additiveTintR = 0.0f;
        float additiveTintG = 0.0f;
        float additiveTintB = 0.0f;
        if (empVisual && empVisual->visualActive && empVisual->plan &&
            empVisual->visualRuleIndex < empVisual->plan->rules.size()) {
            const game::ObjectEmpParameters& rule =
                empVisual->plan->rules[empVisual->visualRuleIndex];
            const float blend = std::clamp(
                empVisual->visualBlend.to_float(), 0.0f, 1.0f);
            for (size_t channel = 0; channel < 3; ++channel) {
                const float start = rule.startColor[channel] * 2.0f - 1.0f;
                const float end = rule.endColor[channel] * 5.0f - 2.5f;
                const float tint = std::lerp(start, end, blend);
                if (channel == 0) additiveTintR += tint;
                if (channel == 1) additiveTintG += tint;
                if (channel == 2) additiveTintB += tint;
            }
        }
        const DisabledTintEnvelope disabledTint = disabledTintEnvelope(
            registry, entity, simulationFrame);
        const float disabledTintScale = disabledTint.scale;
        output.visual.disabledTintMode = disabledTint.mode;
        output.visual.disabledTintStartTick = disabledTint.startTick;
        output.visual.disabledTintReleaseStartScale =
            disabledTint.releaseStartScale;
        output.visual.disabledTintSampleScale = disabledTint.scale;
        additiveTintR -= 0.5f * disabledTintScale;
        additiveTintG -= 0.5f * disabledTintScale;
        additiveTintB -= 0.5f * disabledTintScale;
        if (const ObjectTemporaryWeaponBonusComponent* temporary =
                ecs::try_get<ObjectTemporaryWeaponBonusComponent>(
                    registry, entity);
            temporary && (temporary->current ||
                          temporary->tintReleaseStartFrame != 0)) {
            constexpr float kTintEnvelopeFrames = 30.0f;
            float scale = 0.0f;
            if (temporary->current) {
                output.visual.temporaryBonusTintMode =
                    render::RenderTintEnvelopeMode::Attack;
                output.visual.temporaryBonusTintStartTick =
                    temporary->tintStartedTick;
                const uint64_t age = simulationFrame >= temporary->tintStartedTick
                    ? simulationFrame - temporary->tintStartedTick : 0;
                scale = std::min(
                    1.0f, static_cast<float>(age + 1u) /
                              kTintEnvelopeFrames);
            } else if (simulationFrame >= temporary->tintReleaseTick) {
                output.visual.temporaryBonusTintMode =
                    render::RenderTintEnvelopeMode::Release;
                output.visual.temporaryBonusTintStartTick =
                    temporary->tintReleaseTick;
                output.visual.temporaryBonusTintReleaseStartScale =
                    static_cast<float>(temporary->tintReleaseStartFrame) /
                    kTintEnvelopeFrames;
                const uint64_t age =
                    simulationFrame - temporary->tintReleaseTick;
                // TintEnvelope release uses the authored peak/30 as a fixed
                // per-frame decrement.  A partially attacked tint therefore
                // reaches zero sooner instead of stretching its remaining
                // value across another full 30 frames.
                scale = std::max(
                    0.0f,
                    static_cast<float>(temporary->tintReleaseStartFrame) /
                        kTintEnvelopeFrames -
                        static_cast<float>(age + 1u) /
                            kTintEnvelopeFrames);
            }
            const ObjectHealthComponent* tintHealth =
                ecs::try_get<ObjectHealthComponent>(registry, entity);
            // Gaining-subdual/disabled statuses have priority over frenzy in
            // Drawable's original if/else ladder. The current gameplay
            // projection exposes subdued explicitly, so never overlay frenzy
            // while that higher-priority state is active.
            if ((tintHealth && tintHealth->subdued) ||
                disabledTintScale > 0.0f) scale = 0.0f;
            output.visual.temporaryBonusTintSampleAppliedScale = scale;
            output.visual.temporaryBonusTintInfantry =
                hasObjectKind(kinds, game::ObjectKindOf::Infantry);
            if (output.visual.temporaryBonusTintInfantry) {
                additiveTintG -= 0.7f * scale;
                additiveTintB -= 0.7f * scale;
            } else {
                additiveTintR += 0.2f * scale;
                additiveTintG -= 0.2f * scale;
                additiveTintB -= 0.2f * scale;
            }
        }
        output.visual.scriptFlashBaseTint = {
            additiveTintR, additiveTintG, additiveTintB};
        if (const script::ScriptObjectFlashPresentation* flashDescriptor =
                objectPresentation.flash(identity.id)) {
            output.visual.scriptFlashColor = flashDescriptor->color;
            output.visual.scriptFlashFirstPulseTick =
                flashDescriptor->firstPulseTick;
            output.visual.scriptFlashEndTick = flashDescriptor->endTick;
            output.visual.scriptFlashPulseIntervalTicks =
                flashDescriptor->pulseIntervalTicks;
            output.visual.scriptFlashDecayTicks =
                flashDescriptor->decayTicks;
            output.visual.scriptFlashEnabled = true;
        }
        if (const std::optional<math::vec3> flash =
                objectPresentation.flashTint(identity.id, simulationFrame)) {
            additiveTintR += flash->x();
            additiveTintG += flash->y();
            additiveTintB += flash->z();
        }
        output.visual.scriptFlashTint = {
            additiveTintR, additiveTintG, additiveTintB};
        output.boundingRadius = std::max(
            output.boundingRadius,
            (std::isfinite(visual.boundingRadius)
                 ? std::max(0.0f, visual.boundingRadius) : 0.0f) +
                output.cullingCenterOffset.length());
        if (templateData && channelIndex <
                templateData->drawVisualChannels.size()) {
            const ObjectLocomotionComponent* locomotion =
                ecs::try_get<ObjectLocomotionComponent>(registry, entity);
            const ObjectAirborneComponent* airborne =
                ecs::try_get<ObjectAirborneComponent>(registry, entity);
            const bool aboveTerrain = (airborne && airborne->isAirborne) ||
                (physics && physics->state != ObjectPhysicsMotionState::Grounded);
            const game::ModelDrawVisualChannel& drawChannel =
                templateData->drawVisualChannels[channelIndex];
            const auto trackMarksVisual = std::find_if(
                templateData->trackMarksVisuals.begin(),
                templateData->trackMarksVisuals.end(),
                [&drawChannel](const game::TrackMarksVisualDescriptor& value) {
                    return value.sourceModuleTag == drawChannel.sourceModuleTag &&
                        value.sourceModuleClass == drawChannel.sourceModuleClass;
                });
            if (trackMarksVisual != templateData->trackMarksVisuals.end() &&
                !output.visual.hidden && !aboveTerrain) {
                const TrackMarksRenderDescriptor trackMarks =
                    compileTrackMarksRenderDescriptor(
                        *trackMarksVisual,
                        source.trackMarks);
                if (trackMarks.visual.enabled &&
                    snapshot.trackMarks.size() <
                        trackMarks.performance.maximumTrackedObjects) {
                    const float yaw = std::isfinite(transform.rotation)
                        ? transform.rotation : 0.0f;
                    const uint32_t fadeLifetimeFrames =
                        track_marks::performance_limits::
                            fadeFramesFromMilliseconds(
                                trackMarks.performance.fadeDelayMilliseconds,
                                static_cast<uint32_t>(std::max(
                                    1, source.logicFramesPerSecond)));
                    snapshot.trackMarks.push_back({
                        .objectId = identity.id.value,
                        .position = output.transform.position,
                        .forward = {std::cos(yaw), std::sin(yaw), 0.0f},
                        .textureName = trackMarks.visual.textureName,
                        .leftWidthBone = trackMarks.visual.leftWidthBone,
                        .rightWidthBone = trackMarks.visual.rightWidthBone,
                        .additionalTreadWidth =
                            trackMarks.visual.additionalTreadWidth,
                        // The render-pose bridge replaces this only when both
                        // authored bones resolve. Keep RefCode's exact
                        // 1.4 * MAP_XY_FACTOR fallback as detached input.
                        .trackWidth = trackMarks.visual.fallbackWidth,
                        .edgeSpacing = trackMarks.visual.segmentLength,
                        .maximumEdges = trackMarks.performance.maximumEdges,
                        .opaqueEdges = trackMarks.performance.opaqueEdges,
                        .fadeLifetimeFrames = fadeLifetimeFrames,
                        .moving = sampleVehicleMotive(
                            physics, locomotion).moving,
                    });
                }
            }
        }
        // Drawable::drawIconUI owns a distinct post-world object-icon pass.
        // Freeze the active per-ObjectId emoticon as value data here rather
        // than letting the renderer query ScriptObjectPresentationState or
        // ECS after the logic snapshot has been sealed.
        if (channelIndex == 0) {
        if (const script::ScriptObjectEmoticonPresentation* emoticon =
                objectPresentation.emoticon(identity.id);
            emoticon && !output.visual.hidden &&
            (!emoticon->lastVisibleTick || simulationFrame <= *emoticon->lastVisibleTick)) {
            const float boundedRadius = std::isfinite(output.boundingRadius) &&
                    output.boundingRadius > math::EPSILON
                ? output.boundingRadius
                : 1.0f;
            snapshot.objectIcons.icons.push_back({
                .objectId = identity.id.value,
                // The original derives this from the health-bar region. Our
                // renderer-neutral equivalent uses the frozen model radius,
                // keeping the icon just above the object without a Drawable
                // or a screen-region pointer crossing the boundary.
                .worldAnchor = output.transform.position + math::vec3{
                    0.0f, 0.0f, std::max(1.0f, boundedRadius * 1.15f)},
                .animationName = emoticon->animationName,
                .presentationEpoch = emoticon->stamp.presentationEpoch,
                .presentationSequence = emoticon->stamp.sequence,
                .startTick = emoticon->startTick,
                .lastVisibleTick = emoticon->lastVisibleTick.value_or(0),
                .logicFramesPerSecond = static_cast<uint32_t>(
                    std::max(1, source.logicFramesPerSecond)),
                .permanent = !emoticon->lastVisibleTick.has_value(),
            });
        }
        }
        if (health && health->acceptsDamage &&
            !(status && status->hasAny(game::objectStatusBit(
                game::ObjectStatusFlag::UnderConstruction)))) {
            const bool aflame = status && status->hasAny(
                game::objectStatusBit(game::ObjectStatusFlag::Aflame));
            const auto appendBodyParticles = [&](auto& target) {
                appendBodyParticleDescriptors(
                    target.particleSystemBones, bodyParticleSettings,
                    output.id, identity.id, health->damageState, aflame);
            };
            appendBodyParticles(output.visual);
            if (output.animationCompletionTarget) {
                appendBodyParticles(*output.animationCompletionTarget);
            }
            if (output.animationFinalTarget) {
                appendBodyParticles(*output.animationFinalTarget);
            }
        }
        snapshot.entities.push_back(std::move(output));

}

} // namespace engine::render_extraction_detail
