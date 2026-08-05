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


namespace engine {

using object_spawn_slave_detail::hasKind;

bool ObjectSpawnSlaveSystem::acknowledgeSpawn(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSpawnSlaveRequest& request, ObjectId spawned,
    bool accepted) const {
    const ObjectProductionSystem exitService;
    const auto releaseExit = [&]() noexcept {
        exitService.releaseExternalExit(
            registry, lifecycle, request.exitHost,
            request.exitReservation);
    };
    const std::optional<ecs::entity> masterEntity =
        lifecycle.entityFromIdIncludingPending(request.spawner);
    if (!masterEntity) {
        releaseExit();
        return false;
    }
    ObjectSpawnSlaveComponent* component =
        ecs::try_get<ObjectSpawnSlaveComponent>(registry, *masterEntity);
    if (!component || !component->plan || request.requestId == 0) {
        releaseExit();
        return false;
    }

    ObjectSpawnRuntime* runtime = nullptr;
    bool requireMaster = false;
    bool oneShot = true;
    uint32_t targetCount = 0;
    if (request.kind ==
        ObjectSpawnedByRuntimeComponent::Kind::SpawnBehavior) {
        if (request.ruleIndex >= component->spawns.size() ||
            request.ruleIndex >= component->plan->spawns.size()) {
            releaseExit();
            return false;
        }
        runtime = &component->spawns[request.ruleIndex];
        const game::ObjectSpawnRule& rule =
            component->plan->spawns[request.ruleIndex];
        requireMaster = rule.spawnedRequireSpawner;
        oneShot = rule.oneShot;
        targetCount = rule.spawnNumber;
    } else {
        if (request.ruleIndex >= component->mobNexus.size() ||
            request.ruleIndex >= component->plan->mobNexus.size()) {
            releaseExit();
            return false;
        }
        runtime = &component->mobNexus[request.ruleIndex];
        targetCount = std::min(
            component->plan->mobNexus[request.ruleIndex].slots,
            component->plan->mobNexus[request.ruleIndex]
                .initialPayloadCount);
    }
    const auto pending = std::find_if(
        runtime->pendingRequests.begin(), runtime->pendingRequests.end(),
        [&request](const ObjectSpawnRuntime::PendingRequest& item) {
            return item.requestId == request.requestId &&
                item.emissionSequence == request.emissionSequence &&
                item.templateName == request.templateName;
        });
    if (pending == runtime->pendingRequests.end()) {
        releaseExit();
        return false;
    }
    if (!accepted) {
        releaseExit();
        return true;
    }

    if (request.reclaimedObject && spawned != request.reclaimedObject) {
        releaseExit();
        return false;
    }
    const std::optional<ecs::entity> childEntity =
        lifecycle.entityFromIdIncludingPending(spawned);
    if (!spawned || !childEntity || lifecycle.isPendingDestroy(spawned)) {
        releaseExit();
        return false;
    }
    if (request.exitReservation && !exitService.commitExternalExit(
            registry, lifecycle, request.exitHost,
            request.exitReservation, spawned, request.confirmedTick,
            std::max(1u, request.logicFramesPerSecond))) {
        releaseExit();
        return false;
    }
    if (ObjectProducerComponent* producer =
            ecs::try_get<ObjectProducerComponent>(registry, *childEntity)) {
        producer->producer = request.spawner;
    } else {
        ecs::emplace<ObjectProducerComponent>(
            registry, *childEntity,
            ObjectProducerComponent{.producer = request.spawner});
    }
    ObjectSpawnedByRuntimeComponent link{
        .master = request.spawner,
        .kind = request.kind,
        .ruleIndex = request.ruleIndex,
        .requestId = request.requestId,
        .requireMaster = requireMaster,
    };
    if (ObjectSpawnedByRuntimeComponent* existing =
            ecs::try_get<ObjectSpawnedByRuntimeComponent>(registry,
                                                           *childEntity)) {
        link.revision = existing->revision + 1;
        *existing = link;
    } else {
        ecs::emplace<ObjectSpawnedByRuntimeComponent>(
            registry, *childEntity, link);
    }
    if (ObjectSpawnSlaveComponent* child =
            ecs::try_get<ObjectSpawnSlaveComponent>(registry,
                                                     *childEntity)) {
        for (ObjectSlaveRuntime& slave : child->slaved) {
            slave.master = request.spawner;
            slave.requireMaster = requireMaster;
            slave.guardOffsetInitialized = false;
            slave.repairDestinationValid = false;
            slave.repairState = ObjectSlaveRepairState::None;
            slave.repairPhaseDueTick = 0;
            slave.nextDecisionTick = request.confirmedTick;
            slave.slavedEffectsApplied = true;
            ++slave.revision;
        }
        for (ObjectSlaveRuntime& slave : child->mobMemberSlaved) {
            slave.master = request.spawner;
            slave.requireMaster = requireMaster;
            slave.outsideCatchUpFrames = 0;
            slave.nextDecisionTick = request.confirmedTick;
            slave.decisionClockInitialized = false;
            ++slave.revision;
        }
        if (!child->slaved.empty()) {
            static_cast<void>(ObjectStatusSystem::apply(
                registry, *childEntity,
                {.setMask = game::objectStatusBit(
                     game::ObjectStatusFlag::Unselectable),
                 .confirmedTick = request.confirmedTick}));
            const ObjectStatusComponent* masterStatus =
                ecs::try_get<ObjectStatusComponent>(registry,
                                                     *masterEntity);
            if (masterStatus && masterStatus->hasAny(
                    game::objectStatusBit(
                        game::ObjectStatusFlag::Stealthed))) {
                static_cast<void>(ObjectStatusSystem::apply(
                    registry, *childEntity,
                    {.setMask = game::objectStatusBit(
                         game::ObjectStatusFlag::CanStealth),
                     .confirmedTick = request.confirmedTick}));
            }
        }
    }
    if (request.kind ==
            ObjectSpawnedByRuntimeComponent::Kind::SpawnBehavior) {
        const ObjectKindOfComponent* masterKinds =
            ecs::try_get<ObjectKindOfComponent>(registry, *masterEntity);
        if (hasKind(masterKinds,
                    game::ObjectKindOf::SpawnsAreTheWeapons)) {
            const ObjectStatusComponent* masterStatus =
                ecs::try_get<ObjectStatusComponent>(registry, *masterEntity);
            const bool grantsStealth = masterStatus && masterStatus->hasAny(
                game::objectStatusBit(game::ObjectStatusFlag::CanStealth));
            if (grantsStealth) {
                static_cast<void>(ObjectStatusSystem::apply(
                    registry, *childEntity,
                    {.setMask = game::objectStatusBit(
                         game::ObjectStatusFlag::CanStealth),
                     .confirmedTick = request.confirmedTick}));
            }
        }
    }
    if (std::find(runtime->children.begin(), runtime->children.end(),
                  spawned) == runtime->children.end()) {
        // SpawnBehavior owns an insertion-ordered roster. Death/Delete walk
        // this sequence exactly; sorting by ObjectId changes authored child
        // side-effect order when more than one child is present.
        runtime->children.push_back(spawned);
    }
    runtime->pendingRequests.erase(pending);
    ++runtime->successfulSpawnCount;
    if (request.usedInitialProducerExit &&
        runtime->successfulInitialProducerExitCount !=
            std::numeric_limits<uint32_t>::max()) {
        ++runtime->successfulInitialProducerExitCount;
    }
    if (oneShot && runtime->replacementReadyTicks.empty() &&
        runtime->pendingRequests.empty() &&
        runtime->successfulSpawnCount >= targetCount) {
        runtime->oneShotCompleted = true;
    }
    ++runtime->revision;

    ObjectSpawnChildrenComponent* graph =
        ecs::try_get<ObjectSpawnChildrenComponent>(registry, *masterEntity);
    if (!graph) {
        graph = &ecs::emplace<ObjectSpawnChildrenComponent>(
            registry, *masterEntity);
    }
    const auto graphPosition = std::lower_bound(
        graph->children.begin(), graph->children.end(), spawned);
    if (graphPosition == graph->children.end() ||
        *graphPosition != spawned) {
        graph->children.insert(graphPosition, spawned);
        ++graph->revision;
    }
    return true;
}

} // namespace engine
