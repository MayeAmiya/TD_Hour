#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "VeterancySimulationRules.h"

#include "ContentFloatParsing.h"
#include "VFS.h"
#include "core/data/ini/GeneralsIniParser.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>
namespace engine {
namespace {

using container::asciiEqualIgnoreCase;

constexpr auto trim = container::trimAsciiView;

[[nodiscard]] constexpr VeterancySimulationRules::Scalar identity() noexcept {
    return VeterancySimulationRules::Scalar{int32_t{1}};
}

[[nodiscard]] std::optional<VeterancySimulationRules::Scalar>
parsePercentMultiplier(container::StringView text, container::StringView source,
                       container::StringView field,
                       VeterancySimulationRules::Scalar fallback) {
    text = trim(text);
    if (!text.empty() && text.back() == '%') {
        text.remove_suffix(1);
        text = trim(text);
    }
    const auto percent = game::parseContentFloat(text, {
        .source = source,
        .block = "GameData",
        .module = "VeterancySimulationRules",
        .field = field,
        .fallback = fallback.to_float() * 100.0f,
    });
    if (!percent) return std::nullopt;

    // Convert directly to Q32.32 raw units so an out-of-range percentage is
    // diagnosed before q32_32's intentionally lean floating constructor can
    // perform an invalid signed-integer conversion. Truncation matches the
    // legacy Real-to-fixed ingress used by the other bonus tables.
    constexpr long double kFixedScale = 4294967296.0L;
    const long double raw =
        static_cast<long double>(*percent) * (kFixedScale / 100.0L);
    if (!std::isfinite(raw) ||
        raw < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
        raw > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        game::processContentDiagnostics().warn({
            .source = container::String{source},
            .block = "GameData",
            .module = "VeterancySimulationRules",
            .field = container::String{field},
            .rawValue = container::String{text},
            .adoptedValue = std::to_string(fallback.to_float() * 100.0f),
            .reason = "percentage is not representable in Q32.32; retained the prior/default value",
        });
        return std::nullopt;
    }
    if (*percent <= 0.0f) {
        game::processContentDiagnostics().warn({
            .source = container::String{source},
            .block = "GameData",
            .module = "VeterancySimulationRules",
            .field = container::String{field},
            .rawValue = container::String{text},
            .adoptedValue = std::to_string(*percent),
            .reason = "accepted original non-positive health multiplier domain",
        });
    }
    return VeterancySimulationRules::Scalar::from_raw(
        static_cast<int64_t>(raw));
}

void setError(container::String* error, container::String message) {
    if (error) *error = std::move(message);
}

[[nodiscard]] bool applyGameDataBlocks(
    const container::Vector<game::IniBlock>& blocks,
    VeterancySimulationRules& compiled, bool& modified,
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
            VeterancySimulationRules::Scalar* destination = nullptr;
            if (asciiEqualIgnoreCase(key, "HealthBonus_Veteran")) {
                destination = &compiled.veteranHealthBonus;
            } else if (asciiEqualIgnoreCase(key, "HealthBonus_Elite")) {
                destination = &compiled.eliteHealthBonus;
            } else if (asciiEqualIgnoreCase(key, "HealthBonus_Heroic")) {
                destination = &compiled.heroicHealthBonus;
            }
            // HealthBonus_Regular is intentionally ignored. RefCode comments
            // that field out of GlobalData's parse table and fixes it at 1.
            if (!destination) continue;

            const std::optional<VeterancySimulationRules::Scalar> parsed =
                parsePercentMultiplier(text, source, key, *destination);
            if (!parsed) continue;
            *destination = *parsed;
            modified = true;
        }
    }
    return true;
}

} // namespace

VeterancySimulationRules::Scalar
VeterancySimulationRules::healthBonusForLevelIndex(size_t levelIndex) const noexcept {
    switch (levelIndex) {
    case kVeteranLevelIndex: return veteranHealthBonus;
    case kEliteLevelIndex: return eliteHealthBonus;
    case kHeroicLevelIndex: return heroicHealthBonus;
    case kRegularLevelIndex:
    default: return identity();
    }
}

void VeterancySimulationRules::canonicalize() noexcept {
    // Preserve authored multipliers, including the original zero/negative
    // domain. Ingress has already excluded non-finite/unrepresentable values.
}

bool VeterancySimulationRules::applyLegacyGameDataOverrides(
    container::StringView content, container::StringView sourceName,
    container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parse(content, sourceName)) {
        setError(error, "could not parse GameData modifier '" +
                            container::String{sourceName} + "'");
        return false;
    }
    VeterancySimulationRules candidate = *this;
    bool modified = false;
    if (!applyGameDataBlocks(parser.blocks(), candidate, modified,
                             sourceName, error))
        return false;
    if (!modified) return true;
    candidate.canonicalize();
    *this = candidate;
    return true;
}

bool VeterancySimulationRules::loadFromLegacyGameData(
    container::StringView path, VeterancySimulationRules& rules, container::String* error) {
    if (error) error->clear();

    VeterancySimulationRules compiled;
    bool modified = false;
    auto& vfs = io::VFS::instance();
    if (vfs.exists(path)) {
        game::GeneralsIniParser parser;
        if (!parser.parse(vfs.readAll(path), path) ||
            !applyGameDataBlocks(parser.blocks(), compiled, modified,
                                 path, error)) {
            if (error && error->empty()) {
                *error = "could not parse GameData source '" +
                    container::String{path} + "'";
            }
            return false;
        }
    } else {
        game::GeneralsIniParser parser;
        if (!parser.parseFile(container::String{path}) ||
            !applyGameDataBlocks(parser.blocks(), compiled, modified,
                                 path, error)) {
            if (error && error->empty()) {
                *error = "could not parse GameData source '" +
                    container::String{path} + "'";
            }
            return false;
        }
    }

    compiled.canonicalize();
    rules = compiled;
    return true;
}

} // namespace engine
