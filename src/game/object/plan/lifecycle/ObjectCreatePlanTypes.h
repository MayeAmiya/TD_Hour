#pragma once

#include "core/container/container_types.h"

#include "game/object/definition/CombatProfile.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/data/base/UpgradeCatalog.h"

#include <cstddef>
#include <cstdint>
#include <variant>
namespace game {

struct ThingTemplate;

// CreateModule callbacks share one final-recipe author order, while each
// operation keeps only its own typed payload. std::variant makes unsupported
// field combinations unrepresentable and avoids rebuilding the legacy
// virtual-module graph on every object.
struct ObjectLockWeaponCreate final {
    WeaponSlot weaponSlot = WeaponSlot::Primary;
};

struct ObjectGrantUpgradeCreate final {
    container::String upgrade;
    engine::UpgradeContentId upgradeId =
        engine::INVALID_UPGRADE_CONTENT_ID;
    ObjectStatusMask exemptStatuses = 0;
};

struct ObjectVeterancyGainCreate final {
    ObjectVeterancyLevel startingLevel = ObjectVeterancyLevel::Regular;
    // Empty means the original SCIENCE_INVALID sentinel: no prerequisite.
    container::String scienceRequired;
};

struct ObjectSupplyCenterCreate final {};
struct ObjectSupplyWarehouseCreate final {};

using ObjectCreatePayload = std::variant<
    ObjectLockWeaponCreate,
    ObjectGrantUpgradeCreate,
    ObjectVeterancyGainCreate,
    ObjectSupplyCenterCreate,
    ObjectSupplyWarehouseCreate>;

struct ObjectCreateRule final {
    uint32_t authoredOrder = 0;
    ObjectCreatePayload payload = ObjectLockWeaponCreate{};
};

struct ObjectCreatePlan final {
    container::Vector<ObjectCreateRule> rules;
    container::Vector<container::String> diagnostics;
};

// Compiles the resolved final CreateModule recipe. A null result means that
// no currently typed Create operation exists on the object.
[[nodiscard]] container::SharedPtr<const ObjectCreatePlan>
compileObjectCreatePlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog = nullptr);

} // namespace game

