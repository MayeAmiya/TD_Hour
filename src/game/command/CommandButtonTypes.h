#pragma once

#include <cstdint>

namespace game {

// Zero Hour GUICommandType. Keep this lightweight value contract separate
// from the mutable content store so lockstep commands and UI outcomes can
// carry a typed command identity without retaining catalog storage.
enum class CommandButtonKind : uint8_t {
    Unknown,
    None,
    DozerConstruct,
    DozerConstructCancel,
    UnitBuild,
    CancelUnitBuild,
    PlayerUpgrade,
    ObjectUpgrade,
    CancelUpgrade,
    AttackMove,
    Guard,
    GuardWithoutPursuit,
    GuardFlyingUnitsOnly,
    Stop,
    Waypoints,
    ExitContainer,
    Evacuate,
    ExecuteRailedTransport,
    BeaconDelete,
    SetRallyPoint,
    Sell,
    FireWeapon,
    SpecialPower,
    PurchaseScience,
    HackInternet,
    ToggleOvercharge,
    CombatDrop,
    SwitchWeapon,
    HijackVehicle,
    ConvertToCarBomb,
    SabotageBuilding,
    PlaceBeacon,
    SpecialPowerFromShortcut,
    SpecialPowerConstruct,
    SpecialPowerConstructFromShortcut,
    SelectAllUnitsOfType,
};

// Authored CommandButton.ini ButtonBorderType. This is presentation identity,
// not a command-kind inference: mods may deliberately give otherwise similar
// commands different ControlBar treatment.
enum class CommandButtonBorderType : uint8_t {
    None,
    Build,
    Upgrade,
    Action,
    System,
};

// RefCode routes these controls even while the object is disabled. Keep the
// policy in the value contract so UI availability and confirmed execution
// cannot diverge on Underpowered/EMP/Held state.
[[nodiscard]] constexpr bool commandButtonWorksWhileDisabled(
    CommandButtonKind kind) noexcept {
    switch (kind) {
    case CommandButtonKind::Sell:
    case CommandButtonKind::Evacuate:
    case CommandButtonKind::ExitContainer:
    case CommandButtonKind::BeaconDelete:
    case CommandButtonKind::SetRallyPoint:
    case CommandButtonKind::Stop:
    case CommandButtonKind::DozerConstructCancel:
    case CommandButtonKind::SwitchWeapon:
        return true;
    default:
        return false;
    }
}

} // namespace game
