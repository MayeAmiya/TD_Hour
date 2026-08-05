#pragma once

#include "core/container/hash_containers.h"
#include "game/command/CommandButtonTypes.h"
#include "game/data/base/LegacyIniLoadType.h"

#include <cstdint>

namespace game {

// Values intentionally match ZH CommandOption so the frozen bit set remains
// suitable for command serialization and availability checks.
enum class CommandButtonOption : uint32_t {
    None                       = 0x00000000u,
    NeedTargetEnemyObject      = 0x00000001u,
    NeedTargetNeutralObject    = 0x00000002u,
    NeedTargetAllyObject       = 0x00000004u,
    AllowShrubberyTarget       = 0x00000010u,
    NeedTargetPosition         = 0x00000020u,
    NeedUpgrade                = 0x00000040u,
    NeedSpecialPowerScience    = 0x00000080u,
    OkForMultiSelect           = 0x00000100u,
    ContextModeCommand         = 0x00000200u,
    CheckLike                  = 0x00000400u,
    AllowMineTarget            = 0x00000800u,
    AttackObjectsPosition      = 0x00001000u,
    OptionOne                  = 0x00002000u,
    OptionTwo                  = 0x00004000u,
    OptionThree                = 0x00008000u,
    NotQueueable               = 0x00010000u,
    SingleUseCommand           = 0x00020000u,
    CommandFiredByScript       = 0x00040000u,
    ScriptOnly                 = 0x00080000u,
    IgnoresUnderpowered        = 0x00100000u,
    UsesMineClearingWeaponset  = 0x00200000u,
    CanUseWaypoints            = 0x00400000u,
    MustBeStopped              = 0x00800000u,
};

[[nodiscard]] constexpr uint32_t operator|(
    CommandButtonOption lhs, CommandButtonOption rhs) noexcept {
    return static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs);
}

[[nodiscard]] constexpr bool hasCommandButtonOption(
    uint32_t options, CommandButtonOption option) noexcept {
    return (options & static_cast<uint32_t>(option)) != 0;
}

enum class CommandButtonTargetKind : uint8_t {
    None,
    Position,
    Object,
    ObjectOrPosition,
    Contextual,
};

// The policy describes which selected actors a later unified router may use.
// It does not copy a mutable selection into content.
enum class CommandButtonActorPolicy : uint8_t {
    PrimarySelectedObject,
    AllEligibleSelectedObjects,
    LocalPlayer,
    LocalPlayerCommandCenter,
};

// Cost and cooldown values continue to live in the referenced Object,
// Upgrade, Science, SpecialPower or Weapon definitions. These enums identify
// that single authoritative source; the descriptor never caches a second
// numeric value which could drift from gameplay admission.
enum class CommandButtonCostSource : uint8_t {
    None,
    ObjectTemplate,
    Upgrade,
    Science,
};

enum class CommandButtonCooldownSource : uint8_t {
    None,
    ActorWeapon,
    SpecialPower,
};

struct CommandButtonDescriptor final {
    uint64_t stableId = 0;
    CommandButtonKind kind = CommandButtonKind::Unknown;
    CommandButtonTargetKind targetKind = CommandButtonTargetKind::None;
    CommandButtonActorPolicy actorPolicy =
        CommandButtonActorPolicy::PrimarySelectedObject;
    CommandButtonCostSource costSource = CommandButtonCostSource::None;
    CommandButtonCooldownSource cooldownSource =
        CommandButtonCooldownSource::None;
    uint32_t options = 0;
    // Compiled CommandButton "WeaponSlot" as a game::WeaponSlot index
    // (0 PRIMARY / 1 SECONDARY / 2 TERTIARY), matching RefCode's
    // CommandButton::m_weaponSlot and getWeaponSlot(). This is the only weapon
    // slot carrier: OPTION_ONE/TWO/THREE are Strategy Center battle-plan
    // identity bits (BattlePlanUpdate::getCommandOption) and must never be
    // read as a slot. The field is deliberately a plain index so game_base
    // does not acquire a reverse dependency on game_object's WeaponSlot enum.
    uint8_t weaponSlot = 0;
    bool requiredReferencesPresent = true;

    [[nodiscard]] bool recognized() const noexcept {
        return kind != CommandButtonKind::Unknown &&
            kind != CommandButtonKind::None;
    }

    [[nodiscard]] bool userActivatable() const noexcept {
        return stableId != 0 && recognized() && requiredReferencesPresent &&
            !hasCommandButtonOption(options, CommandButtonOption::ScriptOnly);
    }

    friend bool operator==(const CommandButtonDescriptor&,
                           const CommandButtonDescriptor&) = default;
};

struct CommandButtonTemplate {
    container::String name;
    CommandButtonDescriptor descriptor;
    CommandButtonBorderType borderType = CommandButtonBorderType::None;
    // Raw authored spellings remain temporarily for script/simulation
    // consumers not yet migrated by K-006. ControlBar activation must use the
    // compiled descriptor above.
    container::String command;
    container::String options;
    container::String weaponSlot;
    container::String textLabel;
    container::String descriptionLabel;
    container::String buttonImage;
    container::String radiusCursor;
    container::String cursorName;
    container::String invalidCursorName;
    container::String radiusCursorType;
    container::String unitSpecificSound;
    // Frequently queried authoritative references are compiled once instead
    // of reparsing the generic field list in every script/AI consumer.
    container::String object;
    container::String science;
    container::Vector<container::String> sciences;
    container::String specialPower;
    container::String upgrade;
    container::Vector<std::pair<container::String, container::String>> fields;
};

class CommandButtonStore {
public:
    static CommandButtonStore& instance();

    void clear();
    bool loadFromIni(
        container::StringView filePath,
        ini::LegacyIniLoadType loadType = ini::LegacyIniLoadType::Overwrite);
    const CommandButtonTemplate* find(container::StringView name) const;
    const container::HashMap<container::String, CommandButtonTemplate>& all() const { return m_buttons; }

private:
    container::HashMap<container::String, CommandButtonTemplate> m_buttons;
};

} // namespace game
