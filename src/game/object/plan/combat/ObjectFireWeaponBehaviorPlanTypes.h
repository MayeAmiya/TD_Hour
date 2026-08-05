#pragma once

#include "core/container/container_types.h"
#include "game/data/base/UpgradeCatalog.h"

#include "game/base/DamageTypes.h"
#include "math/fixed/q32_32.h"
#include <cstdint>
#include <limits>
namespace game {

struct ModuleData;
struct ObjectDeathReactionRule;
struct ThingTemplate;

struct ObjectUpgradeMuxRecipe final {
    container::Vector<container::String> triggeredBy;
    container::Vector<container::String> conflictsWith;
    container::Vector<container::String> removesUpgrades;
    engine::UpgradeMask triggeredByMask;
    engine::UpgradeMask conflictsWithMask;
    engine::UpgradeMask removesUpgradesMask;
    container::String upgradeFx;
    bool requiresAllTriggers = false;
    bool masksCompiled = false;
};

void compileObjectUpgradeMuxRecipe(
    ObjectUpgradeMuxRecipe& mux,
    const engine::UpgradeCatalog* catalog) noexcept;

[[nodiscard]] bool objectFireWeaponUpgradeMatches(
    const ObjectUpgradeMuxRecipe& mux,
    const engine::UpgradeMask& playerCompleted,
    const engine::UpgradeMask& objectCompleted,
    const engine::UpgradeCatalog* catalog) noexcept;
[[nodiscard]] bool objectFireWeaponUpgradeHasConflict(
    const ObjectUpgradeMuxRecipe& mux,
    const engine::UpgradeMask& playerCompleted,
    const engine::UpgradeMask& objectCompleted,
    const engine::UpgradeCatalog* catalog) noexcept;
[[nodiscard]] bool objectFireWeaponUpgradeTriggeredBy(
    const ObjectUpgradeMuxRecipe& mux,
    engine::UpgradeContentId upgrade) noexcept;

struct ObjectFireWeaponWhenDamagedParameters final {
    uint32_t authoredOrder = 0;
    container::Array<container::String, 4> reactionWeapons;
    container::Array<container::String, 4> continuousWeapons;
    uint64_t damageTypeMask = std::numeric_limits<uint64_t>::max();
    math::q32_32 damageAmount{};
    ObjectUpgradeMuxRecipe upgradeMux;
    bool startsActive = false;
};

struct ObjectFireWeaponWhenDamagedPlan final {
    container::Vector<ObjectFireWeaponWhenDamagedParameters> rules;
    container::Vector<container::String> diagnostics;
};

struct ObjectFireWeaponWhenDeadParameters final {
    container::String deathWeapon;
    ObjectUpgradeMuxRecipe upgradeMux;
    bool startsActive = false;
};

[[nodiscard]] container::SharedPtr<const ObjectFireWeaponWhenDamagedPlan>
compileObjectFireWeaponWhenDamagedPlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog = nullptr);
[[nodiscard]] container::SharedPtr<const ObjectFireWeaponWhenDeadParameters>
compileObjectFireWeaponWhenDeadParameters(
    const ModuleData& module,
    container::Vector<container::String>* diagnostics = nullptr,
    const engine::UpgradeCatalog* upgradeCatalog = nullptr);

} // namespace game
