#pragma once

#include "core/container/container_types.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
namespace engine::fx::detail {

[[nodiscard]] inline container::StringView trim(container::StringView value) noexcept {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] inline char asciiLower(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

[[nodiscard]] inline container::String canonicalName(container::StringView value) {
    value = trim(value);
    container::String output(value);
    std::transform(output.begin(), output.end(), output.begin(), [](char character) {
        return asciiLower(character);
    });
    return output;
}

[[nodiscard]] inline bool asciiEqual(container::StringView left, container::StringView right) noexcept {
    left = trim(left);
    right = trim(right);
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        if (asciiLower(left[index]) != asciiLower(right[index])) return false;
    }
    return true;
}

[[nodiscard]] inline container::Vector<container::StringView> splitWords(container::StringView value) {
    container::Vector<container::StringView> words;
    value = trim(value);
    size_t cursor = 0;
    while (cursor < value.size()) {
        while (cursor < value.size() && std::isspace(static_cast<unsigned char>(value[cursor]))) {
            ++cursor;
        }
        if (cursor >= value.size()) break;
        const size_t begin = cursor;
        bool quoted = false;
        while (cursor < value.size()) {
            if (value[cursor] == '"') quoted = !quoted;
            if (!quoted && std::isspace(static_cast<unsigned char>(value[cursor]))) break;
            ++cursor;
        }
        words.push_back(value.substr(begin, cursor - begin));
    }
    return words;
}

[[nodiscard]] inline std::optional<float> parseFloat(container::StringView value) noexcept {
    value = trim(value);
    if (value.empty()) return std::nullopt;
    container::String owned(value);
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(owned.c_str(), &end);
    if (end == owned.c_str() || errno == ERANGE || !std::isfinite(parsed)) return std::nullopt;
    while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
    return *end == '\0' ? std::optional<float>{parsed} : std::nullopt;
}

[[nodiscard]] inline std::optional<uint32_t> parseUnsigned(container::StringView value) noexcept {
    value = trim(value);
    if (value.empty()) return std::nullopt;
    container::String owned(value);
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(owned.c_str(), &end, 10);
    if (end == owned.c_str() || errno == ERANGE || parsed > std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;
    }
    while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
    return *end == '\0' ? std::optional<uint32_t>{static_cast<uint32_t>(parsed)} : std::nullopt;
}

[[nodiscard]] inline std::optional<int32_t> parseInt(container::StringView value) noexcept {
    value = trim(value);
    if (value.empty()) return std::nullopt;
    container::String owned(value);
    char* end = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(owned.c_str(), &end, 10);
    if (end == owned.c_str() || errno == ERANGE ||
        parsed < std::numeric_limits<int32_t>::min() ||
        parsed > std::numeric_limits<int32_t>::max()) {
        return std::nullopt;
    }
    while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
    return *end == '\0' ? std::optional<int32_t>{static_cast<int32_t>(parsed)} : std::nullopt;
}

[[nodiscard]] inline std::optional<bool> parseBool(container::StringView value) noexcept {
    if (asciiEqual(value, "yes") || asciiEqual(value, "true") ||
        asciiEqual(value, "on") || trim(value) == "1") {
        return true;
    }
    if (asciiEqual(value, "no") || asciiEqual(value, "false") ||
        asciiEqual(value, "off") || trim(value) == "0") {
        return false;
    }
    return std::nullopt;
}

struct ParsedRange final {
    float minimum = 0.0f;
    float maximum = 0.0f;
};

[[nodiscard]] inline std::optional<ParsedRange> parseRange(container::StringView value) noexcept {
    const container::Vector<container::StringView> words = splitWords(value);
    if (words.empty() || words.size() > 3) return std::nullopt;
    const std::optional<float> minimum = parseFloat(words[0]);
    const std::optional<float> maximum = words.size() >= 2 ? parseFloat(words[1]) : minimum;
    if (!minimum || !maximum) return std::nullopt;
    // INI::parseGameClientRandomVariable accepts "low high [distribution]".
    // The shipped FXList layers use only CONSTANT and UNIFORM; the runtime's
    // min/max representation is exact for both, and CONSTANT is valid only
    // when both authored endpoints are equal (matching RefCode's assertion).
    if (words.size() == 3) {
        const bool constant = asciiEqual(words[2], "CONSTANT");
        if (!constant && !asciiEqual(words[2], "UNIFORM")) return std::nullopt;
        if (constant && *minimum != *maximum) return std::nullopt;
    }
    return ParsedRange{.minimum = *minimum, .maximum = *maximum};
}

struct ParsedVector3 final {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

[[nodiscard]] inline std::optional<ParsedVector3> parseVector3(container::StringView value) noexcept {
    ParsedVector3 output;
    bool hasX = false;
    bool hasY = false;
    bool hasZ = false;
    for (const container::StringView word : splitWords(value)) {
        const size_t colon = word.find(':');
        if (colon == container::StringView::npos || colon == 0 || colon + 1 >= word.size()) continue;
        container::StringView componentText = word.substr(colon + 1);
        // Several shipped FXList offsets contain an accidental trailing colon
        // (for example Y:15:). RefCode's real scanner accepts the numeric
        // prefix. Preserve that narrow compatibility without allowing other
        // arbitrary suffixes through the strict float parser.
        if (componentText.ends_with(':')) componentText.remove_suffix(1);
        const std::optional<float> component = parseFloat(componentText);
        if (!component) return std::nullopt;
        const char axis = asciiLower(word.front());
        if (axis == 'x') {
            output.x = *component;
            hasX = true;
        } else if (axis == 'y') {
            output.y = *component;
            hasY = true;
        } else if (axis == 'z') {
            output.z = *component;
            hasZ = true;
        }
    }
    return hasX && hasY && hasZ ? std::optional<ParsedVector3>{output} : std::nullopt;
}

struct ParsedColor final {
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
};

[[nodiscard]] inline std::optional<ParsedColor> parseColor(container::StringView value) noexcept {
    ParsedColor output;
    bool hasRed = false;
    bool hasGreen = false;
    bool hasBlue = false;
    for (const container::StringView word : splitWords(value)) {
        const size_t colon = word.find(':');
        if (colon == container::StringView::npos || colon == 0 || colon + 1 >= word.size()) continue;
        const std::optional<uint32_t> component = parseUnsigned(word.substr(colon + 1));
        if (!component || *component > 255) return std::nullopt;
        const char channel = asciiLower(word.front());
        if (channel == 'r') {
            output.red = static_cast<uint8_t>(*component);
            hasRed = true;
        } else if (channel == 'g') {
            output.green = static_cast<uint8_t>(*component);
            hasGreen = true;
        } else if (channel == 'b') {
            output.blue = static_cast<uint8_t>(*component);
            hasBlue = true;
        }
    }
    return hasRed && hasGreen && hasBlue ? std::optional<ParsedColor>{output} : std::nullopt;
}

[[nodiscard]] inline std::optional<float> parsePercent(container::StringView value) noexcept {
    value = trim(value);
    if (value.ends_with('%')) {
        value.remove_suffix(1);
        const std::optional<float> percent = parseFloat(value);
        return percent ? std::optional<float>{*percent * 0.01f} : std::nullopt;
    }
    return parseFloat(value);
}

} // namespace engine::fx::detail
