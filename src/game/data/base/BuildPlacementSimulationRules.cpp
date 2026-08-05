#include "BuildPlacementSimulationRules.h"

#include "ContentFloatParsing.h"
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

void warnField(container::StringView source, container::StringView field,
               container::StringView raw, container::String adopted,
               container::String reason) {
    game::processContentDiagnostics().warn({
        .source = container::String{source},
        .block = "GameData",
        .module = "BuildPlacementSimulationRules",
        .field = container::String{field},
        .rawValue = container::String{raw},
        .adoptedValue = std::move(adopted),
        .reason = std::move(reason),
    });
}

[[nodiscard]] std::optional<uint32_t> parseMaxLineObjects(
    container::StringView value, container::StringView source,
    uint32_t fallback) {
    value = container::trimAsciiView(value);
    const container::String owned{value};
    char* end = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(owned.c_str(), &end, 10);
    if (!end || end == owned.c_str() || errno == ERANGE) {
        warnField(source, "MaxLineBuildObjects", value,
                  std::to_string(fallback),
                  "integer has no representable prefix; retained the prior/default value");
        return std::nullopt;
    }
    if (*end != '\0') {
        warnField(source, "MaxLineBuildObjects", value,
                  parsed >= 0 ? std::to_string(parsed)
                              : std::to_string(fallback),
                  "accepted original integer prefix and ignored noncanonical trailing text");
    }
    if (parsed < 0 || parsed > std::numeric_limits<int32_t>::max()) {
        warnField(source, "MaxLineBuildObjects", value,
                  std::to_string(fallback),
                  "signed legacy limit is unsafe/unrepresentable in the bounded planner; retained the prior/default value");
        return std::nullopt;
    }
    return static_cast<uint32_t>(parsed);
}

void setError(container::String* error, container::String message) {
    if (error) *error = std::move(message);
}

[[nodiscard]] bool applyGameDataBlocks(
    const container::Vector<game::IniBlock>& blocks,
    BuildPlacementSimulationRules& compiled, bool& modified,
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
            math::q32_32* destination = nullptr;
            if (asciiEqualIgnoreCase(key, "MinDistFromEdgeOfMapForBuild")) {
                destination = &compiled.minimumDistanceFromMapEdge;
            } else if (asciiEqualIgnoreCase(key, "SupplyBuildBorder")) {
                destination = &compiled.supplyBuildBorder;
            } else if (asciiEqualIgnoreCase(
                           key, "AllowedHeightVariationForBuilding")) {
                destination = &compiled.allowedHeightVariation;
            } else if (asciiEqualIgnoreCase(key, "MaxLineBuildObjects")) {
                const std::optional<uint32_t> parsed = parseMaxLineObjects(
                    value, source, compiled.maxLineBuildObjects);
                if (!parsed) continue;
                compiled.maxLineBuildObjects = *parsed;
                modified = true;
                continue;
            }
            if (!destination) continue;
            const std::optional<float> parsed = game::parseContentFloat(value, {
                .source = source,
                .block = "GameData",
                .module = "BuildPlacementSimulationRules",
                .field = key,
                .fallback = destination->to_float(),
            });
            if (!parsed) continue;
            *destination = math::q32_32{*parsed};
            if (*parsed < 0.0f) {
                warnField(source, key, value, std::to_string(*parsed),
                          "accepted original negative domain; placement consumers derive a zero lower bound where required");
            }
            modified = true;
        }
    }
    return true;
}

} // namespace

void BuildPlacementSimulationRules::canonicalize() noexcept {
    // Finite authored values are preserved. Placement consumers own their
    // explicit safe zero lower bounds.
}

bool BuildPlacementSimulationRules::applyLegacyGameDataOverrides(
    container::StringView content, container::StringView sourceName,
    container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parse(content, sourceName)) {
        setError(error, "could not parse GameData modifier '" +
                            container::String{sourceName} + "'");
        return false;
    }
    BuildPlacementSimulationRules candidate = *this;
    bool modified = false;
    if (!applyGameDataBlocks(parser.blocks(), candidate, modified,
                             sourceName, error))
        return false;
    if (!modified) return true;
    candidate.canonicalize();
    *this = std::move(candidate);
    return true;
}

bool BuildPlacementSimulationRules::loadFromLegacyGameData(
    container::StringView path, BuildPlacementSimulationRules& rules,
    container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parseFile(container::String{path})) {
        setError(error, "could not parse GameData source '" +
                        container::String{path} + "'");
        return false;
    }
    BuildPlacementSimulationRules compiled;
    bool modified = false;
    if (!applyGameDataBlocks(parser.blocks(), compiled, modified, path, error))
        return false;
    compiled.canonicalize();
    rules = compiled;
    return true;
}

} // namespace engine
