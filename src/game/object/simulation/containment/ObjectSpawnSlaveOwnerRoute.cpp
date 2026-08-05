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

void reconcileOwnership(UpdateContext& context) {
    auto& registry = context.registry;
    auto& lifecycle = context.lifecycle;
    const uint64_t confirmedTick = context.confirmedTick;
    auto& damageRequests = context.damageRequests;

    const auto spawnedView = ecs::view<ObjectIdentityComponent,
                                       ObjectSpawnedByRuntimeComponent>(registry);
    for (ecs::entity entity : spawnedView) {
        const ObjectId object =
            spawnedView.template get<ObjectIdentityComponent>(entity).id;
        ObjectSpawnedByRuntimeComponent& link =
            spawnedView.template get<ObjectSpawnedByRuntimeComponent>(entity);
        if (!object || !link.master) continue;
        const ObjectId oldMaster = link.master;
        const bool masterGone = !lifecycle.entityFromId(oldMaster) ||
            lifecycle.isPendingDestroy(oldMaster);
        if (masterGone) {
            if (link.requireMaster) {
                static_cast<void>(lifecycle.requestDestroy(
                    object, ObjectDestroyReason::System, confirmedTick));
                continue;
            }

            // SpawnBehavior::onDie always clears the child's producer before
            // an ordinary (non-RequireSpawner) child becomes an orphan.  A
            // stale producer/master edge would make CanReclaimOrphans reject
            // that child forever.
            if (ObjectProducerComponent* producer =
                    ecs::try_get<ObjectProducerComponent>(registry, entity);
                producer && producer->producer == oldMaster) {
                producer->producer = INVALID_OBJECT_ID;
            }
            if (ObjectSpawnSlaveComponent* childComponent =
                    ecs::try_get<ObjectSpawnSlaveComponent>(registry, entity)) {
                // MobMemberSlavedUpdate unconditionally dies with its nexus;
                // SlavedUpdate instead becomes an unmanned free body so its
                // Physics/SlowDeath owner can perform the crash.
                if (!childComponent->mobMemberSlaved.empty()) {
                    damageRequests.push_back({
                        .target = object,
                        .damageType = game::DamageType::UNRESISTABLE,
                        .deathType = game::DeathType::NORMAL,
                        .forceKill = true,
                        .confirmedTick = confirmedTick,
                    });
                } else if (!childComponent->slaved.empty()) {
                    static_cast<void>(ObjectDisabledSystem::setUntil(
                        registry, entity, ObjectDisabledReason::Unmanned,
                        std::numeric_limits<uint64_t>::max(), confirmedTick));
                    if (ObjectOrderQueueComponent* orders =
                            ecs::try_get<ObjectOrderQueueComponent>(registry,
                                                                    entity)) {
                        orders->orders.clear();
                        ++orders->revision;
                    }
                    static_cast<void>(ObjectStatusSystem::apply(
                        registry, entity,
                        {.clearMask = game::objectStatusBit(
                             game::ObjectStatusFlag::Unselectable),
                         .confirmedTick = confirmedTick}));
                }
                for (ObjectSlaveRuntime& runtime : childComponent->slaved) {
                    if (runtime.master != oldMaster) continue;
                    runtime.master = INVALID_OBJECT_ID;
                    runtime.requireMaster = false;
                    runtime.returningToMaster = false;
                    runtime.repairState = ObjectSlaveRepairState::None;
                    runtime.repairPhaseDueTick = 0;
                    runtime.slavedEffectsApplied = false;
                    ++runtime.revision;
                }
                for (ObjectSlaveRuntime& runtime :
                     childComponent->mobMemberSlaved) {
                    if (runtime.master != oldMaster) continue;
                    runtime.master = INVALID_OBJECT_ID;
                    runtime.requireMaster = false;
                    runtime.returningToMaster = false;
                    runtime.slavedEffectsApplied = false;
                    ++runtime.revision;
                }
            }
            link.master = INVALID_OBJECT_ID;
            link.requireMaster = false;
            ++link.revision;
            continue;
        }
        ObjectSpawnSlaveComponent* childComponent =
            ecs::try_get<ObjectSpawnSlaveComponent>(registry, entity);
        if (childComponent) {
            for (ObjectSlaveRuntime& runtime : childComponent->slaved) {
                runtime.master = link.master;
                runtime.requireMaster = link.requireMaster;
            }
            for (ObjectSlaveRuntime& runtime :
                 childComponent->mobMemberSlaved) {
                runtime.master = link.master;
                runtime.requireMaster = link.requireMaster;
            }
        }
    }
}

} // namespace engine::object_spawn_slave_detail

namespace engine {

bool ObjectSpawnSlaveSystem::onSpawnerDie(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId spawner, uint32_t authoredOrder, uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& outDamage) const {
    const std::optional<ecs::entity> spawnerEntity =
        lifecycle.entityFromIdIncludingPending(spawner);
    ObjectSpawnSlaveComponent* component = spawnerEntity
        ? ecs::try_get<ObjectSpawnSlaveComponent>(registry, *spawnerEntity)
        : nullptr;
    if (!component || !component->plan) return false;
    const auto foundRule = std::find_if(
        component->plan->spawns.begin(), component->plan->spawns.end(),
        [authoredOrder](const game::ObjectSpawnRule& rule) {
            return rule.authoredOrder == authoredOrder;
        });
    if (foundRule == component->plan->spawns.end()) return false;
    const size_t ruleIndex = static_cast<size_t>(
        std::distance(component->plan->spawns.begin(), foundRule));
    if (ruleIndex >= component->spawns.size()) return false;

    const ObjectSpawnRuntime& runtime = component->spawns[ruleIndex];
    uint32_t killSequence = 1;
    for (const ObjectId child : runtime.children) {
        const std::optional<ecs::entity> childEntity =
            lifecycle.entityFromIdIncludingPending(child);
        if (!childEntity) continue;
        if (ObjectProducerComponent* producer =
                ecs::try_get<ObjectProducerComponent>(registry, *childEntity);
            producer && producer->producer == spawner) {
            producer->producer = INVALID_OBJECT_ID;
        }
        if (ObjectSpawnSlaveComponent* childComponent =
                ecs::try_get<ObjectSpawnSlaveComponent>(
                    registry, *childEntity)) {
            uint32_t firstAuthoredOrder = std::numeric_limits<uint32_t>::max();
            ObjectSlaveRuntime* firstRuntime = nullptr;
            if (childComponent->plan) {
                for (size_t index = 0;
                     index < childComponent->plan->slaved.size() &&
                     index < childComponent->slaved.size(); ++index) {
                    const uint32_t order =
                        childComponent->plan->slaved[index].authoredOrder;
                    if (order < firstAuthoredOrder) {
                        firstAuthoredOrder = order;
                        firstRuntime = &childComponent->slaved[index];
                    }
                }
                for (size_t index = 0;
                     index < childComponent->plan->mobMemberSlaved.size() &&
                     index < childComponent->mobMemberSlaved.size(); ++index) {
                    const uint32_t order = childComponent->plan
                        ->mobMemberSlaved[index].authoredOrder;
                    if (order < firstAuthoredOrder) {
                        firstAuthoredOrder = order;
                        firstRuntime = &childComponent->mobMemberSlaved[index];
                    }
                }
            }
            if (firstRuntime && firstRuntime->master == spawner) {
                firstRuntime->slavedEffectsApplied = false;
                ++firstRuntime->revision;
            }
        }
        if (!foundRule->spawnedRequireSpawner) continue;
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, *childEntity);
        if (health && health->effectivelyDead) continue;
        outDamage.push_back({
            .target = child,
            .source = spawner,
            .sourceSequence = killSequence,
            .causalGroup = spawner,
            .damageType = game::DamageType::UNRESISTABLE,
            .deathType = game::DeathType::NORMAL,
            .forceKill = true,
            .confirmedTick = confirmedTick,
        });
        if (killSequence != std::numeric_limits<uint32_t>::max()) {
            ++killSequence;
        }
    }
    return true;
}

void ObjectSpawnSlaveSystem::onSpawnerDelete(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    ObjectId spawner, uint32_t authoredOrder, uint64_t confirmedTick,
    container::Vector<ObjectDeleteDestroyRequest>& outDestroy) const {
    const std::optional<ecs::entity> spawnerEntity =
        lifecycle.entityFromIdIncludingPending(spawner);
    const ObjectSpawnSlaveComponent* component = spawnerEntity
        ? ecs::try_get<ObjectSpawnSlaveComponent>(registry, *spawnerEntity)
        : nullptr;
    if (!component || !component->plan) return;
    const size_t count = std::min(component->plan->spawns.size(),
                                  component->spawns.size());
    for (size_t index = 0; index < count; ++index) {
        const game::ObjectSpawnRule& rule = component->plan->spawns[index];
        if (rule.authoredOrder != authoredOrder ||
            !rule.spawnedRequireSpawner) continue;
        uint32_t localOrdinal = 0;
        for (const ObjectId child : component->spawns[index].children) {
            if (!child || !lifecycle.entityFromIdIncludingPending(child)) {
                continue;
            }
            outDestroy.push_back({
                .object = child,
                .reason = ObjectDestroyReason::System,
                .source = spawner,
                .authoredOrder = authoredOrder,
                .localOrdinal = localOrdinal++,
                .confirmedTick = confirmedTick,
            });
        }
        return;
    }
}

void ObjectSpawnSlaveSystem::onSpawnedObjectDie(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSimulationRules& rules, ObjectId child,
    uint64_t confirmedTick) const {
    const std::optional<ecs::entity> childEntity =
        lifecycle.entityFromIdIncludingPending(child);
    ObjectSpawnedByRuntimeComponent* spawnedBy = childEntity
        ? ecs::try_get<ObjectSpawnedByRuntimeComponent>(registry,
                                                        *childEntity)
        : nullptr;
    if (!spawnedBy || !spawnedBy->master ||
        spawnedBy->kind !=
            ObjectSpawnedByRuntimeComponent::Kind::SpawnBehavior) {
        return;
    }
    const ObjectId spawner = spawnedBy->master;
    const uint32_t ruleIndex = spawnedBy->ruleIndex;
    const std::optional<ecs::entity> spawnerEntity =
        lifecycle.entityFromIdIncludingPending(spawner);
    ObjectSpawnSlaveComponent* component = spawnerEntity
        ? ecs::try_get<ObjectSpawnSlaveComponent>(registry, *spawnerEntity)
        : nullptr;
    if (!component || !component->plan ||
        ruleIndex >= component->plan->spawns.size() ||
        ruleIndex >= component->spawns.size()) {
        spawnedBy->master = INVALID_OBJECT_ID;
        ++spawnedBy->revision;
        return;
    }

    ObjectSpawnRuntime& runtime = component->spawns[ruleIndex];
    const auto found = std::find(
        runtime.children.begin(), runtime.children.end(), child);
    if (found == runtime.children.end()) return;
    runtime.children.erase(found);
    runtime.selfTaskingChildren.erase(
        std::remove(runtime.selfTaskingChildren.begin(),
                    runtime.selfTaskingChildren.end(), child),
        runtime.selfTaskingChildren.end());
    const game::ObjectSpawnRule& rule = component->plan->spawns[ruleIndex];
    if (!rule.oneShot) {
        const uint64_t delay = object_spawn_slave_detail::ticks(
            rule.replacementDelayMilliseconds,
            rules.logicFramesPerSecond);
        const uint64_t due = delay >
                std::numeric_limits<uint64_t>::max() - confirmedTick
            ? std::numeric_limits<uint64_t>::max()
            : confirmedTick + delay;
        runtime.replacementReadyTicks.insert(
            std::upper_bound(runtime.replacementReadyTicks.begin(),
                             runtime.replacementReadyTicks.end(), due),
            due);
    }
    ++runtime.revision;
    spawnedBy->master = INVALID_OBJECT_ID;
    ++spawnedBy->revision;

    if (ObjectSpawnChildrenComponent* graph = spawnerEntity
            ? ecs::try_get<ObjectSpawnChildrenComponent>(registry,
                                                          *spawnerEntity)
            : nullptr) {
        const auto graphChild = std::lower_bound(
            graph->children.begin(), graph->children.end(), child);
        if (graphChild != graph->children.end() && *graphChild == child) {
            graph->children.erase(graphChild);
            ++graph->revision;
        }
    }
}

bool ObjectSpawnSlaveSystem::bindOclSlaveMaster(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, ObjectId master) const noexcept {
    if (!object || !master || lifecycle.isPendingDestroy(object)) return false;
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    if (!entity) return false;
    ObjectSpawnSlaveComponent* component =
        ecs::try_get<ObjectSpawnSlaveComponent>(registry, *entity);
    if (!component || !component->plan) return true;

    bool bound = false;
    for (ObjectSlaveRuntime& runtime : component->slaved) {
        if (runtime.master == master && !runtime.requireMaster) continue;
        runtime.master = master;
        runtime.requireMaster = false;
        runtime.guardOffsetInitialized = false;
        runtime.repairDestinationValid = false;
        runtime.repairState = ObjectSlaveRepairState::None;
        runtime.repairPhaseDueTick = 0;
        runtime.nextDecisionTick = 0;
        // This ingress intentionally carries no confirmed tick. Defer status
        // and stealth projection to update() instead of stamping tick zero.
        runtime.slavedEffectsApplied = false;
        ++runtime.revision;
        bound = true;
    }
    for (ObjectSlaveRuntime& runtime : component->mobMemberSlaved) {
        if (runtime.master == master && !runtime.requireMaster) continue;
        runtime.master = master;
        runtime.requireMaster = false;
        runtime.outsideCatchUpFrames = 0;
        runtime.nextDecisionTick = 0;
        runtime.decisionClockInitialized = false;
        runtime.primaryVictim = INVALID_OBJECT_ID;
        runtime.selfTasking = false;
        ++runtime.revision;
        bound = true;
    }
    return bound || (component->slaved.empty() &&
                     component->mobMemberSlaved.empty());
}

} // namespace engine
