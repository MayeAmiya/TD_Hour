#pragma once

#include "core/container/container_types.h"
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"

#include <compare>
#include <cstdint>
#include <utility>
#include <variant>

namespace engine::script {

// Authored selectors are values, not magic names interpreted throughout the
// runtime.  A dynamic selector carries no fallback spelling: it can only be
// resolved from an explicit invocation context.
struct ScriptObjectSelector final {
    enum class Kind : uint8_t {
        Named,
        ThisObject,
    };

    Kind kind = Kind::Named;
    container::String name;

    [[nodiscard]] static ScriptObjectSelector named(container::String value) {
        return {.kind = Kind::Named, .name = std::move(value)};
    }
    [[nodiscard]] static ScriptObjectSelector thisObject() noexcept {
        return {.kind = Kind::ThisObject};
    }
    [[nodiscard]] bool valid() const noexcept {
        return kind == Kind::ThisObject || !name.empty();
    }
    [[nodiscard]] constexpr auto operator<=>(const ScriptObjectSelector&) const noexcept = default;
};

struct ScriptTeamSelector final {
    enum class Kind : uint8_t {
        ScenarioTeam,
        ThisTeam,
    };

    Kind kind = Kind::ScenarioTeam;
    container::String name;

    [[nodiscard]] static ScriptTeamSelector scenarioTeam(container::String value) {
        return {.kind = Kind::ScenarioTeam, .name = std::move(value)};
    }
    [[nodiscard]] static ScriptTeamSelector thisTeam() noexcept {
        return {.kind = Kind::ThisTeam};
    }
    [[nodiscard]] bool valid() const noexcept {
        return kind == Kind::ThisTeam || !name.empty();
    }
    [[nodiscard]] constexpr auto operator<=>(const ScriptTeamSelector&) const noexcept = default;
};

using ScriptObjectOrTeamSelector =
    std::variant<ScriptObjectSelector, ScriptTeamSelector>;

enum class ScriptInvocationOrigin : uint8_t {
    Automatic,
    Subroutine,
    TeamHook,
    ObjectHook,
    SequentialObject,
    SequentialTeam,
};

// Stable IDs are the complete dynamic execution context.  No Entity, ECS
// view, AI group, pointer, or member span is retained across an instruction.
struct ScriptInvocationContext final {
    ObjectTeamId callingTeam = INVALID_OBJECT_TEAM_ID;
    ObjectTeamId conditionTeam = INVALID_OBJECT_TEAM_ID;
    ObjectId callingObject = INVALID_OBJECT_ID;
    ObjectId conditionObject = INVALID_OBJECT_ID;
    PlayerId currentPlayer = INVALID_PLAYER_ID;
    ScriptInvocationOrigin origin = ScriptInvocationOrigin::Automatic;

    [[nodiscard]] constexpr ObjectTeamId thisTeam() const noexcept {
        return callingTeam ? callingTeam : conditionTeam;
    }
    [[nodiscard]] constexpr ObjectId thisObject() const noexcept {
        return callingObject ? callingObject : conditionObject;
    }

    [[nodiscard]] constexpr auto operator<=>(const ScriptInvocationContext&) const noexcept = default;
};

} // namespace engine::script
