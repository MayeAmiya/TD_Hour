#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <utility>

#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/simulation/status/ObjectAutoHeal.h"
#include "game/object/simulation/status/ObjectBodyRuntime.h"
#include "game/object/simulation/combat/ObjectFireWeaponBehavior.h"
#include "game/object/simulation/combat/ObjectFireUpdates.h"
#include "game/object/simulation/combat/ObjectCombatProfileRuntime.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/terrain/TerrainLogic.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/simulation/economy/ObjectUpgradeDetail.h"

namespace engine
{

using object_upgrade_detail::refreshFireWeaponWhenDeadConflicts;
using object_upgrade_detail::refreshFxListDieConflicts;

void ObjectUpgradeSystem::onPlayerUpgradeCompleted(ecs::registry& registry,
                                                   ObjectLifecycle& lifecycle,
                                                   const ObjectOwnershipIndex& ownership,
                                                   PlayerId player,
                                                   const UpgradeMask& completedUpgrades,
                                                   const ObjectSimulationRules& rules,
                                                   uint64_t confirmedTick,
                                                   ObjectUpgradeExecutionContext context) const
{
    if (!player)
        return;
    for (const ObjectId object : ownership.objects(player))
    {
        const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
        if (!entity || lifecycle.isPendingDestroy(object))
            continue;
        const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, *entity);
        if (!owner || owner->player != player)
            continue;
        activateEligible(registry, *entity, completedUpgrades, rules, confirmedTick, context);
    }
}

void ObjectUpgradeSystem::onObjectOwnerChanged(ecs::registry& registry,
                                               ObjectLifecycle& lifecycle,
                                               ObjectId object,
                                               const UpgradeMask& newOwnerCompletedUpgrades,
                                               const ObjectSimulationRules& rules,
                                               uint64_t confirmedTick) const
{
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity || lifecycle.isPendingDestroy(object))
        return;
    // Capture changes which player's conflict state is observed by FXListDie,
    // but it is not Object::updateUpgradeModules().  Existing UpgradeMux
    // execution remains sticky and dormant rules wait for a real completion,
    // local grant, or construction-complete re-evaluation point.
    static_cast<void>(rules);
    static_cast<void>(confirmedTick);
    // No content snapshot on this path; catalog is optional for name resolution.
    refreshFxListDieConflicts(registry, *entity, newOwnerCompletedUpgrades, nullptr);
    refreshFireWeaponWhenDeadConflicts(registry, *entity,
                                       newOwnerCompletedUpgrades, nullptr);
}

} // namespace engine
