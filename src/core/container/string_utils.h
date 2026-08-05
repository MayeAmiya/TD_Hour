#pragma once

#include "container_types.h"

#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <system_error>

namespace container {

enum class FiniteFloatParseStatus : uint8_t {
    Canonical,
    NonCanonicalTail,
    NoNumericPrefix,
    NonFiniteOrOutOfRange,
};

struct FiniteFloatParseResult final {
    float value = 0.0f;
    size_t consumed = 0;
    FiniteFloatParseStatus status =
        FiniteFloatParseStatus::NoNumericPrefix;

    [[nodiscard]] bool accepted() const noexcept {
        return status == FiniteFloatParseStatus::Canonical ||
            status == FiniteFloatParseStatus::NonCanonicalTail;
    }
    [[nodiscard]] bool nonCanonical() const noexcept {
        return status == FiniteFloatParseStatus::NonCanonicalTail;
    }
    explicit operator bool() const noexcept { return accepted(); }
};

[[nodiscard]] constexpr bool isAsciiWhitespace(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' ||
        value == '\n' || value == '\f' || value == '\v';
}

[[nodiscard]] constexpr char asciiLower(char value) noexcept {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value + ('a' - 'A'))
        : value;
}

[[nodiscard]] constexpr bool asciiEqualIgnoreCase(
    StringView left, StringView right) noexcept {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        if (asciiLower(left[index]) != asciiLower(right[index])) return false;
    }
    return true;
}

[[nodiscard]] constexpr bool startsWithIgnoreCase(
    StringView value, StringView prefix) noexcept {
    return prefix.size() <= value.size() &&
        asciiEqualIgnoreCase(value.substr(0, prefix.size()), prefix);
}

[[nodiscard]] constexpr bool endsWithIgnoreCase(
    StringView value, StringView suffix) noexcept {
    return suffix.size() <= value.size() &&
        asciiEqualIgnoreCase(value.substr(value.size() - suffix.size()), suffix);
}

[[nodiscard]] constexpr StringView trimAsciiView(StringView value) noexcept {
    while (!value.empty() && isAsciiWhitespace(value.front()))
        value.remove_prefix(1);
    while (!value.empty() && isAsciiWhitespace(value.back()))
        value.remove_suffix(1);
    return value;
}

[[nodiscard]] inline String trimAsciiCopy(StringView value) {
    return String{trimAsciiView(value)};
}

// Strict content/config primitive: surrounding ASCII whitespace is allowed,
// but trailing tokens, overflow, NaN and infinity are rejected. Callers keep
// ownership of fallback/default policy.
[[nodiscard]] inline std::optional<float> parseFiniteFloatExact(
    StringView source) noexcept {
    source = trimAsciiView(source);
    if (source.empty()) return std::nullopt;
    float value = 0.0f;
    const char* const begin = source.data();
    const char* const end = begin + source.size();
    const std::from_chars_result parsed = std::from_chars(
        begin, end, value, std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != end ||
        !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

// Generals INI compatibility parser. RefCode's scanReal accepts any finite
// numeric prefix (including leading '+', exponent notation and C-style
// suffixes such as `100.0f`) because it never validates from_chars' end
// pointer. Preserve that content compatibility, but return enough structure
// for authoritative loaders to warn whenever text was ignored.
[[nodiscard]] inline FiniteFloatParseResult parseFiniteFloatCompatible(
    StringView source) noexcept {
    source = trimAsciiView(source);
    if (source.empty()) return {};

    const String owned{source};
    char* end = nullptr;
    errno = 0;
    const float value = std::strtof(owned.c_str(), &end);
    if (end == owned.c_str()) {
        return {
            .status = FiniteFloatParseStatus::NoNumericPrefix,
        };
    }
    const size_t consumed = static_cast<size_t>(end - owned.c_str());
    if (errno == ERANGE || !std::isfinite(value)) {
        return {
            .consumed = consumed,
            .status = FiniteFloatParseStatus::NonFiniteOrOutOfRange,
        };
    }
    return {
        .value = value,
        .consumed = consumed,
        .status = consumed == owned.size()
            ? FiniteFloatParseStatus::Canonical
            : FiniteFloatParseStatus::NonCanonicalTail,
    };
}

// Compatibility primitive for legacy INI readers that historically used
// strtof and accepted a valid finite prefix followed by unrelated text.
// Keep this behavior explicit rather than weakening parseFiniteFloatExact().
[[nodiscard]] inline std::optional<float> parseFiniteFloatPrefix(
    StringView source) noexcept {
    const FiniteFloatParseResult parsed =
        parseFiniteFloatCompatible(source);
    return parsed ? std::optional<float>{parsed.value} : std::nullopt;
}

} // namespace container
