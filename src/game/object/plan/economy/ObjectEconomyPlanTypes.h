#pragma once

#include "core/container/container_types.h"
#include "game/data/base/UpgradeCatalog.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

namespace game {

struct ThingTemplate;

enum class ObjectEconomyModuleKind : uint8_t {
    InternetHackContain,
    AutoFindHealingUpdate,
    ChinookAIUpdate,
    SupplyTruckAIUpdate,
    HackInternetAIUpdate,
    RepairDockUpdate,
    SupplyCenterDockUpdate,
    SupplyWarehouseDockUpdate,
    WorkerAIUpdate,
};

struct ObjectAutoFindHealingRule final {
    uint32_t authoredOrder = 0;
    uint32_t scanRateMilliseconds = 0;
    math::q32_32 scanRange{};
    math::q32_32 neverHealRatio = math::q32_32::from_fraction(19, 20);
    math::q32_32 alwaysHealRatio = math::q32_32::from_fraction(1, 4);
};

// Value-only projection of the common legacy DockUpdate fields.  A negative
// count means a dynamically growing approach queue; zero really is a closed
// queue, matching the original base-class default.
struct ObjectSupplyDockRule final {
    int32_t numberApproachPositions = 0;
    bool allowsPassthrough = true;
};

struct ObjectRepairDockRule final {
    uint32_t authoredOrder = 0;
    ObjectSupplyDockRule dock;
    // INI durations are retained as authored milliseconds.  The fixed
    // per-tick amount is frozen from the docker's missing health on its first
    // action, after conversion with the session logic rate.
    uint32_t timeForFullHealMilliseconds = 0;
};

struct ObjectSupplyTruckRule final {
    uint32_t authoredOrder = 0;
    uint32_t maxBoxes = 0;
    uint32_t supplyCenterActionDelayMilliseconds = 0;
    uint32_t supplyWarehouseActionDelayMilliseconds = 0;
    math::q32_32 supplyWarehouseScanDistance{100};
    container::String suppliesDepletedVoice;
    uint32_t upgradedSupplyBoost = 0;
    engine::UpgradeContentId upgradedSupplyBoostUpgrade =
        engine::INVALID_UPGRADE_CONTENT_ID;
    bool usesUpgradedSupplyBoost = false;
    bool airborneTransport = false;
    bool workerMode = false;
};

struct ObjectHackInternetRule final {
    uint32_t authoredOrder = 0;
    uint32_t unpackTimeMilliseconds = 0;
    uint32_t packTimeMilliseconds = 0;
    uint32_t cashUpdateDelayMilliseconds = 0;
    uint32_t cashUpdateDelayFastMilliseconds = 0;
    uint32_t regularCashAmount = 0;
    uint32_t veteranCashAmount = 0;
    uint32_t eliteCashAmount = 0;
    uint32_t heroicCashAmount = 0;
    uint32_t xpPerCashUpdate = 0;
    math::q32_32 packUnpackVariationFactor{};
};

struct ObjectSupplyCenterDockRule final {
    uint32_t authoredOrder = 0;
    ObjectSupplyDockRule dock;
    uint32_t grantTemporaryStealthMilliseconds = 0;
};

struct ObjectSupplyWarehouseDockRule final {
    uint32_t authoredOrder = 0;
    ObjectSupplyDockRule dock;
    uint32_t startingBoxes = 1;
    bool deleteWhenEmpty = false;
};

struct ObjectEconomyModulePresence final {
    ObjectEconomyModuleKind kind = ObjectEconomyModuleKind::InternetHackContain;
    uint32_t authoredOrder = 0;
};

struct ObjectEconomyPlan final {
    container::Vector<ObjectAutoFindHealingRule> autoFindHealing;
    container::Vector<ObjectRepairDockRule> repairDocks;
    container::Vector<ObjectSupplyTruckRule> supplyTrucks;
    container::Vector<ObjectHackInternetRule> hackInternet;
    container::Vector<ObjectSupplyCenterDockRule> supplyCenterDocks;
    container::Vector<ObjectSupplyWarehouseDockRule> supplyWarehouseDocks;
    container::Vector<ObjectEconomyModulePresence> modules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectEconomyPlan>
compileObjectEconomyPlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog = nullptr);

} // namespace game
