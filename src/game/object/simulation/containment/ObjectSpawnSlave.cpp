#include "game/object/simulation/containment/ObjectSpawnSlave.h"
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


namespace engine {

void ObjectSpawnSlaveSystem::initializeObject(ecs::registry& registry,
                                               ecs::entity entity) const {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!type || !type->archetype || !type->archetype->spawnSlavePlan) return;
    const auto& plan = type->archetype->spawnSlavePlan;
    ObjectSpawnSlaveComponent component{.plan = plan};
    component.spawns.resize(plan->spawns.size());
    component.mobNexus.resize(plan->mobNexus.size());
    component.hordes.resize(plan->hordes.size());
    component.tensileFormations.resize(plan->tensileFormations.size());
    component.slaved.resize(plan->slaved.size());
    component.mobMemberSlaved.resize(plan->mobMemberSlaved.size());
    for (size_t i = 0; i < component.tensileFormations.size(); ++i)
        component.tensileFormations[i].enabled = plan->tensileFormations[i].enabled;
    if (ObjectSpawnSlaveComponent* existing =
            ecs::try_get<ObjectSpawnSlaveComponent>(registry, entity))
        *existing = std::move(component);
    else ecs::emplace<ObjectSpawnSlaveComponent>(registry, entity,
                                                  std::move(component));
    if (std::any_of(plan->slaved.begin(), plan->slaved.end(),
                    [](const game::ObjectSlavedRule& rule) {
                        return rule.repairRatePerSecond > math::q32_32{};
                    })) {
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
            registry, entity,
            ObjectModelConditionContributionSource::Containment,
            repairOwned,
            game::modelConditionMaskOf(game::ModelConditionFlag::Packing),
            0);
    }
    if (!plan->spawns.empty() || !plan->mobNexus.empty() ||
        plan->hiveStructureBody) {
        if (!ecs::try_get<ObjectSpawnChildrenComponent>(registry, entity))
            ecs::emplace<ObjectSpawnChildrenComponent>(registry, entity);
    }
}

} // namespace engine
