#pragma once

#include "core/container/container_types.h"

#include <cstdint>
#include <optional>

namespace game {

// Immutable semantic classifier for SpecialPower.ini Enum. Ordinals match
// RefCode SpecialPowerType / SpecialPowerMaskType::s_bitNameList so authored
// content and branch tables stay aligned with Zero Hour. Invalid is both the
// default and the "Enum omitted" sentinel.
enum class SpecialPowerType : uint16_t {
    Invalid = 0,

    // Superweapons
    DaisyCutter,
    ParadropAmerica,
    CarpetBomb,
    ClusterMines,
    EmpPulse,
    NapalmStrike,
    CashHack,
    NeutronMissile,
    SpySatellite,
    Defector,
    TerrorCell,
    Ambush,
    BlackMarketNuke,
    AnthraxBomb,
    ScudStorm,
    DemoralizeObsolete,
    CrateDrop,
    A10ThunderboltStrike,
    DetonateDirtyNuke,
    ArtilleryBarrage,

    // Special abilities
    MissileDefenderLaserGuidedMissiles,
    RemoteCharges,
    TimedCharges,
    HelixNapalmBomb,
    HackerDisableBuilding,
    TankHunterTntAttack,
    BlackLotusCaptureBuilding,
    BlackLotusDisableVehicleHack,
    BlackLotusStealCashHack,
    InfantryCaptureBuilding,
    RadarVanScan,
    SpyDrone,
    DisguiseAsVehicle,
    BoobyTrap,
    RepairVehicles,
    ParticleUplinkCannon,
    CashBounty,
    ChangeBattlePlans,
    CiaIntelligence,
    CleanupArea,
    LaunchBaikonurRocket,
    SpectreGunship,
    GpsScrambler,
    Frenzy,
    SneakAttack,

    // Shortcut / general-variant powers (RefCode order fixed for identity)
    ChinaCarpetBomb,
    EarlyChinaCarpetBomb,
    LeafletDrop,
    EarlyLeafletDrop,
    EarlyFrenzy,
    CommunicationsDownload,
    EarlyRepairVehicles,
    TankParadrop,
    SupwParticleUplinkCannon,
    AirfDaisyCutter,
    NukeClusterMines,
    NukeNeutronMissile,
    AirfA10ThunderboltStrike,
    AirfSpectreGunship,
    InfaParadropAmerica,
    SlthGpsScrambler,
    AirfCarpetBomb,
    SuprCruiseMissile,
    LazrParticleUplinkCannon,
    SupwNeutronMissile,
    BattleshipBombardment,

    Count,
};

[[nodiscard]] std::optional<SpecialPowerType> tryParseSpecialPowerType(
    container::StringView name) noexcept;

[[nodiscard]] container::StringView specialPowerTypeName(
    SpecialPowerType type) noexcept;

[[nodiscard]] constexpr bool isCaptureBuilding(SpecialPowerType type) noexcept {
    return type == SpecialPowerType::InfantryCaptureBuilding ||
           type == SpecialPowerType::BlackLotusCaptureBuilding;
}

[[nodiscard]] constexpr bool isDisableHack(SpecialPowerType type) noexcept {
    return type == SpecialPowerType::HackerDisableBuilding ||
           type == SpecialPowerType::BlackLotusDisableVehicleHack;
}

[[nodiscard]] constexpr bool isRemoteCharges(SpecialPowerType type) noexcept {
    return type == SpecialPowerType::RemoteCharges;
}

// RefCode ActionManager::canCaptureBuilding. The target must be a structure,
// and KINDOF_CAPTURABLE is required only when the owner is not an outright
// enemy: "we can always capture enemy bldgs, regardless of kindof". No
// KINDOF_REBUILD_HOLE clause exists on this predicate.
[[nodiscard]] constexpr bool requiresCapturableTarget(
    SpecialPowerType type) noexcept {
    return isCaptureBuilding(type);
}

// The hack family (ActionManager::canStealCashViaHacking,
// canDisableBuildingViaHacking, and the SPECIAL_HACKER_DISABLE_BUILDING /
// SPECIAL_CASH_HACK branches of canDoSpecialPowerAtObject) instead demands
// KINDOF_CAPTURABLE unconditionally and excludes KINDOF_REBUILD_HOLE.
// canDisableVehicleViaHacking targets vehicles and is deliberately absent.
[[nodiscard]] constexpr bool requiresHackableTarget(
    SpecialPowerType type) noexcept {
    switch (type) {
        case SpecialPowerType::HackerDisableBuilding:
        case SpecialPowerType::BlackLotusStealCashHack:
        case SpecialPowerType::CashHack:
            return true;
        default:
            return false;
    }
}

// canDisableBuildingViaHacking's exception: a TechFactionBuilding that is not
// explicitly KINDOF_IMMUNE_TO_CAPTURE stays hackable even without
// KINDOF_CAPTURABLE. FactionBuilding.ini documents this for the Supply
// Dropzone, which is authored FS_TECHNOLOGY without CAPTURABLE.
[[nodiscard]] constexpr bool hackableViaTechFactionException(
    SpecialPowerType type) noexcept {
    return type == SpecialPowerType::HackerDisableBuilding;
}

// RefCode SpecialAbilityUpdate booby-trap pre-check set (remote only when the
// command carries a target / is not a no-target detonation).
[[nodiscard]] constexpr bool checksBoobyTrapOnTarget(
    SpecialPowerType type, bool noTargetCommand) noexcept {
    switch (type) {
        case SpecialPowerType::InfantryCaptureBuilding:
        case SpecialPowerType::BlackLotusCaptureBuilding:
        case SpecialPowerType::TankHunterTntAttack:
        case SpecialPowerType::TimedCharges:
        case SpecialPowerType::BoobyTrap:
            return true;
        case SpecialPowerType::RemoteCharges:
            return !noTargetCommand;
        default:
            return false;
    }
}

[[nodiscard]] constexpr bool createsSpecialObject(
    SpecialPowerType type, bool noTargetCommand) noexcept {
    switch (type) {
        case SpecialPowerType::TimedCharges:
        case SpecialPowerType::BoobyTrap:
        case SpecialPowerType::TankHunterTntAttack:
        case SpecialPowerType::HelixNapalmBomb:
            return true;
        case SpecialPowerType::RemoteCharges:
            return !noTargetCommand;
        default:
            return false;
    }
}

} // namespace game
