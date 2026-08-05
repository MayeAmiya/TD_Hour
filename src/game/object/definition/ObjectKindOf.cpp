#include "game/object/definition/ObjectKindOf.h"

#include "core/container/string_utils.h"

namespace game {
namespace {

constexpr container::Array<container::StringView, kObjectKindOfCount>
    kObjectKindOfNames{
        "OBSTACLE", "SELECTABLE", "IMMOBILE", "CAN_ATTACK",
        "STICK_TO_TERRAIN_SLOPE", "CAN_CAST_REFLECTIONS", "SHRUBBERY",
        "STRUCTURE", "INFANTRY", "VEHICLE", "AIRCRAFT", "HUGE_VEHICLE",
        "DOZER", "HARVESTER", "COMMANDCENTER", "LINEBUILD", "SALVAGER",
        "WEAPON_SALVAGER", "TRANSPORT", "BRIDGE", "LANDMARK_BRIDGE",
        "BRIDGE_TOWER", "PROJECTILE", "PRELOAD", "NO_GARRISON",
        "WAVEGUIDE", "WAVE_EFFECT", "NO_COLLIDE", "REPAIR_PAD", "HEAL_PAD",
        "STEALTH_GARRISON", "CASH_GENERATOR", "AIRFIELD", "DRAWABLE_ONLY",
        "MP_COUNT_FOR_VICTORY", "REBUILD_HOLE", "SCORE", "SCORE_CREATE",
        "SCORE_DESTROY", "NO_HEAL_ICON", "CAN_RAPPEL", "PARACHUTABLE",
        "CAN_BE_REPULSED", "MOB_NEXUS", "IGNORED_IN_GUI", "CRATE",
        "CAPTURABLE", "CLEARED_BY_BUILD", "SMALL_MISSILE", "ALWAYS_VISIBLE",
        "UNATTACKABLE", "MINE", "CLEANUP_HAZARD", "PORTABLE_STRUCTURE",
        "ALWAYS_SELECTABLE", "ATTACK_NEEDS_LINE_OF_SIGHT",
        "WALK_ON_TOP_OF_WALL", "DEFENSIVE_WALL", "FS_POWER", "FS_FACTORY",
        "FS_BASE_DEFENSE", "FS_TECHNOLOGY", "AIRCRAFT_PATH_AROUND",
        "LOW_OVERLAPPABLE", "FORCEATTACKABLE", "AUTO_RALLYPOINT",
        "TECH_BUILDING", "POWERED", "PRODUCED_AT_HELIPAD", "DRONE",
        "CAN_SEE_THROUGH_STRUCTURE", "BALLISTIC_MISSILE", "CLICK_THROUGH",
        "SUPPLY_SOURCE_ON_PREVIEW", "PARACHUTE",
        "GARRISONABLE_UNTIL_DESTROYED", "BOAT", "IMMUNE_TO_CAPTURE", "HULK",
        "SHOW_PORTRAIT_WHEN_CONTROLLED", "SPAWNS_ARE_THE_WEAPONS",
        "CANNOT_BUILD_NEAR_SUPPLIES", "SUPPLY_SOURCE", "REVEAL_TO_ALL",
        "DISGUISER", "INERT", "HERO", "IGNORES_SELECT_ALL",
        "DONT_AUTO_CRUSH_INFANTRY", "CLIFF_JUMPER", "FS_SUPPLY_DROPZONE",
        "FS_SUPERWEAPON", "FS_BLACK_MARKET", "FS_SUPPLY_CENTER",
        "FS_STRATEGY_CENTER", "MONEY_HACKER", "ARMOR_SALVAGER",
        "REVEALS_ENEMY_PATHS", "BOOBY_TRAP", "FS_FAKE", "FS_INTERNET_CENTER",
        "BLAST_CRATER", "PROP", "OPTIMIZED_TREE", "FS_ADVANCED_TECH",
        "FS_BARRACKS", "FS_WARFACTORY", "FS_AIRFIELD", "AIRCRAFT_CARRIER",
        "NO_SELECT", "REJECT_UNMANNED", "CANNOT_RETALIATE",
        "TECH_BASE_DEFENSE", "EMP_HARDENED", "DEMOTRAP",
        "CONSERVATIVE_BUILDING", "IGNORE_DOCKING_BONES",
    };

[[nodiscard]] bool isSpace(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' ||
           value == '\n' || value == ',';
}

} // namespace

std::optional<ObjectKindOf>
parseObjectKindOf(container::StringView name) noexcept {
    for (size_t index = 0; index < kObjectKindOfNames.size(); ++index) {
        if (container::asciiEqualIgnoreCase(name, kObjectKindOfNames[index])) {
            return static_cast<ObjectKindOf>(index);
        }
    }
    return std::nullopt;
}

container::StringView objectKindOfName(ObjectKindOf kind) noexcept {
    const size_t index = objectKindOfIndex(kind);
    return index < kObjectKindOfNames.size() ? kObjectKindOfNames[index]
                                             : container::StringView{};
}

bool compileObjectKindOfMask(
    container::StringView text, ObjectKindOfMask& output,
    container::Vector<container::String>* unknownTokens) {
    output.clear();
    bool valid = true;
    while (!text.empty()) {
        while (!text.empty() && isSpace(text.front())) text.remove_prefix(1);
        if (text.empty()) break;
        size_t length = 0;
        while (length < text.size() && !isSpace(text[length])) ++length;
        container::StringView token = text.substr(0, length);
        text.remove_prefix(length);

        bool remove = false;
        while (!token.empty() && (token.front() == '+' || token.front() == '-')) {
            remove = token.front() == '-';
            token.remove_prefix(1);
        }
        if (token.empty() || container::asciiEqualIgnoreCase(token, "NONE")) continue;
        const std::optional<ObjectKindOf> kind = parseObjectKindOf(token);
        if (!kind) {
            valid = false;
            if (unknownTokens) unknownTokens->emplace_back(token);
            continue;
        }
        setObjectKind(output, *kind, !remove);
    }
    return valid;
}

} // namespace game
