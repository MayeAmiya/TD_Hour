#pragma once

#include "core/container/container_types.h"
#include "game/data/base/SpecialPowerCatalog.h"
#include "game/object/creation/ObjectCreationListRuntime.h"
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>
#include <limits>

namespace game {

struct ThingTemplate;
namespace terrain {
class TerrainLogic;
struct MapVisibilitySnapshot;
}

enum class ObjectSpecialPowerKind : uint8_t {
    Unsupported,
    SpyVision,
    ObjectCreationList,
    FireWeapon,
    SpecialAbility,
    CashHack,
    CashBounty,
    Defector,
    CleanupArea,
    BaikonurLaunch,
    ParticleUplink,
};

enum class ObjectSpecialPowerCreateLocation : uint8_t {
    EdgeNearSource,
    EdgeNearTarget,
    AtLocation,
    UseOwnerObject,
    AboveLocation,
    EdgeFarthestFromTarget,
};

struct ObjectSpecialPowerUpgradeOcl final {
    container::String science;
    container::String objectCreationList;
};

struct ObjectSpecialPowerUpgradeMoney final {
    container::String science;
    uint32_t amount = 0;
};

// Immutable projection of every SpecialPowerInterface module on one final
// Object recipe. The common template reference is retained for every module,
// including families whose effect consumer is still being migrated, so
// sabotage/recharge semantics never depend on the individual effect class.
struct ObjectSpecialPowerRule final {
    uint32_t authoredOrder = 0;
    ObjectSpecialPowerKind kind = ObjectSpecialPowerKind::Unsupported;
    container::String moduleClass;
    container::String moduleTag;
    container::String specialPowerTemplate;
    container::String initiateSound;
    uint32_t baseDurationMilliseconds = 0;
    uint32_t bonusDurationPerCapturedMilliseconds = 0;
    uint32_t maximumDurationMilliseconds = 0;
    container::String objectCreationList;
    container::Vector<ObjectSpecialPowerUpgradeOcl> upgradeObjectCreationLists;
    container::Vector<ObjectSpecialPowerUpgradeMoney> upgradeMoneyAmounts;
    container::String referenceObject;
    container::String detonationObject;
    ObjectSpecialPowerCreateLocation createLocation =
        ObjectSpecialPowerCreateLocation::EdgeNearSource;
    uint32_t maximumShotsToFire = 1;
    uint32_t moneyAmount = 0;
    math::q32_32 bountyPercent{};
    math::q32_32 fatCursorRadius{};
    math::q32_32 maxMoveDistanceFromLocation{};
    bool adjustPositionToPassable = false;
    bool scriptedOnly = false;
    bool updateModuleStartsAttack = false;
    bool startsPaused = false;
};

struct ObjectSpecialPowerPlan final {
    container::Vector<ObjectSpecialPowerRule> rules;
    container::Vector<container::String> diagnostics;
    bool hasSpecialPowerCreate = false;
};

[[nodiscard]] container::SharedPtr<const ObjectSpecialPowerPlan>
compileObjectSpecialPowerPlan(const ThingTemplate& templateData);

} // namespace game

