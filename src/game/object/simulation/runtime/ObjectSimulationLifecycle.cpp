#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"
#include "game/object/simulation/runtime/ObjectSimulationDetail.h"

#include "core/container/string_utils.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/navigation/runtime/NavigationSystem.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingFactory.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/plan/movement/ObjectPhysicsPlanTypes.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/status/ObjectBodyRuntime.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"
#include "game/terrain/TerrainLogic.h"

#include <algorithm>
#include <cmath>

namespace engine {

using container::asciiEqualIgnoreCase;
using namespace object_simulation_detail;

namespace {

[[nodiscard]] bool hasKind(const ObjectKindOfComponent* kinds,
                           game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

template <typename T>
void releaseEventCapacity(container::Vector<T>& events) noexcept {
    container::Vector<T>{}.swap(events);
}

} // namespace

void ObjectSimulation::reset() noexcept {
    object_simulation_detail::state(*this).m_fireWeaponCollide.reset();
    object_simulation_detail::state(*this).m_leafletDrop.reset();
    object_simulation_detail::state(*this).m_contactIndex.clear();
    object_simulation_detail::state(*this).m_countermeasures.reset();
    object_simulation_detail::state(*this)
        .m_tactical.releaseToppleGameplayStorage();
    static_cast<void>(object_simulation_detail::state(*this)
        .m_tactical.takeTacticalPresentationEvents());
    object_simulation_detail::state(*this)
        .m_stealth.resetPresentationEvents();
    container::Vector<object_simulation_detail::QueuedDamageRequest>{}.swap(
        object_simulation_detail::state(*this).m_damageRequests);
    container::Vector<object_simulation_detail::QueuedDamageRequest>{}.swap(
        object_simulation_detail::state(*this).m_readyDamageScratch);
    container::Vector<ObjectDamageTransactionIngress>{}.swap(
        object_simulation_detail::state(*this).m_readyDamageIngressScratch);
    container::Vector<ObjectDamageRequest>{}.swap(
        object_simulation_detail::state(*this).m_damageScratch);
    object_simulation_detail::state(*this).m_physicsRequests.clear();
    object_simulation_detail::state(*this).m_physicsScratch.release();
    container::Vector<ObjectHealthEvent>{}.swap(
        object_simulation_detail::state(*this).m_healthEvents);
    object_simulation_detail::state(*this).m_deathEvents.clear();
    object_simulation_detail::state(*this).m_specialPowerCompletionEvents.clear();
    object_simulation_detail::state(*this)
        .m_vehicleNeutralizationRequests.clear();
    object_simulation_detail::state(*this).m_crushDieEvents.clear();
    object_simulation_detail::state(*this).m_instantDeathEffectEvents.clear();
    object_simulation_detail::state(*this).m_createObjectDieEvents.clear();
    object_simulation_detail::state(*this).m_ownershipChangeRequests.clear();
    object_simulation_detail::state(*this).m_createCrateDieEvents.clear();
    object_simulation_detail::state(*this).m_fxListDieEffectEvents.clear();
    object_simulation_detail::state(*this).m_transitionDamageFxEvents.clear();
    container::Vector<ObjectTransitionDamageFxEvent>{}.swap(
        object_simulation_detail::state(*this)
            .m_transitionDamageGameplayScratch);
    object_simulation_detail::state(*this).m_structureEffectEvents.clear();
    object_simulation_detail::state(*this).m_bridgeDeathEffects.clear();
    object_simulation_detail::state(*this).m_heightDiePresentationEvents.clear();
    object_simulation_detail::state(*this).m_slowDeathPhaseEvents.clear();
    container::Vector<ObjectInstantDeathEffectEvent>{}.swap(
        object_simulation_detail::state(*this)
            .m_instantDeathGameplayScratch);
    container::Vector<ObjectSlowDeathPhaseEvent>{}.swap(
        object_simulation_detail::state(*this)
            .m_slowDeathGameplayScratch);
    object_simulation_detail::state(*this).m_movementEvents.clear();
    container::Vector<ai::MovementFeedback>{}.swap(
        object_simulation_detail::state(*this).m_aiMovementFeedback);
    container::Vector<ai::AIFacingFeedback>{}.swap(
        object_simulation_detail::state(*this).m_aiFacingFeedback);
    object_simulation_detail::state(*this).m_physicsEvents.clear();
    object_simulation_detail::state(*this)
        .m_aiMovementObstructionEvents.clear();
    object_simulation_detail::state(*this).m_physicsCrashCommands.clear();
    object_simulation_detail::state(*this).m_experienceEvents.clear();
    object_simulation_detail::state(*this).m_autoDepositEvents.clear();
    object_simulation_detail::state(*this).m_supplyEvents.clear();
    object_simulation_detail::state(*this).m_cratePickupCommands.clear();
    object_simulation_detail::state(*this).m_containmentEvents.clear();
    object_simulation_detail::state(*this)
        .m_transportEvents.gameplay.clear();
    object_simulation_detail::state(*this)
        .m_transportEvents.presentation.clear();
    object_simulation_detail::state(*this).m_weaponBonusUpdateEvents.clear();
    object_simulation_detail::state(*this).m_systemWeaponFireCommands.clear();
    object_simulation_detail::state(*this).m_mineSpawnCommands.clear();
    object_simulation_detail::state(*this).m_minefieldFxEvents.clear();
    object_simulation_detail::state(*this).m_stickyBombPresentationEvents.clear();
    object_simulation_detail::state(*this).m_neutronMissilePresentationEvents.clear();
    object_simulation_detail::state(*this).m_waveGuideEvents.clear();
    object_simulation_detail::state(*this).m_waveGuideBridgeImpacts.clear();
    object_simulation_detail::state(*this).m_missileLauncherFxEvents.clear();
    object_simulation_detail::state(*this).m_particleUplinkPhaseEvents.clear();
    object_simulation_detail::state(*this).m_particleUplinkBeamEvents.clear();
    object_simulation_detail::state(*this).m_particleUplinkScorchEvents.clear();
    object_simulation_detail::state(*this).m_particleUplinkFxEvents.clear();
    object_simulation_detail::state(*this).m_particleUplinkRemnantSpawnRequests.clear();
    object_simulation_detail::state(*this).m_objectCreationListInvocations.clear();
    object_simulation_detail::state(*this).m_objectReplacementInvocations.clear();
    object_simulation_detail::state(*this).m_objectUpgradeFxInvocations.clear();
    object_simulation_detail::state(*this).m_objectFireAudioCommands.clear();
    object_simulation_detail::state(*this).m_empParticleEvents.clear();
    object_simulation_detail::state(*this).m_autoHealParticleEvents.clear();
    object_simulation_detail::state(*this).m_leafletParticleEvents.clear();
    object_simulation_detail::state(*this).m_dynamicShroudDecalEvents.clear();
    object_simulation_detail::state(*this).m_radiusDecalEvents.clear();
    object_simulation_detail::state(*this).m_checkpointNavigationEvents.clear();
    object_simulation_detail::state(*this).m_dynamicGeometryGameplayEvents.clear();
    object_simulation_detail::state(*this).m_dynamicGeometryPresentationEvents.clear();
    object_simulation_detail::state(*this).m_specialPowerExecutionEvents.clear();
    object_simulation_detail::state(*this).m_specialAbilityEffectRequests.clear();
    object_simulation_detail::state(*this).m_specialAbilityFacingRequests.clear();
    object_simulation_detail::state(*this).m_objectDefectionRequests.clear();
    object_simulation_detail::state(*this).m_pilotVehicleTakeoverRequests.clear();
    object_simulation_detail::state(*this).m_specialPowerSpawnRequests.clear();
    object_simulation_detail::state(*this).m_bridgeStateEvents.clear();
    object_simulation_detail::state(*this)
        .m_railedTransportDockAttachCompletions.clear();
    object_simulation_detail::state(*this).m_railroadCarriageSpawnRequests.clear();
    object_simulation_detail::state(*this)
        .m_railroadDisembarkRequests.clear();
    object_simulation_detail::state(*this)
        .m_railroadPresentationEvents.clear();
    object_simulation_detail::state(*this).m_spawnSlaveRequests.clear();
    object_simulation_detail::state(*this)
        .m_airfieldAutomaticProductionRequests.clear();
    object_simulation_detail::state(*this).m_slaveRepairPresentationEvents.clear();
    object_simulation_detail::state(*this).m_tensileFormationEvents.clear();
    object_simulation_detail::state(*this).m_tensileNavigationEvents.clear();
    object_simulation_detail::state(*this).m_completedObjectConstructions.clear();
    object_simulation_detail::state(*this).m_bridgeRepairScaffoldIntents.clear();
    object_simulation_detail::state(*this).m_rebuildExposeIntents.clear();
    object_simulation_detail::state(*this).m_rebuildWorkerIntents.clear();
    object_simulation_detail::state(*this).m_rebuildCompletionIntents.clear();
    object_simulation_detail::state(*this).m_rebuildTargetRemapIntents.clear();
    object_simulation_detail::state(*this).m_deleteWalks.clear();
    object_simulation_detail::state(*this).m_deletePostambleEvents.clear();
    object_simulation_detail::state(*this).m_deleteDestroyRequests.clear();

    // Confirmed gameplay drains deliberately return producer allocations
    // during a match. Match reset is the opposite lifetime boundary: release
    // those retained high-water allocations so one large battle cannot pin
    // event storage into the next session.
    auto& pending = object_simulation_detail::state(*this);
    releaseEventCapacity(pending.m_deleteWalks);
    releaseEventCapacity(pending.m_deleteDestroyRequests);
    releaseEventCapacity(pending.m_systemWeaponFireCommands);
    releaseEventCapacity(pending.m_objectCreationListInvocations);
    releaseEventCapacity(pending.m_objectReplacementInvocations);
    releaseEventCapacity(pending.m_objectUpgradeFxInvocations);
    releaseEventCapacity(pending.m_ownershipChangeRequests);
    releaseEventCapacity(pending.m_objectDefectionRequests);
    releaseEventCapacity(pending.m_pilotVehicleTakeoverRequests);
    releaseEventCapacity(pending.m_railedTransportDockAttachCompletions);
    releaseEventCapacity(pending.m_railroadDisembarkRequests);
    releaseEventCapacity(pending.m_railroadCarriageSpawnRequests);
    releaseEventCapacity(pending.m_spawnSlaveRequests);
    releaseEventCapacity(pending.m_specialPowerSpawnRequests);
    releaseEventCapacity(pending.m_bridgeStateEvents);
    releaseEventCapacity(pending.m_completedObjectConstructions);
    releaseEventCapacity(pending.m_bridgeRepairScaffoldIntents);
    releaseEventCapacity(pending.m_rebuildTargetRemapIntents);
    releaseEventCapacity(pending.m_rebuildExposeIntents);
    releaseEventCapacity(pending.m_rebuildWorkerIntents);
    releaseEventCapacity(pending.m_rebuildCompletionIntents);
    releaseEventCapacity(pending.m_containmentEvents);
    releaseEventCapacity(pending.m_vehicleNeutralizationRequests);
    releaseEventCapacity(pending.m_cratePickupCommands);
    releaseEventCapacity(pending.m_specialAbilityEffectRequests);
    releaseEventCapacity(pending.m_specialAbilityFacingRequests);
    releaseEventCapacity(pending.m_specialPowerCompletionEvents);
    releaseEventCapacity(pending.m_airfieldAutomaticProductionRequests);
    releaseEventCapacity(pending.m_structureEffectEvents);
    releaseEventCapacity(pending.m_createObjectDieEvents);
    releaseEventCapacity(pending.m_createCrateDieEvents);
    releaseEventCapacity(pending.m_physicsCrashCommands);
    releaseEventCapacity(pending.m_aiMovementObstructionEvents);
    releaseEventCapacity(pending.m_mineSpawnCommands);
    releaseEventCapacity(pending.m_particleUplinkRemnantSpawnRequests);
    releaseEventCapacity(pending.m_waveGuideBridgeImpacts);
    releaseEventCapacity(pending.m_checkpointNavigationEvents);
    releaseEventCapacity(pending.m_tensileNavigationEvents);
    releaseEventCapacity(pending.m_dynamicGeometryGameplayEvents);
    releaseEventCapacity(pending.m_transportEvents.gameplay);
    releaseEventCapacity(pending.m_countermeasureFlareGameplayScratch);
    releaseEventCapacity(pending.m_toppleStumpGameplayScratch);
    releaseEventCapacity(pending.m_topplePathfindGameplayScratch);
    object_simulation_detail::state(*this).m_hulkLifetimeOverrideFrames.reset();
    object_simulation_detail::state(*this).m_nextGameplaySubmissionOrdinal = 1;
    object_simulation_detail::state(*this).m_nextBodyTransactionOrdinal = 1;
    object_simulation_detail::state(*this)
        .m_resolvingSingleDamageTransaction = false;
}

void ObjectSimulation::setRules(ObjectSimulationRules rules) noexcept {
    if (rules.movementPenaltyDamageState < ObjectBodyDamageState::Pristine ||
        rules.movementPenaltyDamageState > ObjectBodyDamageState::Rubble) {
        rules.movementPenaltyDamageState = ObjectBodyDamageState::ReallyDamaged;
    }
    if (rules.logicFramesPerSecond == 0) {
        rules.logicFramesPerSecond =
            static_cast<uint32_t>(PhysicsSimulationRules::kLegacyLogicFramesPerSecond);
    }
    rules.logicDeltaSeconds = math::q32_32::from_fraction(
        1, static_cast<int64_t>(rules.logicFramesPerSecond));
    rules.physics.canonicalize();
    rules.gravityUnitsPerSecondSq = rules.physics.gravityUnitsPerSecondSq;
    rules.groundStiffness = rules.physics.groundStiffness;
    rules.structureStiffness = rules.physics.structureStiffness;
    rules.defaultStructureRubbleHeight =
        rules.physics.defaultStructureRubbleHeight;
    rules.baseRegeneration.canonicalize();
    rules.ai.canonicalize();
    rules.veterancy.canonicalize();
    rules.difficulty.canonicalize();
    object_simulation_detail::state(*this).m_rules = rules;
}

void ObjectSimulation::initializeExperience(
    ecs::registry& registry, ecs::entity entity,
    const game::ThingTemplate& templateData,
    uint64_t confirmedTick) const {
    object_simulation_detail::state(*this).m_experience.initializeObject(registry, entity, templateData, confirmedTick);
}


void ObjectSimulation::initializeAutoHeal(
    ecs::registry& registry, ecs::entity entity,
    const UpgradeMask& ownerCompletedUpgrades,
    SimulationRandom& random, uint64_t confirmedTick) const {
    object_simulation_detail::state(*this).m_autoHeal.initializeObject(registry, entity, ownerCompletedUpgrades, random,
                                object_simulation_detail::state(*this).m_rules, confirmedTick);
}


void ObjectSimulation::materializeObjectUpgrades(
    ecs::registry& registry, ecs::entity entity) const {
    object_simulation_detail::state(*this).m_upgrades.materializeObject(registry, entity);
}

void ObjectSimulation::activateInitialObjectUpgrades(
    ecs::registry& registry, ObjectLifecycle& lifecycle, ObjectId object,
    const UpgradeMask& ownerCompletedUpgrades,
    uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context) const {
    object_simulation_detail::state(*this).m_upgrades.reevaluateObjectUpgrades(
        registry, lifecycle, object, ownerCompletedUpgrades, object_simulation_detail::state(*this).m_rules,
        confirmedTick, context);
    object_simulation_detail::state(*this).m_countermeasures.reevaluateObject(
        registry, lifecycle, object, ownerCompletedUpgrades, confirmedTick,
        context.content ? context.content->upgradeCatalog() : nullptr);
}

ObjectCreateExecutionReport ObjectSimulation::executeObjectCreatePhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectOwnershipIndex& ownership, ObjectId object,
    ObjectCreatePhase phase,
    const UpgradeMask& ownerCompletedUpgrades,
    uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context) {
    ObjectCreateExecutionReport report;
    if (!object || lifecycle.isPendingDestroy(object)) return report;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity) return report;
    const ThingTemplateComponent* templateComponent =
        ecs::try_get<ThingTemplateComponent>(registry, *entity);
    const game::ObjectCreatePlan* plan =
        templateComponent && templateComponent->archetype
            ? templateComponent->archetype->createPlan.get()
            : nullptr;
    if (!plan) return report;

    const auto currentOwner = [&]() -> const OwnerComponent* {
        return ecs::try_get<OwnerComponent>(registry, *entity);
    };
    const auto currentOwnerUpgrades = [&]() -> UpgradeMask {
        if (!context.players) return ownerCompletedUpgrades;
        const OwnerComponent* owner = currentOwner();
        const PlayerState* player = owner ? context.players->get(owner->player) : nullptr;
        return player ? player->upgrades.completed : UpgradeMask{};
    };
    const auto isUnderConstruction = [&]() {
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, *entity);
        return status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction));
    };

    for (const game::ObjectCreateRule& rule : plan->rules) {
        if (std::holds_alternative<game::ObjectSupplyCenterCreate>(
                rule.payload)) {
            if (phase != ObjectCreatePhase::BuildComplete) {
                ++report.skipped;
                continue;
            }
            ++report.attempted;
            ObjectSupplyAnchorComponent* supply =
                ecs::try_get<ObjectSupplyAnchorComponent>(registry, *entity);
            if (!supply) {
                supply = &ecs::emplace<ObjectSupplyAnchorComponent>(
                    registry, *entity);
            }
            if (supply->supplyCenterReady) {
                ++report.skipped;
            } else {
                supply->supplyCenterReady = true;
                ++supply->revision;
                supply->lastChangedTick = confirmedTick;
                ++report.applied;
            }
            continue;
        }

        if (std::holds_alternative<game::ObjectSupplyWarehouseCreate>(
                rule.payload)) {
            if (phase != ObjectCreatePhase::Created) {
                ++report.skipped;
                continue;
            }
            ++report.attempted;
            ObjectSupplyAnchorComponent* supply =
                ecs::try_get<ObjectSupplyAnchorComponent>(registry, *entity);
            if (!supply) {
                supply = &ecs::emplace<ObjectSupplyAnchorComponent>(
                    registry, *entity);
            }
            if (supply->supplyWarehouseReady) {
                ++report.skipped;
            } else {
                supply->supplyWarehouseReady = true;
                ++supply->revision;
                supply->lastChangedTick = confirmedTick;
                ++report.applied;
            }
            continue;
        }

        if (const auto* lock =
                std::get_if<game::ObjectLockWeaponCreate>(&rule.payload)) {
            if (phase != ObjectCreatePhase::BuildComplete) {
                ++report.skipped;
                continue;
            }
            ++report.attempted;
            if (setObjectWeaponLock(registry, *entity, lock->weaponSlot,
                                    ObjectWeaponLockType::Permanent)) {
                ++report.applied;
            } else {
                ++report.failed;
            }
            continue;
        }

        if (const auto* grant =
                std::get_if<game::ObjectGrantUpgradeCreate>(&rule.payload)) {
            const bool createFastPath =
                phase == ObjectCreatePhase::Created &&
                (grant->exemptStatuses & game::objectStatusBit(
                    game::ObjectStatusFlag::UnderConstruction)) != 0 &&
                !isUnderConstruction();
            if (phase != ObjectCreatePhase::BuildComplete && !createFastPath) {
                ++report.skipped;
                continue;
            }
            ++report.attempted;
            const UpgradeCatalog* catalog =
                context.content ? context.content->upgradeCatalog() : nullptr;
            const UpgradeDefinition* definition =
                catalog ? catalog->find(grant->upgradeId) : nullptr;
            const OwnerComponent* owner = currentOwner();
            if (!definition || !owner) {
                ++report.failed;
                continue;
            }

            if (definition->type == UpgradeDefinitionType::Player) {
                if (!context.players) {
                    ++report.failed;
                    continue;
                }
                const bool alreadyCompleted =
                    context.players->hasUpgradeComplete(owner->player,
                                                        definition->id);
                if (!alreadyCompleted &&
                    !context.players->markUpgradeComplete(owner->player,
                                                          definition->id)) {
                    ++report.failed;
                    continue;
                }
                const UpgradeMask completed = currentOwnerUpgrades();
                object_simulation_detail::state(*this).m_upgrades.onPlayerUpgradeCompleted(
                    registry, lifecycle, ownership, owner->player, completed,
                    object_simulation_detail::state(*this).m_rules, confirmedTick, context);
                // A newly-created object is not published into the ownership
                // index until the complete spawn transaction commits. Give it
                // the same immediate UpgradeMux pass explicitly in that one
                // narrow window; construction-complete objects are already in
                // the index and must not be evaluated twice.
                if (!ownership.contains(owner->player, object)) {
                    object_simulation_detail::state(*this).m_upgrades.reevaluateObjectUpgrades(
                        registry, lifecycle, object, completed, object_simulation_detail::state(*this).m_rules,
                        confirmedTick, context);
                }
                if (alreadyCompleted)
                    ++report.skipped;
                else
                    ++report.applied;
                if (createFastPath) {
                    static_cast<void>(context.players->recordAcademyUpgrade(
                        owner->player,
                        asciiEqualIgnoreCase(
                            definition->academyClassification,
                            "ACT_UPGRADE_RADAR"),
                        true));
                }
                continue;
            }

            const bool alreadyCompleted = object_simulation_detail::state(*this).m_upgrades.hasObjectUpgrade(
                registry, *entity, definition->id);
            const bool inserted = object_simulation_detail::state(*this).m_upgrades.completeObjectUpgrade(
                    registry, lifecycle, object, definition->id,
                    currentOwnerUpgrades(), object_simulation_detail::state(*this).m_rules, confirmedTick, context);
            if (inserted) {
                ++report.applied;
            } else if (alreadyCompleted) {
                // Object::giveUpgrade is idempotent at the bit level but still
                // re-runs updateUpgradeModules(); completeObjectUpgrade keeps
                // that observable boundary even when it returns false.
                ++report.skipped;
            } else {
                ++report.failed;
            }
            if (createFastPath && context.players &&
                (inserted || alreadyCompleted)) {
                static_cast<void>(context.players->recordAcademyUpgrade(
                    owner->player,
                    asciiEqualIgnoreCase(
                        definition->academyClassification,
                        "ACT_UPGRADE_RADAR"),
                    true));
            }
            continue;
        }

        const auto* veterancy =
            std::get_if<game::ObjectVeterancyGainCreate>(&rule.payload);
        if (!veterancy || phase != ObjectCreatePhase::Created) {
            ++report.skipped;
            continue;
        }
        ++report.attempted;
        const ObjectExperienceComponent* experience =
            ecs::try_get<ObjectExperienceComponent>(registry, *entity);
        const OwnerComponent* owner = currentOwner();
        if (!experience || !experience->trainable || !owner) {
            ++report.skipped;
            continue;
        }
        if (!veterancy->scienceRequired.empty()) {
            const ScienceCatalog* catalog =
                context.content ? context.content->scienceCatalog() : nullptr;
            if (!catalog || !catalog->find(veterancy->scienceRequired)) {
                ++report.failed;
                continue;
            }
            if (!context.players ||
                !context.players->hasScience(owner->player,
                                             veterancy->scienceRequired)) {
                ++report.skipped;
                continue;
            }
        }
        const ObjectExperienceMutation mutation = object_simulation_detail::state(*this).m_experience.setMinimumLevel(
            registry, lifecycle, object, veterancy->startingLevel,
            confirmedTick);
        finalizeExperienceMutation(
            registry, lifecycle, mutation, INVALID_OBJECT_ID,
            currentOwnerUpgrades(), confirmedTick, context,
            false /* VeterancyGainCreate suppresses promotion feedback. */);
        if (mutation.levelChanged) {
            ++report.applied;
        } else if (mutation.accepted) {
            ++report.skipped;
        } else {
            ++report.failed;
        }
    }
    return report;
}

ObjectCreateExecutionReport ObjectSimulationProgressionDomain::onObjectCreated(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectOwnershipIndex& ownership, ObjectId object,
    const UpgradeMask& ownerCompletedUpgrades,
    uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context) {
    return static_cast<ObjectSimulation&>(*this).executeObjectCreatePhase(
        registry, lifecycle, ownership, object, ObjectCreatePhase::Created,
        ownerCompletedUpgrades, confirmedTick, context);
}

ObjectCreateExecutionReport ObjectSimulationProgressionDomain::onObjectBuildCompleted(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectOwnershipIndex& ownership, ObjectId object,
    const UpgradeMask& ownerCompletedUpgrades,
    uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context) {
    return static_cast<ObjectSimulation&>(*this).executeObjectCreatePhase(
        registry, lifecycle, ownership, object,
        ObjectCreatePhase::BuildComplete, ownerCompletedUpgrades,
        confirmedTick, context);
}

void ObjectSimulationProgressionDomain::onObjectConstructionCompleted(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectOwnershipIndex& ownership, ObjectId object,
    const UpgradeMask& ownerCompletedUpgrades,
    uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context) {
    static_cast<void>(onObjectBuildCompleted(
        registry, lifecycle, ownership, object, ownerCompletedUpgrades,
        confirmedTick, context));
    if (context.content) {
        object_simulation_detail::state(*this).m_specialPower.onBuildCompleted(
            registry, lifecycle, object, *context.content, object_simulation_detail::state(*this).m_rules,
            confirmedTick);
    }
    UpgradeMask constructionOwnerUpgrades = ownerCompletedUpgrades;
    if (context.players) {
        const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
        const OwnerComponent* owner = entity
            ? ecs::try_get<OwnerComponent>(registry, *entity)
            : nullptr;
        const PlayerState* player = owner
            ? context.players->get(owner->player)
            : nullptr;
        constructionOwnerUpgrades =
            player ? player->upgrades.completed : UpgradeMask{};
    }
    object_simulation_detail::state(*this).m_upgrades.onConstructionCompleted(
        registry, lifecycle, object, constructionOwnerUpgrades,
        object_simulation_detail::state(*this).m_rules, confirmedTick, context);
}


void ObjectSimulation::initializeObject(ecs::registry& registry, ecs::entity entity,
                                        const game::ThingTemplate& templateData,
                                        const GameContentSnapshot& content,
                                        const game::terrain::TerrainLogic& terrain,
                                        SimulationRandom* random) const {
    TransformComponent* transform = ecs::try_get<TransformComponent>(registry, entity);
    if (!transform) return;
    ObjectFixedTransformComponent* fixedTransform =
        ecs::try_get<ObjectFixedTransformComponent>(registry, entity);
    if (!fixedTransform || !fixedTransform->authoritative) return;
    ObjectTerrainLayerComponent* terrainLayer =
        ecs::try_get<ObjectTerrainLayerComponent>(registry, entity);
    if (!terrainLayer) {
        terrainLayer = &ecs::emplace<ObjectTerrainLayerComponent>(
            registry, entity,
            ObjectTerrainLayerComponent{
                .pathfindLayer = terrain.isLoaded()
                    ? terrain.pathfindLayerForDestinationRaw(
                          fixedTransform->position.x.raw(),
                          fixedTransform->position.y.raw(),
                          fixedTransform->position.z.raw())
                    : game::terrain::kGroundPathfindLayer,
            });
    }
    // GameSession installs the frozen Archetype before this initialization
    // boundary. Copy only the shared immutable death plan into the entity;
    // no live system retains the mutable ThingFactory or reparses ModuleData.
    if (const ThingTemplateComponent* templateComponent =
            ecs::try_get<ThingTemplateComponent>(registry, entity);
        templateComponent && templateComponent->archetype) {
        ObjectDeathReactionComponent value{
            .plan = templateComponent->archetype->deathReactionPlan,
        };
        if (ObjectDeathReactionComponent* existing =
                ecs::try_get<ObjectDeathReactionComponent>(registry, entity)) {
            *existing = std::move(value);
        } else {
            ecs::emplace<ObjectDeathReactionComponent>(registry, entity, std::move(value));
        }
    }
    // TransitionDamageFX is similarly sparse and shares only its immutable
    // state table. Emitter handles stay presentation-owned.
    object_simulation_detail::state(*this).m_transitionDamageFx.initializeObject(registry, entity);
    // BoneFXUpdate is sparse but stateful: every rule owns its own confirmed
    // timer vector and is armed from the current Body state on first update.
    object_simulation_detail::state(*this).m_boneFx.initializeObject(registry, entity);
    object_simulation_detail::state(*this).m_bridge.initializeObject(registry, entity, &content);
    object_simulation_detail::state(*this).m_spawnSlave.initializeObject(registry, entity);
    // The timer component shares the frozen Archetype profile and determines
    // its absolute deadline during spawn, just like legacy UpdateModule
    // construction. Its counter PRF avoids a mutable global RNG and never
    // reparses INI text inside a live match.
    object_simulation_detail::state(*this).m_lifetime.initializeObject(registry, entity, object_simulation_detail::state(*this).m_rules, object_simulation_detail::state(*this).m_sessionSeed,
                                object_simulation_detail::state(*this).m_hulkLifetimeOverrideFrames);
    // BaseRegenerateUpdate's module-local recipe has no fields, but every
    // entity owns an independent wake schedule starting at its spawn tick.
    const ObjectLifecycleComponent* lifecycle =
        ecs::try_get<ObjectLifecycleComponent>(registry, entity);
    object_simulation_detail::state(*this).m_baseRegenerate.initializeObject(registry, entity, object_simulation_detail::state(*this).m_rules,
                                      lifecycle ? lifecycle->createdAtTick : 0);
    // AutoDepositUpdate's constructor similarly schedules its first callback
    // relative to object creation. Its capture arm remains false until that
    // first callback executes, preserving the source load/capture guard.
    object_simulation_detail::state(*this).m_autoDeposit.initializeObject(registry, entity, object_simulation_detail::state(*this).m_rules,
                                   lifecycle ? lifecycle->createdAtTick : 0);
    // Collide is an opt-in sparse component: ordinary objects pay no per-tick
    // module cost, while crate objects share the immutable final recipe.
    object_simulation_detail::state(*this).m_crateCollide.initializeObject(registry, entity);
    object_simulation_detail::state(*this).m_containment.initializeObject(registry, entity, &content, object_simulation_detail::state(*this).m_rules);
    object_simulation_detail::state(*this).m_squishCollide.initializeObject(registry, entity);
    // Poison and WeaponBonusUpdate are sparse behavior families. Each live
    // entity keeps only per-occurrence clocks and the shared frozen recipe.
    object_simulation_detail::state(*this).m_poisoned.initializeObject(registry, entity);
    object_simulation_detail::state(*this).m_weaponBonusUpdate.initializeObject(
        registry, entity, lifecycle ? lifecycle->createdAtTick : 0);
    object_simulation_detail::state(*this).m_fireWeaponBehaviors.initializeObject(
        registry, entity, content, object_simulation_detail::state(*this).m_rules.logicFramesPerSecond,
        lifecycle ? lifecycle->createdAtTick : 0);
    object_simulation_detail::state(*this).m_fireWeaponUpdate.initializeObject(
        registry, entity, content, object_simulation_detail::state(*this).m_rules.logicFramesPerSecond,
        lifecycle ? lifecycle->createdAtTick : 0);
    object_simulation_detail::state(*this).m_fireWeaponCollide.initializeObject(registry, entity, content);
    object_simulation_detail::state(*this).m_oclUpdate.initializeObject(registry, entity, content);
    object_simulation_detail::state(*this).m_overcharge.initializeObject(registry, entity);
    object_simulation_detail::state(*this).m_radiusDecal.initializeObject(registry, entity);
    object_simulation_detail::state(*this).m_techBuilding.initializeObject(registry, entity, object_simulation_detail::state(*this).m_rules);
    object_simulation_detail::state(*this).m_fireUpdates.initializeObject(registry, entity, content);
    object_simulation_detail::state(*this).m_empUpdate.initializeObject(
        registry, entity, random, object_simulation_detail::state(*this).m_rules.logicFramesPerSecond,
        lifecycle ? lifecycle->createdAtTick : 0);
    object_simulation_detail::state(*this).m_leafletDrop.initializeObject(
        registry, entity, object_simulation_detail::state(*this).m_rules.logicFramesPerSecond,
        lifecycle ? lifecycle->createdAtTick : 0);
    object_simulation_detail::state(*this).m_stealth.initializeObject(
        registry, entity, object_simulation_detail::state(*this).m_rules, object_simulation_detail::state(*this).m_sessionSeed,
        lifecycle ? lifecycle->createdAtTick : 0);
    // Every DynamicShroud occurrence converts its authored milliseconds once
    // at spawn and then owns an independent fixed-tick state machine.
    object_simulation_detail::state(*this).m_dynamicShroud.initializeObject(
        registry, entity, object_simulation_detail::state(*this).m_rules,
        lifecycle ? lifecycle->createdAtTick : 0);
    // Geometry Update modules are sparse, but their constructor clocks begin
    // at spawn even when InitialDelay is omitted (minimum one frame).
    object_simulation_detail::state(*this).m_dynamicGeometry.initializeObject(registry, entity, object_simulation_detail::state(*this).m_rules);
    object_simulation_detail::state(*this).m_enemyNear.initializeObject(registry, entity, object_simulation_detail::state(*this).m_rules, random);
    object_simulation_detail::state(*this).m_animationSteering.initializeObject(registry, entity);
    object_simulation_detail::state(*this).m_tactical.initializeObject(
        registry, entity, content, object_simulation_detail::state(*this).m_rules,
        lifecycle ? lifecycle->createdAtTick : 0);
    object_simulation_detail::state(*this).m_economy.initializeObject(
        registry, entity, &content,
        object_simulation_detail::state(*this).m_rules,
        lifecycle ? lifecycle->createdAtTick : 0);
    object_simulation_detail::state(*this).m_builder.initializeObject(
        registry, entity, lifecycle ? lifecycle->createdAtTick : 0);
    object_simulation_detail::state(*this).m_rebuildHole.initializeObject(registry, entity);
    object_simulation_detail::state(*this).m_checkpoint.initializeObject(registry, entity, object_simulation_detail::state(*this).m_rules, random);
    object_simulation_detail::state(*this).m_airfield.initializeObject(registry, entity, object_simulation_detail::state(*this).m_rules);
    object_simulation_detail::state(*this).m_cleanupHazard.initializeObject(
        registry, entity, lifecycle ? lifecycle->createdAtTick : 0);
    object_simulation_detail::state(*this).m_minefield.initializeObject(
        registry, entity, content, object_simulation_detail::state(*this).m_rules,
        lifecycle ? lifecycle->createdAtTick : 0);
    object_simulation_detail::state(*this).m_neutronMissileSlowDeath.initializeObject(registry, entity);
    object_simulation_detail::state(*this).m_countermeasures.initializeObject(
        registry, entity, object_simulation_detail::state(*this).m_rules.logicFramesPerSecond);
    object_simulation_detail::state(*this).m_smartBomb.initializeObject(registry, entity);
    object_simulation_detail::state(*this).m_stickyBomb.initializeObject(registry, entity);
    object_simulation_detail::state(*this).m_waveGuide.initializeObject(
        registry, entity, content, object_simulation_detail::state(*this).m_rules,
        lifecycle ? lifecycle->createdAtTick : 0);
    object_simulation_detail::state(*this).m_spyVision.initializeObject(registry, entity);
    object_simulation_detail::state(*this).m_specialPower.initializeObject(
        registry, entity, content, object_simulation_detail::state(*this).m_rules,
        lifecycle ? lifecycle->createdAtTick : 0);
    object_simulation_detail::state(*this).m_missileLauncherBuilding.initializeObject(
        registry, entity, content, object_simulation_detail::state(*this).m_rules.logicFramesPerSecond,
        lifecycle ? lifecycle->createdAtTick : 0);
    object_simulation_detail::state(*this).m_particleUplinkCannon.initializeObject(
        registry, entity, content, object_simulation_detail::state(*this).m_rules.logicFramesPerSecond,
        lifecycle ? lifecycle->createdAtTick : 0);
    // SupplyWarehouseCripplingBehavior begins asleep and is armed only by a
    // committed HP-decrease event.  Spawn still projects the current Body
    // state into the reusable dock-crippled reason mask.
    object_simulation_detail::state(*this).m_supplyWarehouseCrippling.initializeObject(registry, entity, object_simulation_detail::state(*this).m_rules);
    // FloatUpdate owns only an enabled bit per final recipe occurrence. It
    // shares the later position-authority boundary with HeightDie/Physics.
    object_simulation_detail::state(*this).m_float.initializeObject(registry, entity);
    // HeightDieUpdate starts with an unarmed first-update deadline and its
    // own (-1,-1,-1) direction sentinel. It must be assembled before any
    // physics/locomotor system can publish the first confirmed position.
    object_simulation_detail::state(*this).m_heightDie.initializeObject(registry, entity);
    // Combat profile resolution is independent of locomotor support: a
    // static building, projectile or helper may still take armor-modified
    // Body damage even when it has no movement component.
    initializeResolvedArmor(registry, entity, content);
    container::SharedPtr<const game::ObjectPhysicsPlan> physicsPlan;
    if (const ThingTemplateComponent* templateComponent =
            ecs::try_get<ThingTemplateComponent>(registry, entity);
        templateComponent && templateComponent->archetype) {
        physicsPlan = templateComponent->archetype->physicsPlan;
    }
    if (physicsPlan) {
        ObjectPhysicsComponent physics = compilePhysicsComponent(
            *physicsPlan, *fixedTransform, content, terrain,
            terrainLayer->pathfindLayer, object_simulation_detail::state(*this).m_rules);
        if (ObjectPhysicsComponent* existing = ecs::try_get<ObjectPhysicsComponent>(registry, entity)) {
            *existing = std::move(physics);
        } else {
            ecs::emplace<ObjectPhysicsComponent>(registry, entity, std::move(physics));
        }
    }
    container::Vector<game::FrozenLocomotorTemplate> upgradedLocomotors =
        collectRuntimeLocomotors(
        templateData, content, game::LocomotorSetSlot::NormalUpgraded);
    if (!upgradedLocomotors.empty()) {
        ecs::emplace<ObjectLocomotorSetUpgradeComponent>(registry, entity,
            ObjectLocomotorSetUpgradeComponent{
                .upgraded = std::move(upgradedLocomotors)});
    }
    container::Vector<game::FrozenLocomotorTemplate> locomotors =
        collectRuntimeLocomotors(
        templateData, content, game::LocomotorSetSlot::Normal);
    if (locomotors.empty()) {
        if (const ObjectHealthComponent* health = ecs::try_get<ObjectHealthComponent>(registry, entity)) {
            if (RenderModelComponent* visual = ecs::try_get<RenderModelComponent>(registry, entity)) {
                projectBodyDamageVisual(
                    objectBodyDamagePresentationState(
                        registry, entity, health->damageState),
                    *visual);
            }
        }
        return;
    }

    const LogicFixedVec3 position = readAuthoritativeObjectPosition(
        registry, entity, *transform);
    const math::q32_32 ground = terrain.isLoaded()
        ? math::q32_32::from_raw(terrain.pathfindLayerHeightRawAt(
              terrainLayer->pathfindLayer, position.x.raw(), position.y.raw())
              .value_or(terrain.groundHeightRaw(
                  position.x.raw(), position.y.raw())))
        : position.z;
    ObjectLocomotionComponent locomotion;
    locomotion.profiles = std::move(locomotors);
    applyLocomotorTemplate(locomotion, locomotion.profiles.front());
    chooseLocomotorForPosition(locomotion, terrain, position.x, position.y);
    locomotion.groundOffsetFixed = position.z - ground;
    ecs::emplace<ObjectLocomotionComponent>(registry, entity,
                                            std::move(locomotion));
    if (const ObjectHealthComponent* health = ecs::try_get<ObjectHealthComponent>(registry, entity)) {
        if (RenderModelComponent* visual = ecs::try_get<RenderModelComponent>(registry, entity)) {
            projectBodyDamageVisual(
                objectBodyDamagePresentationState(
                    registry, entity, health->damageState),
                *visual);
        }
    }
}

void ObjectSimulationProgressionDomain::onObjectOwnerChanged(
    ecs::registry& registry, ObjectLifecycle& lifecycle, ObjectId object,
    const UpgradeMask& newOwnerCompletedUpgrades,
    uint64_t confirmedTick, ObjectUpgradeExecutionContext context) {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity || lifecycle.isPendingDestroy(object)) return;
    // RefCode capture forwards module-specific onCapture hooks; it does not
    // invoke Object::updateUpgradeModules.  The UpgradeMux facade therefore
    // refreshes only FXListDie's current-owner conflict gate and leaves every
    // dormant rule untouched until a real re-evaluation boundary.
    object_simulation_detail::state(*this).m_upgrades.onObjectOwnerChanged(registry, lifecycle, object,
                                    newOwnerCompletedUpgrades, object_simulation_detail::state(*this).m_rules,
                                    confirmedTick);
    if (context.players) {
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, *entity);
        if (owner) {
            object_simulation_detail::state(*this).m_autoDeposit.onObjectOwnerChanged(
                registry, lifecycle, object, *context.players, owner->player,
                object_simulation_detail::state(*this).m_rules, confirmedTick, object_simulation_detail::state(*this).m_autoDepositEvents);
            object_simulation_detail::state(*this).m_techBuilding.onObjectOwnerChanged(
                registry, lifecycle, *context.players, object, object_simulation_detail::state(*this).m_rules,
                confirmedTick, object_simulation_detail::state(*this).m_techBuildingEvents, object_simulation_detail::state(*this).m_beaconClientEvents);
        }
    }

    // TransportContain::onCapture is distinct from Object::defect's later
    // removeAllContained(TRUE).  A normal team change asks Transport and
    // RiderChange passengers to leave through the authored door/ExitDelay;
    // a sniped (UNMANNED) vehicle must release them immediately so the first
    // exiting passenger cannot recapture it.  Other Contain families own
    // different capture semantics and are deliberately not folded into this
    // transport hook.
    ObjectContainmentRuntimeComponent* containment =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *entity);
    const ObjectContainmentComponent* contents =
        ecs::try_get<ObjectContainmentComponent>(registry, *entity);
    if (!containment || !containment->plan || !contents ||
        contents->objects.empty()) {
        if (containment) containment->ownerChangeEvacuationPending = false;
        return;
    }
    const auto isOwnerChangeTransportPassenger =
        [&](ObjectId passenger) {
            const std::optional<ecs::entity> passengerEntity =
                lifecycle.entityFromIdIncludingPending(passenger);
            const ObjectContainedByComponent* edge = passengerEntity
                ? ecs::try_get<ObjectContainedByComponent>(
                      registry, *passengerEntity)
                : nullptr;
            if (!edge || edge->container != object ||
                edge->containmentRuleIndex >=
                    containment->plan->rules.size()) {
                return false;
            }
            const ObjectContainmentKind kind = containment->plan->rules[
                edge->containmentRuleIndex].kind;
            return kind == ObjectContainmentKind::Transport ||
                kind == ObjectContainmentKind::RiderChange;
        };
    const bool ownsPassengers = std::any_of(
        contents->objects.begin(), contents->objects.end(),
        [&](const ObjectContainedObjectRecord& record) {
            return isOwnerChangeTransportPassenger(record.object);
        });
    if (!ownsPassengers) {
        containment->ownerChangeEvacuationPending = false;
        return;
    }
    if (isObjectDisabledBy(registry, *entity,
                           ObjectDisabledReason::Unmanned,
                           confirmedTick)) {
        container::Vector<ObjectId> passengers;
        for (const ObjectContainedObjectRecord& record : contents->objects) {
            if (isOwnerChangeTransportPassenger(record.object))
                passengers.push_back(record.object);
        }
        containment->ownerChangeEvacuationPending = false;
        for (const ObjectId passenger : passengers) {
            static_cast<void>(static_cast<ObjectSimulation&>(*this).requestContainment(
                registry, lifecycle,
                {.kind = ObjectContainmentRequestKind::Detach,
                 .container = object,
                 .object = passenger,
                 .confirmedTick = confirmedTick,
                 .force = true},
                context.players, context.content));
        }
    } else {
        containment->ownerChangeEvacuationPending = true;
    }
}

void ObjectSimulation::onObjectDestroyRequested(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context) {
    // Object::onDestroy removes this object from its parent before invoking
    // BehaviorModule::onDelete. This is object-level structure, not an
    // authored Contain capability of the child.
    static_cast<void>(detachContainedObject(
        registry, lifecycle, object, confirmedTick));
    container::Vector<ObjectRailedTransportDockAttachCompletion>
        cancelledDockAttach;
    object_simulation_detail::state(*this).m_bridge.detachObjectRelationships(
        registry, lifecycle, object, confirmedTick, cancelledDockAttach);
    for (const ObjectRailedTransportDockAttachCompletion& completion :
         cancelledDockAttach) {
        object_simulation_detail::state(*this).m_containmentEvents.push_back({
            .kind = ObjectContainmentRequestKind::Attach,
            .container = completion.request.container,
            .object = completion.request.object,
            .confirmedTick = confirmedTick,
            .accepted = false,
        });
    }
    if (const std::optional<ecs::entity> entity =
            lifecycle.entityFromIdIncludingPending(object)) {
        if (const ObjectAIPathMovementComponent* movement =
                ecs::try_get<ObjectAIPathMovementComponent>(registry, *entity)) {
            releaseAIMovementPath(context.navigation, *movement);
            ecs::remove<ObjectAIPathMovementComponent>(registry, *entity);
        }
        if (const ObjectAirfieldComponent* airfield =
                ecs::try_get<ObjectAirfieldComponent>(registry, *entity)) {
            // A carrier can disappear while aircraft still own live parking
            // reservations.  PendingDestroy hides the carrier from ordinary
            // release APIs, so detach their typed deck support here while
            // the carrier runtime is still structurally inspectable.
            container::Vector<ObjectId> parkedAircraft;
            for (const ObjectAirfieldParkingRuntime& parking :
                 airfield->parkingPlaces) {
                for (const ObjectId aircraft : parking.spaces)
                    if (aircraft) parkedAircraft.push_back(aircraft);
            }
            for (const ObjectAirfieldFlightDeckRuntime& deck :
                 airfield->flightDecks) {
                for (const ObjectId aircraft : deck.spaces)
                    if (aircraft) parkedAircraft.push_back(aircraft);
            }
            std::sort(parkedAircraft.begin(), parkedAircraft.end());
            parkedAircraft.erase(
                std::unique(parkedAircraft.begin(), parkedAircraft.end()),
                parkedAircraft.end());
            for (const ObjectId aircraft : parkedAircraft) {
                const std::optional<ecs::entity> aircraftEntity =
                    lifecycle.entityFromIdIncludingPending(aircraft);
                if (!aircraftEntity) continue;
                const ObjectCarrierDeckComponent* carrierDeck =
                    ecs::try_get<ObjectCarrierDeckComponent>(
                        registry, *aircraftEntity);
                if (carrierDeck && carrierDeck->carrier == object) {
                    ecs::remove<ObjectCarrierDeckComponent>(
                        registry, *aircraftEntity);
                    static_cast<void>(ObjectStatusSystem::apply(
                        registry, *aircraftEntity,
                        {.clearMask = game::objectStatusBit(
                             game::ObjectStatusFlag::DeckHeightOffset),
                         .confirmedTick = confirmedTick}));
                }
                if (ObjectAirfieldComponent* aircraftRuntime =
                        ecs::try_get<ObjectAirfieldComponent>(
                            registry, *aircraftEntity)) {
                    for (ObjectJetAiRuntime& jet : aircraftRuntime->jetAi) {
                        if (jet.reservedAirfield != object) continue;
                        jet.reservedAirfield = INVALID_OBJECT_ID;
                        jet.parkingReservation = {};
                        jet.runwayReservation = {};
                        jet.state = ObjectAircraftRuntimeState::Idle;
                    }
                }
            }
        }
    }
    container::SharedPtr<const game::ObjectOnDeletePlan> plan;
    if (const std::optional<ecs::entity> entity =
            lifecycle.entityFromIdIncludingPending(object)) {
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(registry, *entity);
        if (type && type->archetype) plan = type->archetype->onDeletePlan;
    }
    object_simulation_detail::state(*this).m_deleteWalks.push_back({
        .object = object,
        .plan = std::move(plan),
        .submissionOrdinal = reserveGameplaySubmissionOrdinal(),
        .confirmedTick = confirmedTick,
    });
}

ObjectDeleteWalkAdvance ObjectSimulation::advanceDeleteWalk(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    ObjectDeleteWalkState& deleteWalk,
    ObjectUpgradeExecutionContext context) {
    if (deleteWalk.phase == ObjectDeleteWalkPhase::Postamble) {
        return ObjectDeleteWalkAdvance::ReadyForPostamble;
    }
    if (deleteWalk.phase != ObjectDeleteWalkPhase::Behaviors ||
        !deleteWalk.object) {
        return ObjectDeleteWalkAdvance::InvalidState;
    }
    const std::optional<game::ObjectOnDeleteEntry> entry =
        takeNextObjectDeleteEntry(deleteWalk);
    if (!entry) {
        deleteWalk.phase = ObjectDeleteWalkPhase::Postamble;
        return ObjectDeleteWalkAdvance::ReadyForPostamble;
    }

    auto& simulationState = object_simulation_detail::state(*this);
    switch (entry->capability) {
    case game::ObjectOnDeleteCapability::Builder: {
        const size_t scaffoldBegin =
            simulationState.m_bridgeRepairScaffoldIntents.size();
        static_cast<void>(simulationState.m_builder.cancelAllTasks(
            registry, lifecycle, deleteWalk.object,
            deleteWalk.confirmedTick,
            simulationState.m_bridgeRepairScaffoldIntents,
            entry->authoredOrder));
        for (size_t index = scaffoldBegin;
             index < simulationState.m_bridgeRepairScaffoldIntents.size();
             ++index) {
            simulationState.m_bridgeRepairScaffoldIntents[index]
                .submissionOrdinal = reserveGameplaySubmissionOrdinal();
        }
        break;
    }
    case game::ObjectOnDeleteCapability::JetReservations:
        object_simulation_detail::detachDeadAircraftReservations(
            simulationState.m_airfield, registry, lifecycle,
            deleteWalk.object, deleteWalk.confirmedTick,
            simulationState.m_airfieldEvents, entry->authoredOrder);
        break;
    case game::ObjectOnDeleteCapability::BattlePlan:
        static_cast<void>(simulationState.m_tactical.onBattlePlanDelete(
            registry, lifecycle, deleteWalk.object, entry->authoredOrder,
            simulationState.m_rules, context, deleteWalk.confirmedTick));
        break;
    case game::ObjectOnDeleteCapability::PropagandaTower:
        static_cast<void>(simulationState.m_tactical.onPropagandaTowerDie(
            registry, lifecycle, context.players, context.content,
            simulationState.m_rules, context.random, deleteWalk.object,
            entry->authoredOrder, deleteWalk.confirmedTick));
        break;
    case game::ObjectOnDeleteCapability::Spawn:
        simulationState.m_spawnSlave.onSpawnerDelete(
            registry, lifecycle, deleteWalk.object, entry->authoredOrder,
            deleteWalk.confirmedTick,
            simulationState.m_deleteDestroyRequests);
        break;
    case game::ObjectOnDeleteCapability::Containment:
        simulationState.m_containment.onContainerDelete(
            registry, lifecycle, deleteWalk.object, entry->authoredOrder,
            deleteWalk.confirmedTick,
            simulationState.m_deleteDestroyRequests);
        break;
    case game::ObjectOnDeleteCapability::Count:
        return ObjectDeleteWalkAdvance::InvalidState;
    }
    for (ObjectDeleteDestroyRequest& request :
         simulationState.m_deleteDestroyRequests) {
        if (request.submissionOrdinal == 0) {
            request.submissionOrdinal = reserveGameplaySubmissionOrdinal();
        }
    }
    return ObjectDeleteWalkAdvance::BehaviorHandled;
}

bool ObjectSimulation::completeDeleteWalk(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    ObjectDeleteWalkState deleteWalk,
    ObjectUpgradeExecutionContext context) {
    if (deleteWalk.phase != ObjectDeleteWalkPhase::Postamble) return false;
    auto& simulationState = object_simulation_detail::state(*this);

    // SpecialAbilityUpdate calls onExit(true) from destruction rather than
    // BehaviorModule::onDelete. Execute this destructor-equivalent suffix
    // only after every authored callback has completed.
    container::Vector<ObjectDamageRequest> reclaimDamage;
    simulationState.m_tactical.onSpecialAbilityReclaim(
        registry, lifecycle, deleteWalk.object, deleteWalk.confirmedTick,
        reclaimDamage);
    for (ObjectDamageRequest& request : reclaimDamage) {
        queueDamage(std::move(request));
    }

    // Module destructors/resource owners run only after every authored
    // BehaviorModule::onDelete callback. These handlers stop retained
    // presentation emitters, beams, decals and audio; none may manufacture a
    // gameplay onDie consequence for an ordinary direct DELETE.
    simulationState.m_autoHeal.onObjectReclaim(
        registry, lifecycle, deleteWalk.object, deleteWalk.confirmedTick,
        simulationState.m_autoHealParticleEvents);
    simulationState.m_dynamicGeometry.onObjectReclaim(
        registry, lifecycle, deleteWalk.object, deleteWalk.confirmedTick,
        simulationState.m_dynamicGeometryPresentationEvents);
    if (const std::optional<ecs::entity> entity =
            lifecycle.entityFromIdIncludingPending(deleteWalk.object)) {
        simulationState.m_dynamicShroud.terminateObject(
            registry, *entity, deleteWalk.object, deleteWalk.confirmedTick,
            simulationState.m_dynamicShroudDecalEvents);
    }
    static_cast<void>(simulationState.m_radiusDecal.killRadiusDecal(
        registry, lifecycle, deleteWalk.object, deleteWalk.confirmedTick,
        simulationState.m_radiusDecalEvents));
    simulationState.m_techBuilding.onObjectReclaim(
        registry, lifecycle, deleteWalk.object, deleteWalk.confirmedTick,
        simulationState.m_beaconClientEvents);
    simulationState.m_missileLauncherBuilding.onObjectReclaim(
        registry, lifecycle, deleteWalk.object, deleteWalk.confirmedTick,
        simulationState.m_objectFireAudioCommands);
    simulationState.m_particleUplinkCannon.onObjectReclaim(
        registry, lifecycle, deleteWalk.object, deleteWalk.confirmedTick,
        simulationState.m_particleUplinkBeamEvents,
        simulationState.m_particleUplinkPhaseEvents,
        simulationState.m_objectFireAudioCommands);
    simulationState.m_waveGuide.onObjectReclaim(
        registry, lifecycle, deleteWalk.object, deleteWalk.confirmedTick,
        simulationState.m_nextGameplaySubmissionOrdinal,
        simulationState.m_waveGuideEvents,
        simulationState.m_objectFireAudioCommands);
    simulationState.m_airfield.onObjectReclaim(
        registry, lifecycle, simulationState.m_rules, deleteWalk.object,
        deleteWalk.confirmedTick,
        simulationState.m_chinookRopePresentationEvents,
        simulationState.m_radiusDecalEvents);
    if (const std::optional<ecs::entity> entity =
            lifecycle.entityFromIdIncludingPending(deleteWalk.object)) {
        math::q32_32 terrainDecalFadeRate =
            math::q32_32::from_fraction(3, 100);
        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(registry, *entity);
        const ObjectSlowDeathRuntimeComponent* slowRuntime =
            ecs::try_get<ObjectSlowDeathRuntimeComponent>(registry, *entity);
        const ObjectDeathReactionComponent* reactions =
            ecs::try_get<ObjectDeathReactionComponent>(registry, *entity);
        if (hasKind(kinds, game::ObjectKindOf::Infantry) && slowRuntime &&
            reactions && reactions->plan &&
            slowRuntime->selectedRuleIndex < reactions->plan->rules.size()) {
            const game::ObjectDeathReactionRule& selected =
                reactions->plan->rules[slowRuntime->selectedRuleIndex];
            if (selected.slowDeath &&
                selected.slowDeath->sinkRateUnitsPerSecond !=
                    math::q32_32{}) {
                terrainDecalFadeRate = math::q32_32::from_fraction(1, 5);
            }
        }
        setObjectTerrainDecalFade(
            registry, *entity, {}, terrainDecalFadeRate,
            deleteWalk.confirmedTick);
    }
    if (const std::optional<ecs::entity> entity =
            lifecycle.entityFromIdIncludingPending(deleteWalk.object)) {
        ObjectWeaponComponent* weapons =
            ecs::try_get<ObjectWeaponComponent>(registry, *entity);
        if (weapons && weapons->loopingFireSoundStopTick != 0) {
            const game::WeaponTemplate* loopWeapon = context.content
                ? context.content->findWeapon(
                      weapons->loopingFireSoundWeapon)
                : nullptr;
            if (loopWeapon && !loopWeapon->fireSound.empty()) {
                simulationState.m_objectFireAudioCommands.push_back({
                    .kind = ObjectFireAudioCommandKind::StopLoop,
                    .object = deleteWalk.object,
                    .eventName = loopWeapon->fireSound,
                    .confirmedTick = deleteWalk.confirmedTick,
                });
            }
            weapons->loopingFireSoundStopTick = 0;
            weapons->loopingFireSoundWeapon = {};
        }
    }

    // Every valid authored OpenContain occurrence should have emptied its
    // literal list through child DeleteWalks. Malformed/unattributed edges
    // are detached here so physical reclamation cannot leave a dangling
    // ContainedBy relation; no gameplay destruction is invented for them.
    container::Vector<ObjectId> orphanedContents;
    if (const std::optional<ecs::entity> entity =
            lifecycle.entityFromIdIncludingPending(deleteWalk.object)) {
        if (const ObjectContainmentComponent* contents =
                ecs::try_get<ObjectContainmentComponent>(registry, *entity)) {
            orphanedContents.reserve(contents->objects.size());
            for (const ObjectContainedObjectRecord& record :
                 contents->objects) {
                if (record.object) orphanedContents.push_back(record.object);
            }
        }
    }
    for (const ObjectId child : orphanedContents) {
        static_cast<void>(detachContainedObject(
            registry, lifecycle, child, deleteWalk.confirmedTick));
    }
    simulationState.m_deletePostambleEvents.push_back({
        .object = deleteWalk.object,
        .confirmedTick = deleteWalk.confirmedTick,
    });
    deleteWalk.phase = ObjectDeleteWalkPhase::Completed;
    return true;
}

} // namespace engine
