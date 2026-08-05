#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"

#include "game/base/SimulationRandom.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/navigation/runtime/NavigationSystem.h"
#include "game/navigation/integration/NavigationTerrainLayerMapping.h"
#include "game/navigation/integration/NavigationDestinationAdjustment.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/definition/LocomotorTemplate.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/simulation/combat/ObjectCombatProfileRuntime.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/plan/movement/ObjectPhysicsPlanTypes.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"
#include "core/math/wwmath/base/wwmath.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <utility>
#include "game/object/simulation/runtime/ObjectSimulationDetail.h"
#include "game/object/simulation/runtime/ObjectSimulationDamageDetail.h"

namespace engine {

namespace {

using HealthScalar = ObjectHealthComponent::Scalar;

const HealthScalar kHealthZero{};
const HealthScalar kHealthOne{int32_t{1}};

[[nodiscard]] uint64_t millisecondsToTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0 || framesPerSecond == 0) return 0;
    const uint64_t product =
        static_cast<uint64_t>(milliseconds) * framesPerSecond;
    return product / 1000u + (product % 1000u != 0u ? 1u : 0u);
}

[[nodiscard]] uint64_t saturatingAdd(
    uint64_t value, uint64_t increment) noexcept {
    return increment > std::numeric_limits<uint64_t>::max() - value
        ? std::numeric_limits<uint64_t>::max()
        : value + increment;
}

[[nodiscard]] container::String admitDamageFx(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity victimEntity, ObjectHealthComponent& health,
    const ObjectDamageRequest& request, HealthScalar actualDamage,
    const ObjectSimulationRules& rules,
    const GameContentSnapshot* content) {
    if (!content) return {};
    // ActiveBody::doDamageFX runs after every admitted Damaged callback and
    // advances its per-type throttle before DamageFX chooses Minor/Major,
    // even when the resolved amount is zero (the crusher feedback path is
    // the stock example). Ordinary zero requests are rejected before this
    // helper, so retaining zero here does not invent new DamageFX events.
    const game::DamageFxCatalog* catalog = content->damageFxCatalog();
    const ObjectArmorComponent* armor =
        ecs::try_get<ObjectArmorComponent>(registry, victimEntity);
    if (!catalog || !armor || armor->selectedDamageFxName.empty()) return {};

    game::ObjectVeterancyLevel sourceVeterancy =
        game::ObjectVeterancyLevel::Regular;
    if (const std::optional<ecs::entity> source =
            lifecycle.entityFromIdIncludingPending(request.source)) {
        if (const ObjectVeterancyComponent* veterancy =
                ecs::try_get<ObjectVeterancyComponent>(registry, *source)) {
            sourceVeterancy = veterancy->level;
        }
    }

    const game::DamageType damageFxType =
        request.damageFxOverride.value_or(request.damageType);
    const game::DamageFxRule* rule = catalog->findRule(
        armor->selectedDamageFxName, damageFxType, sourceVeterancy);
    if (!rule) return {};
    if (health.hasDamageFxThrottle &&
        health.lastDamageFxType == damageFxType &&
        health.nextDamageFxTick > request.confirmedTick) {
        return {};
    }

    // ZH advances the throttle once a valid DamageFX definition is selected,
    // even when the selected Minor/Major slot is authored as NONE.
    health.hasDamageFxThrottle = true;
    health.lastDamageFxType = damageFxType;
    health.nextDamageFxTick = saturatingAdd(
        request.confirmedTick,
        millisecondsToTicks(rule->throttleTimeMilliseconds,
                            rules.logicFramesPerSecond));
    const game::DamageFxEffectReference* effect = catalog->selectEffect(
        armor->selectedDamageFxName, damageFxType, sourceVeterancy,
        actualDamage);
    return effect ? effect->fxListName : container::String{};
}

} // namespace

namespace object_simulation_detail {

void applyDamageRequest(ObjectSimulation& simulation, ecs::registry& registry,
                        ObjectLifecycle& lifecycle,
                        const ObjectDamageRequest& request,
                        HealthScalar requestedAmount,
                        const ObjectSimulationRules& rules,
                        ObjectUpgradeExecutionContext context,
                        uint64_t sessionSeed,
                        container::Vector<ObjectHealthEvent>& events,
                        ObjectBodyReactionExecutor& reactions,
                        ObjectDamageTransactionResult* transactionResult) {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(request.target);
    if (!entity) {
        appendIgnoredEvent(events, request, nullptr);
        return;
    }
    ObjectHealthComponent* health = ecs::try_get<ObjectHealthComponent>(registry, *entity);
    if (!health) {
        appendIgnoredEvent(events, request, nullptr);
        return;
    }

    if (health->acceptsDamage && health->indestructible &&
        request.damageType != game::DamageType::HEALING) {
        // ActiveBody::attemptDamage returns before force-kill, subdual and
        // specialised damage branches while this overridable runtime flag is
        // set. Direct attemptHealing is a separate RefCode entry point and is
        // therefore still allowed. InactiveBody also owns its UNRESISTABLE
        // one-shot Die path independently of this ActiveBody-only flag.
        appendIgnoredEvent(events, request, health);
        return;
    }

    // InactiveBody normally ignores damage/healing, but its special
    // UNRESISTABLE path must still trigger exactly one death action even
    // though it began effectively dead with zero HP.
    if (!health->acceptsDamage) {
        if (request.damageType == game::DamageType::UNRESISTABLE && !health->terminalDeathIssued) {
            // InactiveBody calls Object::onDie directly. It deliberately
            // skips ActiveBody's damager->scoreTheKill transition because it
            // has no health transaction to credit.
            requestDeath(simulation, registry, lifecycle, *entity, *health,
                         request, requestedAmount, kHealthZero, context,
                         sessionSeed, false, transactionResult);
        } else {
            appendIgnoredEvent(events, request, health);
        }
        return;
    }
    const ObjectKindOfComponent* entryKinds =
        ecs::try_get<ObjectKindOfComponent>(registry, *entity);
    const bool deadBridgeMayHeal =
        request.damageType == game::DamageType::HEALING &&
        (hasKind(entryKinds, game::ObjectKindOf::Bridge) ||
         hasKind(entryKinds, game::ObjectKindOf::BridgeTower));
    if (health->effectivelyDead && !deadBridgeMayHeal) {
        appendIgnoredEvent(events, request, health);
        return;
    }
    // Subdual is a Body-side state transaction even when a caller also marks
    // the request forceKill.  RefCode handles it before the generic kill/HP
    // branch, so a force-kill subdual request must never become ordinary
    // damage or structural destruction.
    if (isSubdualDamage(request.damageType)) {
        const bool wasSubdued = health->subdued;
        applySubdualDamage(registry, *entity, *health, request, requestedAmount, rules, events);
        markObjectDirty(
            registry, *entity,
            objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
                objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
        if (!wasSubdued && health->subdued) {
            if (ObjectMissileProjectileComponent* missile =
                    ecs::try_get<ObjectMissileProjectileComponent>(
                        registry, *entity);
                missile && !missile->jammed) {
                missile->jamPending = true;
            }
        }
        return;
    }
    if (request.damageType == game::DamageType::STATUS) {
        applyTimedStatusDamage(registry, *entity, *health, request,
                               requestedAmount, rules, events);
        return;
    }
    if (!request.forceKill && hasDedicatedDamageBehaviour(request.damageType)) {
        // Do not accidentally turn KILL_PILOT/subdual/status into ordinary HP
        // damage merely because their specialised modules have not arrived.
        appendIgnoredEvent(events, request, health);
        return;
    }

    const HealthScalar previous = health->currentFixed;
    const ObjectBodyDamageState previousState = health->damageState;
    HealthScalar secondLifeEstimate = requestedAmount;
    if (isHealthDamagingDamage(request.damageType)) {
        secondLifeEstimate *= armorMultiplierFor(
            registry, *entity, request.damageType);
    }
    const bool startSecondLife = shouldStartSecondLife(
        *health, request, secondLifeEstimate);
    HealthScalar current = previous;
    HealthScalar applied{};
    HealthScalar resolvedUnclippedDamage{};
    ObjectHealthEventKind eventKind = ObjectHealthEventKind::Ignored;
    const HealthScalar minimumHealth = HealthScalar::max(
        health->minimumHealthFloorFixed,
        health->clampsToOneHealth ? kHealthOne : kHealthZero);

    if (request.forceKill) {
        // ImmortalBody clamps the health delta before ActiveBody applies it:
        // force-kill therefore reduces a live immortal body to exactly one
        // hit point instead of leaving its previous health unchanged.
        current = minimumHealth;
        applied = previous - current;
        // ActiveBody reports the pre-clipping force-kill amount as actual
        // damage dealt and previous-current separately as clipped damage.
        resolvedUnclippedDamage = previous;
        eventKind = ObjectHealthEventKind::Damaged;
    } else if (request.damageType == game::DamageType::HEALING) {
        if (requestedAmount <= kHealthZero) {
            appendIgnoredEvent(events, request, health);
            return;
        }
        const HealthScalar healing = requestedAmount * armorMultiplierFor(
            registry, *entity, request.damageType);
        current = HealthScalar::min(health->maximumFixed, previous + healing);
        applied = current - previous;
        resolvedUnclippedDamage = healing;
        eventKind = ObjectHealthEventKind::Healed;
    } else if (requestedAmount <= kHealthZero &&
               request.emitZeroDamageFeedback) {
        // The first crusher overlap intentionally reaches Damage callbacks
        // with CRUSH/CRUSHED and an unchanged Body. This must not be treated
        // as healing, lethal damage or an ordinary ignored zero request.
        applied = kHealthZero;
        resolvedUnclippedDamage = kHealthZero;
        eventKind = ObjectHealthEventKind::Damaged;
    } else {
        if (requestedAmount <= kHealthZero) {
            appendIgnoredEvent(events, request, health);
            return;
        }
        HealthScalar amount = requestedAmount;
        // HighlanderBody limits ordinary raw requested damage before armor
        // and modifiers, matching the source Body code's ordering.
        if (health->onlyUnresistableCanKill && request.damageType != game::DamageType::UNRESISTABLE) {
            amount = HealthScalar::min(amount, HealthScalar::max(kHealthZero, previous - kHealthOne));
        }
        if (startSecondLife) {
            // The original constrains the first lethal ordinary hit to one
            // HP, then immediately switches the Body to its authored second
            // life.  This is intentionally before the generic zero-health
            // death check below.
            amount = HealthScalar::min(amount, HealthScalar::max(kHealthZero, previous - kHealthOne));
        }
        amount *= damageMultiplierFor(registry, *entity, request.damageType);
        resolvedUnclippedDamage = amount;
        current = HealthScalar::max(minimumHealth, previous - amount);
        applied = previous - current;
        eventKind = ObjectHealthEventKind::Damaged;
    }

    health->previousFixed = previous;
    health->currentFixed = current;
    health->damageState = damageStateFor(current, health->maximumFixed, rules);
    // ActiveBody marks every damaged CAN_BE_REPULSED object as a repulsor
    // when AIData.EnableRepulsors is enabled. The status mutation stays in
    // the damage transaction, so the following AI shadow phase observes the
    // confirmed result without a second registry writer.
    if (rules.ai.enableRepulsors &&
        eventKind == ObjectHealthEventKind::Damaged) {
        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(registry, *entity);
        if (kinds && game::objectHasKind(
                kinds->mask, game::ObjectKindOf::CanBeRepulsed)) {
            static_cast<void>(ObjectStatusSystem::apply(
                registry, *entity,
                {.setMask = game::objectStatusBit(
                     game::ObjectStatusFlag::Repulsor),
                 .confirmedTick = request.confirmedTick}));
            const uint64_t duration =
                static_cast<uint64_t>(rules.logicFramesPerSecond) * 2u;
            const uint64_t clearAtTick = request.confirmedTick >
                    std::numeric_limits<uint64_t>::max() - duration
                ? std::numeric_limits<uint64_t>::max()
                : request.confirmedTick + duration;
            ObjectRepulsorExpiryComponent* expiry =
                ecs::try_get<ObjectRepulsorExpiryComponent>(
                    registry, *entity);
            if (expiry) {
                expiry->clearAtTick = clearAtTick;
            } else {
                ecs::emplace<ObjectRepulsorExpiryComponent>(
                    registry, *entity,
                    ObjectRepulsorExpiryComponent{clearAtTick});
            }
        }
    }
    markObjectDirty(
        registry, *entity,
        objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
            objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
    if (health->damageState == ObjectBodyDamageState::Rubble &&
        previousState != ObjectBodyDamageState::Rubble) {
        applyStructureRubbleGameplayState(
            registry, *entity, rules, request.confirmedTick);
        // A live checkpoint already owns a navigation footprint, so its
        // transition to rubble is a real value-change occurrence. Initial
        // map-object rubble is handled by the Created footprint and does not
        // pass through this damage edge.
        if (const ObjectCheckpointComponent* checkpoint =
                ecs::try_get<ObjectCheckpointComponent>(registry, *entity)) {
            const uint32_t authoredOrder =
                checkpoint->plan && !checkpoint->plan->rules.empty()
                    ? checkpoint->plan->rules.front().authoredOrder
                    : 0;
            simulation.queueCheckpointNavigationChange(
                request.target, authoredOrder, request.confirmedTick);
        }
    }
    if (deadBridgeMayHeal && current > kHealthZero) {
        health->effectivelyDead = false;
        health->terminalDeathIssued = false;
        markObjectDirty(registry, *entity, ObjectDirtyDomain::Spatial);
    }
    rememberPreferredBodyDamageInfo(registry, lifecycle, *health, request);
    if (eventKind == ObjectHealthEventKind::Damaged && context.players &&
        health->lastDamageSourcePlayer) {
        const OwnerComponent* victimOwner =
            ecs::try_get<OwnerComponent>(registry, *entity);
        if (victimOwner) {
            static_cast<void>(context.players->markAttackedBy(
                victimOwner->player, health->lastDamageSourcePlayer));
        }
    }
    const OwnerComponent* damageVictimOwner =
        ecs::try_get<OwnerComponent>(registry, *entity);
    const TransformComponent* damageVictimTransform =
        ecs::try_get<TransformComponent>(registry, *entity);
    const ObjectGeometryComponent* damageVictimGeometry =
        ecs::try_get<ObjectGeometryComponent>(registry, *entity);
    const ObjectKindOfComponent* damageVictimKinds =
        ecs::try_get<ObjectKindOfComponent>(registry, *entity);
    const ThingTemplateComponent* damageVictimTemplate =
        ecs::try_get<ThingTemplateComponent>(registry, *entity);
    const container::SharedPtr<const game::ObjectArchetype>
        damageVictimArchetype = damageVictimTemplate
            ? damageVictimTemplate->archetype
            : nullptr;
    const std::optional<ecs::entity> damageSourceEntity =
        lifecycle.entityFromIdIncludingPending(request.source);
    const OwnerComponent* damageSourceOwner = damageSourceEntity
        ? ecs::try_get<OwnerComponent>(registry, *damageSourceEntity)
        : nullptr;
    const TransformComponent* damageSourceTransform = damageSourceEntity
        ? ecs::try_get<TransformComponent>(registry, *damageSourceEntity)
        : nullptr;
    const ObjectGeometryComponent* damageSourceGeometry = damageSourceEntity
        ? ecs::try_get<ObjectGeometryComponent>(registry, *damageSourceEntity)
        : nullptr;
    const ObjectAirborneComponent* damageSourceAirborne = damageSourceEntity
        ? ecs::try_get<ObjectAirborneComponent>(registry, *damageSourceEntity)
        : nullptr;
    const ObjectStatusComponent* damageSourceStatus = damageSourceEntity
        ? ecs::try_get<ObjectStatusComponent>(registry, *damageSourceEntity)
        : nullptr;
    const bool sourceAirborne =
        (damageSourceAirborne && damageSourceAirborne->isAirborne) ||
        (damageSourceStatus && damageSourceStatus->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::AirborneTarget)));
    const LogicFixedVec3 damageVictimPosition = damageVictimTransform
        ? readAuthoritativeObjectPosition(registry, *entity, *damageVictimTransform)
        : LogicFixedVec3{};
    const LogicFixedVec3 damageSourcePosition =
        damageSourceEntity && damageSourceTransform
            ? readAuthoritativeObjectPosition(
                  registry, *damageSourceEntity, *damageSourceTransform)
            : LogicFixedVec3{};
    container::String damageFxListName;
    if (eventKind == ObjectHealthEventKind::Damaged) {
        damageFxListName = admitDamageFx(
            registry, lifecycle, *entity, *health, request,
            resolvedUnclippedDamage, rules, context.content);
    }
    const size_t primaryHealthEventIndex = events.size();
    events.push_back({
        .kind = eventKind,
        .object = request.target,
        .source = request.source,
        .damageType = request.damageType,
        .damageFxType = request.damageFxOverride.value_or(request.damageType),
        .bodyLastDamageType = health->lastDamageType,
        .damageFxListName = std::move(damageFxListName),
        .deathType = request.deathType,
        .requestedAmount = request.amount,
        .appliedAmountFixed = applied,
        .actualDamageDealtFixed =
            (eventKind == ObjectHealthEventKind::Damaged ||
             eventKind == ObjectHealthEventKind::Healed)
                ? resolvedUnclippedDamage : applied,
        .previousHealth = previous,
        .currentHealth = current,
        .previousState = previousState,
        .currentState = health->damageState,
        .confirmedTick = request.confirmedTick,
        .sourcePlayer = damageSourceOwner
            ? damageSourceOwner->player : INVALID_PLAYER_ID,
        .victimPlayer = damageVictimOwner
            ? damageVictimOwner->player : INVALID_PLAYER_ID,
        .sourceObjectPresent = static_cast<bool>(damageSourceEntity),
        .sourceIsEnemy = context.players && damageSourceOwner &&
            damageVictimOwner &&
            context.players->relationship(damageSourceOwner->player,
                                          damageVictimOwner->player) ==
                PlayerRelationship::Enemies,
        .victimPositionFixed = damageVictimPosition,
        .victimBoundingCircleRadiusFixed = damageVictimGeometry
            ? damageVictimGeometry->boundingCircleRadiusFixed
            : math::q32_32{},
        .victimBoundingSphereRadiusFixed = damageVictimGeometry
            ? damageVictimGeometry->boundingSphereRadiusFixed
            : math::q32_32{},
        .sourcePositionFixed = damageSourcePosition,
        .sourceBoundingSphereRadiusFixed = damageSourceGeometry
            ? damageSourceGeometry->boundingSphereRadiusFixed
            : math::q32_32{},
        .sourceAirborne = sourceAirborne,
        .victimDrone = hasKind(damageVictimKinds,
                               game::ObjectKindOf::Drone),
        .healthDecreased = eventKind == ObjectHealthEventKind::Damaged && applied > kHealthZero,
    });
    // RefCode dispatches DamageModuleInterface::onDamage after Body has
    // committed the new HP/body state but before Object::onDie walks Die
    // modules. Keep that transaction boundary explicit: a lethal reaction
    // weapon must be admitted before a later FireWeaponWhenDead behavior.
    reactions.dispatchDamage(events.back());
    if (health->damageState != previousState) {
        events.push_back({
            .kind = ObjectHealthEventKind::DamageStateChanged,
            .object = request.target,
            .source = request.source,
            .damageType = request.damageType,
            .damageFxType = request.damageFxOverride.value_or(request.damageType),
            .bodyLastDamageType = health->lastDamageType,
            .deathType = request.deathType,
            .requestedAmount = request.amount,
            .appliedAmountFixed = applied,
            .actualDamageDealtFixed =
                (eventKind == ObjectHealthEventKind::Damaged ||
                 eventKind == ObjectHealthEventKind::Healed)
                    ? resolvedUnclippedDamage : applied,
            .previousHealth = previous,
            .currentHealth = current,
            .previousState = previousState,
            .currentState = health->damageState,
            .confirmedTick = request.confirmedTick,
        });
        reactions.dispatchDamageStateChange(events.back());
    }
    // ActiveBody emits the damage-state one-shot after all DamageModule
    // callbacks. Keep the selected authored name on the primary health event
    // so session publication can preserve its ordering relative to VoiceFear
    // without querying a possibly retired ECS entity.
    if (damageVictimArchetype && health->damageState != previousState) {
        const game::ThingTemplate& templateData =
            damageVictimArchetype->templateData;
        if (health->damageState == ObjectBodyDamageState::Damaged) {
            events[primaryHealthEventIndex].damageStateAudioEventName =
                templateData.soundOnDamaged;
        } else if (health->damageState ==
                   ObjectBodyDamageState::ReallyDamaged) {
            events[primaryHealthEventIndex].damageStateAudioEventName =
                templateData.soundOnReallyDamaged;
        }
    }

    // ZH consumes GameLogicRandomValue(0, 99) only on the strict 25% health
    // crossing and plays VoiceFear for values 0..24. Consume the session RNG
    // even when VoiceFear is blank so missing optional content cannot perturb
    // later authoritative random decisions.
    const HealthScalar fearThreshold =
        health->maximumFixed * HealthScalar::from_fraction(1, 4);
    if (previous > fearThreshold && current < fearThreshold &&
        current > kHealthZero && context.random &&
        context.random->integerInclusive(0, 99) < 25 &&
        damageVictimArchetype) {
        events[primaryHealthEventIndex].voiceFearAudioEventName =
            damageVictimArchetype->templateData.voiceFear;
    }
    if (startSecondLife) {
        const HealthScalar secondLifeMaximum =
            HealthScalar::max(kHealthZero, health->secondLifeMaximumHealthFixed);
        const HealthScalar beforeSecondLife = health->currentFixed;
        const ObjectBodyDamageState stateBeforeSecondLife = health->damageState;
        // UndeadBody::startSecondLife calls setMaxHealth(..., FULLY_HEAL):
        // initial/max/current all become the new authored maximum.  It is a
        // body transition, not a destroy/recreate of the entity, so ObjectId,
        // ownership and render identity remain stable.
        health->previousFixed = beforeSecondLife;
        health->maximumFixed = secondLifeMaximum;
        health->initialFixed = secondLifeMaximum;
        health->currentFixed = secondLifeMaximum;
        health->secondLifeActive = true;
        health->effectivelyDead = secondLifeMaximum <= kHealthZero;
        // UndeadBody exposes ARMORSET_SECOND_LIFE at the same transition.
        // The resolved per-object armor sets are already ECS data, so this
        // needs no mutable ArmorStore lookup during the confirmed tick.
        if (ObjectCombatProfileComponent* combat =
                ecs::try_get<ObjectCombatProfileComponent>(registry, *entity)) {
            combat->armorConditions |= game::armorSetConditionBit(game::ArmorSetCondition::SecondLife);
            if (ObjectArmorComponent* armor =
                    ecs::try_get<ObjectArmorComponent>(registry, *entity)) {
                refreshResolvedObjectArmor(*combat, *armor);
            }
        }
        health->damageState = damageStateFor(health->currentFixed, health->maximumFixed, rules);
        events.push_back({
            .kind = ObjectHealthEventKind::SecondLifeStarted,
            .object = request.target,
            .source = request.source,
            .damageType = request.damageType,
            .damageFxType = request.damageFxOverride.value_or(request.damageType),
            .bodyLastDamageType = health->lastDamageType,
            .deathType = request.deathType,
            .requestedAmount = request.amount,
            .appliedAmountFixed = health->currentFixed - beforeSecondLife,
            .actualDamageDealtFixed = health->currentFixed - beforeSecondLife,
            .previousHealth = beforeSecondLife,
            .currentHealth = health->currentFixed,
            .previousState = stateBeforeSecondLife,
            .currentState = health->damageState,
            .confirmedTick = request.confirmedTick,
        });
        // UndeadBody starts its second life through setMaxHealth(FULLY_HEAL).
        // That direct Body mutation refreshes the visual state but does not
        // dispatch DamageModuleInterface::onBodyDamageStateChange or
        // TransitionDamageFX a second time. The fixed-frame visual projection
        // below observes the new state without inventing that callback.
    }
    if (previous > kHealthZero && health->currentFixed <= kHealthZero) {
        // ActiveBody awards score/experience before Object::onDie dispatches
        // any Die modules. Keep the callback at the same Body transition so a
        // death reaction cannot destroy, capture or otherwise hide the killer
        // before credit is committed.
        reactions.awardLethalExperience();
        requestDeath(simulation, registry, lifecycle, *entity, *health,
                     request, resolvedUnclippedDamage, applied, context,
                     sessionSeed, true, transactionResult);
    }
}

} // namespace object_simulation_detail

} // namespace engine
