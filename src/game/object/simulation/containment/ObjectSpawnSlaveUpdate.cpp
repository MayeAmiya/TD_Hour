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

void ObjectSpawnSlaveSystem::update(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const PlayerRegistry* players, const GameContentSnapshot* content,
    const ObjectSpatialIndex* spatialIndex,
    const game::terrain::TerrainLogic* terrain, SimulationRandom* random,
    const ObjectSimulationRules& rules,
    uint64_t confirmedTick,
    uint64_t& nextGameplaySubmissionOrdinal,
    container::Vector<ObjectSpawnSlaveRequest>& spawnRequests,
    container::Vector<ObjectDamageRequest>& damageRequests,
    container::Vector<ObjectSpawnVeterancyRequest>&
        veterancyRequests,
    container::Vector<ObjectBodyHealthProjection>& bodyHealthProjections,
    container::Vector<ObjectDeleteDestroyRequest>& destroyRequests,
    container::Vector<ObjectDefectionRequest>& defectionRequests,
    container::Vector<ObjectSlaveRepairPresentationEvent>&
        repairPresentationEvents,
    container::Vector<ObjectTensileFormationEvent>&
        tensileNavigationEvents,
    container::Vector<ObjectTensileFormationEvent>&
        tensilePresentationEvents) const {
    using namespace object_spawn_slave_detail;

    container::Vector<ObjectId> spatialQueryScratch;
    container::Vector<std::pair<Fixed, ObjectId>> tensileNearestScratch;
    UpdateContext context{
        registry, lifecycle, players, content, spatialIndex, terrain, random,
        rules, confirmedTick, nextGameplaySubmissionOrdinal,
        spawnRequests, damageRequests,
        veterancyRequests, bodyHealthProjections, destroyRequests,
        defectionRequests,
        repairPresentationEvents, tensileNavigationEvents,
        tensilePresentationEvents, spatialQueryScratch,
        tensileNearestScratch,
    };

    container::Vector<Candidate> objects;
    const auto view = ecs::view<ObjectIdentityComponent,
                                ObjectSpawnSlaveComponent>(registry);
    objects.reserve(view.size_hint());
    for (ecs::entity entity : view) {
        const ObjectId id =
            view.template get<ObjectIdentityComponent>(entity).id;
        if (id && lifecycle.entityFromIdIncludingPending(id))
            objects.push_back({id, entity});
    }
    std::sort(objects.begin(), objects.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.id < b.id;
              });

    // Rebuild the many-slaves-to-one-master DroneSpotting projection from
    // authoritative relations below. Empty sets are intentionally retained
    // so the final producer disappearing clears the old weapon condition.
    const auto rangeBonusView =
        ecs::view<ObjectSlaveRangeBonusSourcesComponent>(registry);
    for (const ecs::entity entity : rangeBonusView) {
        rangeBonusView.template get<ObjectSlaveRangeBonusSourcesComponent>(
            entity).sources.clear();
    }

    reconcileOwnership(context);
    for (const Candidate& candidate : objects) {
        const ObjectSpawnSlaveComponent& component =
            ecs::get<ObjectSpawnSlaveComponent>(registry, candidate.entity);
        if (!component.plan) continue;
        updateSpawnAndHordeCandidate(context, objects, candidate);
        updateTensileCandidate(context, objects, candidate);
        updateSlavedCandidate(context, candidate);
    }

    const auto rebuiltRangeBonusView =
        ecs::view<ObjectSlaveRangeBonusSourcesComponent>(registry);
    for (const ecs::entity entity : rebuiltRangeBonusView) {
        ObjectSlaveRangeBonusSourcesComponent& sources =
            rebuiltRangeBonusView.template get<
                ObjectSlaveRangeBonusSourcesComponent>(entity);
        std::sort(sources.sources.begin(), sources.sources.end());
        sources.sources.erase(std::unique(sources.sources.begin(),
                                          sources.sources.end()),
                              sources.sources.end());
        if (setObjectWeaponBonusCondition(
                registry, entity,
                game::WeaponBonusCondition::DroneSpotting,
                !sources.sources.empty(), content, random,
                rules.logicFramesPerSecond, confirmedTick)) {
            ++sources.revision;
        }
    }
}

} // namespace engine
