#include "PendingWorldCommandMode.h"

namespace engine::selection {
namespace {

[[nodiscard]] PendingWorldTargetRelation relations(
    const game::CommandButtonTemplate& button) noexcept {
    using Option = game::CommandButtonOption;
    PendingWorldTargetRelation result = PendingWorldTargetRelation::None;
    if (game::hasCommandButtonOption(
            button.descriptor.options, Option::NeedTargetEnemyObject)) {
        result = result | PendingWorldTargetRelation::Enemy;
    }
    if (game::hasCommandButtonOption(
            button.descriptor.options, Option::NeedTargetAllyObject)) {
        result = result | PendingWorldTargetRelation::Ally;
    }
    if (game::hasCommandButtonOption(
            button.descriptor.options, Option::NeedTargetNeutralObject)) {
        result = result | PendingWorldTargetRelation::Neutral;
    }
    return result;
}

} // namespace

std::optional<PendingWorldCommandMode> resolvePendingWorldCommandMode(
    const game::CommandButtonTemplate& button,
    ObjectId sourceObject) {
    if (!sourceObject || button.name.empty() ||
        !button.descriptor.userActivatable()) {
        return std::nullopt;
    }

    PendingWorldCommandMode mode;
    mode.sourceObject = sourceObject;
    mode.commandButtonName = button.name;
    mode.cursor.validResource = button.cursorName;
    mode.cursor.invalidResource = button.invalidCursorName;
    mode.cursor.radiusResource = button.radiusCursorType;
    mode.voice.unitSpecificSound = button.unitSpecificSound;
    mode.allowedRelations = relations(button);
    mode.allowShrubberyTarget = game::hasCommandButtonOption(
        button.descriptor.options,
        game::CommandButtonOption::AllowShrubberyTarget);
    mode.allowMineTarget = game::hasCommandButtonOption(
        button.descriptor.options,
        game::CommandButtonOption::AllowMineTarget);

    switch (button.descriptor.kind) {
    case game::CommandButtonKind::AttackMove:
        mode.kind = PendingWorldCommandKind::AttackMove;
        mode.targetKind = button.descriptor.targetKind;
        mode.cursor.kind = PendingWorldCursorKind::AttackMove;
        mode.voice.kind = PendingWorldVoiceKind::AttackMove;
        break;
    case game::CommandButtonKind::Guard:
    case game::CommandButtonKind::GuardWithoutPursuit:
    case game::CommandButtonKind::GuardFlyingUnitsOnly:
        mode.kind = PendingWorldCommandKind::Guard;
        mode.targetKind = button.descriptor.targetKind;
        mode.cursor.kind = PendingWorldCursorKind::Guard;
        mode.voice.kind = PendingWorldVoiceKind::Guard;
        break;
    case game::CommandButtonKind::SpecialPower:
    case game::CommandButtonKind::SpecialPowerFromShortcut:
        mode.kind = PendingWorldCommandKind::SpecialPower;
        mode.targetKind = button.descriptor.targetKind;
        mode.cursor.kind = PendingWorldCursorKind::SpecialPower;
        mode.voice.kind = PendingWorldVoiceKind::SpecialPower;
        break;
    case game::CommandButtonKind::FireWeapon:
        mode.kind = PendingWorldCommandKind::FireWeapon;
        mode.targetKind = button.descriptor.targetKind;
        mode.cursor.kind = PendingWorldCursorKind::FireWeapon;
        mode.voice.kind = PendingWorldVoiceKind::FireWeapon;
        break;
    case game::CommandButtonKind::SetRallyPoint:
        mode.kind = PendingWorldCommandKind::SetRallyPoint;
        mode.targetKind = button.descriptor.targetKind;
        mode.cursor.kind = PendingWorldCursorKind::SetRallyPoint;
        break;
    case game::CommandButtonKind::CombatDrop:
        mode.kind = PendingWorldCommandKind::CombatDrop;
        // Retail accepts both MSG_COMBATDROP_AT_OBJECT and
        // MSG_COMBATDROP_AT_LOCATION regardless of which visible target is
        // under the pointer.
        mode.targetKind = button.descriptor.targetKind;
        mode.cursor.kind = PendingWorldCursorKind::CombatDrop;
        mode.voice.kind = PendingWorldVoiceKind::CombatDrop;
        break;
    case game::CommandButtonKind::HijackVehicle:
    case game::CommandButtonKind::ConvertToCarBomb:
    case game::CommandButtonKind::SabotageBuilding:
        mode.kind = PendingWorldCommandKind::IntentionalContact;
        // These three ZH modes always emit createEnterMessage for an object;
        // terrain under an invalid object must not become a Move fallback.
        mode.targetKind = PendingWorldTargetKind::Object;
        mode.cursor.kind = PendingWorldCursorKind::IntentionalContact;
        mode.voice.kind = PendingWorldVoiceKind::IntentionalContact;
        break;
    default:
        return std::nullopt;
    }

    if (mode.targetKind == PendingWorldTargetKind::None) {
        return std::nullopt;
    }
    return mode;
}

} // namespace engine::selection
