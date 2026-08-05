#include "game/session/frame/GameSessionWeaponEventPublisher.h"
#include "game/session/frame/GameSessionFxAnchorSnapshot.h"
#include "game/session/state/GameSessionDomainState.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/component/ObjectDirty.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>

namespace engine {
namespace {

// W3DModelDraw::doWeaponFireFX resolves WeaponFireFXBone from the visual rule
// which is currently being presented. Keep this lookup read-only: all values
// come from the session-frozen archetype and compact visual selection state.
struct CurrentWeaponFireFxBinding final {
    container::String bone;
    uint32_t channelIndex = 0;
    bool exact = false;
};

[[nodiscard]] uint64_t renderPresentationObjectKey(
    ObjectId object, uint32_t channelIndex) noexcept {
    if (!object) return 0;
    return channelIndex == 0
        ? static_cast<uint64_t>(object.value)
        : (static_cast<uint64_t>(channelIndex) << 32u) |
              static_cast<uint64_t>(object.value);
}

[[nodiscard]] container::Vector<CurrentWeaponFireFxBinding>
currentWeaponFireFxBones(
    const ecs::registry& registry, ecs::entity entity,
    game::WeaponSlot slot,
    const game::W3dPristineBoneCatalog* pristineBones,
    uint32_t barrelSequenceOrdinal,
    bool usesFiringPresentation) {
    const size_t slotIndex = static_cast<size_t>(slot);
    if (slotIndex >= game::kWeaponSlotCount) return {};
    const ThingTemplateComponent* source =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const RenderModelComponent* visual =
        ecs::try_get<RenderModelComponent>(registry, entity);
    if (!source || !source->archetype || !visual) return {};

    const game::ThingTemplate& templateData =
        source->archetype->templateData;
    const game::ModelConditionMask presentationConditions =
        usesFiringPresentation
        ? game::weaponFiringModelConditions(
              visual->modelConditionFlags,
              static_cast<uint32_t>(slotIndex))
        : visual->modelConditionFlags;
    container::Vector<CurrentWeaponFireFxBinding> result;
    result.reserve(templateData.drawVisualChannels.size());
    size_t flattenedVisualOffset = 0;
    size_t flattenedTransitionOffset = 0;
    for (size_t channelIndex = 0;
         channelIndex < templateData.drawVisualChannels.size();
         ++channelIndex) {
        const game::ModelDrawVisualChannel& channel =
            templateData.drawVisualChannels[channelIndex];
        const RenderModelChannelState* state =
            channelIndex < visual->channels.size()
            ? &visual->channels[channelIndex] : nullptr;
        const game::ModelWeaponBoneDefinition* weaponBones = nullptr;
        size_t normalVisualIndex = std::numeric_limits<size_t>::max();
        size_t poseRuleIndex = std::numeric_limits<size_t>::max();
        if (!usesFiringPresentation && state &&
            state->activeTransitionRuleIndex < channel.transitions.size()) {
            weaponBones = &channel.transitions[
                state->activeTransitionRuleIndex].weaponBones[slotIndex];
            poseRuleIndex = templateData.modelConditionVisuals.size() +
                flattenedTransitionOffset +
                state->activeTransitionRuleIndex;
        } else if (!usesFiringPresentation && state &&
                   state->waitingSourceVisualRuleIndex <
                       channel.conditionVisuals.size()) {
            normalVisualIndex = state->waitingSourceVisualRuleIndex;
            poseRuleIndex = flattenedVisualOffset + normalVisualIndex;
            weaponBones = &channel.conditionVisuals[
                normalVisualIndex].weaponBones[slotIndex];
        } else {
            size_t visualIndex = usesFiringPresentation
                ? game::selectModelConditionVisualRuleIndex(
                      channel, presentationConditions)
                : state ? state->resolvedVisualRuleIndex : UINT32_MAX;
            if (visualIndex >= channel.conditionVisuals.size()) {
                visualIndex = game::selectModelConditionVisualRuleIndex(
                    channel, presentationConditions);
            }
            if (visualIndex < channel.conditionVisuals.size()) {
                normalVisualIndex = visualIndex;
                poseRuleIndex = flattenedVisualOffset + normalVisualIndex;
                weaponBones = &channel.conditionVisuals[
                    visualIndex].weaponBones[slotIndex];
            }
        }
        if (weaponBones && poseRuleIndex != std::numeric_limits<size_t>::max() &&
            pristineBones && pristineBones->isLoaded()) {
            const game::W3dWeaponBarrelTable table =
                pristineBones->resolveWeaponBarrels(
                    source->archetype->name,
                    poseRuleIndex,
                    weaponBones->fireFxBone, weaponBones->recoilBone,
                    weaponBones->muzzleFlash, weaponBones->launchBone);
            if (!table.barrels.empty()) {
                const uint32_t sequence = std::max<uint32_t>(
                    1u, barrelSequenceOrdinal);
                const size_t barrelIndex = sequence - 1u < table.barrels.size()
                    ? static_cast<size_t>(sequence - 1u) : 0u;
                const game::W3dWeaponBarrelEntry& barrel =
                    table.barrels[barrelIndex];
                if (!barrel.fireFxBone.empty()) {
                    result.push_back({
                        .bone = barrel.fireFxBone,
                        .channelIndex = static_cast<uint32_t>(channelIndex),
                        .exact = true,
                    });
                    return result;
                }
                flattenedVisualOffset += channel.conditionVisuals.size();
                flattenedTransitionOffset += channel.transitions.size();
                continue;
            }
        }
        if (weaponBones && !weaponBones->fireFxBone.empty()) {
            result.push_back({
                .bone = weaponBones->fireFxBone,
                .channelIndex = static_cast<uint32_t>(channelIndex),
            });
            return result;
        }
        flattenedVisualOffset += channel.conditionVisuals.size();
        flattenedTransitionOffset += channel.transitions.size();
    }

    if (!result.empty() || !templateData.drawVisualChannels.empty()) {
        return result;
    }

    // Compatibility path for templates compiled before per-Draw channels.
    if (!usesFiringPresentation &&
        visual->activeTransitionRuleIndex <
            templateData.modelConditionTransitions.size()) {
        result.push_back({
            .bone = templateData.modelConditionTransitions[
                visual->activeTransitionRuleIndex]
                .weaponBones[slotIndex].fireFxBone,
        });
        return result;
    }
    size_t visualIndex = usesFiringPresentation
        ? game::selectModelConditionVisualRuleIndex(
              templateData, presentationConditions)
        : visual->resolvedVisualRuleIndex;
    if (visualIndex >= templateData.modelConditionVisuals.size()) {
        visualIndex = game::selectModelConditionVisualRuleIndex(
            templateData, presentationConditions);
    }
    if (visualIndex < templateData.modelConditionVisuals.size()) {
        result.push_back({
            .bone = templateData.modelConditionVisuals[visualIndex]
                .weaponBones[slotIndex].fireFxBone,
        });
    }
    return result;
}

} // namespace

GameSessionWeaponEventPublisher::GameSessionWeaponEventPublisher(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionScriptPresentationState& presentation,
    GameSessionGameplayPublicationPort publication) noexcept
    : m_content(content),
      m_world(world),
      m_presentation(presentation),
      m_publication(publication) {}

void GameSessionWeaponEventPublisher::publish(
    container::Vector<ObjectWeaponEvent> events) {
    for (const ObjectWeaponEvent& event : events) {
        if (event.kind == ObjectWeaponEventKind::FireSoundLoopStarted ||
            event.kind == ObjectWeaponEventKind::FireSoundLoopStopped) {
            if (!event.audioEventName.empty()) {
                static_cast<void>(m_publication.emitAudioControlEvent({
                    .kind = game::GameAudioControlKind::SetObjectLoopingSoundEnabled,
                    .enabled = event.kind ==
                        ObjectWeaponEventKind::FireSoundLoopStarted,
                    .eventName = event.audioEventName,
                    .object = event.source,
                }));
            }
            continue;
        }
        if (event.kind == ObjectWeaponEventKind::RapidFireVoice) {
            if (!event.audioEventName.empty()) {
                const std::optional<game::FxInvocationAnchor> anchor =
                    session_fx::snapshotAnchor(
                        m_world.m_registry,
                        m_world.m_objects, event.source);
                static_cast<void>(m_publication.emitAudioEvent({
                    .eventName = event.audioEventName,
                    .emitter = event.source,
                    .owner = event.source,
                    .position = anchor
                        ? std::optional<math::vec3>{anchor->position}
                        : std::nullopt,
                }));
            }
            continue;
        }
        if (event.kind != ObjectWeaponEventKind::Fired) continue;
        const std::optional<ecs::entity> sourceEntity =
            m_world.m_objects.entityFromIdIncludingPending(event.source);
        const std::optional<PlayerId> sourcePlayer = event.sourcePlayer
            ? std::optional<PlayerId>{event.sourcePlayer} : std::nullopt;
        const ObjectContainedByComponent* sourceContainment = sourceEntity
            ? ecs::try_get<ObjectContainedByComponent>(
                  m_world.m_registry, *sourceEntity)
            : nullptr;
        const bool sourceFiresFromEnclosingContainer =
            sourceContainment && sourceContainment->container &&
            sourceContainment->enclosing;
        const ObjectId enclosingContainer = sourceFiresFromEnclosingContainer
            ? sourceContainment->container : INVALID_OBJECT_ID;
        if (sourceEntity) {
            ecs::remove<ObjectUndetectedDefectorComponent>(
                m_world.m_registry, *sourceEntity);
        }
        const game::WeaponTemplate* definition =
            m_content.m_contentSnapshot.findWeapon(event.content);
        if (!definition) continue;
        if (sourceFiresFromEnclosingContainer) {
            const std::optional<ecs::entity> hostEntity =
                m_world.m_objects.entityFromIdIncludingPending(
                    enclosingContainer);
            ObjectGarrisonFirePointComponent* firePoints = hostEntity
                ? ecs::try_get<ObjectGarrisonFirePointComponent>(
                      m_world.m_registry, *hostEntity)
                : nullptr;
            if (firePoints) {
                const auto assignment = std::lower_bound(
                    firePoints->assignments.begin(),
                    firePoints->assignments.end(), event.source,
                    [](const ObjectGarrisonFirePointAssignment& value,
                       ObjectId id) {
                        return value.occupant < id;
                    });
                if (assignment != firePoints->assignments.end() &&
                    assignment->occupant == event.source) {
                    assignment->pointPosition = event.sourcePosition;
                    assignment->lastEffectFireTick = event.confirmedTick;
                    assignment->lastEffectFireSequence =
                        event.sourceShotSequence != 0
                        ? event.sourceShotSequence : 1u;
                    // RefCode deliberately omits the GarrisonGun muzzle
                    // flash for poison weapons, while retaining the barrel
                    // and the weapon's ordinary FX/audio path.
                    assignment->suppressMuzzleFlash =
                        definition->damageType == game::DamageType::POISON;
                    ++firePoints->revision;
                    markObjectDirty(
                        m_world.m_registry, *hostEntity,
                        ObjectDirtyDomain::RenderExtraction);
                }
            }
        }
        if (definition->damageType == game::DamageType::DEPLOY &&
            sourceEntity) {
            const ObjectContainmentRuntimeComponent* containment =
                ecs::try_get<ObjectContainmentRuntimeComponent>(
                    m_world.m_registry, *sourceEntity);
            const bool assaultTransport = containment && containment->plan &&
                std::any_of(
                    containment->plan->behaviorRules.begin(),
                    containment->plan->behaviorRules.end(),
                    [](const ObjectTransportBehaviorRule& rule) {
                        return rule.kind ==
                            ObjectTransportBehaviorKind::AssaultTransportAI;
                    });
            if (assaultTransport) {
                // RefCode's DAMAGE_DEPLOY special case calls
                // AssaultTransportAIInterface::beginAssault(victim).  Keep
                // that Weapon -> Containment edge as a stable-ID request.
                static_cast<void>(
                    m_world.m_objectSimulation.requestTransportBehavior(
                        m_world.m_registry, m_world.m_objects, {
                            .kind = ObjectTransportBehaviorRequestKind::AssaultTransportUpdate,
                            .object = event.source,
                            .target = event.target,
                            .confirmedTick = event.confirmedTick,
                        }));
            }
        }
        const uint32_t barrelSequenceOrdinal =
            event.sourceBarrelSequenceOrdinal != 0
            ? event.sourceBarrelSequenceOrdinal
            : game::weaponBarrelSequenceOrdinal(
                  event.sourceShotSequence, definition->shotsPerBarrel);
        if (sourceEntity) {
            if (RenderModelComponent* visual =
                    ecs::try_get<RenderModelComponent>(
                        m_world.m_registry, *sourceEntity)) {
                const size_t slot = static_cast<size_t>(event.slot);
                if (slot < visual->lastWeaponFireTicks.size()) {
                    visual->lastWeaponFireTicks[slot] = event.confirmedTick;
                    visual->lastWeaponFireSequences[slot] =
                        std::max<uint32_t>(1u, barrelSequenceOrdinal);
                }
                if (definition->fixed.weaponRecoilRadians !=
                    math::q32_32{}) {
                    const ObjectFixedTransformComponent* transform =
                        ecs::try_get<ObjectFixedTransformComponent>(
                            m_world.m_registry, *sourceEntity);
                    const ObjectLocomotionComponent* locomotion =
                        ecs::try_get<ObjectLocomotionComponent>(
                            m_world.m_registry, *sourceEntity);
                    if (transform && locomotion) {
                        const math::q32_32 localRecoilDirection =
                            event.recoilDirectionRadians -
                            transform->yawRadians +
                            math::q32_32::from_raw(13'493'037'705ll);
                        const math::q32_32_sincos direction =
                            math::fixed_sincos(localRecoilDirection);
                        visual->weaponRecoilPitchRate +=
                            definition->fixed.weaponRecoilRadians *
                            direction.cosine;
                        visual->weaponRecoilRollRate +=
                            definition->fixed.weaponRecoilRadians *
                            direction.sine;
                    }
                }
                markObjectDirty(
                    m_world.m_registry, *sourceEntity,
                    ObjectDirtyDomain::RenderExtraction);
            }
        }
        std::optional<game::FxInvocationAnchor> primary =
            session_fx::snapshotAnchor(
                m_world.m_registry, m_world.m_objects, event.source);
        std::optional<game::FxInvocationAnchor> secondary =
            session_fx::snapshotAnchor(
                m_world.m_registry, m_world.m_objects, event.target);
        if (event.hasFrozenPositions &&
            (sourceFiresFromEnclosingContainer || !primary)) {
            // GarrisonContain fires the passenger's weapon from its selected
            // FIREPOINT, not from the enclosed passenger transform and not
            // from a bone on that hidden Drawable. Combat already seals the
            // exact fixed-point source position into the Fired event; consume
            // it as a world-position presentation anchor. Retaining the host
            // ObjectId preserves local ownership/visibility attribution.
            primary = game::FxInvocationAnchor{
                .object = sourceFiresFromEnclosingContainer
                    ? enclosingContainer : event.source,
                .position = {
                    event.sourcePosition.x.to_float(),
                    event.sourcePosition.y.to_float(),
                    event.sourcePosition.z.to_float()},
            };
        }
        if (!secondary && event.hasFrozenPositions) {
            secondary = game::FxInvocationAnchor{
                .object = event.target,
                .position = {
                    event.impactPosition.x.to_float(),
                    event.impactPosition.y.to_float(),
                    event.impactPosition.z.to_float()},
            };
        }
        const size_t veterancyIndex = std::min<size_t>(
            static_cast<size_t>(event.veterancy),
            game::WeaponTemplate::kVeterancyLevelCount - 1);
        const container::String& fireFxName =
            definition->fireFXs[veterancyIndex];
        std::optional<math::vec3> position;
        if (primary) position = primary->position;
        if (!definition->fireSound.empty() &&
            definition->fireSoundLoopTimeMilliseconds == 0) {
            static_cast<void>(m_publication.emitAudioEvent({
                .eventName = definition->fireSound,
                .emitter = event.source,
                .owner = event.source,
                .position = position,
            }));
        }
        if (!definition->laserName.empty() && primary && secondary) {
            game::FxInvocationAnchor laserTarget = *secondary;
            const std::optional<ecs::entity> targetEntity =
                m_world.m_objects.entityFromIdIncludingPending(event.target);
            const bool projectileTarget = targetEntity &&
                ecs::try_get<ObjectProjectileComponent>(
                    m_world.m_registry, *targetEntity) != nullptr;
            const ObjectAirborneComponent* airborne = targetEntity
                ? ecs::try_get<ObjectAirborneComponent>(
                      m_world.m_registry, *targetEntity)
                : nullptr;
            const bool airborneTarget = airborne && airborne->isAirborne;
            if (!projectileTarget && !airborneTarget) {
                // Weapon::createLaser raises ordinary ground-object targets
                // ten world units, but aims directly at projectiles/airborne
                // objects whose transform already denotes the intended hit.
                laserTarget.position[2] += 10.0f;
            }
            static_cast<void>(m_publication.emitFxInvocationEvent({
                .directBeam = game::FxDirectBeamRequest{
                    .objectTemplate = definition->laserName,
                },
                .anchorKind = definition->laserBoneName.empty()
                    ? game::FxInvocationAnchorKind::ObjectAttachment
                    : game::FxInvocationAnchorKind::BonePosition,
                .primary = *primary,
                .secondary = laserTarget,
                .sourcePlayer = sourcePlayer,
                .boneName = definition->laserBoneName,
            }));
        }
        if (event.fxPolicy == game::WeaponFxPolicy::Play &&
            !fireFxName.empty() && primary) {
            container::Vector<CurrentWeaponFireFxBinding> fireFxBindings =
                sourceEntity && !sourceFiresFromEnclosingContainer
                ? currentWeaponFireFxBones(
                      m_world.m_registry, *sourceEntity, event.slot,
                      m_content.m_contentSnapshot.pristineBoneCatalog(),
                      barrelSequenceOrdinal,
                      event.usesFiringPresentation)
                : container::Vector<CurrentWeaponFireFxBinding>{};
            if (fireFxBindings.empty()) {
                fireFxBindings.push_back({});
            }
            for (const CurrentWeaponFireFxBinding& fireFx : fireFxBindings) {
                game::FxInvocationAnchor primaryAnchor = *primary;
                if (!fireFx.bone.empty()) {
                    primaryAnchor.presentationObjectKey =
                        renderPresentationObjectKey(
                            event.source, fireFx.channelIndex);
                }
                static_cast<void>(m_publication.emitFxInvocationEvent({
                    .fxListName = fireFxName,
                    .anchorKind = fireFx.bone.empty()
                        ? game::FxInvocationAnchorKind::WorldPosition
                        : game::FxInvocationAnchorKind::BonePosition,
                    .primary = primaryAnchor,
                    .secondary = secondary,
                    .sourcePlayer = sourcePlayer,
                    .boneName = fireFx.bone,
                    .boneNameIsPrefix =
                        !fireFx.bone.empty() && !fireFx.exact,
                    .boneNameSequenceOrdinal =
                        !fireFx.bone.empty() && !fireFx.exact
                        ? barrelSequenceOrdinal
                        : 0u,
                    .boneNamePrefixFallsBackToBare =
                        !fireFx.bone.empty() && !fireFx.exact,
                    .primarySpeed =
                        definition->fixed.weaponSpeed.to_float(),
                    .overrideRadius =
                        definition->fixed.primaryDamageRadius.to_float(),
                }));
            }
        }
    }
}

} // namespace engine
