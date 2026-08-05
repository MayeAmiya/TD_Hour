#pragma once

#include "core/container/container_types.h"
#include "game/object/definition/CombatProfile.h"
#include "game/object/definition/ObjectKindOf.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/creation/ObjectCreationListCatalog.h"
#include "game/object/creation/ObjectCreationListRuntime.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

namespace game {

struct ThingTemplate;

struct ObjectGenerateMinefieldRule final {
    uint32_t authoredOrder = 0;
    container::String mineName;
    container::String upgradedMineName;
    container::String upgradedTriggeredBy;
    container::Vector<container::String> triggeredBy;
    container::Vector<container::String> conflictsWith;
    engine::UpgradeMask triggeredByMask;
    engine::UpgradeMask conflictsWithMask;
    engine::UpgradeContentId upgradedTriggerId =
        engine::INVALID_UPGRADE_CONTENT_ID;
    container::String generationFx;
    math::q32_32 distanceAroundObject{int32_t{40}};
    math::q32_32 minesPerSquareFoot = math::q32_32::from_fraction(1, 100);
    math::q32_32 randomJitter{};
    math::q32_32 skipIfThisMuchUnderStructure =
        math::q32_32::from_fraction(33, 100);
    bool requiresAllTriggers = false;
    bool generateOnlyOnDeath = false;
    bool borderOnly = true;
    bool smartBorder = false;
    bool smartBorderSkipInterior = true;
    bool alwaysCircular = false;
    bool upgradable = false;
    bool hasAuthoredDistanceAroundObject = false;
    bool hasAuthoredMinesPerSquareFoot = false;
    bool upgradeMasksCompiled = false;
};

enum class ObjectMineRelationship : uint8_t {
    Allies = 1u << 0u,
    Enemies = 1u << 1u,
    Neutral = 1u << 2u,
};

using ObjectMineRelationshipMask = uint8_t;

struct ObjectMinefieldRule final {
    uint32_t authoredOrder = 0;
    container::String detonationWeapon;
    container::String creationList;
    ObjectMineRelationshipMask detonatedBy =
        static_cast<ObjectMineRelationshipMask>(ObjectMineRelationship::Enemies) |
        static_cast<ObjectMineRelationshipMask>(ObjectMineRelationship::Neutral);
    uint32_t creatorDeathCheckMilliseconds = 1000;
    uint32_t scootMilliseconds = 0;
    uint32_t numVirtualMines = 1;
    math::q32_32 repeatDetonateMoveThreshold{int32_t{1}};
    math::q32_32 healthPercentToDrainPerSecond{};
    bool stopsRegenAfterCreatorDies = true;
    bool regenerates = false;
    bool workersDetonate = false;
};

struct ObjectDemoTrapRule final {
    uint32_t authoredOrder = 0;
    WeaponSlot detonationWeaponSlot = WeaponSlot::Primary;
    WeaponSlot proximityModeWeaponSlot = WeaponSlot::Primary;
    WeaponSlot manualModeWeaponSlot = WeaponSlot::Primary;
    math::q32_32 triggerDetonationRange{};
    game::ObjectKindOfMask ignoreTargetKindMask{};
    uint32_t scanMilliseconds = 0;
    container::String detonationWeapon;
    bool defaultsToProximityMode = false;
    bool friendlyDetonation = false;
    bool detonateWhenKilled = false;
};

struct ObjectMinefieldPlan final {
    container::Vector<ObjectGenerateMinefieldRule> generators;
    container::Vector<ObjectMinefieldRule> mines;
    container::Vector<ObjectDemoTrapRule> demoTraps;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectMinefieldPlan>
compileObjectMinefieldPlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog = nullptr);

} // namespace game

namespace game::terrain { class TerrainLogic; }
