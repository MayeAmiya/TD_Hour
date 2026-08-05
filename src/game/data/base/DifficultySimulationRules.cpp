#include "DifficultySimulationRules.h"

#include "ContentFloatParsing.h"
#include "VFS.h"
#include "core/container/string_utils.h"
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

[[nodiscard]] std::optional<DifficultySimulationRules::Scalar>
parsePercentMultiplier(container::StringView text, container::StringView source,
                       container::StringView field,
                       DifficultySimulationRules::Scalar fallback) {
    text = trim(text);
    if (!text.empty() && text.back() == '%') {
        text.remove_suffix(1);
        text = trim(text);
    }
    const auto percent = game::parseContentFloat(text, {
        .source = source,
        .block = "GameData",
        .module = "DifficultySimulationRules",
        .field = field,
        .fallback = fallback.to_float() * 100.0f,
    });
    if (!percent) return std::nullopt;
    constexpr long double kFixedScale = 4294967296.0L;
    const long double raw =
        static_cast<long double>(*percent) * (kFixedScale / 100.0L);
    if (!std::isfinite(raw) ||
        raw < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
        raw > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        game::processContentDiagnostics().warn({
            .source = container::String{source},
            .block = "GameData",
            .module = "DifficultySimulationRules",
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
            .module = "DifficultySimulationRules",
            .field = container::String{field},
            .rawValue = container::String{text},
            .adoptedValue = std::to_string(*percent),
            .reason = "accepted original non-positive solo health multiplier domain",
        });
    }
    return DifficultySimulationRules::Scalar::from_raw(
        static_cast<int64_t>(raw));
}

void setError(container::String* error, container::String message) {
    if (error) *error = std::move(message);
}

[[nodiscard]] DifficultySimulationRules::Scalar* destinationFor(
    DifficultySimulationRules& rules, container::StringView key) noexcept {
    struct Binding final {
        container::StringView key;
        size_t playerKind = 0;
        size_t difficulty = 0;
    };
    static constexpr container::Array<Binding, 6> bindings{{
        {"HumanSoloPlayerHealthBonus_Easy", 0, 0},
        {"HumanSoloPlayerHealthBonus_Normal", 0, 1},
        {"HumanSoloPlayerHealthBonus_Hard", 0, 2},
        {"AISoloPlayerHealthBonus_Easy", 1, 0},
        {"AISoloPlayerHealthBonus_Normal", 1, 1},
        {"AISoloPlayerHealthBonus_Hard", 1, 2},
    }};
    for (const Binding& binding : bindings) {
        if (asciiEqualIgnoreCase(key, binding.key))
            return &rules.healthMultipliers[binding.playerKind]
                                           [binding.difficulty];
    }
    return nullptr;
}

[[nodiscard]] bool applyGameDataBlocks(
    const container::Vector<game::IniBlock>& blocks,
    DifficultySimulationRules& compiled, bool& modified,
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
            DifficultySimulationRules::Scalar* destination =
                destinationFor(compiled, key);
            if (!destination) continue;
            const auto parsed = parsePercentMultiplier(
                text, source, key, *destination);
            if (!parsed) continue;
            *destination = *parsed;
            modified = true;
        }
    }
    return true;
}

} // namespace

DifficultySimulationRules::Scalar DifficultySimulationRules::healthMultiplier(
    size_t playerKind, size_t difficulty) const noexcept {
    if (playerKind >= kPlayerKindCount || difficulty >= kDifficultyCount)
        return Scalar{int32_t{1}};
    return healthMultipliers[playerKind][difficulty];
}

void DifficultySimulationRules::canonicalize() noexcept {
    // Preserve authored multipliers. Ingress already excluded non-finite and
    // unrepresentable percentages.
}

bool DifficultySimulationRules::applyLegacyGameDataOverrides(
    container::StringView content, container::StringView sourceName,
    container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parse(content, sourceName)) {
        setError(error, "could not parse GameData modifier '" +
                            container::String{sourceName} + "'");
        return false;
    }
    DifficultySimulationRules candidate = *this;
    bool modified = false;
    if (!applyGameDataBlocks(parser.blocks(), candidate, modified,
                             sourceName, error))
        return false;
    if (!modified) return true;
    candidate.canonicalize();
    *this = candidate;
    return true;
}

bool DifficultySimulationRules::loadFromLegacyGameData(
    container::StringView path, DifficultySimulationRules& rules,
    container::String* error) {
    if (error) error->clear();
    DifficultySimulationRules compiled;
    bool modified = false;
    auto& vfs = io::VFS::instance();
    if (vfs.exists(path)) {
        game::GeneralsIniParser parser;
        if (!parser.parse(vfs.readAll(path), path) ||
            !applyGameDataBlocks(parser.blocks(), compiled, modified,
                                 path, error)) {
            if (error && error->empty())
                *error = "could not parse GameData source '" +
                    container::String{path} + "'";
            return false;
        }
    } else {
        game::GeneralsIniParser parser;
        if (!parser.parseFile(container::String{path}) ||
            !applyGameDataBlocks(parser.blocks(), compiled, modified,
                                 path, error)) {
            if (error && error->empty())
                *error = "could not parse GameData source '" +
                    container::String{path} + "'";
            return false;
        }
    }
    compiled.canonicalize();
    rules = compiled;
    return true;
}

} // namespace engine
