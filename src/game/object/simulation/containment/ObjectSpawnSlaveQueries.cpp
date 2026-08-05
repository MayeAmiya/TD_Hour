#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/object/simulation/containment/ObjectSpawnSlaveDetail.h"
#include "core/container/string_utils.h"

#include "game/base/DamageTypes.h"
#include "game/base/SimulationRandom.h"
#include "game/object/definition/CombatProfile.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/ai/runtime/ObjectAIOrderAdmission.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/runtime/ObjectAIOpportunityTargetPolicy.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
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

using object_spawn_slave_detail::Fixed;
using object_spawn_slave_detail::alive;
using object_spawn_slave_detail::distanceSquared;
using object_spawn_slave_detail::hasKind;

ObjectId ObjectSpawnSlaveSystem::closestSpawnChild(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId spawner, const LogicFixedVec3& position) const noexcept {
    const std::optional<ecs::entity> spawnerEntity =
        lifecycle.entityFromId(spawner);
    const ObjectSpawnSlaveComponent* component = spawnerEntity
        ? ecs::try_get<ObjectSpawnSlaveComponent>(registry, *spawnerEntity)
        : nullptr;
    if (!component || !component->plan) return INVALID_OBJECT_ID;

    ObjectId closest = INVALID_OBJECT_ID;
    Fixed closestDistance{};
    for (const ObjectSpawnRuntime& runtime : component->spawns) {
        for (const ObjectId child : runtime.children) {
            const std::optional<ecs::entity> childEntity =
                lifecycle.entityFromId(child);
            const TransformComponent* transform = childEntity
                ? ecs::try_get<TransformComponent>(registry, *childEntity)
                : nullptr;
            if (!transform || lifecycle.isPendingDestroy(child)) continue;
            const LogicFixedVec3 childPosition =
                readAuthoritativeObjectPosition(
                    registry, *childEntity, *transform);
            const Fixed dx = childPosition.x - position.x;
            const Fixed dy = childPosition.y - position.y;
            const Fixed distance = dx * dx + dy * dy;
            if (!closest || distance < closestDistance ||
                (distance == closestDistance && child < closest)) {
                closest = child;
                closestDistance = distance;
            }
        }
    }
    return closest;
}

container::Vector<ObjectId> ObjectSpawnSlaveSystem::spawnChildren(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId spawner) const {
    container::Vector<ObjectId> result;
    const std::optional<ecs::entity> spawnerEntity =
        lifecycle.entityFromId(spawner);
    const ObjectSpawnSlaveComponent* component = spawnerEntity
        ? ecs::try_get<ObjectSpawnSlaveComponent>(registry, *spawnerEntity)
        : nullptr;
    if (!component || !component->plan) return result;
    for (const ObjectSpawnRuntime& runtime : component->spawns) {
        for (const ObjectId child : runtime.children) {
            if (lifecycle.entityFromId(child) &&
                !lifecycle.isPendingDestroy(child)) {
                if (std::find(result.begin(), result.end(), child) ==
                    result.end()) {
                    result.push_back(child);
                }
            }
        }
    }
    return result;
}

bool ObjectSpawnSlaveSystem::maySpawnSelfTaskAI(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId spawner, uint32_t ruleIndex,
    math::q32_32 maximumSelfTaskersRatio) const noexcept {
    if (maximumSelfTaskersRatio <= Fixed{}) return false;
    const std::optional<ecs::entity> spawnerEntity =
        lifecycle.entityFromId(spawner);
    const ObjectSpawnSlaveComponent* component = spawnerEntity
        ? ecs::try_get<ObjectSpawnSlaveComponent>(registry, *spawnerEntity)
        : nullptr;
    if (!component || ruleIndex >= component->spawns.size()) return false;
    const ObjectSpawnRuntime& runtime = component->spawns[ruleIndex];
    if (!runtime.lastAttackCommandWasAi) return false;

    uint32_t liveCount = 0;
    uint32_t selfTaskingCount = 0;
    for (const ObjectId child : runtime.children) {
        if (!lifecycle.entityFromId(child) ||
            lifecycle.isPendingDestroy(child)) continue;
        ++liveCount;
        if (std::binary_search(runtime.selfTaskingChildren.begin(),
                               runtime.selfTaskingChildren.end(), child)) {
            ++selfTaskingCount;
        }
    }
    if (liveCount == 0) return false;
    const Fixed ratio = Fixed{static_cast<int32_t>(selfTaskingCount)} /
        Fixed{static_cast<int32_t>(liveCount)};
    return ratio < maximumSelfTaskersRatio;
}

bool ObjectSpawnSlaveSystem::setSpawnChildSelfTasking(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId spawner, uint32_t ruleIndex, ObjectId child,
    bool selfTasking) const noexcept {
    const std::optional<ecs::entity> spawnerEntity =
        lifecycle.entityFromId(spawner);
    const std::optional<ecs::entity> childEntity = lifecycle.entityFromId(child);
    ObjectSpawnSlaveComponent* component = spawnerEntity
        ? ecs::try_get<ObjectSpawnSlaveComponent>(registry, *spawnerEntity)
        : nullptr;
    const ObjectSpawnedByRuntimeComponent* link = childEntity
        ? ecs::try_get<ObjectSpawnedByRuntimeComponent>(registry, *childEntity)
        : nullptr;
    if (!component || ruleIndex >= component->spawns.size() || !link ||
        link->kind != ObjectSpawnedByRuntimeComponent::Kind::SpawnBehavior ||
        link->master != spawner || link->ruleIndex != ruleIndex) {
        return false;
    }
    ObjectSpawnRuntime& runtime = component->spawns[ruleIndex];
    if (std::find(runtime.children.begin(), runtime.children.end(), child) ==
        runtime.children.end()) {
        return false;
    }
    const auto found = std::lower_bound(runtime.selfTaskingChildren.begin(),
                                        runtime.selfTaskingChildren.end(),
                                        child);
    if (selfTasking) {
        if (found != runtime.selfTaskingChildren.end() && *found == child)
            return true;
        runtime.selfTaskingChildren.insert(found, child);
    } else {
        if (found == runtime.selfTaskingChildren.end() || *found != child)
            return true;
        runtime.selfTaskingChildren.erase(found);
    }
    ++runtime.revision;
    return true;
}

ObjectHiveDamageRoute ObjectSpawnSlaveSystem::routeHiveDamage(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectDamageRequest& request) const {
    const std::optional<ecs::entity> hiveEntity =
        lifecycle.entityFromId(request.target);
    if (!hiveEntity) return ObjectHiveDamageRoute::ApplyToHive;
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, *hiveEntity);
    if (!type || !type->archetype ||
        type->archetype->templateData.body.kind != game::ObjectBodyKind::HiveStructure)
        return ObjectHiveDamageRoute::ApplyToHive;
    const uint32_t bit = static_cast<uint32_t>(request.damageType);
    if (bit >= 64u ||
        (type->archetype->templateData.body.hivePropagateDamageTypesMask &
         (UINT64_C(1) << bit)) == 0)
        return ObjectHiveDamageRoute::ApplyToHive;
    const ObjectSpawnSlaveComponent* spawn =
        ecs::try_get<ObjectSpawnSlaveComponent>(registry, *hiveEntity);
    const bool hasSpawnInterface = spawn && spawn->plan &&
        !spawn->plan->spawns.empty();
    const ObjectContainmentComponent* contents =
        ecs::try_get<ObjectContainmentComponent>(registry, *hiveEntity);
    // HiveStructureBody uses SpawnBehavior when that interface exists and
    // considers rider containment only as the alternative interface. It must
    // not mix MobNexus children into the SpawnBehavior roster or fall through
    // to riders merely because a live SpawnBehavior currently has no slaves.
    if (!hasSpawnInterface && !contents)
        return ObjectHiveDamageRoute::ApplyToHive;
    ObjectId closest = INVALID_OBJECT_ID;
    Fixed closestDistance = Fixed::from_raw(std::numeric_limits<int64_t>::max());
    const std::optional<ecs::entity> sourceEntity =
        lifecycle.entityFromIdIncludingPending(request.source);
    if (!sourceEntity) return ObjectHiveDamageRoute::ApplyToHive;
    const TransformComponent* sourceTransform = sourceEntity
        ? ecs::try_get<TransformComponent>(registry, *sourceEntity) : nullptr;
    const LogicFixedVec3 sourcePosition = sourceTransform
        ? readAuthoritativeObjectPosition(
              registry, *sourceEntity, *sourceTransform)
        : LogicFixedVec3{};
    const auto consider = [&](ObjectId candidate) {
        const std::optional<ecs::entity> candidateEntity =
            lifecycle.entityFromId(candidate);
        if (!candidateEntity || !alive(registry, *candidateEntity)) return;
        const TransformComponent* candidateTransform =
            ecs::try_get<TransformComponent>(registry, *candidateEntity);
        const Fixed distance = sourceTransform && candidateTransform
            ? distanceSquared(
                  sourcePosition,
                  readAuthoritativeObjectPosition(
                      registry, *candidateEntity, *candidateTransform))
            : Fixed{};
        if (!closest || distance < closestDistance ||
            (distance == closestDistance && candidate < closest)) {
            closest = candidate;
            closestDistance = distance;
        }
    };
    if (hasSpawnInterface) {
        for (const ObjectSpawnRuntime& runtime : spawn->spawns) {
            for (const ObjectId child : runtime.children) consider(child);
        }
    } else {
        for (const ObjectContainedObjectRecord& member : contents->objects) {
            consider(member.object);
        }
    }
    if (closest) {
        request.target = closest;
        return ObjectHiveDamageRoute::RoutedToSlave;
    }
    if ((type->archetype->templateData.body.hiveSwallowDamageTypesMask &
         (UINT64_C(1) << bit)) != 0)
        return ObjectHiveDamageRoute::Swallowed;
    return ObjectHiveDamageRoute::ApplyToHive;
}


} // namespace engine
