#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"

#include "game/base/SimulationRandom.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/player/PlayerRegistry.h"
#include "game/terrain/MapVisibilityAuthority.h"
#include "game/terrain/TerrainLogic.h"

#include <limits>
#include <optional>
#include <utility>

namespace engine {

void ObjectSimulation::updateOrdersAndTacticalPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context) {
    if (context.content && context.random) {
        object_simulation_detail::state(*this).m_fireWeaponBehaviors.updateContinuous(
            registry, lifecycle, *context.content, *context.random,
            object_simulation_detail::state(*this).m_rules.logicFramesPerSecond, confirmedTick,
            object_simulation_detail::state(*this).m_nextGameplaySubmissionOrdinal,
            object_simulation_detail::state(*this).m_systemWeaponFireCommands);
        object_simulation_detail::state(*this).m_fireWeaponUpdate.update(
            registry, lifecycle, *context.content, *context.random,
            object_simulation_detail::state(*this).m_rules.logicFramesPerSecond, confirmedTick,
            object_simulation_detail::state(*this).m_nextGameplaySubmissionOrdinal,
            object_simulation_detail::state(*this).m_systemWeaponFireCommands);
        if (context.players) {
            object_simulation_detail::state(*this).m_oclUpdate.update(
                registry, lifecycle, *context.players, *context.content,
                terrain, *context.random, object_simulation_detail::state(*this).m_rules.logicFramesPerSecond,
                confirmedTick, object_simulation_detail::state(*this).m_nextGameplaySubmissionOrdinal,
                object_simulation_detail::state(*this).m_objectCreationListInvocations);
            object_simulation_detail::state(*this).m_fireUpdates.updateOclAfterCooldown(
                registry, lifecycle, *context.players, *context.content,
                object_simulation_detail::state(*this).m_rules.logicFramesPerSecond, confirmedTick,
                object_simulation_detail::state(*this).m_nextGameplaySubmissionOrdinal,
                object_simulation_detail::state(*this).m_objectCreationListInvocations);
        }
        object_simulation_detail::state(*this).m_fireUpdates.updateSpread(
            registry, lifecycle, *context.content, *context.random,
            object_simulation_detail::state(*this).m_rules.logicFramesPerSecond, confirmedTick,
            object_simulation_detail::state(*this).m_nextGameplaySubmissionOrdinal,
            object_simulation_detail::state(*this).m_objectCreationListInvocations, object_simulation_detail::state(*this).m_objectFireAudioCommands);
    }

    // PowerPlantUpdate is a sparse wake/sleep state machine. Keep it in the
    // same confirmed simulation boundary as the typed UpgradeMux consumer so
    // its model conditions never depend on renderer frame timing.
    object_simulation_detail::state(*this).m_upgrades.update(registry, lifecycle, object_simulation_detail::state(*this).m_rules, confirmedTick);
    if (context.players) {
        if (context.content) {
            const size_t specialPowerEventBegin =
                object_simulation_detail::state(*this)
                    .m_specialPowerExecutionEvents.size();
            object_simulation_detail::state(*this).m_specialPower.consumeOrders(
                registry, lifecycle, *context.players, *context.content,
                object_simulation_detail::state(*this).m_rules, object_simulation_detail::state(*this).m_spyVision, object_simulation_detail::state(*this).m_cleanupHazard, object_simulation_detail::state(*this).m_tactical, terrain, context.random,
                context.spatialIndex, context.navigation,
                context.mapVisibility, confirmedTick,
                object_simulation_detail::state(*this).m_nextGameplaySubmissionOrdinal,
                object_simulation_detail::state(*this).m_objectCreationListInvocations,
                object_simulation_detail::state(*this).m_objectDefectionRequests,
                object_simulation_detail::state(*this).m_specialPowerSpawnRequests,
                object_simulation_detail::state(*this)
                    .m_specialAbilityEffectRequests,
                object_simulation_detail::state(*this).m_specialPowerExecutionEvents);
            // SpecialPower activation is a typed gameplay fact. Airfield
            // reacts only when the source actually owns a matching Spectre
            // capability; script and player orders therefore share exactly
            // the same spawn/targeting path.
            for (size_t index = specialPowerEventBegin;
                 index < object_simulation_detail::state(*this)
                             .m_specialPowerExecutionEvents.size();
                 ++index) {
                object_simulation_detail::state(*this)
                    .m_airfield.emitSpectreSpecialPowerSpawns(
                        registry, lifecycle, *context.content, terrain,
                        object_simulation_detail::state(*this).m_rules,
                        object_simulation_detail::state(*this)
                            .m_specialPowerExecutionEvents[index],
                        object_simulation_detail::state(*this)
                            .m_nextGameplaySubmissionOrdinal,
                        object_simulation_detail::state(*this)
                            .m_specialPowerSpawnRequests,
                        object_simulation_detail::state(*this)
                            .m_radiusDecalEvents);
            }
        }
        object_simulation_detail::state(*this).m_spyVision.update(
            registry, lifecycle, *context.players,
            object_simulation_detail::state(*this).m_rules, confirmedTick,
            context.content ? context.content->upgradeCatalog() : nullptr);
        if (context.content) {
            auto& tacticalDamage =
                object_simulation_detail::state(*this).m_damageScratch;
            tacticalDamage.clear();
            object_simulation_detail::state(*this).m_tactical.update(
                registry, lifecycle, *context.players, *context.content,
                terrain,
                context.aiTargetPriority,
                context.mapVisibility,
                context.navigation,
                object_simulation_detail::state(*this).m_rules, context.random,
                confirmedTick, context.rankLevelLimit,
                object_simulation_detail::state(*this)
                    .m_nextGameplaySubmissionOrdinal,
                tacticalDamage,
                object_simulation_detail::state(*this)
                    .m_objectDefectionRequests,
                object_simulation_detail::state(*this)
                    .m_specialAbilityEffectRequests,
                object_simulation_detail::state(*this)
                    .m_specialAbilityFacingRequests);
            for (ObjectDamageRequest& request : tacticalDamage) {
                queueDamage(std::move(request));
            }
        }
    }
}

bool ObjectSimulation::executeSpecialAbilityEffect(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    ObjectSpecialAbilityEffectRequest effect,
    ObjectUpgradeExecutionContext context) {
    if (!context.players || !context.content) return false;
    auto& simulationState = object_simulation_detail::state(*this);
    container::Vector<ObjectDamageRequest> damage;
    switch (effect.kind) {
    case ObjectSpecialAbilityEffectKind::SpawnSpecialObject: {
        const std::optional<ecs::entity> sourceEntity =
            lifecycle.entityFromId(effect.source);
        const OwnerComponent* owner = sourceEntity
            ? ecs::try_get<OwnerComponent>(registry, *sourceEntity) : nullptr;
        const PrimaryTeamComponent* team = sourceEntity
            ? ecs::try_get<PrimaryTeamComponent>(registry, *sourceEntity)
            : nullptr;
        const ObjectFixedTransformComponent* transform = sourceEntity
            ? ecs::try_get<ObjectFixedTransformComponent>(registry,
                                                           *sourceEntity)
            : nullptr;
        if (!owner || !team || !team->team || !transform ||
            !transform->authoritative || effect.objectTemplate.empty() ||
            !context.content->findObjectArchetype(effect.objectTemplate)) {
            break;
        }
        simulationState.m_specialPowerSpawnRequests.push_back({
            .source = effect.source,
            .target = effect.target,
            .owner = owner->player,
            .primaryTeam = team->team,
            .objectTemplate = std::move(effect.objectTemplate),
            .attachToBone = std::move(effect.attachToBone),
            .position = transform->position,
            .effectPosition = effect.position,
            .completion =
                ObjectSpecialPowerSpawnCompletionKind::SpecialAbility,
            .specialAbilityRuleIndex = effect.ruleIndex,
            .authoredOrder = effect.authoredOrder,
            .activationSequence = effect.activationSequence,
            .emissionSequence = reserveGameplaySubmissionOrdinal(),
            .confirmedTick = effect.confirmedTick,
            .hasEffectPosition = effect.hasPosition,
            .attachStickyBomb = effect.attachStickyBomb,
        });
        break;
    }
    case ObjectSpecialAbilityEffectKind::DetonateSpecialObjects:
        for (const ObjectId bomb : effect.objects) {
            static_cast<void>(detonateStickyBomb(
                registry, lifecycle, *context.content, bomb,
                sticky_bomb::DetonationTrigger::Remote,
                effect.confirmedTick));
        }
        break;
    case ObjectSpecialAbilityEffectKind::TriggerTargetBoobyTrap:
        static_cast<void>(simulationState.m_stickyBomb
            .detonateHostileBoobyTrapOnTarget(
                registry, lifecycle, *context.players, *context.content,
                effect.source, effect.target, effect.confirmedTick, damage,
                simulationState.m_stickyBombPresentationEvents));
        break;
    case ObjectSpecialAbilityEffectKind::DestroySpecialObjects:
        for (const ObjectId object : effect.objects) {
            if (!lifecycle.entityFromId(object)) continue;
            damage.push_back({
                .target = object,
                .source = effect.source,
                .sourceSequence = effect.authoredOrder,
                .forceKill = true,
                .confirmedTick = effect.confirmedTick,
            });
        }
        break;
    case ObjectSpecialAbilityEffectKind::DisguiseAsTarget:
        static_cast<void>(simulationState.m_stealth.disguiseAsObject(
            registry, lifecycle, effect.source, effect.target,
            simulationState.m_rules, effect.confirmedTick));
        break;
    case ObjectSpecialAbilityEffectKind::MarkDetected:
        static_cast<void>(simulationState.m_stealth.markDetected(
            registry, lifecycle, effect.source, 0,
            simulationState.m_rules, effect.confirmedTick));
        break;
    case ObjectSpecialAbilityEffectKind::AwardExperience: {
        const std::optional<ecs::entity> sourceEntity =
            lifecycle.entityFromId(effect.source);
        const OwnerComponent* owner = sourceEntity
            ? ecs::try_get<OwnerComponent>(registry, *sourceEntity) : nullptr;
        const PlayerState* player = owner
            ? context.players->get(owner->player) : nullptr;
        const UpgradeMask upgrades = player
            ? player->upgrades.completed : UpgradeMask{};
        const ObjectExperienceMutation mutation =
            simulationState.m_experience.addPoints(
                registry, lifecycle, effect.source, effect.value, true,
                effect.confirmedTick);
        finalizeExperienceMutation(
            registry, lifecycle, mutation, INVALID_OBJECT_ID, upgrades,
            effect.confirmedTick, context);
        break;
    }
    case ObjectSpecialAbilityEffectKind::RestartRecharge: {
        const bool restarted = simulationState.m_specialPower.restartRecharge(
            registry, lifecycle, effect.source, effect.specialPower,
            *context.content, simulationState.m_rules,
            effect.confirmedTick);
        if (!restarted || !effect.marksSpecialPowerTriggered) break;
        const std::optional<ecs::entity> sourceEntity =
            lifecycle.entityFromId(effect.source);
        const OwnerComponent* owner = sourceEntity
            ? ecs::try_get<OwnerComponent>(registry, *sourceEntity) : nullptr;
        const SpecialPowerDefinition* definition =
            context.content->findSpecialPower(effect.specialPower);
        uint64_t readyTick = effect.confirmedTick;
        const ObjectSpecialPowerComponent* powers = sourceEntity
            ? ecs::try_get<ObjectSpecialPowerComponent>(registry,
                                                         *sourceEntity)
            : nullptr;
        if (definition && powers) {
            for (const ObjectSpecialPowerRuntime& runtime :
                 powers->instances) {
                if (runtime.content == definition->id) {
                    readyTick = runtime.readyTick;
                    break;
                }
            }
        }
        if (owner && definition) {
            simulationState.m_specialPowerExecutionEvents.push_back({
                .source = effect.source,
                .player = owner->player,
                .content = definition->id,
                .kind = game::ObjectSpecialPowerKind::SpecialAbility,
                .status = ObjectSpecialPowerExecutionStatus::Activated,
                .confirmedTick = effect.confirmedTick,
                .readyTick = readyTick,
                .scriptTriggered = true,
            });
        }
        break;
    }
    }
    for (ObjectDamageRequest& request : damage)
        queueDamage(std::move(request));
    return true;
}

void ObjectSimulation::updateParticleUplinkPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context) {
    if (context.content) {
        object_simulation_detail::state(*this).m_missileLauncherBuilding.update(
            registry, lifecycle, *context.content,
            object_simulation_detail::state(*this).m_rules.logicFramesPerSecond, confirmedTick,
            object_simulation_detail::state(*this).m_nextGameplaySubmissionOrdinal, object_simulation_detail::state(*this).m_missileLauncherFxEvents,
            object_simulation_detail::state(*this).m_objectFireAudioCommands);
    }
    auto& particleUplinkDamage =
        object_simulation_detail::state(*this).m_damageScratch;
    particleUplinkDamage.clear();
    container::Vector<ObjectParticleUplinkRevealRequest>
        particleUplinkReveals;
    object_simulation_detail::state(*this).m_particleUplinkCannon.update(
        registry, lifecycle, terrain, object_simulation_detail::state(*this).m_rules.logicFramesPerSecond,
        confirmedTick, object_simulation_detail::state(*this).m_nextGameplaySubmissionOrdinal,
        particleUplinkDamage,
        object_simulation_detail::state(*this).m_particleUplinkPhaseEvents, object_simulation_detail::state(*this).m_particleUplinkBeamEvents,
        object_simulation_detail::state(*this).m_particleUplinkScorchEvents,
        particleUplinkReveals,
        object_simulation_detail::state(*this).m_particleUplinkFxEvents,
        object_simulation_detail::state(*this).m_particleUplinkRemnantSpawnRequests,
        object_simulation_detail::state(*this).m_objectFireAudioCommands);
    // ZH performs the temporary reveal/unreveal pulse before the same update
    // reaches its damage pulse. Drain this producer-owned typed journal here;
    // delaying it to the session presentation barrier reverses that order.
    if (context.mapVisibilityAuthority) {
        for (const ObjectParticleUplinkRevealRequest& reveal :
             particleUplinkReveals) {
            static_cast<void>(context.mapVisibilityAuthority->revealCircle(
                reveal.owner, reveal.position.x, reveal.position.y,
                reveal.revealRange));
        }
    }
    for (ObjectDamageRequest& request : particleUplinkDamage)
        queueDamage(std::move(request));
}

void ObjectSimulation::updateDynamicSightPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    uint64_t confirmedTick, ObjectUpgradeExecutionContext context) {
    // Dynamic sight is an authoritative confirmed-frame value. It advances
    // even when no renderer delta is supplied and publishes only the last
    // interval-committed Q32.32 radius to map visibility.
    object_simulation_detail::state(*this).m_dynamicShroud.update(
        registry, lifecycle, confirmedTick, object_simulation_detail::state(*this).m_dynamicShroudDecalEvents);
    object_simulation_detail::state(*this).m_radiusDecal.update(registry, lifecycle, confirmedTick,
                         object_simulation_detail::state(*this).m_radiusDecalEvents);
    if (context.players) {
        object_simulation_detail::state(*this).m_enemyNear.update(registry, lifecycle, *context.players, object_simulation_detail::state(*this).m_rules,
                           context.mapVisibility, confirmedTick);
        object_simulation_detail::state(*this).m_checkpoint.update(registry, lifecycle, *context.players, object_simulation_detail::state(*this).m_rules,
                            confirmedTick,
                            object_simulation_detail::state(*this)
                                .m_nextGameplaySubmissionOrdinal,
                            object_simulation_detail::state(*this)
                                .m_checkpointNavigationEvents);
        object_simulation_detail::state(*this).m_techBuilding.update(
            registry, lifecycle, *context.players, object_simulation_detail::state(*this).m_rules, confirmedTick,
            object_simulation_detail::state(*this).m_techBuildingEvents, object_simulation_detail::state(*this).m_beaconClientEvents);
    }
}

} // namespace engine
