#pragma once

#include "core/container/container_types.h"

#include "game/object/definition/ObjectKindOf.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
namespace game {

struct ThingTemplate;
namespace terrain {
class TerrainLogic;
}

enum class ObjectCrateCollideKind : uint8_t {
    Heal,
    Money,
    Shroud,
    Unit,
    Veterancy,
    Salvage,
    ConvertToCarBomb,
    ConvertToHijackedVehicle,
    SabotageCommandCenter,
    SabotageFakeBuilding,
    SabotageInternetCenter,
    SabotageMilitaryFactory,
    SabotagePowerPlant,
    SabotageSuperweapon,
    SabotageSupplyCenter,
    SabotageSupplyDropzone,
};

enum class ObjectSalvageCrateReward : uint8_t {
    None,
    Armor,
    Weapon,
    Veterancy,
    Money,
};

struct ObjectCrateUpgradeBoost final {
    container::String upgrade;
    int32_t amount = 0;
};

// Immutable projection of CrateCollideModuleData plus the small payload owned
// by each concrete stock crate class. Runtime collision never reparses an INI
// string and never retains a legacy Object/Player/FX pointer.
struct ObjectCrateCollideRule final {
    ObjectCrateCollideKind kind = ObjectCrateCollideKind::Heal;
    uint32_t authoredOrder = 0;
    ObjectKindOfMask requiredKinds{};
    ObjectKindOfMask forbiddenKinds{};
    container::String pickupScience;
    container::String executeFx;
    container::String executeAnimation;
    // Presentation metadata is still frozen in the content plan so the
    // simulation event does not quantize authoring floats at pickup time.
    math::q32_32 executeAnimationTimeSeconds{};
    math::q32_32 executeAnimationZRisePerSecond{};
    bool executeAnimationFades = true;
    bool forbidOwnerPlayer = false;
    bool buildingPickup = false;
    bool humanOnly = false;
    // The modern baseline keeps the 2026 duplicate-pickup fix enabled. An
    // authored Yes explicitly restores the retail same-frame multi pickup.
    bool allowMultiPickup = false;

    int64_t moneyProvided = 0;
    container::Vector<ObjectCrateUpgradeBoost> upgradedBoosts;
    uint32_t unitCount = 0;
    container::String unitName;
    uint32_t veterancyEffectRange = 0;
    bool veterancyAddsOwnerVeterancy = false;
    bool veterancyIsPilot = false;
    math::q32_32 salvageWeaponChance{int32_t{1}};
    math::q32_32 salvageLevelChance =
        math::q32_32::from_fraction(1, 4);
    // Parsed for exact content compatibility. RefCode never reads this field:
    // money is the unconditional fallback after armor/weapon/level fail.
    math::q32_32 salvageMoneyChance =
        math::q32_32::from_fraction(3, 4);
    int32_t salvageMinimumMoney = 25;
    int32_t salvageMaximumMoney = 75;
    container::String convertFxList;
    uint32_t sabotageDurationMilliseconds = 0;
    uint32_t stealCashAmount = 0;
};

struct ObjectCrateCollidePlan final {
    container::Vector<ObjectCrateCollideRule> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectCrateCollidePlan>
compileObjectCrateCollidePlan(const ThingTemplate& templateData);

} // namespace game
