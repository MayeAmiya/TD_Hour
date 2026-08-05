#pragma once

#include "core/container/container_types.h"

#include "game/base/GameSettings.h"

#include <charconv>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <system_error>

namespace engine {

// Match slots are the eight command-authoring seats exposed by lobby/network
// code. They are not the complete map Player namespace.
struct MatchPlayerSlotId final {
    uint8_t value = 0xff;

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return value < static_cast<uint8_t>(MAX_SLOTS);
    }
    constexpr explicit operator bool() const noexcept { return isValid(); }
    constexpr auto operator<=>(const MatchPlayerSlotId&) const noexcept = default;
};

inline constexpr MatchPlayerSlotId INVALID_MATCH_PLAYER_SLOT_ID{};

// RefCode's SidesList/PlayerList can represent sixteen map players while a
// multiplayer game has only eight command slots. Keep those namespaces
// separate now, before scenario/script ownership is connected to ECS.
inline constexpr size_t MAP_PLAYER_COUNT = 16;

struct PlayerId final {
    uint8_t value = 0xff;

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return value <= static_cast<uint8_t>(MAP_PLAYER_COUNT);
    }
    [[nodiscard]] constexpr bool isMapPlayer() const noexcept {
        return value < static_cast<uint8_t>(MAP_PLAYER_COUNT);
    }
    [[nodiscard]] constexpr bool isNeutral() const noexcept {
        return value == static_cast<uint8_t>(MAP_PLAYER_COUNT);
    }
    constexpr explicit operator bool() const noexcept { return isValid(); }
    constexpr auto operator<=>(const PlayerId&) const noexcept = default;

    [[nodiscard]] static constexpr PlayerId fromSlot(MatchPlayerSlotId slot) noexcept {
        return slot ? PlayerId{slot.value} : PlayerId{};
    }
};

inline constexpr PlayerId INVALID_PLAYER_ID{};
// Neutral has a stable extra identity, never inferred from roster density or
// allowed to collide with Player_0..Player_15 map aliases.
inline constexpr PlayerId NEUTRAL_PLAYER_ID{static_cast<uint8_t>(MAP_PLAYER_COUNT)};

// `Player_<N>` is our explicit, generated map-player spelling.  It is not a
// grammar for arbitrary legacy SidesList player names: shipped campaign maps
// legitimately contain names such as `player0001`, which RefCode treats as
// ordinary authored names.  Keeping the underscore and canonical decimal
// form mandatory prevents those names from being silently rewritten to a
// different PlayerId by map import, scripts or scenario compilation.
[[nodiscard]] inline std::optional<PlayerId> parseCanonicalMapPlayerAlias(
    container::StringView alias) noexcept {
    constexpr container::StringView prefix = "Player_";
    if (alias.size() <= prefix.size()) return std::nullopt;
    for (size_t index = 0; index < prefix.size(); ++index) {
        const char value = alias[index];
        const char lower = value >= 'A' && value <= 'Z'
            ? static_cast<char>(value + ('a' - 'A')) : value;
        if (lower != static_cast<char>(prefix[index] >= 'A' && prefix[index] <= 'Z'
                                       ? prefix[index] + ('a' - 'A') : prefix[index])) {
            return std::nullopt;
        }
    }

    const container::StringView digits = alias.substr(prefix.size());
    // `Player_0` is valid, `Player_00` is intentionally not: the latter can
    // be a real author-facing SidesList name rather than a generated alias.
    if (digits.size() > 1 && digits.front() == '0') return std::nullopt;
    uint32_t value = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (error != std::errc{} || end != digits.data() + digits.size() || value >= MAP_PLAYER_COUNT) {
        return std::nullopt;
    }
    return PlayerId{static_cast<uint8_t>(value)};
}

inline constexpr size_t PLAYER_SLOT_COUNT = static_cast<size_t>(MAX_SLOTS);
inline constexpr size_t PLAYER_REGISTRY_CAPACITY = MAP_PLAYER_COUNT + 1;

// Frozen ruleset handles.  A handle is assigned by a canonical catalog order;
// source strings remain data, not simulation identity.
struct FactionTemplateId final {
    uint32_t value = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept { return value != 0; }
    constexpr explicit operator bool() const noexcept { return isValid(); }
    constexpr auto operator<=>(const FactionTemplateId&) const noexcept = default;
};

inline constexpr FactionTemplateId INVALID_FACTION_TEMPLATE_ID{};

struct MultiplayerColorId final {
    uint32_t value = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept { return value != 0; }
    constexpr explicit operator bool() const noexcept { return isValid(); }
    constexpr auto operator<=>(const MultiplayerColorId&) const noexcept = default;
};

inline constexpr MultiplayerColorId INVALID_MULTIPLAYER_COLOR_ID{};

// Multiplayer alliance grouping only establishes default diplomacy.  It must
// remain distinct from ScriptTeamId, which is an immutable scenario-team
// definition, and ObjectTeamId, which is a live per-session team instance.
struct AllianceGroupId final {
    uint8_t value = 0xff;

    [[nodiscard]] constexpr bool isValid() const noexcept { return value != 0xff; }
    constexpr explicit operator bool() const noexcept { return isValid(); }
    constexpr auto operator<=>(const AllianceGroupId&) const noexcept = default;
};

inline constexpr AllianceGroupId INVALID_ALLIANCE_GROUP_ID{};

struct ScriptTeamId final {
    uint32_t value = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept { return value != 0; }
    constexpr explicit operator bool() const noexcept { return isValid(); }
    constexpr auto operator<=>(const ScriptTeamId&) const noexcept = default;
};

inline constexpr ScriptTeamId INVALID_SCRIPT_TEAM_ID{};

// A live team instance is the unique primary grouping of an object.  It is
// intentionally distinct from ScriptTeamId: one authored scenario definition
// may later create several runtime teams, while every object has at most one
// ObjectTeamId at a time.  Player default teams use the same type.
struct ObjectTeamId final {
    uint32_t value = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept { return value != 0; }
    constexpr explicit operator bool() const noexcept { return isValid(); }
    constexpr auto operator<=>(const ObjectTeamId&) const noexcept = default;
};

inline constexpr ObjectTeamId INVALID_OBJECT_TEAM_ID{};

enum class PlayerControllerKind : uint8_t {
    Human,
    Ai,
    Observer,
    Neutral,
};

// Simulation setup for an AI-controlled player. Keeping it distinct from the
// controller preserves the original Easy/Normal/Hard roster semantics until
// a future AI system consumes the value.
enum class AiDifficulty : uint8_t {
    None,
    Easy,
    Normal,
    Hard,
};

// Participation is independent from controller ownership.  In particular,
// an observer still has a lobby slot/player identity but has no faction,
// resources, spawn reservation or lockstep authoring permission.
enum class PlayerParticipationKind : uint8_t {
    Participant,
    Observer,
};

enum class PlayerLifeState : uint8_t {
    Setup,
    Active,
    Defeated,
    Observer,
};

// Directed relation, matching original Player->Player semantics.  A lobby
// alliance is only an initializer; scripts may later change one direction.
enum class PlayerRelationship : uint8_t {
    Allies,
    Enemies,
    Neutral,
};

} // namespace engine

template <>
struct std::hash<engine::MatchPlayerSlotId> {
    size_t operator()(engine::MatchPlayerSlotId value) const noexcept {
        return std::hash<uint8_t>{}(value.value);
    }
};

template <>
struct std::hash<engine::PlayerId> {
    size_t operator()(engine::PlayerId value) const noexcept {
        return std::hash<uint8_t>{}(value.value);
    }
};

template <>
struct std::hash<engine::FactionTemplateId> {
    size_t operator()(engine::FactionTemplateId value) const noexcept {
        return std::hash<uint32_t>{}(value.value);
    }
};

template <>
struct std::hash<engine::AllianceGroupId> {
    size_t operator()(engine::AllianceGroupId value) const noexcept {
        return std::hash<uint8_t>{}(value.value);
    }
};

template <>
struct std::hash<engine::MultiplayerColorId> {
    size_t operator()(engine::MultiplayerColorId value) const noexcept {
        return std::hash<uint32_t>{}(value.value);
    }
};

template <>
struct std::hash<engine::ScriptTeamId> {
    size_t operator()(engine::ScriptTeamId value) const noexcept {
        return std::hash<uint32_t>{}(value.value);
    }
};

template <>
struct std::hash<engine::ObjectTeamId> {
    size_t operator()(engine::ObjectTeamId value) const noexcept {
        return std::hash<uint32_t>{}(value.value);
    }
};
