#pragma once

#include "ContentDiagnostics.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "core/container/string_utils.h"

#include <optional>
#include <cctype>
#include <utility>

namespace game {

struct ContentFloatContext final {
    container::StringView source;
    uint32_t sourceLine = 0;
    container::StringView block;
    container::StringView definition;
    container::StringView module;
    container::StringView field = "Real";
    float fallback = 0.0f;
};

inline void warnContentFloatFallback(
    container::StringView raw, const ContentFloatContext& context,
    container::String reason) {
    processContentDiagnostics().warn({
        .source = container::String{context.source},
        .sourceLine = context.sourceLine,
        .block = container::String{context.block},
        .definition = container::String{context.definition},
        .module = container::String{context.module},
        .field = container::String{context.field},
        .rawValue = container::String{raw},
        .adoptedValue = std::to_string(context.fallback),
        .reason = std::move(reason),
    });
}

[[nodiscard]] inline std::optional<float> parseContentFloat(
    container::StringView raw, const ContentFloatContext& context) {
    const container::FiniteFloatParseResult parsed =
        container::parseFiniteFloatCompatible(raw);
    if (parsed.accepted()) {
        // RefCode's Real scanner accepts a finite numeric prefix and leaves
        // the rest of the authored line untouched. Shipped content relies on
        // this for missing-semicolon prose, percent/f suffixes and a handful
        // of stray End tokens. A successfully consumed finite prefix is
        // therefore ordinary compatibility, not degraded content.
        return parsed.value;
    }

    processContentDiagnostics().warn({
        .source = container::String{context.source},
        .sourceLine = context.sourceLine,
        .block = container::String{context.block},
        .definition = container::String{context.definition},
        .module = container::String{context.module},
        .field = container::String{context.field},
        .rawValue = container::String{raw},
        .adoptedValue = std::to_string(context.fallback),
        .reason = parsed.status ==
                container::FiniteFloatParseStatus::NoNumericPrefix
            ? "no numeric prefix; retained the prior/default safe value"
            : "numeric prefix was NaN, infinity or out of range; retained the prior/default safe value",
    });
    return std::nullopt;
}

[[nodiscard]] inline ContentFloatContext contentFloatContextAt(
    const IniBlock& block, size_t valueIndex, container::StringView field,
    float fallback = 0.0f, container::StringView module = {}) noexcept {
    const IniSourceLocation source = block.valueSource(valueIndex);
    return {
        .source = source.pathView(),
        .sourceLine = source.line,
        .block = block.type,
        .definition = block.name,
        .module = module,
        .field = field,
        .fallback = fallback,
    };
}

[[nodiscard]] inline float parseContentFloatOr(
    container::StringView raw, const ContentFloatContext& context) {
    return parseContentFloat(raw, context).value_or(context.fallback);
}

} // namespace game
