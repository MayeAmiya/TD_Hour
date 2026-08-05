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

#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
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

using object_upgrade_detail::inventoryContains;
using object_upgrade_detail::inventoryErase;
using object_upgrade_detail::inventoryInsert;
using object_upgrade_detail::objectInventory;
using object_upgrade_detail::resetMuxesForRemovedUpgrade;

bool ObjectUpgradeSystem::canReceiveObjectUpgrade(const ecs::registry& registry,
                                                  ecs::entity entity,
                                                  const UpgradeMask& ownerCompletedUpgrades,
                                                  UpgradeContentId prospectiveUpgrade) const noexcept
{
    if (!upgradeIdInMaskRange(prospectiveUpgrade) ||
        inventoryContains(
            ecs::try_get<ObjectUpgradeInventoryComponent>(registry, entity),
            prospectiveUpgrade))
    {
        return false;
    }
    const UpgradeMask& localCompleted = objectInventory(registry, entity);
    const auto matchesCompiledMux = [&](
        const game::ObjectUpgradeMuxRecipe& mux) {
        if (!mux.masksCompiled) return false;
        UpgradeMask completed = ownerCompletedUpgrades | localCompleted;
        upgradeMaskSet(completed, prospectiveUpgrade);
        if (completed.test_for_any(mux.conflictsWithMask)) return false;
        return mux.requiresAllTriggers
            ? completed.test_for_all(mux.triggeredByMask)
            : completed.test_for_any(mux.triggeredByMask);
    };

    if (const ObjectUpgradeComponent* component = ecs::try_get<ObjectUpgradeComponent>(registry, entity);
        component && component->plan)
    {
        const size_t count = std::min(component->plan->rules.size(), component->instances.size());
        for (size_t index = 0; index < count; ++index)
        {
            if (component->instances[index].activated)
                continue;
            const game::ObjectUpgradeRule& rule = component->plan->rules[index];
            UpgradeMask prospectiveCompleted =
                ownerCompletedUpgrades | localCompleted;
            upgradeMaskSet(prospectiveCompleted, prospectiveUpgrade);
            const bool matches = rule.upgradeMasksCompiled &&
                !prospectiveCompleted.test_for_any(rule.conflictsWithMask) &&
                (rule.requiresAllTriggers
                    ? prospectiveCompleted.test_for_all(rule.triggeredByMask)
                    : prospectiveCompleted.test_for_any(rule.triggeredByMask));
            if (matches)
            {
                return true;
            }
        }
    }

    if (const ObjectAutoHealComponent* component = ecs::try_get<ObjectAutoHealComponent>(registry, entity);
        component && component->plan)
    {
        const size_t count = std::min(component->plan->rules.size(), component->instances.size());
        for (size_t index = 0; index < count; ++index)
        {
            const game::ObjectAutoHealParameters& parameters = component->plan->rules[index];
            if (component->instances[index].upgradeActivated ||
                component->instances[index].stopped)
            {
                continue;
            }
            UpgradeMask completed = ownerCompletedUpgrades | localCompleted;
            upgradeMaskSet(completed, prospectiveUpgrade);
            const bool matches = parameters.upgradeMasksCompiled &&
                !completed.test_for_any(parameters.conflictsWithMask) &&
                (parameters.requiresAllTriggers
                    ? completed.test_for_all(parameters.triggeredByMask)
                    : completed.test_for_any(parameters.triggeredByMask));
            if (matches)
            {
                return true;
            }
        }
    }

    if (const ObjectFireWeaponWhenDamagedComponent* component =
            ecs::try_get<ObjectFireWeaponWhenDamagedComponent>(registry,
                                                                entity);
        component && component->plan) {
        const size_t count = std::min(component->plan->rules.size(),
                                      component->instances.size());
        for (size_t index = 0; index < count; ++index) {
            const auto& parameters = component->plan->rules[index];
            if (component->instances[index].upgradeActivated) continue;
            if (matchesCompiledMux(parameters.upgradeMux)) return true;
        }
    }

    if (const ObjectFireOclAfterCooldownComponent* component =
            ecs::try_get<ObjectFireOclAfterCooldownComponent>(registry,
                                                               entity);
        component && component->plan) {
        const size_t count = std::min(component->plan->rules.size(),
                                      component->instances.size());
        for (size_t index = 0; index < count; ++index) {
            const auto& parameters = component->plan->rules[index];
            if (component->instances[index].upgradeActivated) continue;
            if (matchesCompiledMux(parameters.upgradeMux)) return true;
        }
    }

    const ObjectDeathReactionComponent* reactions = ecs::try_get<ObjectDeathReactionComponent>(registry, entity);
    const ObjectFxListDieRuntimeComponent* fxRuntime = ecs::try_get<ObjectFxListDieRuntimeComponent>(registry, entity);
    const ObjectFireWeaponWhenDeadRuntimeComponent* fireDead =
        ecs::try_get<ObjectFireWeaponWhenDeadRuntimeComponent>(registry,
                                                               entity);
    if (!reactions || !reactions->plan)
        return false;
    const size_t count = reactions->plan->rules.size();
    for (size_t index = 0; index < count; ++index)
    {
        const game::ObjectDeathReactionRule& rule = reactions->plan->rules[index];
        if (rule.kind == game::ObjectDeathReactionKind::FxList &&
            rule.fxListDie && fxRuntime && index < fxRuntime->rules.size() &&
            !fxRuntime->rules[index].activated) {
            UpgradeMask completed = ownerCompletedUpgrades | localCompleted;
            upgradeMaskSet(completed, prospectiveUpgrade);
            const bool matches = rule.fxListDie->upgradeMasksCompiled &&
                !completed.test_for_any(
                    rule.fxListDie->conflictsWithMask) &&
                (rule.fxListDie->requiresAllTriggers
                    ? completed.test_for_all(
                          rule.fxListDie->triggeredByMask)
                    : completed.test_for_any(
                          rule.fxListDie->triggeredByMask));
            if (matches) return true;
        }
        if (rule.kind ==
                game::ObjectDeathReactionKind::FireWeaponWhenDead &&
            rule.fireWeaponWhenDead && fireDead &&
            index < fireDead->rules.size() &&
            !fireDead->rules[index].upgradeActivated) {
            const auto& mux = rule.fireWeaponWhenDead->upgradeMux;
            if (matchesCompiledMux(mux)) return true;
        }
    }
    return false;
}

bool ObjectUpgradeSystem::hasObjectUpgrade(const ecs::registry& registry,
                                           ecs::entity entity,
                                           UpgradeContentId upgrade) const noexcept
{
    return inventoryContains(ecs::try_get<ObjectUpgradeInventoryComponent>(registry, entity), upgrade);
}

bool ObjectUpgradeSystem::completeObjectUpgrade(ecs::registry& registry,
                                                ObjectLifecycle& lifecycle,
                                                ObjectId object,
                                                UpgradeContentId upgrade,
                                                const UpgradeMask& ownerCompletedUpgrades,
                                                const ObjectSimulationRules& rules,
                                                uint64_t confirmedTick,
                                                ObjectUpgradeExecutionContext context) const
{
    if (!object || !upgradeIdInMaskRange(upgrade) ||
        lifecycle.isPendingDestroy(object))
        return false;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity)
        return false;

    ObjectUpgradeInventoryComponent* inventory = ecs::try_get<ObjectUpgradeInventoryComponent>(registry, *entity);
    if (inventoryContains(inventory, upgrade))
    {
        // Object::giveUpgrade sets an already-owned bit idempotently but still
        // calls updateUpgradeModules(). A duplicate synthetic veterancy grant
        // must therefore remain a real mux re-evaluation boundary.
        activateEligible(registry, *entity, ownerCompletedUpgrades,
                         rules, confirmedTick, context);
        return false;
    }
    if (!inventory)
    {
        inventory = &ecs::emplace<ObjectUpgradeInventoryComponent>(
            registry, *entity);
    }
    if (!inventoryInsert(*inventory, upgrade))
        return false;
    // Object::giveUpgrade stores the local bit first, even during construction,
    // then asks the mux to evaluate. The latter correctly short-circuits until
    // construction completes without losing the completed local upgrade.
    activateEligible(registry, *entity, ownerCompletedUpgrades, rules, confirmedTick, context);
    return true;
}

bool ObjectUpgradeSystem::removeObjectUpgrade(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, UpgradeContentId upgrade) const noexcept {
    if (!object || !upgradeIdInMaskRange(upgrade)) return false;
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    if (!entity) return false;
    ObjectUpgradeInventoryComponent* inventory =
        ecs::try_get<ObjectUpgradeInventoryComponent>(registry, *entity);
    if (!inventory || !inventoryErase(*inventory, upgrade)) return false;
    // RefCode's removeUpgrade does not undo MaxHealth/Status/etc. It only
    // resets matching UpgradeMux instances so a later re-grant can execute.
    resetMuxesForRemovedUpgrade(
        registry, *entity, upgrade);
    return true;
}

void ObjectUpgradeSystem::reevaluateObjectUpgrades(
    ecs::registry& registry,
    ObjectLifecycle& lifecycle,
    ObjectId object,
    const UpgradeMask& ownerCompletedUpgrades,
    const ObjectSimulationRules& rules,
    uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context) const
{
    if (!object || lifecycle.isPendingDestroy(object))
        return;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity)
        return;
    activateEligible(registry, *entity, ownerCompletedUpgrades,
                     rules, confirmedTick, context);
}

void ObjectUpgradeSystem::onConstructionCompleted(ecs::registry& registry,
                                                  ObjectLifecycle& lifecycle,
                                                  ObjectId object,
                                                  const UpgradeMask& ownerCompletedUpgrades,
                                                  const ObjectSimulationRules& rules,
                                                  uint64_t confirmedTick,
                                                  ObjectUpgradeExecutionContext context) const
{
    if (!object || lifecycle.isPendingDestroy(object))
        return;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity)
        return;
    activateEligible(registry, *entity, ownerCompletedUpgrades, rules, confirmedTick, context);
}


} // namespace engine
