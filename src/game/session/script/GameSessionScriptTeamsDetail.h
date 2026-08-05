#pragma once

#include "core/container/string_utils.h"
#include "game/command/CommandButtonStore.h"
#include "game/object/definition/ObjectArchetype.h"

#include <algorithm>
#include <cctype>

namespace engine::script_team_detail {

inline constexpr auto equalsAsciiIgnoreCase =
    container::asciiEqualIgnoreCase;

[[nodiscard]] inline bool kindOfListContains(
    const game::ObjectArchetype* archetype,
    game::ObjectKindOf sought) noexcept {
    return archetype && game::objectHasKind(archetype->kindOfMask, sought);
}

[[nodiscard]] inline bool kindOfListContains(
    const container::SharedPtr<const game::ObjectArchetype>& archetype,
    game::ObjectKindOf sought) noexcept {
    return kindOfListContains(archetype.get(), sought);
}

[[nodiscard]] inline const container::String* commandButtonField(
    const game::CommandButtonTemplate& button,
    container::StringView key) noexcept {
    for (auto it = button.fields.rbegin(); it != button.fields.rend(); ++it) {
        if (equalsAsciiIgnoreCase(it->first, key)) return &it->second;
    }
    return nullptr;
}

[[nodiscard]] inline bool commandButtonHasOption(
    const game::CommandButtonTemplate& button,
    container::StringView option) noexcept {
    size_t cursor = 0;
    while (cursor < button.options.size()) {
        while (cursor < button.options.size() &&
               (std::isspace(static_cast<unsigned char>(button.options[cursor])) ||
                button.options[cursor] == ',' || button.options[cursor] == '|')) {
            ++cursor;
        }
        const size_t begin = cursor;
        while (cursor < button.options.size() &&
               !std::isspace(static_cast<unsigned char>(button.options[cursor])) &&
               button.options[cursor] != ',' && button.options[cursor] != '|') {
            ++cursor;
        }
        if (begin != cursor && equalsAsciiIgnoreCase(
                container::StringView{button.options}.substr(begin, cursor - begin),
                option)) return true;
    }
    return false;
}

// RefCode's COMMAND_OPTION_NEED_OBJECT_TARGET (ScriptActions.cpp:2093) is a
// C++ bit mask, never an authored spelling: "NEED_OBJECT_TARGET" appears in no
// shipped INI and in no CommandButtonStore option name, so testing for it
// always answered false. The mask is exactly the union of the three
// relationship-scoped object-target options.
[[nodiscard]] inline bool commandButtonNeedsObjectTarget(
    const game::CommandButtonTemplate& button) noexcept {
    return commandButtonHasOption(button, "NEED_TARGET_ENEMY_OBJECT") ||
        commandButtonHasOption(button, "NEED_TARGET_NEUTRAL_OBJECT") ||
        commandButtonHasOption(button, "NEED_TARGET_ALLY_OBJECT");
}

} // namespace engine::script_team_detail
