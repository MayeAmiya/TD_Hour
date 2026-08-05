#include "core/container/container_types.h"
#include "ObjectModuleCatalog.h"
namespace game {

namespace {

// Kept numerically identical to RefCode's ModuleInterfaceType.  This file is
// intentionally data-only: it documents the original registration table
// without importing old factory allocation or virtual dispatch machinery.
constexpr uint32_t kUpdate = 0x00000001u;
constexpr uint32_t kDie = 0x00000002u;
constexpr uint32_t kDamage = 0x00000004u;
constexpr uint32_t kCreate = 0x00000008u;
constexpr uint32_t kCollide = 0x00000010u;
constexpr uint32_t kBody = 0x00000020u;
constexpr uint32_t kContain = 0x00000040u;
constexpr uint32_t kUpgrade = 0x00000080u;
constexpr uint32_t kSpecialPower = 0x00000100u;
constexpr uint32_t kClientUpdate = 0x00000800u;

template <size_t Count>
[[nodiscard]] constexpr bool contains(const container::Array<container::StringView, Count>& names,
                                      container::StringView sought) noexcept {
    for (const container::StringView name : names) {
        if (name == sought) return true;
    }
    return false;
}

constexpr auto kUpdateDamageUpgrade = std::to_array<container::StringView>({
    "AutoHealBehavior", "FireWeaponWhenDamagedBehavior",
});

constexpr auto kUpdateOnly = std::to_array<container::StringView>({
    "GrantStealthBehavior", "BridgeScaffoldBehavior", "DumbProjectileBehavior",
    "AssistedTargetingUpdate", "AutoFindHealingUpdate", "StealthDetectorUpdate",
    "StealthUpdate", "DeletionUpdate", "SmartBombTargetHomingUpdate",
    "DynamicShroudClearingRangeUpdate", "DeployStyleAIUpdate", "AssaultTransportAIUpdate",
    "HordeUpdate", "EnemyNearUpdate", "LifetimeUpdate", "RadiusDecalUpdate", "EMPUpdate",
    "AutoDepositUpdate", "WeaponBonusUpdate", "MissileAIUpdate",
    "FireSpreadUpdate", "FireWeaponUpdate", "FloatUpdate", "TensileFormationUpdate",
    "HeightDieUpdate", "ChinookAIUpdate", "JetAIUpdate", "AIUpdateInterface",
    "SupplyTruckAIUpdate", "DeliverPayloadAIUpdate", "HackInternetAIUpdate",
    "DynamicGeometryInfoUpdate", "FirestormDynamicGeometryInfoUpdate",
    "PointDefenseLaserUpdate", "CleanupHazardUpdate", "CommandButtonHuntUpdate",
    "PilotFindVehicleUpdate", "DemoTrapUpdate", "ParticleUplinkCannonUpdate",
    "SpectreGunshipUpdate", "SpectreGunshipDeploymentUpdate", "BattlePlanUpdate",
    "ProjectileStreamUpdate", "QueueProductionExitUpdate", "RepairDockUpdate",
    "PrisonDockUpdate", "RailedTransportDockUpdate", "DefaultProductionExitUpdate",
    "SpawnPointProductionExitUpdate", "SlavedUpdate", "MobMemberSlavedUpdate", "OCLUpdate",
    "SpecialAbilityUpdate", "MissileLauncherBuildingUpdate",
    "SupplyCenterProductionExitUpdate", "SupplyCenterDockUpdate",
    "SupplyWarehouseDockUpdate", "DozerAIUpdate", "POWTruckAIUpdate",
    "RailedTransportAIUpdate", "ProneUpdate", "StickyBombUpdate",
    "FireOCLAfterWeaponCooldownUpdate", "HijackerUpdate", "BoneFXUpdate", "RadarUpdate",
    "AnimationSteeringUpdate", "TransportAIUpdate", "WanderAIUpdate", "WaveGuideUpdate",
    "WorkerAIUpdate", "PowerPlantUpdate", "CheckpointUpdate",
});

constexpr auto kUpdateDie = std::to_array<container::StringView>({
    "NeutronBlastBehavior", "SlowDeathBehavior", "HelicopterSlowDeathBehavior",
    "NeutronMissileSlowDeathBehavior", "PropagandaTowerBehavior", "BunkerBusterBehavior",
    "ParkingPlaceBehavior", "FlightDeckBehavior", "RebuildHoleBehavior", "TechBuildingBehavior",
    "BattleBusSlowDeathBehavior", "JetSlowDeathBehavior", "NeutronMissileUpdate",
    "ProductionUpdate", "StructureToppleUpdate", "StructureCollapseUpdate",
    "LeafletDropBehavior",
});

constexpr auto kUpdateDieDamage = std::to_array<container::StringView>({
    "BridgeBehavior", "SpawnBehavior",
});

constexpr auto kDieDamage = std::to_array<container::StringView>({
    "BridgeTowerBehavior",
});

constexpr auto kUpdateUpgrade = std::to_array<container::StringView>({
    "CountermeasuresBehavior", "SpyVisionUpdate",
});

constexpr auto kUpdateCollide = std::to_array<container::StringView>({
    "PhysicsBehavior", "RailroadBehavior", "ToppleUpdate",
});

constexpr auto kDieOnly = std::to_array<container::StringView>({
    "InstantDeathBehavior", "DestroyDie", "CrushDie", "DamDie", "CreateCrateDie",
    "CreateObjectDie", "EjectPilotDie", "SpecialPowerCompletionDie", "RebuildHoleExposeDie",
    "UpgradeDie", "KeepObjectDie",
});

constexpr auto kFullContain = std::to_array<container::StringView>({
    "CaveContain", "TunnelContain",
});

constexpr auto kContainModules = std::to_array<container::StringView>({
    "OpenContain", "HealContain", "GarrisonContain", "InternetHackContain", "TransportContain",
    "RiderChangeContain", "RailedTransportContain", "MobNexusContain", "OverlordContain",
    "HelixContain", "ParachuteContain", "POWTruckBehavior", "PrisonBehavior",
    "PropagandaCenterBehavior",
});

constexpr auto kUpdateDamage = std::to_array<container::StringView>({
    "OverchargeBehavior", "PoisonedBehavior", "SupplyWarehouseCripplingBehavior",
    "BaseRegenerateUpdate", "FlammableUpdate",
});

constexpr auto kDieUpgrade = std::to_array<container::StringView>({
    "FireWeaponWhenDeadBehavior", "FXListDie",
});

constexpr auto kUpdateDieUpgrade = std::to_array<container::StringView>({
    "GenerateMinefieldBehavior",
});

constexpr auto kUpdateDieDamageCollide = std::to_array<container::StringView>({
    "MinefieldBehavior",
});

constexpr auto kSpecialPowers = std::to_array<container::StringView>({
    "BaikonurLaunchPower", "CashHackSpecialPower", "DefectorSpecialPower",
    "DemoralizeSpecialPower", "OCLSpecialPower", "FireWeaponPower", "SpecialAbility",
    "SpyVisionSpecialPower", "CashBountyPower", "CleanupAreaPower",
});

constexpr auto kUpgrades = std::to_array<container::StringView>({
    "CostModifierUpgrade", "ActiveShroudUpgrade", "ArmorUpgrade", "CommandSetUpgrade",
    "GrantScienceUpgrade", "PassengersFireUpgrade", "StatusBitsUpgrade", "SubObjectsUpgrade",
    "StealthUpgrade", "RadarUpgrade", "PowerPlantUpgrade", "LocomotorSetUpgrade",
    "ObjectCreationUpgrade", "ReplaceObjectUpgrade", "ModelConditionUpgrade",
    "UnpauseSpecialPowerUpgrade", "WeaponBonusUpgrade", "WeaponSetUpgrade",
    "ExperienceScalarUpgrade", "MaxHealthUpgrade",
});

constexpr auto kCreates = std::to_array<container::StringView>({
    "LockWeaponCreate", "SupplyCenterCreate", "SupplyWarehouseCreate",
    "SpecialPowerCreate", "GrantUpgradeCreate", "VeterancyGainCreate",
});

constexpr auto kDamageOnly = std::to_array<container::StringView>({
    "BoneFXDamage", "TransitionDamageFX",
});

constexpr auto kCollideOnly = std::to_array<container::StringView>({
    "FireWeaponCollide", "SquishCollide", "HealCrateCollide", "MoneyCrateCollide",
    "ShroudCrateCollide", "UnitCrateCollide", "VeterancyCrateCollide",
    "ConvertToCarBombCrateCollide", "ConvertToHijackedVehicleCrateCollide",
    "SabotageCommandCenterCrateCollide", "SabotageFakeBuildingCrateCollide",
    "SabotageInternetCenterCrateCollide", "SabotageMilitaryFactoryCrateCollide",
    "SabotagePowerPlantCrateCollide", "SabotageSuperweaponCrateCollide",
    "SabotageSupplyCenterCrateCollide", "SabotageSupplyDropzoneCrateCollide",
    "SalvageCrateCollide",
});

constexpr auto kBodies = std::to_array<container::StringView>({
    "InactiveBody", "ActiveBody", "HighlanderBody", "ImmortalBody", "StructureBody",
    "HiveStructureBody", "UndeadBody",
});

constexpr auto kClientUpdates = std::to_array<container::StringView>({
    "LaserUpdate", "AnimatedParticleSysBoneClientUpdate", "SwayClientUpdate", "BeaconClientUpdate",
});

[[nodiscard]] constexpr bool isAiModule(container::StringView moduleClass) noexcept {
    // In RefCode this was ModuleData::isAiModuleData(), not a virtual module
    // query. All stock AI data classes either carry this canonical suffix,
    // are the base AIUpdateInterface itself, or are FlightDeckBehavior.
    return moduleClass.ends_with("AIUpdate") || moduleClass == "AIUpdateInterface" ||
        moduleClass == "FlightDeckBehavior";
}

[[nodiscard]] constexpr ObjectOnDieHandlerKind onDieHandler(
    container::StringView moduleClass) noexcept {
    if (contains(kDieOnly, moduleClass) ||
        contains(kDieUpgrade, moduleClass) ||
        moduleClass == "NeutronBlastBehavior" ||
        moduleClass == "SlowDeathBehavior" ||
        moduleClass == "HelicopterSlowDeathBehavior" ||
        moduleClass == "NeutronMissileSlowDeathBehavior" ||
        moduleClass == "RebuildHoleBehavior" ||
        moduleClass == "BattleBusSlowDeathBehavior" ||
        moduleClass == "JetSlowDeathBehavior" ||
        moduleClass == "StructureToppleUpdate" ||
        moduleClass == "StructureCollapseUpdate" ||
        moduleClass == "LeafletDropBehavior") {
        return ObjectOnDieHandlerKind::DeathReaction;
    }
    if (moduleClass == "BridgeBehavior" ||
        moduleClass == "BridgeTowerBehavior") {
        return ObjectOnDieHandlerKind::Bridge;
    }
    if (moduleClass == "ParkingPlaceBehavior" ||
        moduleClass == "FlightDeckBehavior") {
        return ObjectOnDieHandlerKind::Airfield;
    }
    if (moduleClass == "ProductionUpdate") {
        return ObjectOnDieHandlerKind::Production;
    }
    if (moduleClass == "SpawnBehavior") {
        return ObjectOnDieHandlerKind::Spawn;
    }
    if (moduleClass == "TechBuildingBehavior") {
        return ObjectOnDieHandlerKind::TechBuilding;
    }
    if (moduleClass == "PropagandaTowerBehavior") {
        return ObjectOnDieHandlerKind::PropagandaTower;
    }
    if (contains(kFullContain, moduleClass) ||
        contains(kContainModules, moduleClass) ||
        moduleClass == "BunkerBusterBehavior") {
        return ObjectOnDieHandlerKind::Containment;
    }
    if (moduleClass == "GenerateMinefieldBehavior" ||
        moduleClass == "MinefieldBehavior") {
        return ObjectOnDieHandlerKind::Minefield;
    }
    if (moduleClass == "NeutronMissileUpdate") {
        return ObjectOnDieHandlerKind::NeutronMissile;
    }
    return ObjectOnDieHandlerKind::None;
}

template <size_t Count>
[[nodiscard]] constexpr bool allOnDieModulesRouted(
    const container::Array<container::StringView, Count>& names) noexcept {
    for (const container::StringView name : names) {
        if (onDieHandler(name) == ObjectOnDieHandlerKind::None) return false;
    }
    return true;
}

static_assert(allOnDieModulesRouted(kUpdateDie));
static_assert(allOnDieModulesRouted(kUpdateDieDamage));
static_assert(allOnDieModulesRouted(kDieDamage));
static_assert(allOnDieModulesRouted(kDieOnly));
static_assert(allOnDieModulesRouted(kFullContain));
static_assert(allOnDieModulesRouted(kContainModules));
static_assert(allOnDieModulesRouted(kDieUpgrade));
static_assert(allOnDieModulesRouted(kUpdateDieUpgrade));
static_assert(allOnDieModulesRouted(kUpdateDieDamageCollide));

[[nodiscard]] constexpr ObjectModuleCatalogEntry behavior(uint32_t mask,
                                                           container::StringView moduleClass) noexcept {
    return {.interfaceMask = mask,
            .domain = ObjectModuleCatalogDomain::Behavior,
            .onDieHandler = (mask & kDie) != 0
                ? onDieHandler(moduleClass)
                : ObjectOnDieHandlerKind::None,
            .isAiModule = isAiModule(moduleClass)};
}

} // namespace

std::optional<ObjectModuleCatalogEntry>
findObjectModuleCatalogEntry(container::StringView moduleClass) noexcept {
    if (contains(kUpdateDamageUpgrade, moduleClass)) return behavior(kUpdate | kDamage | kUpgrade, moduleClass);
    if (contains(kUpdateOnly, moduleClass)) return behavior(kUpdate, moduleClass);
    if (contains(kUpdateDie, moduleClass)) return behavior(kUpdate | kDie, moduleClass);
    if (contains(kUpdateDieDamage, moduleClass)) return behavior(kUpdate | kDie | kDamage, moduleClass);
    if (contains(kDieDamage, moduleClass)) return behavior(kDie | kDamage, moduleClass);
    if (contains(kUpdateUpgrade, moduleClass)) return behavior(kUpdate | kUpgrade, moduleClass);
    if (contains(kUpdateCollide, moduleClass)) return behavior(kUpdate | kCollide, moduleClass);
    if (contains(kDieOnly, moduleClass)) return behavior(kDie, moduleClass);
    if (contains(kFullContain, moduleClass)) {
        return behavior(kUpdate | kDie | kDamage | kCreate | kCollide | kContain, moduleClass);
    }
    if (contains(kContainModules, moduleClass)) {
        return behavior(kUpdate | kDie | kDamage | kCollide | kContain, moduleClass);
    }
    if (contains(kUpdateDamage, moduleClass)) return behavior(kUpdate | kDamage, moduleClass);
    if (contains(kDieUpgrade, moduleClass)) return behavior(kDie | kUpgrade, moduleClass);
    if (contains(kUpdateDieUpgrade, moduleClass)) return behavior(kUpdate | kDie | kUpgrade, moduleClass);
    if (contains(kUpdateDieDamageCollide, moduleClass)) {
        return behavior(kUpdate | kDie | kDamage | kCollide, moduleClass);
    }
    if (contains(kSpecialPowers, moduleClass)) return behavior(kSpecialPower, moduleClass);
    if (contains(kUpgrades, moduleClass)) return behavior(kUpgrade, moduleClass);
    if (contains(kCreates, moduleClass)) return behavior(kCreate, moduleClass);
    if (contains(kDamageOnly, moduleClass)) return behavior(kDamage, moduleClass);
    if (contains(kCollideOnly, moduleClass)) return behavior(kCollide, moduleClass);
    if (contains(kBodies, moduleClass)) return behavior(kBody, moduleClass);
    if (contains(kClientUpdates, moduleClass)) {
        return ObjectModuleCatalogEntry{.interfaceMask = kClientUpdate,
                                        .domain = ObjectModuleCatalogDomain::ClientUpdate,
                                        .isAiModule = false};
    }
    return std::nullopt;
}

bool isIgnoredLegacyObjectModule(container::StringView moduleClass) noexcept {
    // Original product-edition preorder decoration. GeneralsTD has no
    // preorder/CD-key entitlement concept; retain only no-op INI compatibility.
    return moduleClass == "PreorderCreate";
}

} // namespace game
