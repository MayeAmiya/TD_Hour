#include "EconomySimulationRules.h"

#include "ContentFloatParsing.h"
#include "core/container/string_utils.h"
#include "core/data/ini/GeneralsIniParser.h"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <optional>
#include <utility>
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

namespace engine {
namespace {

using container::asciiEqualIgnoreCase;

constexpr auto trim = container::trimAsciiView;

struct ParsedRatio final {
    int64_t numerator = 0;
    uint64_t denominator = 1;
};

[[nodiscard]] std::optional<ParsedRatio>
parsePercent(container::StringView value, container::StringView source,
             const EconomySimulationRules& fallback) {
    value = trim(value);
    if (!value.empty() && value.back() == '%') value.remove_suffix(1);
    value = trim(value);
    const float fallbackPercent = fallback.sellPercentageDenominator != 0
        ? static_cast<float>(
              static_cast<long double>(fallback.sellPercentageNumerator) *
              100.0L /
              static_cast<long double>(fallback.sellPercentageDenominator))
        : 0.0f;
    const auto percent = game::parseContentFloat(value, {
        .source = source,
        .block = "GameData",
        .module = "EconomySimulationRules",
        .field = "SellPercentage",
        .fallback = fallbackPercent,
    });
    if (!percent) return std::nullopt;
    // Preserve substantially more precision than the original Real while
    // converting once at content load. Settlement itself remains integer and
    // deterministic. Four decimal places in the authored percent maps to an
    // exact millionth of the final ratio.
    constexpr uint64_t kRatioScale = 1'000'000;
    const long double scaled = static_cast<long double>(*percent) *
        static_cast<long double>(kRatioScale) / 100.0L;
    if (scaled < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
        scaled > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        game::processContentDiagnostics().warn({
            .source = container::String{source},
            .block = "GameData",
            .module = "EconomySimulationRules",
            .field = "SellPercentage",
            .rawValue = container::String{value},
            .adoptedValue = std::to_string(fallbackPercent),
            .reason = "percentage is not representable as the settlement ratio; retained the prior/default value",
        });
        return std::nullopt;
    }
    if (*percent < 0.0f || *percent > 100.0f) {
        game::processContentDiagnostics().warn({
            .source = container::String{source},
            .block = "GameData",
            .module = "EconomySimulationRules",
            .field = "SellPercentage",
            .rawValue = container::String{value},
            .adoptedValue = std::to_string(*percent),
            .reason = "accepted original percentage outside the conventional 0..100 domain",
        });
    }
    return ParsedRatio{
        .numerator = static_cast<int64_t>(std::llround(scaled)),
        .denominator = kRatioScale,
    };
}

[[nodiscard]] std::optional<int64_t> parseNonNegativeInteger(
    container::StringView value) noexcept {
    value = trim(value);
    int64_t parsed = 0;
    const char* const begin = value.data();
    const char* const end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed < 0)
        return std::nullopt;
    return parsed;
}

void setError(container::String* error, container::String message) {
    if (error) *error = std::move(message);
}

[[nodiscard]] bool applyGameDataBlocks(
    const container::Vector<game::IniBlock>& blocks,
    EconomySimulationRules& compiled, bool& modified,
    container::StringView source, container::String* /*error*/) {
    for (const game::IniBlock& block : blocks) {
        if (!asciiEqualIgnoreCase(block.type, "GameData")) continue;
        const auto blockDiagnosticScope =
            game::contentDiagnosticProvenanceScope(block.source);
        for (size_t valueIndex = 0;
             valueIndex < block.values.size(); ++valueIndex) {
            const auto& [key, value] = block.values[valueIndex];
            const auto fieldDiagnosticScope =
                game::contentDiagnosticProvenanceScope(
                    block.valueSource(valueIndex));
            if (asciiEqualIgnoreCase(key, "ValuePerSupplyBox")) {
                if (const std::optional<int64_t> parsed =
                        parseNonNegativeInteger(value)) {
                    compiled.valuePerSupplyBox = *parsed;
                    modified = true;
                } else {
                    game::processContentDiagnostics().warn({
                        .source = container::String{source},
                        .block = "GameData",
                        .module = "EconomySimulationRules",
                        .field = "ValuePerSupplyBox",
                        .rawValue = container::String{value},
                        .adoptedValue = std::to_string(
                            compiled.valuePerSupplyBox),
                        .reason = "expected a non-negative integer; retained the prior/default value",
                    });
                }
                continue;
            }
            if (asciiEqualIgnoreCase(key, "SellPercentage")) {
                const std::optional<ParsedRatio> parsed =
                    parsePercent(value, source, compiled);
                if (!parsed) continue;
                compiled.sellPercentageNumerator = parsed->numerator;
                compiled.sellPercentageDenominator = parsed->denominator;
                modified = true;
            }
        }
    }
    return true;
}

} // namespace

void EconomySimulationRules::canonicalize() noexcept {
    if (sellPercentageDenominator == 0) return;
    if (sellPercentageNumerator == 0) {
        sellPercentageDenominator = 1;
        return;
    }
    const uint64_t magnitude = sellPercentageNumerator < 0
        ? static_cast<uint64_t>(-(sellPercentageNumerator + 1)) + 1u
        : static_cast<uint64_t>(sellPercentageNumerator);
    const uint64_t divisor = std::gcd(magnitude, sellPercentageDenominator);
    if (divisor > 1) {
        sellPercentageNumerator /= static_cast<int64_t>(divisor);
        sellPercentageDenominator /= divisor;
    }
}

int64_t EconomySimulationRules::applySellPercentage(int64_t value) const noexcept {
    if (value <= 0 || sellPercentageNumerator <= 0 ||
        sellPercentageDenominator == 0) return 0;
    const uint64_t left = static_cast<uint64_t>(value);
    const uint64_t right = static_cast<uint64_t>(sellPercentageNumerator);
    const uint64_t denominator = sellPercentageDenominator;
#if defined(_MSC_VER) && defined(_M_X64)
    uint64_t high = 0;
    const uint64_t low = _umul128(left, right, &high);
    if (high >= denominator) return std::numeric_limits<int64_t>::max();
    uint64_t remainder = 0;
    const uint64_t quotient = _udiv128(high, low, denominator, &remainder);
    return quotient > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
        ? std::numeric_limits<int64_t>::max()
        : static_cast<int64_t>(quotient);
#else
    const __uint128_t product = static_cast<__uint128_t>(left) * right;
    const __uint128_t quotient = product / denominator;
    return quotient > static_cast<__uint128_t>(std::numeric_limits<int64_t>::max())
        ? std::numeric_limits<int64_t>::max()
        : static_cast<int64_t>(quotient);
#endif
}

bool EconomySimulationRules::applyLegacyGameDataOverrides(
    container::StringView content, container::StringView sourceName,
    container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parse(content, sourceName)) {
        setError(error, "could not parse GameData modifier '" +
                            container::String{sourceName} + "'");
        return false;
    }
    EconomySimulationRules candidate = *this;
    bool modified = false;
    if (!applyGameDataBlocks(parser.blocks(), candidate, modified,
                             sourceName, error))
        return false;
    if (!modified) return true;
    candidate.canonicalize();
    *this = candidate;
    return true;
}

bool EconomySimulationRules::loadFromLegacyGameData(
    container::StringView path, EconomySimulationRules& rules,
    container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parseFile(container::String{path})) {
        setError(error, "could not parse GameData source '" +
                        container::String{path} + "'");
        return false;
    }
    EconomySimulationRules compiled;
    bool modified = false;
    if (!applyGameDataBlocks(parser.blocks(), compiled, modified, path, error))
        return false;
    compiled.canonicalize();
    rules = compiled;
    return true;
}

} // namespace engine
