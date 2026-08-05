#include "core/container/container_types.h"
#include "CommandButtonStore.h"
#include "core/container/string_utils.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "debug/debug.h"

#include <algorithm>
#include <array>

namespace game {
namespace {

using container::asciiEqualIgnoreCase;

struct CommandName final {
    container::StringView name;
    CommandButtonKind kind = CommandButtonKind::Unknown;
};

constexpr std::array kCommandNames{
    CommandName{"NONE", CommandButtonKind::None},
    CommandName{"DOZER_CONSTRUCT", CommandButtonKind::DozerConstruct},
    CommandName{"DOZER_CONSTRUCT_CANCEL", CommandButtonKind::DozerConstructCancel},
    CommandName{"UNIT_BUILD", CommandButtonKind::UnitBuild},
    CommandName{"CANCEL_UNIT_BUILD", CommandButtonKind::CancelUnitBuild},
    CommandName{"PLAYER_UPGRADE", CommandButtonKind::PlayerUpgrade},
    CommandName{"OBJECT_UPGRADE", CommandButtonKind::ObjectUpgrade},
    CommandName{"CANCEL_UPGRADE", CommandButtonKind::CancelUpgrade},
    CommandName{"ATTACK_MOVE", CommandButtonKind::AttackMove},
    CommandName{"GUARD", CommandButtonKind::Guard},
    CommandName{"GUARD_WITHOUT_PURSUIT", CommandButtonKind::GuardWithoutPursuit},
    CommandName{"GUARD_FLYING_UNITS_ONLY", CommandButtonKind::GuardFlyingUnitsOnly},
    CommandName{"STOP", CommandButtonKind::Stop},
    CommandName{"WAYPOINTS", CommandButtonKind::Waypoints},
    CommandName{"EXIT_CONTAINER", CommandButtonKind::ExitContainer},
    CommandName{"EVACUATE", CommandButtonKind::Evacuate},
    CommandName{"EXECUTE_RAILED_TRANSPORT", CommandButtonKind::ExecuteRailedTransport},
    CommandName{"BEACON_DELETE", CommandButtonKind::BeaconDelete},
    CommandName{"SET_RALLY_POINT", CommandButtonKind::SetRallyPoint},
    CommandName{"SELL", CommandButtonKind::Sell},
    CommandName{"FIRE_WEAPON", CommandButtonKind::FireWeapon},
    CommandName{"SPECIAL_POWER", CommandButtonKind::SpecialPower},
    CommandName{"PURCHASE_SCIENCE", CommandButtonKind::PurchaseScience},
    CommandName{"HACK_INTERNET", CommandButtonKind::HackInternet},
    CommandName{"TOGGLE_OVERCHARGE", CommandButtonKind::ToggleOvercharge},
    CommandName{"COMBATDROP", CommandButtonKind::CombatDrop},
    CommandName{"SWITCH_WEAPON", CommandButtonKind::SwitchWeapon},
    CommandName{"HIJACK_VEHICLE", CommandButtonKind::HijackVehicle},
    CommandName{"CONVERT_TO_CARBOMB", CommandButtonKind::ConvertToCarBomb},
    CommandName{"SABOTAGE_BUILDING", CommandButtonKind::SabotageBuilding},
    CommandName{"PLACE_BEACON", CommandButtonKind::PlaceBeacon},
    CommandName{"SPECIAL_POWER_FROM_SHORTCUT", CommandButtonKind::SpecialPowerFromShortcut},
    CommandName{"SPECIAL_POWER_FROM_COMMAND_CENTER", CommandButtonKind::SpecialPowerFromShortcut},
    CommandName{"SPECIAL_POWER_CONSTRUCT", CommandButtonKind::SpecialPowerConstruct},
    CommandName{"SPECIAL_POWER_CONSTRUCT_FROM_SHORTCUT", CommandButtonKind::SpecialPowerConstructFromShortcut},
    CommandName{"SELECT_ALL_UNITS_OF_TYPE", CommandButtonKind::SelectAllUnitsOfType},
};

struct OptionName final {
    container::StringView name;
    CommandButtonOption option = CommandButtonOption::None;
};

constexpr std::array kOptionNames{
    OptionName{"NEED_TARGET_ENEMY_OBJECT", CommandButtonOption::NeedTargetEnemyObject},
    OptionName{"NEED_TARGET_NEUTRAL_OBJECT", CommandButtonOption::NeedTargetNeutralObject},
    OptionName{"NEED_TARGET_ALLY_OBJECT", CommandButtonOption::NeedTargetAllyObject},
    OptionName{"ALLOW_SHRUBBERY_TARGET", CommandButtonOption::AllowShrubberyTarget},
    OptionName{"NEED_TARGET_POS", CommandButtonOption::NeedTargetPosition},
    OptionName{"NEED_UPGRADE", CommandButtonOption::NeedUpgrade},
    OptionName{"NEED_SPECIAL_POWER_SCIENCE", CommandButtonOption::NeedSpecialPowerScience},
    OptionName{"OK_FOR_MULTI_SELECT", CommandButtonOption::OkForMultiSelect},
    OptionName{"CONTEXTMODE_COMMAND", CommandButtonOption::ContextModeCommand},
    OptionName{"CHECK_LIKE", CommandButtonOption::CheckLike},
    OptionName{"ALLOW_MINE_TARGET", CommandButtonOption::AllowMineTarget},
    OptionName{"ATTACK_OBJECTS_POSITION", CommandButtonOption::AttackObjectsPosition},
    OptionName{"OPTION_ONE", CommandButtonOption::OptionOne},
    OptionName{"OPTION_TWO", CommandButtonOption::OptionTwo},
    OptionName{"OPTION_THREE", CommandButtonOption::OptionThree},
    OptionName{"NOT_QUEUEABLE", CommandButtonOption::NotQueueable},
    OptionName{"SINGLE_USE_COMMAND", CommandButtonOption::SingleUseCommand},
    OptionName{"COMMAND_FIRED_BY_SCRIPT", CommandButtonOption::CommandFiredByScript},
    OptionName{"SCRIPT_ONLY", CommandButtonOption::ScriptOnly},
    OptionName{"IGNORES_UNDERPOWERED", CommandButtonOption::IgnoresUnderpowered},
    OptionName{"USES_MINE_CLEARING_WEAPONSET", CommandButtonOption::UsesMineClearingWeaponset},
    OptionName{"CAN_USE_WAYPOINTS", CommandButtonOption::CanUseWaypoints},
    OptionName{"MUST_BE_STOPPED", CommandButtonOption::MustBeStopped},
};

[[nodiscard]] constexpr bool isOptionTokenCharacter(char value) noexcept {
    return (value >= 'a' && value <= 'z') ||
        (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9') || value == '_';
}

[[nodiscard]] uint64_t stableButtonId(container::StringView name) noexcept {
    constexpr uint64_t kOffset = 14695981039346656037ull;
    constexpr uint64_t kPrime = 1099511628211ull;
    uint64_t hash = kOffset;
    for (const unsigned char value : name) {
        hash ^= value;
        hash *= kPrime;
    }
    return hash == 0 ? 1 : hash;
}

[[nodiscard]] CommandButtonKind parseCommandKind(
    container::StringView value) noexcept {
    if (container::trimAsciiView(value).empty()) {
        // NonCommand_* and similar ControlBar entries are passive UI state
        // slots. They deliberately have no GUI command and must remain
        // displayable while being non-activatable.
        return CommandButtonKind::None;
    }
    for (const CommandName& candidate : kCommandNames) {
        if (asciiEqualIgnoreCase(value, candidate.name)) return candidate.kind;
    }
    return CommandButtonKind::Unknown;
}

[[nodiscard]] uint32_t parseOptions(container::StringView options) noexcept {
    uint32_t result = 0;
    size_t cursor = 0;
    while (cursor < options.size()) {
        while (cursor < options.size() &&
               !isOptionTokenCharacter(options[cursor])) {
            ++cursor;
        }
        const size_t begin = cursor;
        while (cursor < options.size() &&
               isOptionTokenCharacter(options[cursor])) {
            ++cursor;
        }
        if (begin == cursor) continue;
        const container::StringView token = options.substr(begin, cursor - begin);
        for (const OptionName& candidate : kOptionNames) {
            if (asciiEqualIgnoreCase(token, candidate.name)) {
                result |= static_cast<uint32_t>(candidate.option);
                break;
            }
        }
    }
    return result;
}

[[nodiscard]] container::StringView firstLookupToken(
    container::StringView value) noexcept {
    size_t cursor = 0;
    while (cursor < value.size() && !isOptionTokenCharacter(value[cursor])) {
        ++cursor;
    }
    const size_t begin = cursor;
    while (cursor < value.size() && isOptionTokenCharacter(value[cursor])) {
        ++cursor;
    }
    return value.substr(begin, cursor - begin);
}

[[nodiscard]] CommandButtonBorderType parseButtonBorderType(
    container::StringView value) noexcept {
    const container::StringView token = firstLookupToken(value);
    if (asciiEqualIgnoreCase(token, "BUILD")) {
        return CommandButtonBorderType::Build;
    }
    if (asciiEqualIgnoreCase(token, "UPGRADE")) {
        return CommandButtonBorderType::Upgrade;
    }
    if (asciiEqualIgnoreCase(token, "ACTION")) {
        return CommandButtonBorderType::Action;
    }
    if (asciiEqualIgnoreCase(token, "SYSTEM")) {
        return CommandButtonBorderType::System;
    }
    return CommandButtonBorderType::None;
}

// RefCode parses this field with INI::parseLookupList, which consumes exactly
// one token: the authored "WeaponSlot = SECONDARY TERTIARY" of
// Command_ChinaNeutronWarhead resolves to SECONDARY and the trailing token is
// silently discarded. The spellings mirror TheWeaponSlotTypeNamesLookupList and
// game::WeaponSlot; they are duplicated here rather than linked because
// game_base must not acquire a reverse dependency on game_object.
[[nodiscard]] uint8_t parseWeaponSlot(container::StringView value) noexcept {
    const container::StringView token = firstLookupToken(value);
    if (asciiEqualIgnoreCase(token, "SECONDARY")) return 1;
    if (asciiEqualIgnoreCase(token, "TERTIARY")) return 2;
    return 0;
}

[[nodiscard]] bool hasObjectTarget(uint32_t options) noexcept {
    constexpr uint32_t kObjectTargetMask =
        static_cast<uint32_t>(CommandButtonOption::NeedTargetEnemyObject) |
        static_cast<uint32_t>(CommandButtonOption::NeedTargetNeutralObject) |
        static_cast<uint32_t>(CommandButtonOption::NeedTargetAllyObject);
    return (options & kObjectTargetMask) != 0;
}

[[nodiscard]] CommandButtonTargetKind targetKind(
    CommandButtonKind kind, uint32_t options) noexcept {
    if (kind == CommandButtonKind::AttackMove ||
        kind == CommandButtonKind::SetRallyPoint) {
        return CommandButtonTargetKind::Position;
    }
    if (kind == CommandButtonKind::CombatDrop) {
        return CommandButtonTargetKind::ObjectOrPosition;
    }
    if (hasCommandButtonOption(options,
            CommandButtonOption::ContextModeCommand)) {
        return CommandButtonTargetKind::Contextual;
    }
    const bool object = hasObjectTarget(options);
    const bool position = hasCommandButtonOption(
        options, CommandButtonOption::NeedTargetPosition);
    if (object && position) return CommandButtonTargetKind::ObjectOrPosition;
    if (object) return CommandButtonTargetKind::Object;
    if (position) return CommandButtonTargetKind::Position;
    return CommandButtonTargetKind::None;
}

[[nodiscard]] CommandButtonActorPolicy actorPolicy(
    CommandButtonKind kind, uint32_t options) noexcept {
    if (kind == CommandButtonKind::SpecialPowerFromShortcut ||
        kind == CommandButtonKind::SpecialPowerConstructFromShortcut) {
        return CommandButtonActorPolicy::LocalPlayerCommandCenter;
    }
    if (kind == CommandButtonKind::PurchaseScience ||
        kind == CommandButtonKind::PlaceBeacon ||
        kind == CommandButtonKind::SelectAllUnitsOfType) {
        return CommandButtonActorPolicy::LocalPlayer;
    }
    return hasCommandButtonOption(options,
               CommandButtonOption::OkForMultiSelect)
        ? CommandButtonActorPolicy::AllEligibleSelectedObjects
        : CommandButtonActorPolicy::PrimarySelectedObject;
}

[[nodiscard]] CommandButtonCostSource costSource(
    CommandButtonKind kind) noexcept {
    switch (kind) {
    case CommandButtonKind::DozerConstruct:
    case CommandButtonKind::UnitBuild:
    case CommandButtonKind::SpecialPowerConstruct:
    case CommandButtonKind::SpecialPowerConstructFromShortcut:
        return CommandButtonCostSource::ObjectTemplate;
    case CommandButtonKind::PlayerUpgrade:
    case CommandButtonKind::ObjectUpgrade:
        return CommandButtonCostSource::Upgrade;
    case CommandButtonKind::PurchaseScience:
        return CommandButtonCostSource::Science;
    default:
        return CommandButtonCostSource::None;
    }
}

[[nodiscard]] CommandButtonCooldownSource cooldownSource(
    CommandButtonKind kind) noexcept {
    switch (kind) {
    case CommandButtonKind::FireWeapon:
    case CommandButtonKind::SwitchWeapon:
        return CommandButtonCooldownSource::ActorWeapon;
    case CommandButtonKind::SpecialPower:
    case CommandButtonKind::SpecialPowerFromShortcut:
    case CommandButtonKind::SpecialPowerConstruct:
    case CommandButtonKind::SpecialPowerConstructFromShortcut:
        return CommandButtonCooldownSource::SpecialPower;
    default:
        return CommandButtonCooldownSource::None;
    }
}

[[nodiscard]] bool requiredReferencesPresent(
    const CommandButtonTemplate& button) noexcept {
    switch (button.descriptor.costSource) {
    case CommandButtonCostSource::ObjectTemplate:
        if (button.object.empty()) return false;
        break;
    case CommandButtonCostSource::Upgrade:
        if (button.upgrade.empty()) return false;
        break;
    case CommandButtonCostSource::Science:
        if (button.science.empty()) return false;
        break;
    case CommandButtonCostSource::None:
        break;
    }
    if (button.descriptor.cooldownSource ==
            CommandButtonCooldownSource::SpecialPower &&
        button.specialPower.empty()) {
        return false;
    }
    return true;
}

void compileDescriptor(CommandButtonTemplate& button) noexcept {
    CommandButtonDescriptor& descriptor = button.descriptor;
    descriptor.stableId = stableButtonId(button.name);
    descriptor.kind = parseCommandKind(button.command);
    descriptor.options = parseOptions(button.options);
    descriptor.weaponSlot = parseWeaponSlot(button.weaponSlot);
    descriptor.targetKind = targetKind(descriptor.kind, descriptor.options);
    descriptor.actorPolicy = actorPolicy(descriptor.kind, descriptor.options);
    descriptor.costSource = costSource(descriptor.kind);
    descriptor.cooldownSource = cooldownSource(descriptor.kind);
    descriptor.requiredReferencesPresent = requiredReferencesPresent(button);
}

void applyCommandButtonField(CommandButtonTemplate& button,
                             const container::String& key,
                             const container::String& value) {
    // The legacy object has one storage location per FieldParse entry. Keep
    // the compatibility list flattened the same way so repeated modifiers do
    // not make reverse field lookup progressively longer.
    auto existing = std::find_if(
        button.fields.rbegin(), button.fields.rend(),
        [&key](const auto& field) { return field.first == key; });
    if (existing == button.fields.rend()) {
        button.fields.emplace_back(key, value);
    } else {
        existing->second = value;
    }
    if (key == "Command") button.command = value;
    else if (key == "Options") button.options = value;
    else if (key == "WeaponSlot") button.weaponSlot = value;
    else if (key == "Object") button.object = value;
    else if (key == "Science") {
        button.sciences.clear();
        size_t cursor = 0;
        while (cursor < value.size()) {
            while (cursor < value.size() &&
                   (container::isAsciiWhitespace(value[cursor]) ||
                    value[cursor] == ',')) {
                ++cursor;
            }
            const size_t begin = cursor;
            while (cursor < value.size() &&
                   !container::isAsciiWhitespace(value[cursor]) &&
                   value[cursor] != ',') {
                ++cursor;
            }
            if (begin != cursor) {
                const container::StringView science =
                    value.substr(begin, cursor - begin);
                // INI::parseScienceVector treats None as a vector-reset
                // sentinel, not as one optional prerequisite.  Normalize it
                // when the authored field is parsed so every consumer sees
                // the same empty requirement set.  Leaving `None` beside a
                // real science made the ControlBar require the real science
                // while authoritative production correctly accepted it.
                if (container::asciiEqualIgnoreCase(science, "None")) {
                    button.sciences.clear();
                    break;
                }
                button.sciences.emplace_back(science);
            }
        }
        button.science = button.sciences.empty()
            ? container::String{} : button.sciences.front();
    }
    else if (key == "TextLabel") button.textLabel = value;
    // Shipped INI and RefCode's s_commandButtonFieldParseTable both spell this
    // key "DescriptLabel". "DescriptionLabel" never appears in authored data,
    // so matching it discarded every command button tooltip.
    else if (key == "DescriptLabel") button.descriptionLabel = value;
    else if (key == "ButtonImage") button.buttonImage = value;
    else if (key == "ButtonBorderType") {
        button.borderType = parseButtonBorderType(value);
    }
    else if (key == "RadiusCursor") button.radiusCursor = value;
    else if (key == "CursorName") button.cursorName = value;
    else if (key == "InvalidCursorName") button.invalidCursorName = value;
    else if (key == "RadiusCursorType") button.radiusCursorType = value;
    else if (key == "UnitSpecificSound") button.unitSpecificSound = value;
    else if (key == "SpecialPower") button.specialPower = value;
    else if (key == "Upgrade") button.upgrade = value;
}

} // namespace

CommandButtonStore& CommandButtonStore::instance() {
    static CommandButtonStore s_instance;
    return s_instance;
}

void CommandButtonStore::clear() {
    m_buttons.clear();
}

bool CommandButtonStore::loadFromIni(
    container::StringView filePath, ini::LegacyIniLoadType loadType) {
    GeneralsIniParser parser;
    const container::String path{filePath};
    if (!parser.parseFile(path)) {
        return false;
    }

    for (const auto& block : parser.blocks()) {
        if (block.type != "CommandButton") continue;

        if (block.name.empty()) continue;

        CommandButtonTemplate button;
        const auto existing = m_buttons.find(block.name);
        if (existing != m_buttons.end()) {
            // ZH patches an existing CommandButton in-place for ordinary
            // loads. CreateOverrides first copies the final override and then
            // patches it. A flattened immutable store has the same effective
            // value in both cases, so preserve every field not re-authored.
            button = existing->second;
            if (!ini::createsOverrides(loadType)) {
                TD_LOG_WARN(
                    "[CommandButtonStore] Duplicate CommandButton '{}' is patched in source order, matching ZH Overwrite behavior",
                    block.name);
            }
        }
        button.name = block.name;

        for (const auto& [key, value] : block.values) {
            applyCommandButtonField(button, key, value);
        }

        compileDescriptor(button);
        if (button.descriptor.kind == CommandButtonKind::Unknown) {
            TD_LOG_WARN(
                "[CommandButtonStore] CommandButton '{}' has unsupported Command '{}'; player activation is disabled",
                button.name, button.command);
        } else if (!button.descriptor.requiredReferencesPresent) {
            TD_LOG_WARN(
                "[CommandButtonStore] CommandButton '{}' is missing a required content reference; player activation is disabled",
                button.name);
        }
        m_buttons.insert_or_assign(button.name, std::move(button));
    }

    TD_LOG_INFO("[CommandButtonStore] Loaded {} command buttons from {}", m_buttons.size(), path);
    return true;
}

const CommandButtonTemplate* CommandButtonStore::find(container::StringView name) const {
    auto it = m_buttons.find(container::String{name});
    return it != m_buttons.end() ? &it->second : nullptr;
}

} // namespace game
