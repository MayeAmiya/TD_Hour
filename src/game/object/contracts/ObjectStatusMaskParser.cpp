#include "game/object/contracts/ObjectDeathReaction.h"

#include "core/container/string_utils.h"

#include <cctype>
#include <optional>

namespace game {
namespace {

constexpr ObjectStatusMask kAllStatuses = objectStatusKnownMask();

using container::asciiEqualIgnoreCase;

[[nodiscard]] container::Vector<container::StringView> splitTokens(
    container::StringView value) {
    container::Vector<container::StringView> tokens;
    size_t cursor = 0;
    while (cursor < value.size()) {
        while (cursor < value.size() &&
               (std::isspace(static_cast<unsigned char>(value[cursor])) ||
                value[cursor] == ',')) {
            ++cursor;
        }
        const size_t begin = cursor;
        while (cursor < value.size() &&
               !std::isspace(static_cast<unsigned char>(value[cursor])) &&
               value[cursor] != ',') {
            ++cursor;
        }
        if (begin != cursor) {
            tokens.push_back(value.substr(begin, cursor - begin));
        }
    }
    return tokens;
}

[[nodiscard]] std::optional<ObjectStatusFlag>
parseStatusToken(container::StringView token) noexcept {
    struct Name final {
        container::StringView token;
        ObjectStatusFlag value;
    };
    // These spellings are the source ObjectStatusMaskType table.  The
    // OBJECT_STATUS_* aliases make API-produced state equally usable without
    // widening ordinary content parsing into a stringly-typed runtime.
    constexpr Name names[] = {
        {"DESTROYED", ObjectStatusFlag::Destroyed},
        {"OBJECT_STATUS_DESTROYED", ObjectStatusFlag::Destroyed},
        {"CAN_ATTACK", ObjectStatusFlag::CanAttack},
        {"OBJECT_STATUS_CAN_ATTACK", ObjectStatusFlag::CanAttack},
        {"UNDER_CONSTRUCTION", ObjectStatusFlag::UnderConstruction},
        {"OBJECT_STATUS_UNDER_CONSTRUCTION", ObjectStatusFlag::UnderConstruction},
        {"UNSELECTABLE", ObjectStatusFlag::Unselectable},
        {"OBJECT_STATUS_UNSELECTABLE", ObjectStatusFlag::Unselectable},
        {"NO_COLLISIONS", ObjectStatusFlag::NoCollisions},
        {"OBJECT_STATUS_NO_COLLISIONS", ObjectStatusFlag::NoCollisions},
        {"NO_ATTACK", ObjectStatusFlag::NoAttack},
        {"OBJECT_STATUS_NO_ATTACK", ObjectStatusFlag::NoAttack},
        {"AIRBORNE_TARGET", ObjectStatusFlag::AirborneTarget},
        {"PARACHUTING", ObjectStatusFlag::Parachuting},
        {"REPULSOR", ObjectStatusFlag::Repulsor},
        {"HIJACKED", ObjectStatusFlag::Hijacked},
        {"AFLAME", ObjectStatusFlag::Aflame},
        {"BURNED", ObjectStatusFlag::Burned},
        {"WET", ObjectStatusFlag::Wet},
        {"IS_FIRING_WEAPON", ObjectStatusFlag::IsFiringWeapon},
        {"IS_BRAKING", ObjectStatusFlag::IsBraking},
        {"STEALTHED", ObjectStatusFlag::Stealthed},
        {"DETECTED", ObjectStatusFlag::Detected},
        {"CAN_STEALTH", ObjectStatusFlag::CanStealth},
        {"SOLD", ObjectStatusFlag::Sold},
        {"UNDERGOING_REPAIR", ObjectStatusFlag::UndergoingRepair},
        {"RECONSTRUCTING", ObjectStatusFlag::Reconstructing},
        {"MASKED", ObjectStatusFlag::Masked},
        {"IS_ATTACKING", ObjectStatusFlag::IsAttacking},
        {"USING_ABILITY", ObjectStatusFlag::IsUsingAbility},
        {"IS_USING_ABILITY", ObjectStatusFlag::IsUsingAbility},
        {"IS_AIMING_WEAPON", ObjectStatusFlag::IsAimingWeapon},
        {"NO_ATTACK_FROM_AI", ObjectStatusFlag::NoAttackFromAi},
        {"IGNORING_STEALTH", ObjectStatusFlag::IgnoringStealth},
        {"IS_CARBOMB", ObjectStatusFlag::IsCarBomb},
        {"DECK_HEIGHT_OFFSET", ObjectStatusFlag::DeckHeightOffset},
        {"STATUS_RIDER1", ObjectStatusFlag::Rider1},
        {"STATUS_RIDER2", ObjectStatusFlag::Rider2},
        {"STATUS_RIDER3", ObjectStatusFlag::Rider3},
        {"STATUS_RIDER4", ObjectStatusFlag::Rider4},
        {"STATUS_RIDER5", ObjectStatusFlag::Rider5},
        {"STATUS_RIDER6", ObjectStatusFlag::Rider6},
        {"STATUS_RIDER7", ObjectStatusFlag::Rider7},
        {"STATUS_RIDER8", ObjectStatusFlag::Rider8},
        {"FAERIE_FIRE", ObjectStatusFlag::FaerieFire},
        {"KILLING_SELF", ObjectStatusFlag::MissileKillingSelf},
        {"MISSILE_KILLING_SELF", ObjectStatusFlag::MissileKillingSelf},
        {"REASSIGN_PARKING", ObjectStatusFlag::ReassignParking},
        {"BOOBY_TRAPPED", ObjectStatusFlag::BoobyTrapped},
        {"IMMOBILE", ObjectStatusFlag::Immobile},
        {"DISGUISED", ObjectStatusFlag::Disguised},
        {"DEPLOYED", ObjectStatusFlag::Deployed},
        {"SCRIPT_UNSTEALTHED", ObjectStatusFlag::ScriptUnstealthed},
        {"OBJECT_STATUS_AIRBORNE_TARGET", ObjectStatusFlag::AirborneTarget},
        {"OBJECT_STATUS_PARACHUTING", ObjectStatusFlag::Parachuting},
        {"OBJECT_STATUS_REPULSOR", ObjectStatusFlag::Repulsor},
        {"OBJECT_STATUS_HIJACKED", ObjectStatusFlag::Hijacked},
        {"OBJECT_STATUS_AFLAME", ObjectStatusFlag::Aflame},
        {"OBJECT_STATUS_BURNED", ObjectStatusFlag::Burned},
        {"OBJECT_STATUS_WET", ObjectStatusFlag::Wet},
        {"OBJECT_STATUS_STEALTHED", ObjectStatusFlag::Stealthed},
        {"OBJECT_STATUS_DETECTED", ObjectStatusFlag::Detected},
        {"OBJECT_STATUS_CAN_STEALTH", ObjectStatusFlag::CanStealth},
        {"OBJECT_STATUS_SOLD", ObjectStatusFlag::Sold},
        {"OBJECT_STATUS_RECONSTRUCTING", ObjectStatusFlag::Reconstructing},
        {"OBJECT_STATUS_MASKED", ObjectStatusFlag::Masked},
        {"OBJECT_STATUS_DEPLOYED", ObjectStatusFlag::Deployed},
        {"OBJECT_STATUS_SCRIPT_UNSTEALTHED", ObjectStatusFlag::ScriptUnstealthed},
        {"OBJECT_STATUS_IMMOBILE", ObjectStatusFlag::Immobile},
        {"OBJECT_STATUS_DISGUISED", ObjectStatusFlag::Disguised},
        {"OBJECT_STATUS_BOOBY_TRAPPED", ObjectStatusFlag::BoobyTrapped},
        {"OBJECT_STATUS_REASSIGN_PARKING", ObjectStatusFlag::ReassignParking},
    };
    for (const Name& name : names) {
        if (asciiEqualIgnoreCase(token, name.token)) return name.value;
    }
    return std::nullopt;
}

[[nodiscard]] ObjectStatusMask parseStatusMask(container::StringView value, bool& resolved, ObjectStatusMask initialMask) {
    ObjectStatusMask mask = initialMask;
    bool sawNormalToken = false;
    bool sawAddOrSubtract = false;
    for (const container::StringView token : splitTokens(value)) {
        if (asciiEqualIgnoreCase(token, "NONE")) {
            if (sawNormalToken || sawAddOrSubtract) resolved = false;
            mask = 0;
            break;
        }
        if (asciiEqualIgnoreCase(token, "ALL")) {
            if (sawNormalToken || sawAddOrSubtract) resolved = false;
            mask = kAllStatuses;
            sawNormalToken = true;
            continue;
        }
        const bool add = !token.empty() && token.front() == '+';
        const bool subtract = !token.empty() && token.front() == '-';
        if ((add || subtract) && sawNormalToken) {
            resolved = false;
            continue;
        }
        if (!add && !subtract && sawAddOrSubtract) {
            resolved = false;
            continue;
        }
        const std::optional<ObjectStatusFlag> status =
            parseStatusToken((add || subtract) ? token.substr(1) : token);
        if (!status) {
            resolved = false;
            continue;
        }
        const ObjectStatusMask bit = objectStatusBit(*status);
        if (subtract) {
            mask &= ~bit;
            sawAddOrSubtract = true;
        } else {
            if (!add && !sawNormalToken) mask = 0;
            mask |= bit;
            if (add) sawAddOrSubtract = true;
            else sawNormalToken = true;
        }
    }
    return mask;
}

} // namespace

ObjectStatusMaskParseResult parseObjectStatusMask(container::StringView value, ObjectStatusMask initialMask)
{
    ObjectStatusMaskParseResult result;
    result.mask = parseStatusMask(value, result.resolved, initialMask);
    return result;
}

} // namespace game

