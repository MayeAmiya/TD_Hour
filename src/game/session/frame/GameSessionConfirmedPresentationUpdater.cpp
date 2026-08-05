#include "game/session/frame/GameSessionConfirmedPresentationUpdater.h"

#include "game/session/frame/GameSessionVisualAnimationUpdate.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/render/LocomotorDrawPresentation.h"
#include "game/render/VehicleDrawPresentation.h"
#include "game/audio/GameAudioEvents.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/component/ObjectDirty.h"

#include <algorithm>

namespace engine {

void GameSessionConfirmedPresentationUpdater::update(float deltaSeconds) {
    bool night = m_presentation.m_renderGameDataSettings.visual
        .defaultTimeOfDay == RenderTimeOfDay::Night;
    if (m_content.m_terrain.isLoaded()) {
        const auto& heightfield = m_content.m_terrain.map().heightfield();
        if (heightfield.globalLighting &&
            heightfield.globalLighting->timeOfDay != 0) {
            // Serialized TimeOfDay is INVALID=0, MORNING=1 through NIGHT=4.
            night = heightfield.globalLighting->timeOfDay == 4;
        }
    }
    static_cast<void>(updateAuthoritativeObjectModelConditions(
        m_world.m_registry, m_world.m_objects,
        m_world.m_modelConditionAuthority,
        {
            .localPlayer = m_content.m_players.localPlayerId(),
            .players = &m_content.m_players,
            .forceModelsToFollowTimeOfDay =
                m_presentation.m_renderGameDataSettings.visual
                    .forceModelsToFollowTimeOfDay,
            .forceModelsToFollowWeather =
                m_presentation.m_renderGameDataSettings.visual
                    .forceModelsToFollowWeather,
            .night = night,
            .snowy = m_presentation.m_renderGameDataSettings.visual
                .defaultWeather == RenderWeather::Snowy,
        },
        m_presentation.m_confirmedTick));
    updateLocomotorDrawPresentation(
        m_world.m_registry, m_world.m_objects,
        m_content.m_objectSimulationRules.logicDeltaSeconds,
        m_presentation.m_confirmedTick);
    updateVehicleDrawPresentation(
        m_world.m_registry, m_content.m_terrain, deltaSeconds,
        m_presentation.m_confirmedTick);
    const auto recoilObjects = ecs::view<
        RenderModelComponent, const ObjectLocomotionComponent>(
            m_world.m_registry);
    for (const ecs::entity entity : recoilObjects) {
        RenderModelComponent& visual = recoilObjects.template get<
            RenderModelComponent>(entity);
        const ObjectLocomotionComponent& locomotion =
            recoilObjects.template get<const ObjectLocomotionComponent>(
                entity);
        const math::q32_32 oldPitch = visual.weaponRecoilPitch;
        const math::q32_32 oldRoll = visual.weaponRecoilRoll;
        const auto advance = [](math::q32_32 stiffness,
                                math::q32_32 damping,
                                math::q32_32 negativeLimit,
                                math::q32_32 positiveLimit,
                                math::q32_32& value,
                                math::q32_32& rate) {
            stiffness = math::q32_32::clamp(
                stiffness, math::q32_32{}, math::q32_32{1});
            damping = math::q32_32::clamp(
                damping, math::q32_32{}, math::q32_32{1});
            rate += -stiffness * value - damping * rate;
            value += rate;
            value = math::q32_32::clamp(
                value, -math::q32_32::abs(negativeLimit),
                math::q32_32::abs(positiveLimit));
            constexpr int64_t kRestRaw = 1ll << 12u;
            if (math::q32_32::abs(value).raw() <= kRestRaw &&
                math::q32_32::abs(rate).raw() <= kRestRaw) {
                value = {};
                rate = {};
            }
        };
        advance(locomotion.pitchStiffness, locomotion.pitchDamping,
                locomotion.accelerationPitchLimitRadians,
                locomotion.decelerationPitchLimitRadians,
                visual.weaponRecoilPitch,
                visual.weaponRecoilPitchRate);
        advance(locomotion.rollStiffness, locomotion.rollDamping,
                locomotion.accelerationPitchLimitRadians,
                locomotion.decelerationPitchLimitRadians,
                visual.weaponRecoilRoll,
                visual.weaponRecoilRollRate);
        if (oldPitch != visual.weaponRecoilPitch ||
            oldRoll != visual.weaponRecoilRoll ||
            visual.weaponRecoilPitchRate != math::q32_32{} ||
            visual.weaponRecoilRollRate != math::q32_32{}) {
            markObjectDirty(
                m_world.m_registry, entity,
                ObjectDirtyDomain::RenderExtraction);
        }
    }
    const auto vehicles = ecs::view<
        const ObjectIdentityComponent, const ThingTemplateComponent,
        VehicleDrawPresentationComponent>(m_world.m_registry);
    for (const ecs::entity entity : vehicles) {
        const ObjectIdentityComponent& identity = vehicles.template get<
            const ObjectIdentityComponent>(entity);
        const ThingTemplateComponent& type = vehicles.template get<
            const ThingTemplateComponent>(entity);
        VehicleDrawPresentationComponent& vehicle = vehicles.template get<
            VehicleDrawPresentationComponent>(entity);
        if (!identity.id || !type.archetype) continue;
        const game::ThingTemplate& recipe = type.archetype->templateData;
        const bool landed = std::any_of(
            vehicle.channels.begin(), vehicle.channels.end(),
            [tick = m_presentation.m_confirmedTick](
                const VehicleDrawChannelPresentationState& channel) {
                return channel.landingTriggerSequence != 0 &&
                    channel.landingTriggerSequence == tick;
            });
        if (landed) {
            const container::StringView cue =
                recipe.perUnitSound("TruckLandingSound");
            if (!cue.empty()) {
                static_cast<void>(m_presentation.m_audioJournal.emit({
                    .eventName = container::String{cue},
                    .emitter = identity.id,
                    .owner = identity.id,
                }, static_cast<int32_t>(m_content.m_startInfo.seed)));
            }
        }
        const bool powersliding = std::any_of(
            vehicle.channels.begin(), vehicle.channels.end(),
            [](const VehicleDrawChannelPresentationState& channel) {
                return channel.powersliding;
            });
        if (powersliding != vehicle.powerslideAudioActive) {
            const container::StringView cue =
                recipe.perUnitSound("TruckPowerslideSound");
            if (!cue.empty()) {
                static_cast<void>(m_presentation.m_audioJournal.emit({
                    .kind = game::GameAudioControlKind::
                        SetObjectLoopingSoundEnabled,
                    .enabled = powersliding,
                    .eventName = container::String{cue},
                    .object = identity.id,
                }));
            }
            vehicle.powerslideAudioActive = powersliding;
        }
    }
    const auto turretObjects = ecs::view<
        const ObjectIdentityComponent, const ThingTemplateComponent,
        const ObjectWeaponComponent, RenderModelComponent>(
            m_world.m_registry);
    for (const ecs::entity entity : turretObjects) {
        const ObjectIdentityComponent& identity = turretObjects.template get<
            const ObjectIdentityComponent>(entity);
        const ThingTemplateComponent& type = turretObjects.template get<
            const ThingTemplateComponent>(entity);
        const ObjectWeaponComponent& weapons = turretObjects.template get<
            const ObjectWeaponComponent>(entity);
        RenderModelComponent& visual = turretObjects.template get<
            RenderModelComponent>(entity);
        if (!identity.id || !type.archetype) continue;
        bool rotating = false;
        bool retainForContinuousFire = false;
        for (const ObjectTurretRuntime& turret : weapons.turrets) {
            if (turret.controlledWeaponSlots == 0) continue;
            rotating = rotating || turret.rotating;
            retainForContinuousFire = retainForContinuousFire ||
                (turret.firesWhileTurning &&
                 turret.continuousFireSoundUntilTick >
                    m_presentation.m_confirmedTick);
        }
        const bool requested = rotating ||
            (visual.turretMoveAudioActive && retainForContinuousFire);
        if (requested == visual.turretMoveAudioActive) continue;
        // TurretMoveStart is a distinct one-shot; the loop is retained only
        // for the duration of actual rotation/continuous-fire settle.  The
        // old implementation parsed the authored start cue but silently
        // discarded this confirmed rising edge.
        if (requested) {
            const container::StringView startCue =
                type.archetype->templateData.perUnitSound(
                    "TurretMoveStart");
            if (!startCue.empty()) {
                static_cast<void>(m_presentation.m_audioJournal.emit({
                    .eventName = container::String{startCue},
                    .emitter = identity.id,
                    .owner = identity.id,
                }, static_cast<int32_t>(m_content.m_startInfo.seed)));
            }
        }
        const container::StringView cue =
            type.archetype->templateData.perUnitSound("TurretMoveLoop");
        if (!cue.empty()) {
            static_cast<void>(m_presentation.m_audioJournal.emit({
                .kind = game::GameAudioControlKind::
                    SetObjectLoopingSoundEnabled,
                .enabled = requested,
                .eventName = container::String{cue},
                .object = identity.id,
            }));
        }
        visual.turretMoveAudioActive = requested;
    }
    const auto constructionObjects = ecs::view<
        const ObjectIdentityComponent, const ThingTemplateComponent,
        RenderModelComponent>(m_world.m_registry);
    for (const ecs::entity entity : constructionObjects) {
        const ObjectIdentityComponent& identity =
            constructionObjects.template get<
                const ObjectIdentityComponent>(entity);
        const ThingTemplateComponent& type =
            constructionObjects.template get<
                const ThingTemplateComponent>(entity);
        RenderModelComponent& visual = constructionObjects.template get<
            RenderModelComponent>(entity);
        if (!identity.id || !type.archetype) continue;
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(m_world.m_registry, entity);
        const ObjectConstructionSiteComponent* site =
            ecs::try_get<ObjectConstructionSiteComponent>(
                m_world.m_registry, entity);
        const bool active = status && site && site->builder &&
            site->completedFrames != 0 && status->hasAny(
                game::objectStatusBit(
                    game::ObjectStatusFlag::UnderConstruction));
        const game::ThingTemplate& recipe =
            type.archetype->templateData;
        if (active != visual.underConstructionAudioActive) {
            const container::StringView cue =
                recipe.perUnitSound("UnderConstruction");
            if (!cue.empty()) {
                static_cast<void>(m_presentation.m_audioJournal.emit({
                    .kind = game::GameAudioControlKind::
                        SetObjectLoopingSoundEnabled,
                    .enabled = active,
                    .eventName = container::String{cue},
                    .object = identity.id,
                }));
            }
            visual.underConstructionAudioActive = active;
        }

        const bool stealthed = status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::Stealthed));
        const bool detected = status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::Detected));
        if (!visual.stealthAudioInitialized) {
            visual.stealthAudioInitialized = true;
            visual.stealthedAudioSnapshot = stealthed;
            visual.detectedAudioSnapshot = detected;
            continue;
        }
        const auto emitObjectCue = [&](container::StringView cue) {
            if (cue.empty()) return;
            static_cast<void>(m_presentation.m_audioJournal.emit({
                .eventName = container::String{cue},
                .emitter = identity.id,
                .owner = identity.id,
            }, static_cast<int32_t>(m_content.m_startInfo.seed)));
        };
        // RefCode plays SoundStealthOn on both voluntary stealth-state edges
        // (the name is historical), SoundStealthOff on first detection, and
        // SoundStealthOn again when a locally controlled object loses its
        // detected flag.
        if (stealthed != visual.stealthedAudioSnapshot)
            emitObjectCue(recipe.soundStealthOn);
        if (detected != visual.detectedAudioSnapshot) {
            if (detected) {
                emitObjectCue(recipe.soundStealthOff);
            } else {
                const OwnerComponent* owner =
                    ecs::try_get<OwnerComponent>(m_world.m_registry, entity);
                if (owner && owner->player ==
                        m_content.m_players.localPlayerId()) {
                    emitObjectCue(recipe.soundStealthOn);
                }
            }
        }
        visual.stealthedAudioSnapshot = stealthed;
        visual.detectedAudioSnapshot = detected;
    }
    session_animation::updateConfirmedClocks(
        m_world.m_registry, deltaSeconds, m_presentation.m_confirmedTick);
}

} // namespace engine
