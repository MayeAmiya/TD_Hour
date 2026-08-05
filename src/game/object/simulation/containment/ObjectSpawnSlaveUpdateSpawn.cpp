#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/object/simulation/containment/ObjectSpawnSlaveDetail.h"
#include "core/container/string_utils.h"

#include "game/base/DamageTypes.h"
#include "game/base/SimulationRandom.h"
#include "game/object/definition/CombatProfile.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/runtime/ObjectAIOpportunityTargetPolicy.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/lifecycle/ObjectDeleteWalk.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/data/base/UpgradeCatalog.h"
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

void updateSpawnAndHordeCandidate(
    UpdateContext& context, const container::Vector<Candidate>& objects,
    const Candidate& candidate) {
    auto& registry = context.registry;
    auto& lifecycle = context.lifecycle;
    const PlayerRegistry* players = context.players;
    const GameContentSnapshot* content = context.content;
    SimulationRandom* random = context.random;
    const ObjectSimulationRules& rules = context.rules;
    const uint64_t confirmedTick = context.confirmedTick;
    auto& spawnRequests = context.spawnRequests;
    auto& damageRequests = context.damageRequests;
    auto& veterancyRequests = context.veterancyRequests;
    auto& bodyHealthProjections = context.bodyHealthProjections;
    auto& destroyRequests = context.destroyRequests;

        ObjectSpawnSlaveComponent& component =
            ecs::get<ObjectSpawnSlaveComponent>(registry, candidate.entity);
        if (!component.plan) return;
        ObjectSpawnChildrenComponent* graph =
            ecs::try_get<ObjectSpawnChildrenComponent>(registry,
                                                        candidate.entity);
        if (graph) {
            std::sort(graph->children.begin(), graph->children.end());
            graph->children.erase(std::unique(graph->children.begin(),
                                              graph->children.end()),
                                  graph->children.end());
        }
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, candidate.entity);
        const PrimaryTeamComponent* team =
            ecs::try_get<PrimaryTeamComponent>(registry, candidate.entity);
        TransformComponent* transform =
            ecs::try_get<TransformComponent>(registry, candidate.entity);
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, candidate.entity);
        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(registry, candidate.entity);
        const bool underConstruction = status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction));
        const bool sold = status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::Sold));
        const bool reconstructing = status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::Reconstructing));
        const bool maySpawn = owner && !owner->player.isNeutral() && team &&
            team->team && transform && !underConstruction && !sold &&
            !lifecycle.isPendingDestroy(candidate.id);
        const uint64_t sparseInterval = std::max<uint64_t>(
            1, std::max(1u, rules.logicFramesPerSecond) / 2u);
        container::Vector<ObjectId> canonicalChildren;
        const auto saturatingDeadline = [](uint64_t now,
                                           uint64_t delay) noexcept {
            return delay > std::numeric_limits<uint64_t>::max() - now
                ? std::numeric_limits<uint64_t>::max() : now + delay;
        };
        const auto reconcileChildren = [&]<typename Rule>(
            ObjectSpawnRuntime& runtime, const Rule& rule,
            bool replaceDead) {
            container::Vector<ObjectId> live;
            live.reserve(runtime.children.size());
            for (const ObjectId child : runtime.children) {
                if (lifecycle.entityFromId(child) &&
                    !lifecycle.isPendingDestroy(child)) {
                    live.push_back(child);
                    canonicalChildren.push_back(child);
                    continue;
                }
                if (replaceDead) {
                    const uint64_t delay = ticks(
                        rule.replacementDelayMilliseconds,
                        rules.logicFramesPerSecond);
                    runtime.replacementReadyTicks.push_back(
                        saturatingDeadline(confirmedTick, delay));
                }
                ++runtime.revision;
            }
            runtime.children = std::move(live);
            runtime.selfTaskingChildren.erase(
                std::remove_if(
                    runtime.selfTaskingChildren.begin(),
                    runtime.selfTaskingChildren.end(),
                    [&runtime](ObjectId child) {
                        return std::find(runtime.children.begin(),
                                         runtime.children.end(), child) ==
                            runtime.children.end();
                    }),
                runtime.selfTaskingChildren.end());
            std::sort(runtime.replacementReadyTicks.begin(),
                      runtime.replacementReadyTicks.end());
        };
        const auto findReclaimableOrphan = [&](
            const game::ObjectSpawnRule& rule,
            const container::Vector<ObjectId>& alreadyReclaimed) -> ObjectId {
            if (!rule.canReclaimOrphans || rule.oneShot ||
                rule.templateNames.empty() || !owner || !transform) {
                return INVALID_OBJECT_ID;
            }
            struct OrphanCandidate final {
                ObjectId object = INVALID_OBJECT_ID;
                Fixed distance{};
            };
            std::optional<OrphanCandidate> best;
            const LogicFixedVec3 spawnerPosition =
                readAuthoritativeObjectPosition(
                    registry, candidate.entity, *transform);
            const auto orphanView = ecs::view<ObjectIdentityComponent,
                                             OwnerComponent,
                                             ThingTemplateComponent,
                                             TransformComponent>(registry);
            for (ecs::entity orphanEntity : orphanView) {
                const ObjectIdentityComponent& identity =
                    orphanView.template get<ObjectIdentityComponent>(
                        orphanEntity);
                if (!identity.id || identity.id == candidate.id ||
                    lifecycle.isPendingDestroy(identity.id)) {
                    continue;
                }
                if (std::find(alreadyReclaimed.begin(),
                              alreadyReclaimed.end(),
                              identity.id) != alreadyReclaimed.end()) {
                    continue;
                }
                if (const ObjectHealthComponent* health =
                        ecs::try_get<ObjectHealthComponent>(
                            registry, orphanEntity);
                    health && health->effectivelyDead) {
                    continue;
                }
                const OwnerComponent& orphanOwner =
                    orphanView.template get<OwnerComponent>(orphanEntity);
                if (orphanOwner.player != owner->player) continue;
                const ThingTemplateComponent& orphanType =
                    orphanView.template get<ThingTemplateComponent>(
                        orphanEntity);
                bool templateMatches = false;
                for (const container::String& templateName :
                     rule.templateNames) {
                    if (container::asciiEqualIgnoreCase(
                            orphanType.name, templateName)) {
                        templateMatches = true;
                        break;
                    }
                }
                if (!templateMatches) continue;
                if (const ObjectSpawnedByRuntimeComponent* spawnedBy =
                        ecs::try_get<ObjectSpawnedByRuntimeComponent>(
                            registry, orphanEntity);
                    spawnedBy && spawnedBy->master) {
                    continue;
                }
                if (const ObjectProducerComponent* producer =
                        ecs::try_get<ObjectProducerComponent>(
                            registry, orphanEntity);
                    producer && producer->producer) {
                    continue;
                }
                const TransformComponent& orphanTransform =
                    orphanView.template get<TransformComponent>(orphanEntity);
                const Fixed distance =
                    distanceSquared2D(
                        spawnerPosition,
                        readAuthoritativeObjectPosition(
                            registry, orphanEntity, orphanTransform));
                if (!best || distance < best->distance ||
                    (distance == best->distance &&
                     identity.id < best->object)) {
                    best = OrphanCandidate{
                        .object = identity.id,
                        .distance = distance,
                    };
                }
            }
            return best ? best->object : INVALID_OBJECT_ID;
        };
        const auto emitPending = [&](ObjectSpawnRuntime& runtime,
                                     const auto& rule, size_t ruleIndex,
                                     ObjectSpawnedByRuntimeComponent::Kind kind,
                                     bool scoreAsBuilt,
                                     bool containInSpawner,
                                     bool exitByBudding,
                                     uint32_t initialBurst) {
            uint32_t producerExitRequestsThisTick = 0;
            container::Vector<ObjectId> reclaimedThisTick;
            for (ObjectSpawnRuntime::PendingRequest& pending :
                 runtime.pendingRequests) {
                if (pending.lastAttemptTick == confirmedTick) continue;
                pending.lastAttemptTick = confirmedTick;
                ObjectId reclaimedObject = INVALID_OBJECT_ID;
                if constexpr (std::is_same_v<std::decay_t<decltype(rule)>,
                                             game::ObjectSpawnRule>) {
                    reclaimedObject =
                        findReclaimableOrphan(rule, reclaimedThisTick);
                    if (reclaimedObject) {
                        reclaimedThisTick.push_back(reclaimedObject);
                        spawnRequests.push_back({
                            .kind = kind,
                            .spawner = candidate.id,
                            .owner = owner->player,
                            .primaryTeam = team->team,
                            .templateName = pending.templateName,
                            .reclaimedObject = reclaimedObject,
                            .authoredOrder = rule.authoredOrder,
                            .emissionSequence = pending.emissionSequence,
                            .ruleIndex = static_cast<uint32_t>(ruleIndex),
                            .requestId = pending.requestId,
                            .submissionOrdinal =
                                reserveGameplaySubmissionOrdinal(context),
                            .scoreAsBuilt = false,
                            .containInSpawner = false,
                            .logicFramesPerSecond =
                                std::max(1u, rules.logicFramesPerSecond),
                            .confirmedTick = confirmedTick,
                        });
                        continue;
                    }
                }
                ObjectId exitHost = candidate.id;
                ecs::entity exitHostEntity = candidate.entity;
                const TransformComponent* exitHostTransform = transform;
                bool initialProducerExit = false;
                if (!containInSpawner && exitByBudding &&
                    runtime.successfulInitialProducerExitCount +
                            producerExitRequestsThisTick <
                        initialBurst) {
                    const ObjectProducerComponent* producer =
                        ecs::try_get<ObjectProducerComponent>(
                            registry, candidate.entity);
                    const std::optional<ecs::entity> producerEntity = producer
                        ? lifecycle.entityFromId(producer->producer)
                        : std::nullopt;
                    const ObjectProductionExitComponent* producerExit =
                        producerEntity
                        ? ecs::try_get<ObjectProductionExitComponent>(
                              registry, *producerEntity)
                        : nullptr;
                    const TransformComponent* producerTransform =
                        producerEntity
                        ? ecs::try_get<TransformComponent>(
                              registry, *producerEntity)
                        : nullptr;
                    const ObjectKindOfComponent* producerKinds = producerEntity
                        ? ecs::try_get<ObjectKindOfComponent>(
                              registry, *producerEntity)
                        : nullptr;
                    const bool producerIsStructure =
                        hasKind(producerKinds,
                                game::ObjectKindOf::Structure);
                    if (producer && producer->producer && producerIsStructure &&
                        producerExit && producerExit->plan &&
                        producerTransform) {
                        exitHost = producer->producer;
                        exitHostEntity = *producerEntity;
                        exitHostTransform = producerTransform;
                        initialProducerExit = true;
                    }
                }

                ObjectProductionExitReservation exitReservation;
                ObjectProductionSystem exitService;
                if (!containInSpawner) {
                    if (!content) continue;
                    std::optional<ObjectProductionExitReservation> reserved =
                        exitService.reserveExternalExit(
                            registry, lifecycle, *content, exitHost,
                            confirmedTick);
                    // A busy/destroyed Barracks is not fatal to the authored
                    // occurrence. RefCode falls back to budding from the
                    // nexus and does not consume InitialBurst in that case.
                    if (!reserved && initialProducerExit) {
                        exitHost = candidate.id;
                        exitHostEntity = candidate.entity;
                        exitHostTransform = transform;
                        initialProducerExit = false;
                        reserved = exitService.reserveExternalExit(
                            registry, lifecycle, *content, exitHost,
                            confirmedTick);
                    }
                    if (!reserved) continue;
                    exitReservation = *reserved;
                    if (initialProducerExit) {
                        ++producerExitRequestsThisTick;
                    }
                }

                LogicFixedVec3 spawnPosition =
                    readAuthoritativeObjectPosition(
                        registry, candidate.entity, *transform);
                math::q32_32 spawnYaw = readAuthoritativeObjectYaw(
                    registry, candidate.entity, *transform);
                LogicFixedVec3 exitTarget{};
                bool hasExitTarget = false;
                bool holdAfterSpawn = false;
                std::optional<uint32_t> initialPathfindLayer;
                const ObjectProductionExitComponent* exit =
                    ecs::try_get<ObjectProductionExitComponent>(
                        registry, exitHostEntity);
                const ObjectTerrainLayerComponent* exitLayer =
                    ecs::try_get<ObjectTerrainLayerComponent>(
                        registry, exitHostEntity);
                if (exitLayer) {
                    initialPathfindLayer = exitLayer->pathfindLayer;
                }

                if (exitByBudding && !initialProducerExit &&
                    !containInSpawner) {
                    // The original implementation measured from the nexus,
                    // not from the previous child. Use Q32.32 and ObjectId as
                    // the explicit stable tie-break for equal distances.
                    const LogicFixedVec3 nexusPosition =
                        readAuthoritativeObjectPosition(
                            registry, candidate.entity, *transform);
                    ObjectId budHost = INVALID_OBJECT_ID;
                    std::optional<math::q32_32> closestDistanceSquared;
                    const TransformComponent* budTransform = nullptr;
                    const ObjectTerrainLayerComponent* budLayer = nullptr;
                    for (const ObjectId child : runtime.children) {
                        const std::optional<ecs::entity> childEntity =
                            lifecycle.entityFromId(child);
                        if (!childEntity || lifecycle.isPendingDestroy(child)) {
                            continue;
                        }
                        const TransformComponent* childTransform =
                            ecs::try_get<TransformComponent>(
                                registry, *childEntity);
                        if (!childTransform) continue;
                        const LogicFixedVec3 childPosition =
                            readAuthoritativeObjectPosition(
                                registry, *childEntity, *childTransform);
                        const math::q32_32 dx =
                            childPosition.x - nexusPosition.x;
                        const math::q32_32 dy =
                            childPosition.y - nexusPosition.y;
                        const math::q32_32 distanceSquared =
                            dx * dx + dy * dy;
                        if (!closestDistanceSquared ||
                            distanceSquared < *closestDistanceSquared ||
                            (distanceSquared == *closestDistanceSquared &&
                             child < budHost)) {
                            closestDistanceSquared = distanceSquared;
                            budHost = child;
                            budTransform = childTransform;
                            budLayer = ecs::try_get<
                                ObjectTerrainLayerComponent>(
                                    registry, *childEntity);
                        }
                    }
                    const TransformComponent& sourceTransform = budTransform
                        ? *budTransform : *transform;
                    const ecs::entity sourceEntity = budTransform
                        ? *lifecycle.entityFromId(budHost)
                        : candidate.entity;
                    const LogicFixedVec3 sourcePosition =
                        readAuthoritativeObjectPosition(
                            registry, sourceEntity, sourceTransform);
                    const math::q32_32 sourceYaw =
                        readAuthoritativeObjectYaw(
                            registry, sourceEntity, sourceTransform);
                    spawnPosition = sourcePosition;
                    spawnYaw = sourceYaw;
                    if (budLayer) {
                        initialPathfindLayer = budLayer->pathfindLayer;
                    } else if (!budTransform) {
                        const ObjectTerrainLayerComponent* parentLayer =
                            ecs::try_get<ObjectTerrainLayerComponent>(
                                registry, candidate.entity);
                        if (parentLayer) {
                            initialPathfindLayer = parentLayer->pathfindLayer;
                        }
                    }
                    exitTarget = sourcePosition;
                    hasExitTarget = true;
                } else if (exit && exit->plan && exitHostTransform) {
                    const LogicFixedVec3 exitHostPosition =
                        readAuthoritativeObjectPosition(
                            registry, exitHostEntity, *exitHostTransform);
                    const math::q32_32 exitHostYaw =
                        readAuthoritativeObjectYaw(
                            registry, exitHostEntity, *exitHostTransform);
                    const bool reservedLocalTransform =
                        exitReservation.hasLocalTransform;
                    LogicFixedVec3 create = transformLocalPointFixed(
                        exitHostPosition, exitHostYaw,
                        reservedLocalTransform ? exitReservation.localX
                                               : exit->plan->unitCreatePointX,
                        reservedLocalTransform ? exitReservation.localY
                                               : exit->plan->unitCreatePointY,
                        reservedLocalTransform ? exitReservation.localZ
                                               : exit->plan->unitCreatePointZ);
                    if (reservedLocalTransform && exitLayer &&
                        context.terrain) {
                        create.z = math::q32_32::from_raw(
                            context.terrain->pathfindLayerHeightRawAt(
                                exitLayer->pathfindLayer,
                                create.x.raw(), create.y.raw())
                                .value_or(context.terrain->groundHeightRaw(
                                    create.x.raw(), create.y.raw())));
                    }
                    spawnPosition = create;
                    spawnYaw = exitHostYaw +
                        (reservedLocalTransform
                             ? exitReservation.localYaw
                             : math::q32_32{});
                    holdAfterSpawn = exitReservation.kind ==
                        game::ObjectProductionExitKind::SpawnPoint;
                    if (!holdAfterSpawn) {
                        exitTarget = transformLocalPointFixed(
                            exitHostPosition, exitHostYaw,
                            exit->plan->naturalRallyPointX,
                            exit->plan->naturalRallyPointY,
                            exit->plan->naturalRallyPointZ);
                        hasExitTarget = true;
                    }
                }
                spawnRequests.push_back({
                    .kind = kind,
                    .spawner = candidate.id,
                    .owner = owner->player,
                    .primaryTeam = team->team,
                    .templateName = pending.templateName,
                    .position = spawnPosition,
                    .yawRadians = spawnYaw,
                    .exitTarget = exitTarget,
                    .initialPathfindLayer = initialPathfindLayer,
                    .exitHost = exitHost,
                    .exitReservation = exitReservation,
                    .authoredOrder = rule.authoredOrder,
                    .emissionSequence = pending.emissionSequence,
                    .ruleIndex = static_cast<uint32_t>(ruleIndex),
                    .requestId = pending.requestId,
                    .submissionOrdinal =
                        reserveGameplaySubmissionOrdinal(context),
                    .scoreAsBuilt = scoreAsBuilt,
                    .hasExitTarget = hasExitTarget,
                    .holdAfterSpawn = holdAfterSpawn,
                    .exitByBudding = exitByBudding,
                    .usedInitialProducerExit = initialProducerExit,
                    .containInSpawner = containInSpawner,
                    .logicFramesPerSecond =
                        std::max(1u, rules.logicFramesPerSecond),
                    .confirmedTick = confirmedTick,
                });
            }
        };
        for (size_t i = 0; i < component.spawns.size(); ++i) {
            const game::ObjectSpawnRule& rule = component.plan->spawns[i];
            ObjectSpawnRuntime& runtime = component.spawns[i];
            if (!runtime.initialized) {
                runtime.replacementReadyTicks.assign(
                    rule.spawnNumber, uint64_t{0});
                if (const ObjectOrderQueueComponent* masterOrders =
                        ecs::try_get<ObjectOrderQueueComponent>(
                            registry, candidate.entity)) {
                    runtime.observedMasterExternalOrderRevision =
                        masterOrders->externalRevision;
                }
                runtime.observedMasterDisabledMask = objectDisabledMask(
                    registry, candidate.entity, confirmedTick);
                runtime.initialized = true;
                ++runtime.revision;
            }
            reconcileChildren(runtime, rule, !rule.oneShot);

            // RefCode's AIGroup forwards AttackObject/AttackPosition to a
            // SpawnBehavior roster unless SlavesHaveFreeWill is authored.
            // Idle/Stop is always forwarded.  Keep that command fan-out in
            // this relationship owner instead of teaching the generic AI
            // runtime about SpawnBehavior internals.
            const ObjectOrderQueueComponent* masterOrders =
                ecs::try_get<ObjectOrderQueueComponent>(registry,
                                                         candidate.entity);
            const ObjectOrderIntent* masterAttack =
                masterOrders && !masterOrders->orders.empty() &&
                        masterOrders->orders.front().kind ==
                            ObjectOrderKind::Attack &&
                        (masterOrders->orders.front().source !=
                             ObjectOrderSource::System ||
                         masterOrders->orders.front().systemPurpose ==
                             ObjectOrderSystemPurpose::Generic)
                    ? &masterOrders->orders.front()
                    : nullptr;
            if (masterAttack) {
                const bool fromAi =
                    masterAttack->source == ObjectOrderSource::System;
                if (runtime.lastAttackCommandWasAi != fromAi) {
                    runtime.lastAttackCommandWasAi = fromAi;
                    ++runtime.revision;
                }
            }
            const bool explicitStop = masterOrders &&
                masterOrders->externalRevision !=
                    runtime.observedMasterExternalOrderRevision &&
                masterOrders->orders.empty();
            if ((!rule.slavesHaveFreeWill && masterAttack) || explicitStop) {
                for (const ObjectId child : runtime.children) {
                    const std::optional<ecs::entity> childEntity =
                        lifecycle.entityFromId(child);
                    const ThingTemplateComponent* childType = childEntity
                        ? ecs::try_get<ThingTemplateComponent>(registry,
                                                               *childEntity)
                        : nullptr;
                    if (!childEntity || !childType || !childType->archetype ||
                        !childType->archetype->hasAiUpdate) {
                        continue;
                    }
                    ObjectOrderQueueComponent* childOrders =
                        ecs::try_get<ObjectOrderQueueComponent>(registry,
                                                                *childEntity);
                    if (!childOrders) {
                        childOrders = &ecs::emplace<ObjectOrderQueueComponent>(
                            registry, *childEntity);
                    }
                    if (explicitStop) {
                        childOrders->orders.clear();
                        // externalRevision is the cancellation signal for an
                        // already-admitted AI operation, so Stop must advance
                        // it even after that operation consumed its queue head.
                        ++childOrders->revision;
                        ++childOrders->externalRevision;
                        continue;
                    }

                    ObjectOrderIntent slaveAttack = *masterAttack;
                    slaveAttack.systemPurpose =
                        ObjectOrderSystemPurpose::Generic;
                    slaveAttack.systemPurposeInstance = rule.authoredOrder;
                    // The original object-target path deliberately calls
                    // aiForceAttackObject for slaves even when the master's
                    // own group order was an ordinary attack.
                    if (slaveAttack.targetObject)
                        slaveAttack.forceAttack = true;
                    const ObjectOrderIntent* current =
                        childOrders->orders.empty()
                            ? nullptr : &childOrders->orders.front();
                    const bool alreadySynchronized = current &&
                        current->kind == slaveAttack.kind &&
                        current->source == slaveAttack.source &&
                        current->issuedTick == slaveAttack.issuedTick &&
                        current->sourceSequence ==
                            slaveAttack.sourceSequence &&
                        current->targetObject == slaveAttack.targetObject &&
                        current->targetX == slaveAttack.targetX &&
                        current->targetY == slaveAttack.targetY &&
                        current->targetZ == slaveAttack.targetZ &&
                        current->hasTargetPosition ==
                            slaveAttack.hasTargetPosition &&
                        current->maximumShots == slaveAttack.maximumShots &&
                        current->forceAttack == slaveAttack.forceAttack &&
                        current->systemPurpose ==
                            ObjectOrderSystemPurpose::Generic &&
                        current->systemPurposeInstance == rule.authoredOrder;
                    if (alreadySynchronized) continue;
                    childOrders->orders.clear();
                    childOrders->orders.push_back(std::move(slaveAttack));
                    ++childOrders->revision;
                    if (masterAttack->source != ObjectOrderSource::System)
                        ++childOrders->externalRevision;
                }
            }
            if (masterOrders && masterOrders->externalRevision !=
                    runtime.observedMasterExternalOrderRevision) {
                runtime.observedMasterExternalOrderRevision =
                    masterOrders->externalRevision;
                ++runtime.revision;
            }

            // A disabled SPAWNS_ARE_THE_WEAPONS host disables its weapon
            // slaves for the same reason/deadline (the EMP stinger-site
            // patch in RefCode), then clears only reasons that the host just
            // cleared. Active reasons are also copied to newly spawned or
            // reclaimed children without erasing unrelated child-local ones.
            if (hasKind(kinds,
                        game::ObjectKindOf::SpawnsAreTheWeapons)) {
                const ObjectDisabledMask masterDisabled = objectDisabledMask(
                    registry, candidate.entity, confirmedTick);
                const ObjectDisabledMask newlyCleared =
                    runtime.observedMasterDisabledMask & ~masterDisabled;
                for (const ObjectId child : runtime.children) {
                    const std::optional<ecs::entity> childEntity =
                        lifecycle.entityFromId(child);
                    if (!childEntity) continue;
                    for (uint8_t value = 0;
                         value < static_cast<uint8_t>(
                                     ObjectDisabledReason::Count);
                         ++value) {
                        const ObjectDisabledReason reason =
                            static_cast<ObjectDisabledReason>(value);
                        if (reason == ObjectDisabledReason::Subdued) continue;
                        const ObjectDisabledMask bit = objectDisabledBit(reason);
                        if ((masterDisabled & bit) != 0) {
                            static_cast<void>(ObjectDisabledSystem::setUntil(
                                registry, *childEntity, reason,
                                objectDisabledUntil(registry,
                                                    candidate.entity,
                                                    reason),
                                confirmedTick));
                        } else if ((newlyCleared & bit) != 0) {
                            static_cast<void>(ObjectDisabledSystem::clear(
                                registry, *childEntity, reason,
                                confirmedTick));
                        }
                    }
                }
                if (runtime.observedMasterDisabledMask != masterDisabled) {
                    runtime.observedMasterDisabledMask = masterDisabled;
                    ++runtime.revision;
                }

                // StealthUpgrade delegates its SPAWNS_ARE_THE_WEAPONS grant
                // to SpawnBehavior in RefCode.  Project the authoritative
                // master CanStealth fact to every live occurrence child; the
                // StealthUpdate owner remains responsible for deciding when
                // those objects are actually allowed to become stealthed.
                const bool grantsStealth = status && status->hasAny(
                    game::objectStatusBit(
                        game::ObjectStatusFlag::CanStealth));
                if (grantsStealth) {
                    for (const ObjectId child : runtime.children) {
                        const std::optional<ecs::entity> childEntity =
                            lifecycle.entityFromId(child);
                        if (!childEntity) continue;
                        static_cast<void>(ObjectStatusSystem::apply(
                            registry, *childEntity,
                            {.setMask = game::objectStatusBit(
                                 game::ObjectStatusFlag::CanStealth),
                             .confirmedTick = confirmedTick}));
                    }
                }
            }
            if (reconstructing && rule.oneShot) {
                runtime.replacementReadyTicks.clear();
                runtime.pendingRequests.clear();
                runtime.oneShotCompleted = true;
                ++runtime.revision;
                continue;
            }
            if (!maySpawn || rule.templateNames.empty() ||
                runtime.oneShotCompleted ||
                confirmedTick < runtime.nextUpdateTick) continue;
            runtime.nextUpdateTick = saturatingDeadline(
                confirmedTick, sparseInterval);
            const size_t occupied = runtime.children.size() +
                runtime.pendingRequests.size();
            size_t available = occupied >= rule.spawnNumber
                ? 0 : rule.spawnNumber - occupied;
            while (available != 0 &&
                   !runtime.replacementReadyTicks.empty() &&
                   runtime.replacementReadyTicks.front() <= confirmedTick) {
                runtime.replacementReadyTicks.erase(
                    runtime.replacementReadyTicks.begin());
                uint64_t requestId = runtime.nextRequestId++;
                if (requestId == 0) requestId = runtime.nextRequestId++;
                const container::String& name = rule.templateNames[
                    runtime.nextTemplateIndex % rule.templateNames.size()];
                runtime.pendingRequests.push_back({
                    .requestId = requestId,
                    .templateName = name,
                    .emissionSequence = runtime.emissionSequence++,
                    .lastAttemptTick = std::numeric_limits<uint64_t>::max(),
                });
                ++runtime.nextTemplateIndex;
                --available;
                ++runtime.revision;
            }
            emitPending(runtime, rule, i,
                        ObjectSpawnedByRuntimeComponent::Kind::SpawnBehavior,
                        true, false, rule.exitByBudding,
                        rule.initialBurst);
        }
        for (size_t i = 0; i < component.mobNexus.size(); ++i) {
            const game::ObjectMobNexusRule& rule = component.plan->mobNexus[i];
            ObjectSpawnRuntime& runtime = component.mobNexus[i];
            if (!runtime.initialized) {
                runtime.replacementReadyTicks.assign(
                    std::min(rule.slots, rule.initialPayloadCount),
                    uint64_t{0});
                runtime.initialized = true;
                ++runtime.revision;
            }
            // MobNexus payload is a one-shot exact relation. Lost occupants
            // are removed from the graph but never repopulated here.
            struct NoReplacementRule final {
                uint32_t replacementDelayMilliseconds = 0;
            } noReplacement;
            reconcileChildren(runtime, noReplacement, false);
            if (!maySpawn || runtime.oneShotCompleted ||
                rule.initialPayloadTemplate.empty() ||
                confirmedTick < runtime.nextUpdateTick) continue;
            runtime.nextUpdateTick = saturatingDeadline(
                confirmedTick, sparseInterval);
            const size_t occupied = runtime.children.size() +
                runtime.pendingRequests.size();
            size_t available = occupied >= rule.slots
                ? 0 : rule.slots - occupied;
            while (available != 0 &&
                   !runtime.replacementReadyTicks.empty() &&
                   runtime.replacementReadyTicks.front() <= confirmedTick) {
                runtime.replacementReadyTicks.erase(
                    runtime.replacementReadyTicks.begin());
                uint64_t requestId = runtime.nextRequestId++;
                if (requestId == 0) requestId = runtime.nextRequestId++;
                runtime.pendingRequests.push_back({
                    .requestId = requestId,
                    .templateName = rule.initialPayloadTemplate,
                    .emissionSequence = runtime.emissionSequence++,
                    .lastAttemptTick = std::numeric_limits<uint64_t>::max(),
                });
                --available;
                ++runtime.revision;
            }
            emitPending(runtime, rule, i,
                        ObjectSpawnedByRuntimeComponent::Kind::MobNexus,
                        false, true, false, 0);
        }

        std::sort(canonicalChildren.begin(), canonicalChildren.end());
        canonicalChildren.erase(
            std::unique(canonicalChildren.begin(), canonicalChildren.end()),
            canonicalChildren.end());
        if (graph && graph->children != canonicalChildren) {
            graph->children = std::move(canonicalChildren);
            ++graph->revision;
        }

        bool hasAggregateHealth = false;
        bool aggregateOccurrenceLostAllChildren = false;
        uint32_t aggregateAuthoredOrder =
            std::numeric_limits<uint32_t>::max();
        ObjectHealthComponent::Scalar aggregateCurrent{};
        ObjectHealthComponent::Scalar aggregatePerfect{};
        game::ObjectVeterancyLevel highestVeterancy =
            game::ObjectVeterancyLevel::Regular;
        if (const ObjectVeterancyComponent* parentVeterancy =
                ecs::try_get<ObjectVeterancyComponent>(
                    registry, candidate.entity)) {
            highestVeterancy = parentVeterancy->level;
        }
        container::Vector<ObjectId> aggregateChildren;
        for (size_t i = 0; i < component.spawns.size(); ++i) {
            const game::ObjectSpawnRule& rule = component.plan->spawns[i];
            if (!rule.aggregateHealth) continue;
            hasAggregateHealth = true;
            aggregateAuthoredOrder = std::min(
                aggregateAuthoredOrder, rule.authoredOrder);
            const ObjectSpawnRuntime& runtime = component.spawns[i];
            ObjectHealthComponent::Scalar liveMaximum{};
            uint64_t liveCount = 0;
            for (const ObjectId child : runtime.children) {
                const std::optional<ecs::entity> childEntity =
                    lifecycle.entityFromId(child);
                const ObjectHealthComponent* health = childEntity
                    ? ecs::try_get<ObjectHealthComponent>(registry,
                                                           *childEntity)
                    : nullptr;
                if (!childEntity || !health || health->effectivelyDead)
                    continue;
                aggregateCurrent += health->currentFixed;
                liveMaximum += health->maximumFixed;
                ++liveCount;
                aggregateChildren.push_back(child);
                if (const ObjectVeterancyComponent* childVeterancy =
                        ecs::try_get<ObjectVeterancyComponent>(
                            registry, *childEntity)) {
                    if (static_cast<uint8_t>(childVeterancy->level) >
                        static_cast<uint8_t>(highestVeterancy)) {
                        highestVeterancy = childVeterancy->level;
                    }
                }
            }
            if (liveCount != 0) {
                aggregatePerfect +=
                    (liveMaximum /
                     ObjectHealthComponent::Scalar{static_cast<int32_t>(
                         std::min<uint64_t>(liveCount, INT32_MAX))}) *
                    ObjectHealthComponent::Scalar{static_cast<int32_t>(
                        std::min<uint32_t>(rule.spawnNumber, INT32_MAX))};
            } else if (runtime.successfulSpawnCount != 0) {
                aggregateOccurrenceLostAllChildren = true;
            }
        }
        if (hasAggregateHealth) {
            static_cast<void>(ObjectStatusSystem::apply(
                registry, candidate.entity,
                {.setMask = game::objectStatusBit(
                     game::ObjectStatusFlag::Masked),
                 .confirmedTick = confirmedTick}));
            const ObjectHealthComponent* parentHealth =
                ecs::try_get<ObjectHealthComponent>(registry,
                                                     candidate.entity);
            if (parentHealth) {
                ObjectHealthComponent::Scalar desired{};
                if (!aggregateOccurrenceLostAllChildren &&
                    aggregatePerfect > ObjectHealthComponent::Scalar{}) {
                    const ObjectHealthComponent::Scalar ratio =
                        ObjectHealthComponent::Scalar::min(
                            ObjectHealthComponent::Scalar{int32_t{1}},
                            ObjectHealthComponent::Scalar::max(
                                ObjectHealthComponent::Scalar{},
                                aggregateCurrent / aggregatePerfect));
                    desired = parentHealth->maximumFixed * ratio;
                }
                if (aggregateOccurrenceLostAllChildren) {
                    destroyRequests.push_back({
                        .object = candidate.id,
                        .reason = ObjectDestroyReason::System,
                        .source = candidate.id,
                        .authoredOrder = aggregateAuthoredOrder,
                        .submissionOrdinal =
                            reserveGameplaySubmissionOrdinal(context),
                        .confirmedTick = confirmedTick,
                    });
                } else {
                    bodyHealthProjections.push_back({
                        .object = candidate.id,
                        .source = candidate.id,
                        .desiredHealth = desired,
                        .authoredOrder = aggregateAuthoredOrder,
                        .confirmedTick = confirmedTick,
                    });
                }
            }
            if (const ObjectVeterancyComponent* parentVeterancy =
                    ecs::try_get<ObjectVeterancyComponent>(
                        registry, candidate.entity);
                parentVeterancy && parentVeterancy->level !=
                    highestVeterancy) {
                veterancyRequests.push_back({
                    .object = candidate.id,
                    .level = highestVeterancy,
                });
            }
            std::sort(aggregateChildren.begin(), aggregateChildren.end());
            aggregateChildren.erase(
                std::unique(aggregateChildren.begin(),
                            aggregateChildren.end()),
                aggregateChildren.end());
            for (const ObjectId child : aggregateChildren) {
                const std::optional<ecs::entity> childEntity =
                    lifecycle.entityFromId(child);
                const ObjectVeterancyComponent* childVeterancy = childEntity
                    ? ecs::try_get<ObjectVeterancyComponent>(registry,
                                                              *childEntity)
                    : nullptr;
                if (childVeterancy && childVeterancy->level !=
                        highestVeterancy) {
                    veterancyRequests.push_back({
                        .object = child,
                        .level = highestVeterancy,
                    });
                }
            }
        }

        const ObjectContainmentComponent* contents =
            ecs::try_get<ObjectContainmentComponent>(registry,
                                                      candidate.entity);
        for (const game::ObjectMobNexusRule& rule : component.plan->mobNexus) {
            if (!contents || rule.healthRegenPerSecond <= Fixed{}) continue;
            for (const ObjectContainedObjectRecord& member : contents->objects) {
                const std::optional<ecs::entity> memberEntity =
                    lifecycle.entityFromId(member.object);
                const ObjectHealthComponent* health = memberEntity
                    ? ecs::try_get<ObjectHealthComponent>(registry, *memberEntity)
                    : nullptr;
                if (!health || health->currentFixed >= health->maximumFixed) continue;
                damageRequests.push_back({
                    .target = member.object, .source = candidate.id,
                    // HEALING takes a POSITIVE amount: the resolver discards
                    // any HEALING request with amount <= 0, so the negated form
                    // used here made the authored regen a silent no-op.
                    .amount = health->maximumFixed *
                        rule.healthRegenPerSecond /
                        Fixed{static_cast<int32_t>(std::max(1u, rules.logicFramesPerSecond))},
                    .damageType = game::DamageType::HEALING,
                    .confirmedTick = confirmedTick,
                });
            }
        }

        const ThingTemplateComponent* selfType =
            ecs::try_get<ThingTemplateComponent>(registry, candidate.entity);
        for (size_t i = 0; i < component.hordes.size(); ++i) {
            const game::ObjectHordeRule& rule = component.plan->hordes[i];
            ObjectHordeRuntime& runtime = component.hordes[i];
            if (!transform || confirmedTick < runtime.nextUpdateTick) continue;
            const LogicFixedVec3 selfPosition =
                readAuthoritativeObjectPosition(
                    registry, candidate.entity, *transform);
            container::Vector<std::pair<ecs::entity, ObjectId>> hordeMembers;
            const auto hordeCandidates = ecs::view<
                ObjectIdentityComponent, TransformComponent>(registry);
            hordeMembers.reserve(hordeCandidates.size_hint());
            for (ecs::entity otherEntity : hordeCandidates) {
                const ObjectId otherId = hordeCandidates.template get<
                    ObjectIdentityComponent>(otherEntity).id;
                if (!otherId || !lifecycle.entityFromId(otherId) ||
                    !alive(registry, otherEntity)) continue;
                const TransformComponent* otherTransform =
                    ecs::try_get<TransformComponent>(registry, otherEntity);
                if (!otherTransform ||
                    distanceSquared(
                        selfPosition,
                        readAuthoritativeObjectPosition(
                            registry, otherEntity, *otherTransform)) >
                        rule.radius * rule.radius) {
                    continue;
                }
                const OwnerComponent* otherOwner =
                    ecs::try_get<OwnerComponent>(registry, otherEntity);
                if (rule.alliesOnly && owner) {
                    const bool allied = players
                        ? relationshipBetweenObjects(
                              registry, *players, candidate.entity,
                              otherEntity) == PlayerRelationship::Allies
                        : otherOwner && otherOwner->player == owner->player;
                    if (!allied) continue;
                }
                const ThingTemplateComponent* otherType =
                    ecs::try_get<ThingTemplateComponent>(registry, otherEntity);
                if (rule.exactMatch && selfType && otherType &&
                    selfType->name != otherType->name) continue;
                if (!hasAnyKind(ecs::try_get<ObjectKindOfComponent>(registry,
                                                                    otherEntity),
                                rule.kindOf)) continue;
                hordeMembers.emplace_back(otherEntity, otherId);
            }
            // The legacy query excludes the centre object from its count and
            // compares against Count-1. Our view includes it, yielding the
            // same authored Count threshold for normal Horde-capable units.
            const bool trueHordeMember =
                hordeMembers.size() >= static_cast<size_t>(rule.count);
            bool inHorde = trueHordeMember;
            if (!inHorde && rule.rubOffRadius > Fixed{}) {
                const Fixed rubOffRadiusSquared =
                    rule.rubOffRadius * rule.rubOffRadius;
                for (const auto& [otherEntity, otherId] : hordeMembers) {
                    if (otherId == candidate.id) continue;
                    const TransformComponent* otherTransform =
                        ecs::try_get<TransformComponent>(registry,
                                                         otherEntity);
                    if (!otherTransform ||
                        distanceSquared(
                            selfPosition,
                            readAuthoritativeObjectPosition(
                                registry, otherEntity, *otherTransform)) >
                            rubOffRadiusSquared) {
                        continue;
                    }
                    const ObjectSpawnSlaveComponent* otherComponent =
                        ecs::try_get<ObjectSpawnSlaveComponent>(registry,
                                                                otherEntity);
                    // Object::getHordeUpdateInterface returns the first
                    // authored HordeUpdate interface on the neighbour.
                    if (otherComponent && !otherComponent->hordes.empty() &&
                        otherComponent->hordes.front().trueHordeMember) {
                        inHorde = true;
                        break;
                    }
                }
            }
            const bool wasInHorde = runtime.inHorde;
            if (inHorde != runtime.inHorde ||
                trueHordeMember != runtime.trueHordeMember) {
                runtime.inHorde = inHorde;
                runtime.trueHordeMember = trueHordeMember;
                runtime.hasFlag = inHorde && !rule.flagSubObjectNames.empty();
                ++runtime.revision;
            }
            const bool moraleAction =
                rule.actionKind == game::ObjectHordeActionKind::Horde ||
                rule.actionKind == game::ObjectHordeActionKind::HordeFixed;
            const ObjectWeaponBonusComponent* existingBonus =
                ecs::try_get<ObjectWeaponBonusComponent>(
                    registry, candidate.entity);
            const bool demoralized = existingBonus &&
                (existingBonus->conditions & game::weaponBonusConditionBit(
                    game::WeaponBonusCondition::Demoralized)) != 0;
            const OwnerComponent* hordeOwner =
                ecs::try_get<OwnerComponent>(registry, candidate.entity);
            const UpgradeCatalog* upgradeCatalog =
                content ? content->upgradeCatalog() : nullptr;
            const bool hasNationalism = !demoralized &&
                rule.allowedNationalism &&
                players && hordeOwner && upgradeCatalog &&
                players->hasUpgradeComplete(
                    hordeOwner->player, well_known_upgrade::Nationalism,
                    *upgradeCatalog);
            const bool hasFanaticism = !demoralized &&
                rule.allowedNationalism && players && hordeOwner &&
                upgradeCatalog &&
                players->hasUpgradeComplete(
                    hordeOwner->player, well_known_upgrade::Fanaticism,
                    *upgradeCatalog);
            const bool fixedMorale =
                rule.actionKind == game::ObjectHordeActionKind::HordeFixed;
            const bool nationalism = hasNationalism &&
                (!fixedMorale || inHorde);
            const bool fanaticism = fixedMorale
                ? hasFanaticism && inHorde
                : hasFanaticism && hasNationalism;
            const auto setMorale = [&](game::WeaponBonusCondition condition,
                                       bool enabled) {
                static_cast<void>(setObjectWeaponBonusCondition(
                    registry, candidate.entity, condition, enabled,
                    content, random, rules.logicFramesPerSecond,
                    confirmedTick));
            };
            if (moraleAction) {
                setMorale(game::WeaponBonusCondition::Horde,
                          !demoralized && inHorde);
                setMorale(
                    game::WeaponBonusCondition::Nationalism, nationalism);
                // Stock Action=HORDE uses the classic dependency: Fanaticism
                // is evaluated only through the allowed Nationalism path.
                setMorale(
                    game::WeaponBonusCondition::Fanaticism, fanaticism);
            }
            const ObjectKindOfComponent* selfKinds =
                ecs::try_get<ObjectKindOfComponent>(
                    registry, candidate.entity);
            const bool portableStructure =
                hasKind(selfKinds,
                        game::ObjectKindOf::PortableStructure);
            if (portableStructure) {
                if (wasInHorde) {
                    setObjectTerrainDecalFade(
                        registry, candidate.entity, {},
                        math::q32_32::from_fraction(3, 100),
                        confirmedTick);
                }
            } else if (demoralized) {
                setObjectTerrainDecalKind(
                    registry, candidate.entity,
                    ObjectTerrainDecalKind::Demoralized,
                    confirmedTick, false);
            } else if (inHorde) {
                const bool infantry =
                    hasKind(selfKinds, game::ObjectKindOf::Infantry);
                ObjectTerrainDecalKind decalKind;
                if (fanaticism) {
                    decalKind = ObjectTerrainDecalKind::HordeFanaticism;
                } else if (nationalism) {
                    decalKind = infantry
                        ? ObjectTerrainDecalKind::HordeNationalismInfantry
                        : ObjectTerrainDecalKind::HordeNationalismVehicle;
                } else {
                    decalKind = infantry
                        ? ObjectTerrainDecalKind::HordeInfantry
                        : ObjectTerrainDecalKind::HordeVehicle;
                }
                setObjectTerrainDecalKind(
                    registry, candidate.entity, decalKind,
                    confirmedTick, true);
                if (!wasInHorde) {
                    setObjectTerrainDecalFade(
                        registry, candidate.entity, math::q32_32{int32_t{1}},
                        math::q32_32::from_fraction(3, 100),
                        confirmedTick);
                }
            } else if (wasInHorde) {
                setObjectTerrainDecalFade(
                    registry, candidate.entity, {},
                    math::q32_32::from_fraction(3, 100),
                    confirmedTick);
            }
            runtime.nextUpdateTick = confirmedTick +
                std::max<uint64_t>(1, ticks(rule.updateRateMilliseconds,
                                            rules.logicFramesPerSecond));
        }
}

} // namespace engine::object_spawn_slave_detail
