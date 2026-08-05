#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "BaseRegenerationRules.h"

#include "ContentFloatParsing.h"
#include "core/data/ini/GeneralsIniParser.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

namespace engine {
namespace {

using container::asciiEqualIgnoreCase;

constexpr auto trim = container::trimAsciiView;

void warnField(container::StringView source, container::StringView field,
               container::StringView raw, container::String adopted,
               container::String reason) {
    game::processContentDiagnostics().warn({
        .source = container::String{source},
        .block = "GameData",
        .module = "BaseRegenerationRules",
        .field = container::String{field},
        .rawValue = container::String{raw},
        .adoptedValue = std::move(adopted),
        .reason = std::move(reason),
    });
}

[[nodiscard]] std::optional<BaseRegenerationRules::Scalar>
parsePercentToUnit(container::StringView text, container::StringView source,
                   BaseRegenerationRules::Scalar fallback) {
    text = trim(text);
    if (!text.empty() && text.back() == '%') text.remove_suffix(1);
    text = trim(text);
    const auto percent = game::parseContentFloat(text, {
        .source = source,
        .block = "GameData",
        .module = "BaseRegenerationRules",
        .field = "BaseRegenHealthPercentPerSecond",
        .fallback = fallback.to_float() * 100.0f,
    });
    if (!percent) return std::nullopt;

    const double unit = static_cast<double>(*percent) * 0.01;

    constexpr double maximum = static_cast<double>(std::numeric_limits<int32_t>::max());
    constexpr double minimum = static_cast<double>(std::numeric_limits<int32_t>::min());
    if (unit >= maximum || unit <= minimum) {
        warnField(source, "BaseRegenHealthPercentPerSecond", text,
                  std::to_string(fallback.to_float() * 100.0f),
                  "percentage is not representable in Q32.32; retained the prior/default value");
        return std::nullopt;
    }
    if (unit < 0.0) {
        warnField(source, "BaseRegenHealthPercentPerSecond", text,
                  std::to_string(*percent),
                  "accepted original negative domain; regeneration remains disabled at use time");
    }
    return BaseRegenerationRules::Scalar{unit};
}

[[nodiscard]] std::optional<uint32_t> parseMilliseconds(
    container::StringView text, container::StringView source,
    uint32_t fallback) {
    text = trim(text);
    const container::String owned{text};
    char* end = nullptr;
    errno = 0;
    const long long value = std::strtoll(owned.c_str(), &end, 10);
    if (end == owned.c_str() || errno == ERANGE) {
        warnField(source, "BaseRegenDelay", text, std::to_string(fallback),
                  "duration has no representable integer prefix; retained the prior/default value");
        return std::nullopt;
    }
    const uint32_t adopted = static_cast<uint32_t>(value);
    if (*end != '\0') {
        warnField(source, "BaseRegenDelay", text, std::to_string(adopted),
                  "accepted original integer prefix and ignored noncanonical trailing text");
    }
    if (value < 0 || static_cast<unsigned long long>(value) >
                         std::numeric_limits<uint32_t>::max()) {
        warnField(source, "BaseRegenDelay", text, std::to_string(adopted),
                  "accepted original signed-to-unsigned duration conversion");
    }
    return adopted;
}

void setError(container::String* error, container::String message) {
    if (error) *error = std::move(message);
}

[[nodiscard]] bool applyGameDataBlocks(
    const container::Vector<game::IniBlock>& blocks,
    BaseRegenerationRules& compiled, bool& modified,
    container::StringView source, container::String* /*error*/) {
    for (const game::IniBlock& block : blocks) {
        if (!asciiEqualIgnoreCase(block.type, "GameData")) continue;
        const auto blockDiagnosticScope =
            game::contentDiagnosticProvenanceScope(block.source);
        for (size_t valueIndex = 0;
             valueIndex < block.values.size(); ++valueIndex) {
            const auto& [key, text] = block.values[valueIndex];
            const auto fieldDiagnosticScope =
                game::contentDiagnosticProvenanceScope(
                    block.valueSource(valueIndex));
            if (asciiEqualIgnoreCase(key, "BaseRegenHealthPercentPerSecond")) {
                const std::optional<BaseRegenerationRules::Scalar> parsed =
                    parsePercentToUnit(text, source,
                                       compiled.healthPercentPerSecond);
                if (!parsed) {
                    continue;
                }
                compiled.healthPercentPerSecond = *parsed;
            } else if (asciiEqualIgnoreCase(key, "BaseRegenDelay")) {
                const std::optional<uint32_t> parsed = parseMilliseconds(
                    text, source, compiled.damageDelayMilliseconds);
                if (!parsed) {
                    continue;
                }
                compiled.damageDelayMilliseconds = *parsed;
            } else {
                continue;
            }
            modified = true;
        }
    }
    return true;
}

} // namespace

void BaseRegenerationRules::canonicalize() noexcept {
    // Authored domain values are preserved. enabled()/the regeneration
    // consumer already derives the safe non-positive behavior explicitly.
}

bool BaseRegenerationRules::applyLegacyGameDataOverrides(
    container::StringView content, container::StringView sourceName,
    container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parse(content, sourceName)) {
        setError(error, "could not parse GameData modifier '" +
                            container::String{sourceName} + "'");
        return false;
    }
    BaseRegenerationRules candidate = *this;
    bool modified = false;
    if (!applyGameDataBlocks(parser.blocks(), candidate, modified,
                             sourceName, error))
        return false;
    if (!modified) return true;
    candidate.canonicalize();
    *this = candidate;
    return true;
}

bool BaseRegenerationRules::loadFromLegacyGameData(container::StringView path,
                                                   BaseRegenerationRules& rules,
                                                   container::String* error) {
    if (error) error->clear();

    game::GeneralsIniParser parser;
    if (!parser.parseFile(container::String{path})) {
        setError(error, "could not parse GameData source '" + container::String{path} + "'");
        return false;
    }

    BaseRegenerationRules compiled;
    bool modified = false;
    if (!applyGameDataBlocks(parser.blocks(), compiled, modified, path, error))
        return false;

    compiled.canonicalize();
    rules = compiled;
    return true;
}

} // namespace engine
