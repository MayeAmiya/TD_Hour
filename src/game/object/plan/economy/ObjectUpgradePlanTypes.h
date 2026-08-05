#pragma once

#include "core/container/container_types.h"

#include <cstdint>
#include <limits>
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/creation/ObjectCreationListRuntime.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

namespace game
{

struct ThingTemplate;
namespace terrain {
class TerrainLogic;
class MapVisibilityAuthority;
struct MapVisibilitySnapshot;
}

// A deliberately small, typed subset of the legacy UpgradeModule family.
// Each rule retains the shared UpgradeMux predicate as data, while its actual
// mutation is an explicit ECS operation instead of a virtual Object* call.
// Additional legacy upgrade classes can extend this enum without changing the
// player-upgrade completion transaction or reintroducing a generic module
// hierarchy into confirmed simulation.
enum class ObjectUpgradeOperation : uint8_t
{
    MaxHealth,
    ArmorSetPlayerUpgrade,
    WeaponSetPlayerUpgrade,
    WeaponBonusPlayerUpgrade,
    PowerPlant,
    StatusBits,
    ModelCondition,
    LocomotorSet,
    GrantScience,
    CommandSet,
    SubObjects,
    ExperienceScalar,
    CostModifier,
    Stealth,
    ObjectCreation,
    ReplaceObject,
    ActiveShroud,
    Radar,
    PassengersFire,
    UnpauseSpecialPower,
};

// RefCode ActiveBody::setMaxHealth has four independent current-health
// policies. Keep the authored names separate from the fixed-point operation
// so a future BattlePlanUpdate or object-local upgrade can reuse the same
// value rule without inheriting an ActiveBody pointer.
enum class ObjectMaxHealthChangeType : uint8_t
{
    SameCurrentHealth,
    PreserveRatio,
    AddCurrentHealthToo,
    FullyHeal,
};

struct ObjectUpgradeRule final
{
    uint32_t authoredOrder = 0;
    ObjectUpgradeOperation operation = ObjectUpgradeOperation::MaxHealth;

    container::Vector<container::String> triggeredBy;
    container::Vector<container::String> conflictsWith;
    container::Vector<container::String> removesUpgrades;
    // Snapshot-frozen identities. The authoring names above are retained for
    // diagnostics only; confirmed simulation never resolves them.
    engine::UpgradeMask triggeredByMask;
    engine::UpgradeMask conflictsWithMask;
    engine::UpgradeMask removesUpgradesMask;
    engine::UpgradeContentId triggerAltId =
        engine::INVALID_UPGRADE_CONTENT_ID;
    bool upgradeMasksCompiled = false;
    bool appliesChemicalSuitsDecal = false;
    bool requiresAllTriggers = false;
    container::String upgradeFx;

    math::q32_32 addMaxHealth{};
    ObjectMaxHealthChangeType maxHealthChangeType = ObjectMaxHealthChangeType::SameCurrentHealth;
    ObjectStatusMask statusToSet = 0;
    ObjectStatusMask statusToClear = 0;
    ModelConditionMask modelCondition;
    container::String grantScience;
    container::String commandSet;
    container::String commandSetAlt;
    container::String triggerAlt = "none";
    container::Vector<container::String> showSubObjects;
    container::Vector<container::String> hideSubObjects;
    math::q32_32 addExperienceScalar{};
    // The vector is retained only as authored/provenance data.  Confirmed
    // simulation reads the frozen mask below.
    container::Vector<container::String> costModifierKinds;
    ObjectKindOfMask costModifierKindMask{};
    // Signed delta: -0.2 means 20% cheaper, matching RefCode Player's
    // getProductionCostChangeBasedOnKindOf multiplier contract.
    math::q32_32 costModifierPercentage{};
    container::String objectCreationList;
    container::String replacementObject;
    math::q32_32 newShroudRange{};
    bool radarDisableProof = false;
    container::String specialPowerTemplate;
};

// PowerPlantUpdate is a tiny render-state controller shared by
// PowerPlantUpgrade and the future OverchargeBehavior. Its plan is frozen
// next to the typed upgrade rules, but it remains a distinct module in the
// source recipe and does not itself grant power.
struct ObjectPowerPlantPlan final
{
    uint32_t authoredOrder = 0;
    uint32_t rodsExtendMilliseconds = 0;
};

struct ObjectRadarUpdatePlan final {
    uint32_t authoredOrder = 0;
    uint32_t extendMilliseconds = 0;
};

struct ObjectUpgradePlan final
{
    container::Vector<ObjectUpgradeRule> rules;
    container::SharedPtr<const ObjectPowerPlantPlan> powerPlant;
    container::SharedPtr<const ObjectRadarUpdatePlan> radarUpdate;
    container::Vector<container::String> diagnostics;
};

// Compiles the final inherited recipe once. A null plan means the object has
// neither a currently supported UpgradeModule nor PowerPlantUpdate state.
[[nodiscard]] container::SharedPtr<const ObjectUpgradePlan>
compileObjectUpgradePlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog = nullptr);

// Pure UpgradeMux predicate. Player technology is a sealed-catalog UpgradeMask;
// object-local inventory remains exact-case name vectors. Trigger/conflict
// names resolve through the catalog when testing the player mask.
[[nodiscard]] bool objectUpgradeMatches(
    const ObjectUpgradeRule& rule, const engine::UpgradeMask& completedUpgrades,
    const engine::UpgradeCatalog* catalog) noexcept;

// Object::updateUpgradeModules combines the controlling player's completed
// technology and the object's own local upgrades before testing a mux.  Keep
// that explicitly representable at the ECS boundary: PLAYER technology never
// leaks into an individual object inventory, while OBJECT upgrades can still
// participate in RequiresAllTriggers and conflict checks.
[[nodiscard]] bool objectUpgradeMatches(
    const ObjectUpgradeRule& rule,
    const engine::UpgradeMask& playerCompletedUpgrades,
    const engine::UpgradeMask& objectCompletedUpgrades,
    const engine::UpgradeCatalog* catalog) noexcept;

} // namespace game

