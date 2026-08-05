#pragma once

#include "game/object/simulation/economy/ObjectUpgrade.h"

namespace engine::object_upgrade_detail
{

[[nodiscard]] bool inventoryContains(
    const ObjectUpgradeInventoryComponent* inventory,
    UpgradeContentId upgrade) noexcept;
[[nodiscard]] bool inventoryInsert(
    ObjectUpgradeInventoryComponent& inventory,
    UpgradeContentId upgrade) noexcept;
[[nodiscard]] bool inventoryErase(
    ObjectUpgradeInventoryComponent& inventory,
    UpgradeContentId upgrade) noexcept;
[[nodiscard]] const UpgradeMask& objectInventory(
    const ecs::registry& registry,
    ecs::entity entity) noexcept;
void applyRule(
    ecs::registry& registry,
    ecs::entity entity,
    const game::ObjectUpgradeRule& rule,
    const UpgradeMask& ownerCompletedUpgrades,
    const UpgradeMask& objectCompletedUpgrades,
    const ObjectSimulationRules& rules,
    uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context);

void resetMuxesForRemovedUpgrade(
    ecs::registry& registry,
    ecs::entity entity,
    UpgradeContentId removedUpgrade) noexcept;
void processUpgradeRemovals(
    ecs::registry& registry,
    ecs::entity entity,
    const UpgradeMask& removals) noexcept;
void refreshFxListDieConflicts(
    ecs::registry& registry,
    ecs::entity entity,
    const UpgradeMask& ownerCompletedUpgrades,
    const UpgradeCatalog* catalog) noexcept;
void refreshFireWeaponWhenDeadConflicts(
    ecs::registry& registry,
    ecs::entity entity,
    const UpgradeMask& ownerCompletedUpgrades,
    const UpgradeCatalog* catalog) noexcept;
void activateAllEligibleMuxes(
    ecs::registry& registry,
    ecs::entity entity,
    const UpgradeMask& ownerCompletedUpgrades,
    const ObjectSimulationRules& rules,
    uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context);

void beginPowerPlantExtension(
    ecs::registry& registry,
    ecs::entity entity,
    ObjectPowerPlantComponent& powerPlant,
    const ObjectSimulationRules& rules,
    uint64_t confirmedTick);
void completePowerPlantExtension(
    ecs::registry& registry,
    ecs::entity entity,
    ObjectPowerPlantComponent& powerPlant,
    uint64_t confirmedTick);
void retractPowerPlant(
    ecs::registry& registry,
    ecs::entity entity,
    ObjectPowerPlantComponent& powerPlant,
    uint64_t confirmedTick);
void beginRadarExtension(
    ecs::registry& registry,
    ecs::entity entity,
    ObjectRadarUpdateComponent& radar,
    const ObjectSimulationRules& rules,
    uint64_t confirmedTick);
void completeRadarExtension(
    ecs::registry& registry,
    ecs::entity entity,
    ObjectRadarUpdateComponent& radar,
    uint64_t confirmedTick);

} // namespace engine::object_upgrade_detail
