#pragma once

#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include <cstddef>
#include <cstdint>
#include <optional>
namespace game {

enum class DamageType : uint8_t {
    EXPLOSION = 0,
    CRUSH,
    ARMOR_PIERCING,
    SMALL_ARMS,
    GATTLING,
    RADIATION,
    FLAME,
    LASER,
    SNIPER,
    POISON,
    HEALING,
    UNRESISTABLE,
    WATER,
    DEPLOY,
    SURRENDER,
    HACK,
    KILL_PILOT,
    PENALTY,
    FALLING,
    MELEE,
    DISARM,
    HAZARD_CLEANUP,
    PARTICLE_BEAM,
    TOPPLING,
    INFANTRY_MISSILE,
    AURORA_BOMB,
    LAND_MINE,
    JET_MISSILES,
    STEALTHJET_MISSILES,
    MOLOTOV_COCKTAIL,
    COMANCHE_VULCAN,
    SUBDUAL_MISSILE,
    SUBDUAL_VEHICLE,
    SUBDUAL_BUILDING,
    SUBDUAL_UNRESISTABLE,
    MICROWAVE,
    KILL_GARRISONED,
    STATUS,
    COUNT
};

enum class DeathType : uint8_t {
    NORMAL = 0,
    // RefCode deliberately assigns NONE a real serialized value.  We do not
    // support old save/replay binaries, but preserving the authored enum
    // order prevents legacy Weapon.ini values from being silently mapped to
    // the wrong death reaction.
    NONE,
    CRUSHED,
    BURNED,
    EXPLODED,
    POISONED,
    TOPPLED,
    FLOODED,
    SUICIDED,
    LASERED,
    DETONATED,
    SPLATTED,
    POISONED_BETA,
    EXTRA_2,
    EXTRA_3,
    EXTRA_4,
    EXTRA_5,
    EXTRA_6,
    EXTRA_7,
    EXTRA_8,
    POISONED_GAMMA,
    COUNT
};

inline constexpr int DAMAGE_TYPE_COUNT = static_cast<int>(DamageType::COUNT);

[[nodiscard]] inline bool equalDamageTypeToken(container::StringView lhs, container::StringView rhs) noexcept {
    return container::asciiEqualIgnoreCase(lhs, rhs);
}

// Unlike the historical fallback parser below, this typed form distinguishes
// an unknown token from EXPLOSION. Content compilers use it for bit masks so
// a typo cannot silently route damage as a different type.
[[nodiscard]] inline std::optional<DamageType> tryParseDamageType(container::StringView value) noexcept {
    constexpr container::Array<container::StringView, static_cast<std::size_t>(DamageType::COUNT)> kNames = {
        "EXPLOSION", "CRUSH", "ARMOR_PIERCING", "SMALL_ARMS", "GATTLING", "RADIATION",
        "FLAME", "LASER", "SNIPER", "POISON", "HEALING", "UNRESISTABLE", "WATER",
        "DEPLOY", "SURRENDER", "HACK", "KILL_PILOT", "PENALTY", "FALLING", "MELEE",
        "DISARM", "HAZARD_CLEANUP", "PARTICLE_BEAM", "TOPPLING", "INFANTRY_MISSILE",
        "AURORA_BOMB", "LAND_MINE", "JET_MISSILES", "STEALTHJET_MISSILES",
        "MOLOTOV_COCKTAIL", "COMANCHE_VULCAN", "SUBDUAL_MISSILE", "SUBDUAL_VEHICLE",
        "SUBDUAL_BUILDING", "SUBDUAL_UNRESISTABLE", "MICROWAVE", "KILL_GARRISONED", "STATUS",
    };
    for (std::size_t index = 0; index < kNames.size(); ++index) {
        if (equalDamageTypeToken(value, kNames[index])) {
            return static_cast<DamageType>(index);
        }
    }
    return std::nullopt;
}

inline DamageType parseDamageType(container::StringView s) {
    if (const std::optional<DamageType> parsed = tryParseDamageType(s)) return *parsed;
    if (s == "EXPLOSION") return DamageType::EXPLOSION;
    if (s == "CRUSH") return DamageType::CRUSH;
    if (s == "ARMOR_PIERCING") return DamageType::ARMOR_PIERCING;
    if (s == "SMALL_ARMS") return DamageType::SMALL_ARMS;
    if (s == "GATTLING") return DamageType::GATTLING;
    if (s == "RADIATION") return DamageType::RADIATION;
    if (s == "FLAME") return DamageType::FLAME;
    if (s == "LASER") return DamageType::LASER;
    if (s == "SNIPER") return DamageType::SNIPER;
    if (s == "POISON") return DamageType::POISON;
    if (s == "HEALING") return DamageType::HEALING;
    if (s == "UNRESISTABLE") return DamageType::UNRESISTABLE;
    if (s == "WATER") return DamageType::WATER;
    if (s == "DEPLOY") return DamageType::DEPLOY;
    if (s == "SURRENDER") return DamageType::SURRENDER;
    if (s == "HACK") return DamageType::HACK;
    if (s == "KILL_PILOT") return DamageType::KILL_PILOT;
    if (s == "PENALTY") return DamageType::PENALTY;
    if (s == "FALLING") return DamageType::FALLING;
    if (s == "MELEE") return DamageType::MELEE;
    if (s == "DISARM") return DamageType::DISARM;
    if (s == "HAZARD_CLEANUP") return DamageType::HAZARD_CLEANUP;
    if (s == "PARTICLE_BEAM") return DamageType::PARTICLE_BEAM;
    if (s == "TOPPLING") return DamageType::TOPPLING;
    if (s == "INFANTRY_MISSILE") return DamageType::INFANTRY_MISSILE;
    if (s == "AURORA_BOMB") return DamageType::AURORA_BOMB;
    if (s == "LAND_MINE") return DamageType::LAND_MINE;
    if (s == "JET_MISSILES") return DamageType::JET_MISSILES;
    if (s == "STEALTHJET_MISSILES") return DamageType::STEALTHJET_MISSILES;
    if (s == "MOLOTOV_COCKTAIL") return DamageType::MOLOTOV_COCKTAIL;
    if (s == "COMANCHE_VULCAN") return DamageType::COMANCHE_VULCAN;
    if (s == "SUBDUAL_MISSILE") return DamageType::SUBDUAL_MISSILE;
    if (s == "SUBDUAL_VEHICLE") return DamageType::SUBDUAL_VEHICLE;
    if (s == "SUBDUAL_BUILDING") return DamageType::SUBDUAL_BUILDING;
    if (s == "SUBDUAL_UNRESISTABLE") return DamageType::SUBDUAL_UNRESISTABLE;
    if (s == "MICROWAVE") return DamageType::MICROWAVE;
    if (s == "KILL_GARRISONED") return DamageType::KILL_GARRISONED;
    if (s == "STATUS") return DamageType::STATUS;
    return DamageType::EXPLOSION;
}

inline DeathType parseDeathType(container::StringView s) {
    constexpr container::Array<container::StringView, static_cast<std::size_t>(DeathType::COUNT)> kNames = {
        "NORMAL", "NONE", "CRUSHED", "BURNED", "EXPLODED", "POISONED", "TOPPLED",
        "FLOODED", "SUICIDED", "LASERED", "DETONATED", "SPLATTED", "POISONED_BETA",
        "EXTRA_2", "EXTRA_3", "EXTRA_4", "EXTRA_5", "EXTRA_6", "EXTRA_7", "EXTRA_8",
        "POISONED_GAMMA",
    };
    for (std::size_t index = 0; index < kNames.size(); ++index) {
        if (equalDamageTypeToken(s, kNames[index])) return static_cast<DeathType>(index);
    }
    return DeathType::NORMAL;
}

} // namespace game
