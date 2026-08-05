#pragma once

#include "core/container/string_utils.h"

#include <cctype>
#include <optional>

namespace game {

// Load/compile boundary only: parse ZH-style bool tokens into bool.
// Runtime paths must already hold typed bool fields.
[[nodiscard]] inline std::optional<bool> tryParseContentBool(
    container::StringView raw) noexcept {
    raw = container::trimAsciiView(raw);
    if (raw.empty()) return std::nullopt;
    if (container::asciiEqualIgnoreCase(raw, "YES") ||
        container::asciiEqualIgnoreCase(raw, "TRUE") ||
        container::asciiEqualIgnoreCase(raw, "ON") ||
        raw == "1") {
        return true;
    }
    if (container::asciiEqualIgnoreCase(raw, "NO") ||
        container::asciiEqualIgnoreCase(raw, "FALSE") ||
        container::asciiEqualIgnoreCase(raw, "OFF") ||
        raw == "0") {
        return false;
    }
    return std::nullopt;
}

[[nodiscard]] inline bool parseContentBool(
    container::StringView raw, bool fallback = false) noexcept {
    if (const std::optional<bool> parsed = tryParseContentBool(raw))
        return *parsed;
    return fallback;
}

} // namespace game
