#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/object/simulation/containment/ObjectSpawnSlaveDetail.h"
#include "core/container/string_utils.h"

#include "game/base/DamageTypes.h"
#include "game/base/SimulationRandom.h"
#include "game/object/definition/CombatProfile.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/runtime/ObjectAIOpportunityTargetPolicy.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/terrain/TerrainLogic.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <type_traits>


namespace engine::object_spawn_slave_detail {

void updateSlavedCandidate(UpdateContext& context, const Candidate& candidate) {
    const ObjectSpawnSlaveSystem spawnSlaveSystem;
    auto& registry = context.registry;
    auto& lifecycle = context.lifecycle;
    const PlayerRegistry* players = context.players;
    const GameContentSnapshot* content = context.content;
    const ObjectSpatialIndex* spatialIndex = context.spatialIndex;
    SimulationRandom* random = context.random;
    const ObjectSimulationRules& rules = context.rules;
    const uint64_t confirmedTick = context.confirmedTick;
    auto& damageRequests = context.damageRequests;
    auto& defectionRequests = context.defectionRequests;
    auto& repairPresentationEvents = context.repairPresentationEvents;
    ObjectSpawnSlaveComponent& component =
        ecs::get<ObjectSpawnSlaveComponent>(registry, candidate.entity);
    const uint64_t sparseInterval = std::max<uint64_t>(
        1, std::max(1u, rules.logicFramesPerSecond) / 2u);

        const auto updateSlave = [&](ObjectSlaveRuntime& runtime,
                                     const game::ObjectMobMemberSlavedRule* mobRule,
                                     const game::ObjectSlavedRule* slaveRule) {
            if (!runtime.master) return;
            const ObjectId masterId = runtime.master;
            const std::optional<ecs::entity> masterEntity =
                lifecycle.entityFromIdIncludingPending(masterId);
            const bool masterUnavailable = !masterEntity ||
                lifecycle.isPendingDestroy(masterId) ||
                (masterEntity && !alive(registry, *masterEntity)) ||
                (slaveRule && masterEntity && isObjectDisabledBy(
                    registry, *masterEntity, ObjectDisabledReason::Unmanned,
                    confirmedTick));
            if (masterUnavailable) {
                if (mobRule) {
                    damageRequests.push_back({
                        .target = candidate.id,
                        .damageType = game::DamageType::UNRESISTABLE,
                        .deathType = game::DeathType::NORMAL,
                        .forceKill = true,
                        .confirmedTick = confirmedTick,
                    });
                } else if (runtime.requireMaster) {
                    static_cast<void>(lifecycle.requestDestroy(
                        candidate.id, ObjectDestroyReason::System, confirmedTick));
                } else {
                    static_cast<void>(ObjectDisabledSystem::setUntil(
                        registry, candidate.entity,
                        ObjectDisabledReason::Unmanned,
                        OBJECT_DISABLED_FOREVER_TICK, confirmedTick));
                    if (ObjectOrderQueueComponent* queue =
                            ecs::try_get<ObjectOrderQueueComponent>(
                                registry, candidate.entity)) {
                        queue->orders.clear();
                        ++queue->revision;
                    }
                    static_cast<void>(ObjectStatusSystem::apply(
                        registry, candidate.entity,
                        {.clearMask = game::objectStatusBit(
                             game::ObjectStatusFlag::Unselectable),
                         .confirmedTick = confirmedTick}));
                }
                runtime.master = INVALID_OBJECT_ID;
                runtime.returningToMaster = false;
                runtime.repairState = ObjectSlaveRepairState::None;
                runtime.repairPhaseDueTick = 0;
                runtime.slavedEffectsApplied = false;
                ++runtime.revision;
                return;
            }

            const ObjectFixedTransformComponent* masterTransform =
                ecs::try_get<ObjectFixedTransformComponent>(
                    registry, *masterEntity);
            const ObjectFixedTransformComponent* slaveTransform =
                ecs::try_get<ObjectFixedTransformComponent>(
                    registry, candidate.entity);
            if (!masterTransform || !masterTransform->authoritative ||
                !slaveTransform || !slaveTransform->authoritative) return;
            const LogicFixedVec3 masterPosition = masterTransform->position;
            const LogicFixedVec3 slavePosition = slaveTransform->position;

            const auto selectLocomotorSet = [&](game::LocomotorSetSlot slot) {
                ObjectLocomotionComponent* locomotion =
                    ecs::try_get<ObjectLocomotionComponent>(
                        registry, candidate.entity);
                const ThingTemplateComponent* type =
                    ecs::try_get<ThingTemplateComponent>(
                        registry, candidate.entity);
                if (!locomotion || !type || !type->archetype || !content)
                    return false;
                return applyObjectLocomotorSet(
                    *locomotion, type->archetype->templateData,
                    *content, slot);
            };
            const auto setRepairModelCondition =
                [&](game::ModelConditionFlag selected) {
                    static const game::ModelConditionMask repairOwned =
                        game::modelConditionMaskOf(
                            game::ModelConditionFlag::Packing,
                            game::ModelConditionFlag::Unpacking,
                            game::ModelConditionFlag::FiringB,
                            game::ModelConditionFlag::FiringC,
                            game::ModelConditionFlag::BetweenFiringShotsB,
                            game::ModelConditionFlag::BetweenFiringShotsC,
                            game::ModelConditionFlag::ReloadingB,
                            game::ModelConditionFlag::ReloadingC);
                    publishObjectModelConditionContribution(
                        registry, candidate.entity,
                        ObjectModelConditionContributionSource::Containment,
                        repairOwned,
                        game::modelConditionMaskOf(selected),
                        confirmedTick);
                };

            const uint32_t authoredOrder = slaveRule
                ? slaveRule->authoredOrder : mobRule->authoredOrder;
            if (slaveRule) {
                const OwnerComponent* masterOwner =
                    ecs::try_get<OwnerComponent>(registry, *masterEntity);
                const OwnerComponent* slaveOwner =
                    ecs::try_get<OwnerComponent>(registry,
                                                  candidate.entity);
                const bool allied = masterOwner && slaveOwner &&
                    (players
                        ? players->relationship(masterOwner->player,
                                                slaveOwner->player) ==
                              PlayerRelationship::Allies
                        : masterOwner->player == slaveOwner->player);
                if (masterOwner && masterOwner->player && slaveOwner &&
                    slaveOwner->player != masterOwner->player && !allied) {
                    // SlavedUpdate::update calls Object::defect(masterTeam)
                    // when a captured/hijacked master is no longer allied to
                    // its slave. Ownership remains a session transaction;
                    // this module publishes only stable IDs and the desired
                    // controlling player.
                    defectionRequests.push_back({
                        .source = masterId,
                        .target = candidate.id,
                        .newOwner = masterOwner->player,
                        .authoredOrder = authoredOrder,
                        .submissionOrdinal =
                            reserveGameplaySubmissionOrdinal(context),
                        .confirmedTick = confirmedTick,
                    });
                }
            }
            const auto isOwnedOrder = [authoredOrder](
                                          const ObjectOrderIntent& order) {
                return order.source == ObjectOrderSource::System &&
                    order.systemPurpose ==
                        ObjectOrderSystemPurpose::SlaveReturn &&
                    order.systemPurposeInstance == authoredOrder;
            };
            const auto orderQueue = [&]() -> ObjectOrderQueueComponent& {
                if (ObjectOrderQueueComponent* existing =
                        ecs::try_get<ObjectOrderQueueComponent>(
                            registry, candidate.entity)) {
                    return *existing;
                }
                return ecs::emplace<ObjectOrderQueueComponent>(
                    registry, candidate.entity);
            };
            const auto issueMove = [&](const LogicFixedVec3& goal) {
                ObjectOrderQueueComponent& queue = orderQueue();
                if (!queue.orders.empty() &&
                    isOwnedOrder(queue.orders.front()) &&
                    queue.orders.front().hasTargetPosition &&
                    queue.orders.front().targetX == goal.x &&
                    queue.orders.front().targetY == goal.y &&
                    queue.orders.front().targetZ == goal.z) {
                    return;
                }
                queue.orders.clear();
                queue.orders.push_back({
                    .kind = ObjectOrderKind::Move,
                    .source = ObjectOrderSource::System,
                    .issuedTick = confirmedTick,
                    .sourceSequence = runtime.nextCommandSequence++,
                    .targetX = goal.x,
                    .targetY = goal.y,
                    .targetZ = goal.z,
                    .hasTargetPosition = true,
                    .systemPurpose = ObjectOrderSystemPurpose::SlaveReturn,
                    .systemPurposeInstance = authoredOrder,
                });
                ++queue.revision;
                runtime.returningToMaster = true;
                ++runtime.revision;
            };
            const auto issueAttack = [&](ObjectId target) {
                if (!target) return;
                ObjectOrderQueueComponent& queue = orderQueue();
                if (!queue.orders.empty() &&
                    isOwnedOrder(queue.orders.front()) &&
                    queue.orders.front().kind == ObjectOrderKind::Attack &&
                    queue.orders.front().targetObject == target) {
                    return;
                }
                queue.orders.clear();
                queue.orders.push_back({
                    .kind = ObjectOrderKind::Attack,
                    .source = ObjectOrderSource::System,
                    .issuedTick = confirmedTick,
                    .sourceSequence = runtime.nextCommandSequence++,
                    .targetObject = target,
                    .maximumShots = 999u,
                    .systemPurpose =
                        ObjectOrderSystemPurpose::SlaveReturn,
                    .systemPurposeInstance = authoredOrder,
                });
                ++queue.revision;
                runtime.returningToMaster = false;
                ++runtime.revision;
            };
            const auto clearOwnedOrder = [&](bool includeAttack = false) {
                ObjectOrderQueueComponent* queue =
                    ecs::try_get<ObjectOrderQueueComponent>(registry,
                                                             candidate.entity);
                if (!queue || queue->orders.empty() ||
                    !isOwnedOrder(queue->orders.front())) return;
                if (!includeAttack && queue->orders.front().kind !=
                        ObjectOrderKind::Move) return;
                queue->orders.erase(queue->orders.begin());
                ++queue->revision;
                runtime.returningToMaster = false;
                ++runtime.revision;
            };
            const auto sampleOffset = [&](uint32_t radius) {
                const Fixed fixedRadius{static_cast<int32_t>(radius)};
                const Fixed fullTurn = Fixed::from_raw(26'986'075'409ll);
                const Fixed fixedAngle = random
                    ? random->fixedInclusive(Fixed{}, fullTurn)
                    : Fixed{};
                return LogicFixedVec3{
                    fixedRadius * math::fixed_cos(fixedAngle),
                    fixedRadius * math::fixed_sin(fixedAngle), Fixed{}};
            };
            const auto boundedGoal = [&](const LogicFixedVec3& desiredPosition,
                                         uint32_t maximumRange) {
                const Fixed dx = desiredPosition.x - masterPosition.x;
                const Fixed dy = desiredPosition.y - masterPosition.y;
                const Fixed lengthSquared = dx * dx + dy * dy;
                const Fixed limit{static_cast<int32_t>(maximumRange)};
                if (maximumRange == 0 || lengthSquared <= limit * limit)
                    return desiredPosition;
                const Fixed length = Fixed::sqrt(lengthSquared);
                if (length <= Fixed{}) return masterPosition;
                return LogicFixedVec3{
                    masterPosition.x + dx * limit / length,
                    masterPosition.y + dy * limit / length,
                    desiredPosition.z};
            };

            if (slaveRule && !runtime.slavedEffectsApplied) {
                static_cast<void>(ObjectStatusSystem::apply(
                    registry, candidate.entity,
                    {.setMask = game::objectStatusBit(
                         game::ObjectStatusFlag::Unselectable),
                     .confirmedTick = confirmedTick}));
                const ObjectStatusComponent* masterStatus =
                    ecs::try_get<ObjectStatusComponent>(registry,
                                                         *masterEntity);
                if (masterStatus && masterStatus->hasAny(
                        game::objectStatusBit(
                            game::ObjectStatusFlag::Stealthed))) {
                    static_cast<void>(ObjectStatusSystem::apply(
                        registry, candidate.entity,
                        {.setMask = game::objectStatusBit(
                             game::ObjectStatusFlag::CanStealth),
                         .confirmedTick = confirmedTick}));
                }
                runtime.slavedEffectsApplied = true;
                if (slaveRule->repairRatePerSecond > Fixed{} &&
                    runtime.repairState ==
                        ObjectSlaveRepairState::None) {
                    setRepairModelCondition(game::ModelConditionFlag::Packing);
                }
                ++runtime.revision;
            }

            if (slaveRule && slaveRule->stayOnSameLayerAsMaster) {
                const ObjectTerrainLayerComponent* masterLayer =
                    ecs::try_get<ObjectTerrainLayerComponent>(registry,
                                                               *masterEntity);
                const uint32_t desiredLayer = masterLayer
                    ? masterLayer->pathfindLayer : 0u;
                ObjectTerrainLayerComponent* layer =
                    ecs::try_get<ObjectTerrainLayerComponent>(
                        registry, candidate.entity);
                if (!layer) {
                    ecs::emplace<ObjectTerrainLayerComponent>(
                        registry, candidate.entity,
                        ObjectTerrainLayerComponent{
                            .pathfindLayer = desiredLayer,
                            .lastChangedTick = confirmedTick});
                } else {
                    static_cast<void>(layer->assign(desiredLayer,
                                                    confirmedTick));
                }
            }

            const ObjectOrderQueueComponent* masterOrders =
                ecs::try_get<ObjectOrderQueueComponent>(registry,
                                                         *masterEntity);
            const ObjectOrderIntent* masterOrder =
                masterOrders && !masterOrders->orders.empty()
                ? &masterOrders->orders.front() : nullptr;
            ObjectId masterTarget = INVALID_OBJECT_ID;
            if (const ObjectWeaponComponent* masterWeapons =
                    ecs::try_get<ObjectWeaponComponent>(registry,
                                                         *masterEntity)) {
                masterTarget = masterWeapons->target;
            }
            if (!masterTarget && masterOrder &&
                masterOrder->kind == ObjectOrderKind::Attack) {
                masterTarget = masterOrder->targetObject;
            }
            const std::optional<ecs::entity> targetEntity = masterTarget
                ? lifecycle.entityFromId(masterTarget) : std::nullopt;
            const ObjectFixedTransformComponent* targetTransform = targetEntity
                ? ecs::try_get<ObjectFixedTransformComponent>(
                      registry, *targetEntity)
                : nullptr;
            if (!targetTransform || !targetTransform->authoritative)
                masterTarget = INVALID_OBJECT_ID;
            if (mobRule) {
                // Weapon-A suppression for upgraded Mob members is composed
                // by ObjectModelConditionAuthority after the normal WeaponSet
                // producer.  Writing RenderModelComponent here bypassed dirty
                // tracking and was immediately vulnerable to being restored
                // by the end-of-tick authority pass.
                // Retail evaluates this low-priority controller every 16
                // legacy 30 Hz frames; CatchUpCrisisBailTime is explicitly
                // counted in calls to update, not logic frames.
                if (!runtime.decisionClockInitialized) {
                    const int32_t initialCounter = random
                        ? random->integerInclusive(0, 20) : 0;
                    const uint32_t legacyFramesToFirstDecision =
                        initialCounter >= 15
                        ? 0u : static_cast<uint32_t>(15 - initialCounter);
                    runtime.nextDecisionTick = confirmedTick +
                        legacyFramesAtSessionRate(
                            legacyFramesToFirstDecision,
                            rules.logicFramesPerSecond);
                    runtime.decisionClockInitialized = true;
                    ++runtime.revision;
                }
                if (confirmedTick < runtime.nextDecisionTick) return;
                const uint64_t interval = std::max<uint64_t>(
                    1u, legacyFramesAtSessionRate(
                        16u, rules.logicFramesPerSecond));
                runtime.nextDecisionTick = confirmedTick + interval;

                if (masterTarget &&
                    runtime.primaryVictim != masterTarget) {
                    runtime.primaryVictim = masterTarget;
                    ++runtime.revision;
                }

                const auto setSelfTasking = [&](bool enabled) {
                    if (runtime.selfTasking == enabled) return;
                    const ObjectSpawnedByRuntimeComponent* link =
                        ecs::try_get<ObjectSpawnedByRuntimeComponent>(
                            registry, candidate.entity);
                    if (link && link->master == masterId &&
                        link->kind ==
                            ObjectSpawnedByRuntimeComponent::Kind::
                                SpawnBehavior) {
                        static_cast<void>(spawnSlaveSystem.setSpawnChildSelfTasking(
                            registry, lifecycle, masterId,
                            link->ruleIndex, candidate.id, enabled));
                    }
                    runtime.selfTasking = enabled;
                    ++runtime.revision;
                };
                ObjectLocomotionComponent* slaveLocomotion =
                    ecs::try_get<ObjectLocomotionComponent>(
                        registry, candidate.entity);
                const ObjectLocomotionComponent* masterLocomotion =
                    ecs::try_get<ObjectLocomotionComponent>(
                        registry, *masterEntity);

                const Fixed must{
                    static_cast<int32_t>(mobRule->mustCatchUpRadius)};
                const Fixed masterDistance =
                    distanceSquared(slavePosition, masterPosition);
                if (masterDistance > must * must) {
                    LogicFixedVec3 catchUp = masterPosition;
                    if (masterOrder && masterOrder->kind ==
                            ObjectOrderKind::Move &&
                        masterOrder->hasTargetPosition) {
                        catchUp = {
                            masterOrder->targetX,
                            masterOrder->targetY,
                            masterOrder->targetZ};
                    }
                    if (masterOrder && masterOrder->kind ==
                            ObjectOrderKind::Move) {
                        const auto fixedDistanceToGoal = [](
                                const ObjectLocomotionComponent* locomotion,
                                const LogicFixedVec3& position) {
                            if (!locomotion) return Fixed{};
                            const Fixed dx = locomotion->goal.x - position.x;
                            const Fixed dy = locomotion->goal.y - position.y;
                            return Fixed::sqrt(dx * dx + dy * dy);
                        };
                        const Fixed masterPathDistance = fixedDistanceToGoal(
                            masterLocomotion, masterPosition);
                        const Fixed slavePathDistance = fixedDistanceToGoal(
                            slaveLocomotion, slavePosition);
                        static_cast<void>(selectLocomotorSet(
                            masterPathDistance > slavePathDistance
                                ? game::LocomotorSetSlot::Wander
                                : game::LocomotorSetSlot::Panic));
                    } else {
                        static_cast<void>(selectLocomotorSet(
                            game::LocomotorSetSlot::Panic));
                    }
                    const Fixed critical = must * Fixed{int32_t{3}};
                    if (masterDistance > critical * critical) {
                        if (runtime.outsideCatchUpFrames !=
                                std::numeric_limits<uint32_t>::max()) {
                            ++runtime.outsideCatchUpFrames;
                        }
                        if (runtime.outsideCatchUpFrames >
                                mobRule->catchUpCrisisBailFrames) {
                            damageRequests.push_back({
                                .target = candidate.id,
                                .damageType = game::DamageType::UNRESISTABLE,
                                .deathType = game::DeathType::NORMAL,
                                .forceKill = true,
                                .confirmedTick = confirmedTick,
                            });
                            ++runtime.revision;
                            return;
                        }
                        if (runtime.outsideCatchUpFrames >
                                mobRule->catchUpCrisisBailFrames / 3u) {
                            catchUp = masterPosition;
                        }
                    }
                    issueMove(catchUp);
                    return;
                } else {
                    if (runtime.outsideCatchUpFrames != 0) {
                        runtime.outsideCatchUpFrames = 0;
                        ++runtime.revision;
                    }
                    // Preserve a propagated/master attack, but release only
                    // this module's catch-up Move once the member reunites.
                    // The idle branch below may then run the shared mood
                    // ranker without erasing an existing victim.
                    clearOwnedOrder();
                }

                ObjectOrderQueueComponent* slaveOrders =
                    ecs::try_get<ObjectOrderQueueComponent>(
                        registry, candidate.entity);
                const ObjectOrderIntent* slaveOrder = slaveOrders &&
                        !slaveOrders->orders.empty()
                    ? &slaveOrders->orders.front() : nullptr;
                if ((slaveLocomotion && slaveLocomotion->state !=
                         ObjectLocomotionState::Idle) ||
                    (slaveOrder && slaveOrder->kind ==
                         ObjectOrderKind::Move)) {
                    const int32_t seed = random
                        ? random->integerInclusive(0, 10) : 0;
                    if (seed == 1) {
                        static_cast<void>(selectLocomotorSet(
                            game::LocomotorSetSlot::Wander));
                    } else if (seed == 2) {
                        static_cast<void>(selectLocomotorSet(
                            game::LocomotorSetSlot::Panic));
                    } else if (seed == 3) {
                        static_cast<void>(selectLocomotorSet(
                            game::LocomotorSetSlot::Normal));
                    }
                    return;
                }

                const bool masterIsMoving =
                    (masterLocomotion && masterLocomotion->state !=
                        ObjectLocomotionState::Idle) ||
                    (masterOrder && masterOrder->kind ==
                        ObjectOrderKind::Move);
                const bool masterIdle = !masterTarget &&
                    !masterIsMoving &&
                    (!masterOrder || masterOrder->kind !=
                        ObjectOrderKind::Attack);
                if (masterIdle) {
                    clearOwnedOrder(true);
                    runtime.primaryVictim = INVALID_OBJECT_ID;
                    setSelfTasking(false);
                    return;
                }

                ObjectId currentVictim = slaveOrder &&
                        slaveOrder->kind == ObjectOrderKind::Attack
                    ? slaveOrder->targetObject : INVALID_OBJECT_ID;
                if (currentVictim &&
                    !lifecycle.entityFromId(currentVictim)) {
                    currentVictim = INVALID_OBJECT_ID;
                    clearOwnedOrder(true);
                }
                const ObjectSpawnedByRuntimeComponent* link =
                    ecs::try_get<ObjectSpawnedByRuntimeComponent>(
                        registry, candidate.entity);
                const bool maySelfTask = link &&
                    link->master == masterId &&
                    link->kind == ObjectSpawnedByRuntimeComponent::Kind::
                        SpawnBehavior &&
                    spawnSlaveSystem.maySpawnSelfTaskAI(
                        registry, lifecycle, masterId, link->ruleIndex,
                        mobRule->squirrelliness);
                if (maySelfTask && players && spatialIndex) {
                    const OwnerComponent* selfOwner =
                        ecs::try_get<OwnerComponent>(registry,
                                                     candidate.entity);
                    container::Vector<ObjectAIOpportunityTargetCandidate>
                        moodCandidates;
                    int64_t maximumAcquisitionDistanceRaw = 0;
                    if (content) {
                        const ObjectCombatProfileComponent* combat =
                            ecs::try_get<ObjectCombatProfileComponent>(
                                registry, candidate.entity);
                        const game::WeaponSetProfile* set = combat &&
                                combat->profile
                            ? combat->profile->findBestWeaponSet(
                                  combat->weaponConditions)
                            : nullptr;
                        if (set) {
                            for (const game::WeaponSlotProfile& slot :
                                 set->slots) {
                                const game::WeaponTemplate* weapon =
                                    content->findWeapon(
                                        slot.weaponTemplateName);
                                if (!weapon) continue;
                                maximumAcquisitionDistanceRaw = std::max(
                                    maximumAcquisitionDistanceRaw,
                                    math::q32_32::max(
                                        math::q32_32{},
                                        weapon->fixed.attackRange).raw());
                            }
                        }
                    }
                    for (const ObjectSpatialRecord& record :
                         spatialIndex->records()) {
                        if (maximumAcquisitionDistanceRaw <= 0) break;
                        if (!record.object ||
                            record.object == candidate.id) continue;
                        const std::optional<ecs::entity> target =
                            lifecycle.entityFromId(record.object);
                        if (!target) continue;
                        const OwnerComponent* targetOwner =
                            ecs::try_get<OwnerComponent>(registry, *target);
                        const ObjectHealthComponent* targetHealth =
                            ecs::try_get<ObjectHealthComponent>(registry,
                                                                *target);
                        const ObjectStatusComponent* targetStatus =
                            ecs::try_get<ObjectStatusComponent>(registry,
                                                                *target);
                        const ObjectContainedByComponent* contained =
                            ecs::try_get<ObjectContainedByComponent>(
                                registry, *target);
                        const ObjectKindOfComponent* targetKinds =
                            ecs::try_get<ObjectKindOfComponent>(
                                registry, *target);
                        if (!selfOwner || !targetOwner) continue;
                        const PlayerRelationship relation =
                            players->relationship(
                                selfOwner->player, targetOwner->player);
                        const bool stealthed = targetStatus &&
                            targetStatus->hasAny(game::objectStatusBit(
                                game::ObjectStatusFlag::Stealthed));
                        const bool detected = targetStatus &&
                            targetStatus->hasAny(game::objectStatusBit(
                                game::ObjectStatusFlag::Detected));
                        const bool disguised = targetStatus &&
                            targetStatus->hasAny(game::objectStatusBit(
                                game::ObjectStatusFlag::Disguised));
                        const ObjectFixedTransformComponent* targetTransform =
                            ecs::try_get<ObjectFixedTransformComponent>(
                                registry, *target);
                        if (!targetTransform ||
                            !targetTransform->authoritative) continue;
                        const LogicFixedVec3 targetPosition =
                            targetTransform->position;
                        moodCandidates.push_back({
                            .target = record.object,
                            .position = {
                                .xRaw = targetPosition.x.raw(),
                                .yRaw = targetPosition.y.raw(),
                                .zRaw = targetPosition.z.raw(),
                            },
                            .attackable = targetHealth &&
                                targetHealth->acceptsDamage,
                            .unattackable = targetKinds &&
                                game::objectHasKind(
                                    targetKinds->mask,
                                    game::ObjectKindOf::Unattackable),
                            .effectivelyDead = targetHealth &&
                                targetHealth->effectivelyDead,
                            .containedPassenger = contained &&
                                contained->enclosing,
                            .hiddenStealth = stealthed && !detected &&
                                !disguised,
                            .sameOwner = selfOwner->player ==
                                targetOwner->player,
                            .allied = relation ==
                                PlayerRelationship::Allies,
                            .enemy = relation ==
                                PlayerRelationship::Enemies,
                            .maximumAcquisitionDistanceRaw =
                                maximumAcquisitionDistanceRaw,
                        });
                    }
                    const ObjectId moodTarget =
                        selectObjectAIOpportunityTarget(
                            ai::AIOpportunityAttackMoveQueryCommandKind::
                                FindMoodTarget,
                            {.xRaw = slavePosition.x.raw(),
                             .yRaw = slavePosition.y.raw(),
                             .zRaw = slavePosition.z.raw()},
                            moodCandidates);
                    if (moodTarget && moodTarget != currentVictim) {
                        issueAttack(moodTarget);
                        currentVictim = moodTarget;
                        setSelfTasking(true);
                    }
                }
                if (!currentVictim) {
                    if (runtime.primaryVictim &&
                        lifecycle.entityFromId(runtime.primaryVictim)) {
                        issueAttack(runtime.primaryVictim);
                    }
                    setSelfTasking(false);
                }
                return;
            }

            // SlavedUpdate contributes DroneSpotting independently of its
            // sparse movement decision. This keeps the bonus exact while a
            // target or drone crosses the authored threshold between updates.
            if (slaveRule->distanceToTargetForRangeBonus != 0 &&
                masterTarget && targetTransform) {
                const Fixed range{static_cast<int32_t>(
                    slaveRule->distanceToTargetForRangeBonus)};
                if (distanceSquared2D(
                        slavePosition,
                        targetTransform->position) <
                        range * range) {
                    ObjectSlaveRangeBonusSourcesComponent* sources =
                        ecs::try_get<ObjectSlaveRangeBonusSourcesComponent>(
                            registry, *masterEntity);
                    if (!sources) {
                        sources = &ecs::emplace<
                            ObjectSlaveRangeBonusSourcesComponent>(
                                registry, *masterEntity);
                    }
                    sources->sources.push_back(candidate.id);
                }
            }

            const ObjectHealthComponent* masterHealth =
                ecs::try_get<ObjectHealthComponent>(registry, *masterEntity);
            const bool canRepair = slaveRule->repairRatePerSecond > Fixed{} &&
                masterHealth && masterHealth->maximumFixed > Fixed{};
            const bool masterDamaged = canRepair &&
                masterHealth->currentFixed < masterHealth->maximumFixed;
            const Fixed healthPercent = canRepair
                ? masterHealth->currentFixed * Fixed{int32_t{100}} /
                    masterHealth->maximumFixed
                : Fixed{int32_t{100}};
            const bool priorityRepair = masterDamaged &&
                healthPercent <= Fixed{static_cast<int32_t>(
                    slaveRule->repairWhenBelowHealthPercent)};
            const uint64_t sparseInterval = std::max<uint64_t>(
                1u, legacyFramesAtSessionRate(
                    7u, rules.logicFramesPerSecond));
            const bool repairActive =
                runtime.repairState != ObjectSlaveRepairState::None;
            if (!repairActive && confirmedTick < runtime.nextDecisionTick)
                return;
            if (!repairActive)
                runtime.nextDecisionTick = confirmedTick + sparseInterval;

            const auto randomDuration = [&](uint32_t minimumMilliseconds,
                                            uint32_t maximumMilliseconds) {
                const uint32_t low = std::min(minimumMilliseconds,
                                              maximumMilliseconds);
                const uint32_t high = std::max(minimumMilliseconds,
                                               maximumMilliseconds);
                const uint32_t maximumRandomDuration =
                    static_cast<uint32_t>(
                        std::numeric_limits<int32_t>::max());
                const int32_t lowSample = static_cast<int32_t>(
                    std::min(low, maximumRandomDuration));
                const int32_t highSample = static_cast<int32_t>(
                    std::min(high, maximumRandomDuration));
                const uint32_t sampled = random
                    ? static_cast<uint32_t>(random->integerInclusive(
                          lowSample, highSample))
                    : low;
                return std::max<uint64_t>(1u, ticks(
                    sampled, rules.logicFramesPerSecond));
            };
            const auto finishRepair = [&]() {
                ObjectLocomotionComponent* locomotion =
                    ecs::try_get<ObjectLocomotionComponent>(
                        registry, candidate.entity);
                const bool resetLocomotor =
                    runtime.repairState != ObjectSlaveRepairState::None ||
                    (locomotion && (locomotion->ultraAccurate ||
                                    locomotion->usePreciseZPosition));
                if (runtime.repairState != ObjectSlaveRepairState::None) {
                    runtime.repairState = ObjectSlaveRepairState::None;
                    runtime.repairPhaseDueTick = 0;
                    runtime.repairDestinationValid = false;
                    runtime.nextDecisionTick = confirmedTick + sparseInterval;
                    setRepairModelCondition(game::ModelConditionFlag::Packing);
                    ++runtime.revision;
                }
                if (resetLocomotor) {
                    static_cast<void>(selectLocomotorSet(
                        game::LocomotorSetSlot::Normal));
                }
                if (locomotion) {
                    locomotion->ultraAccurate = false;
                    locomotion->usePreciseZPosition = false;
                }
            };
            const auto projectRepairTransition =
                [&](ObjectSlaveRepairState before) {
                    if (before == runtime.repairState) return;
                    switch (runtime.repairState) {
                    case ObjectSlaveRepairState::Unpacking:
                        setRepairModelCondition(game::ModelConditionFlag::Unpacking);
                        break;
                    case ObjectSlaveRepairState::Extending:
                        setRepairModelCondition(game::ModelConditionFlag::FiringB);
                        break;
                    case ObjectSlaveRepairState::Welding:
                        repairPresentationEvents.push_back({
                            .object = candidate.id,
                            .master = masterId,
                            .particleSystem =
                                slaveRule->repairWeldingSystem,
                            .boneName = slaveRule->repairWeldingFxBone,
                            .lifetimeTicks = runtime.repairPhaseDueTick >
                                    confirmedTick
                                ? std::min<uint64_t>(
                                      std::numeric_limits<uint64_t>::max() /
                                          std::max<uint32_t>(
                                              1u,
                                              rules.logicFramesPerSecond),
                                      runtime.repairPhaseDueTick -
                                          confirmedTick) *
                                      std::max<uint32_t>(
                                          1u,
                                          rules.logicFramesPerSecond)
                                : 0u,
                            .authoredOrder = slaveRule->authoredOrder,
                            .confirmedTick = confirmedTick,
                        });
                        break;
                    case ObjectSlaveRepairState::Retracting:
                        setRepairModelCondition(game::ModelConditionFlag::FiringC);
                        static_cast<void>(selectLocomotorSet(
                            game::LocomotorSetSlot::Panic));
                        if (ObjectLocomotionComponent* locomotion =
                                ecs::try_get<ObjectLocomotionComponent>(
                                    registry, candidate.entity)) {
                            locomotion->ultraAccurate = true;
                            locomotion->usePreciseZPosition = true;
                        }
                        break;
                    case ObjectSlaveRepairState::None:
                    case ObjectSlaveRepairState::Ready:
                        break;
                    }
                };
            const auto repair = [&]() {
                const ObjectSlaveRepairState before = runtime.repairState;
                const Fixed closeEnough{int32_t{12}};
                const bool close = distanceSquared2D(
                    slavePosition, masterPosition) <
                    closeEnough * closeEnough;
                if (!close) {
                    if (!runtime.repairDestinationValid) {
                        const Fixed minimum = Fixed::min(
                            slaveRule->repairMinAltitude,
                            slaveRule->repairMaxAltitude);
                        const Fixed maximum = Fixed::max(
                            slaveRule->repairMinAltitude,
                            slaveRule->repairMaxAltitude);
                        const Fixed altitude = random
                            ? random->fixedInclusive(minimum, maximum)
                            : minimum;
                        runtime.repairDestination = masterPosition;
                        runtime.repairDestination.z += altitude;
                        runtime.repairDestinationValid = true;
                    }
                    issueMove(runtime.repairDestination);
                    if (runtime.repairState ==
                            ObjectSlaveRepairState::None) {
                        runtime.repairState =
                            ObjectSlaveRepairState::Unpacking;
                        runtime.repairPhaseDueTick = confirmedTick +
                            std::max<uint64_t>(1u,
                                legacyFramesAtSessionRate(
                                    15u, rules.logicFramesPerSecond));
                        ++runtime.revision;
                    }
                    const Fixed preciseRadius = [&]() {
                        const ObjectGeometryComponent* geometry =
                            ecs::try_get<ObjectGeometryComponent>(
                                registry, *masterEntity);
                        return geometry
                            ? Fixed::max(
                                  Fixed{},
                                  geometry->boundingSphereRadiusFixed *
                                      Fixed{int32_t{2}})
                            : Fixed{};
                    }();
                    if (ObjectLocomotionComponent* locomotion =
                            ecs::try_get<ObjectLocomotionComponent>(
                                registry, candidate.entity)) {
                        locomotion->usePreciseZPosition =
                            preciseRadius > Fixed{} &&
                            distanceSquared2D(
                                slavePosition, masterPosition) <
                                preciseRadius * preciseRadius;
                    }
                    projectRepairTransition(before);
                    return;
                }

                runtime.repairDestinationValid = false;
                switch (runtime.repairState) {
                case ObjectSlaveRepairState::None:
                    runtime.repairState = ObjectSlaveRepairState::Unpacking;
                    runtime.repairPhaseDueTick = confirmedTick +
                        std::max<uint64_t>(1u,
                            legacyFramesAtSessionRate(
                                15u, rules.logicFramesPerSecond));
                    ++runtime.revision;
                    break;
                case ObjectSlaveRepairState::Unpacking:
                    if (confirmedTick >= runtime.repairPhaseDueTick) {
                        runtime.repairState = ObjectSlaveRepairState::Ready;
                        runtime.repairPhaseDueTick = confirmedTick +
                            randomDuration(
                                slaveRule->repairMinReadyMilliseconds,
                                slaveRule->repairMaxReadyMilliseconds);
                        ++runtime.revision;
                    }
                    break;
                case ObjectSlaveRepairState::Ready:
                    if (confirmedTick >= runtime.repairPhaseDueTick) {
                        runtime.repairState =
                            ObjectSlaveRepairState::Extending;
                        runtime.repairPhaseDueTick = confirmedTick +
                            std::max<uint64_t>(1u,
                                legacyFramesAtSessionRate(
                                    5u, rules.logicFramesPerSecond));
                        ++runtime.revision;
                    }
                    break;
                case ObjectSlaveRepairState::Extending:
                    if (confirmedTick >= runtime.repairPhaseDueTick) {
                        runtime.repairState = ObjectSlaveRepairState::Welding;
                        runtime.repairPhaseDueTick = confirmedTick +
                            randomDuration(
                                slaveRule->repairMinWeldMilliseconds,
                                slaveRule->repairMaxWeldMilliseconds);
                        ++runtime.revision;
                    }
                    break;
                case ObjectSlaveRepairState::Welding:
                    damageRequests.push_back({
                        .target = masterId,
                        .source = candidate.id,
                        // HEALING takes a POSITIVE amount: the resolver
                        // discards any HEALING request with amount <= 0, so the
                        // negated form used here ran the whole welding state
                        // machine and its FX without ever repairing the master.
                        .amount = slaveRule->repairRatePerSecond /
                            Fixed{static_cast<int32_t>(std::max(
                                1u, rules.logicFramesPerSecond))},
                        .damageType = game::DamageType::HEALING,
                        .confirmedTick = confirmedTick,
                    });
                    if (confirmedTick >= runtime.repairPhaseDueTick) {
                        runtime.repairState =
                            ObjectSlaveRepairState::Retracting;
                        runtime.repairPhaseDueTick = confirmedTick +
                            std::max<uint64_t>(1u,
                                legacyFramesAtSessionRate(
                                    5u, rules.logicFramesPerSecond));
                        if (slaveRule->repairRange != 0) {
                            runtime.repairDestination = masterPosition;
                            const LogicFixedVec3 offset = sampleOffset(
                                slaveRule->repairRange);
                            runtime.repairDestination.x += offset.x;
                            runtime.repairDestination.y += offset.y;
                            const Fixed minimum = Fixed::min(
                                slaveRule->repairMinAltitude,
                                slaveRule->repairMaxAltitude);
                            const Fixed maximum = Fixed::max(
                                slaveRule->repairMinAltitude,
                                slaveRule->repairMaxAltitude);
                            runtime.repairDestination.z += random
                                ? random->fixedInclusive(minimum, maximum)
                                : minimum;
                            runtime.repairDestinationValid = true;
                            issueMove(runtime.repairDestination);
                        }
                        ++runtime.revision;
                    }
                    break;
                case ObjectSlaveRepairState::Retracting:
                    if (confirmedTick >= runtime.repairPhaseDueTick) {
                        runtime.repairState = ObjectSlaveRepairState::Ready;
                        runtime.repairPhaseDueTick = confirmedTick +
                            randomDuration(
                                slaveRule->repairMinReadyMilliseconds,
                                slaveRule->repairMaxReadyMilliseconds);
                        ++runtime.revision;
                    }
                    break;
                }
                projectRepairTransition(before);
            };

            if (priorityRepair) {
                repair();
                return;
            }
            if (slaveRule->attackRange != 0 && masterTarget &&
                targetTransform) {
                finishRepair();
                LogicFixedVec3 goal = boundedGoal(
                    targetTransform->position,
                    slaveRule->attackRange);
                if (slaveRule->attackWanderRange != 0) {
                    const LogicFixedVec3 offset = sampleOffset(
                        slaveRule->attackWanderRange);
                    goal.x += offset.x;
                    goal.y += offset.y;
                }
                issueMove(goal);
                return;
            }
            if (slaveRule->scoutRange != 0 && masterOrder &&
                masterOrder->kind == ObjectOrderKind::Move &&
                masterOrder->hasTargetPosition) {
                const LogicFixedVec3 destination{
                    masterOrder->targetX,
                    masterOrder->targetY,
                    masterOrder->targetZ};
                const Fixed halfGuard = Fixed{static_cast<int32_t>(
                    slaveRule->guardMaxRange)} / Fixed{int32_t{2}};
                if (distanceSquared2D(masterPosition, destination) >
                        halfGuard * halfGuard) {
                    finishRepair();
                    LogicFixedVec3 goal = boundedGoal(
                        destination, slaveRule->scoutRange);
                    if (slaveRule->scoutWanderRange != 0) {
                        const LogicFixedVec3 offset = sampleOffset(
                            slaveRule->scoutWanderRange);
                        goal.x += offset.x;
                        goal.y += offset.y;
                    }
                    issueMove(goal);
                    return;
                }
            }
            if (masterDamaged) {
                repair();
                return;
            }

            finishRepair();
            if (slaveRule->guardMaxRange == 0) {
                clearOwnedOrder();
                return;
            }
            if (!runtime.guardOffsetInitialized) {
                runtime.guardOffset = sampleOffset(
                    slaveRule->guardWanderRange);
                runtime.guardOffsetInitialized = true;
                ++runtime.revision;
            }
            LogicFixedVec3 pinned = masterPosition;
            pinned.x += runtime.guardOffset.x;
            pinned.y += runtime.guardOffset.y;
            const Fixed closeEnough{int32_t{15}};
            const Fixed maximum{static_cast<int32_t>(
                slaveRule->guardMaxRange)};
            const Fixed stray = maximum * Fixed{int32_t{2}};
            ObjectOrderQueueComponent* queue =
                ecs::try_get<ObjectOrderQueueComponent>(registry,
                                                         candidate.entity);
            const bool idle = !queue || queue->orders.empty() ||
                (!queue->orders.empty() &&
                 isOwnedOrder(queue->orders.front()));
            if ((idle && distanceSquared(slavePosition, pinned) >
                    closeEnough * closeEnough) ||
                distanceSquared(slavePosition, masterPosition) >
                    stray * stray) {
                issueMove(pinned);
            } else if (distanceSquared(slavePosition, pinned) <=
                           closeEnough * closeEnough) {
                clearOwnedOrder();
            }
        };
        for (size_t i = 0; i < component.slaved.size(); ++i)
            updateSlave(component.slaved[i], nullptr,
                        &component.plan->slaved[i]);
        for (size_t i = 0; i < component.mobMemberSlaved.size(); ++i)
            updateSlave(component.mobMemberSlaved[i],
                        &component.plan->mobMemberSlaved[i], nullptr);
}

} // namespace engine::object_spawn_slave_detail
