#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "PhysicsSimulationRules.h"

#include "core/data/ini/GeneralsIniParser.h"
#include "ContentFloatParsing.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace engine {
namespace {

using container::asciiEqualIgnoreCase;

[[nodiscard]] bool parseFiniteFloat(
    container::StringView text, container::StringView source,
    container::StringView field, float fallback, float& value) {
    const std::optional<float> parsed =
        game::parseContentFloat(text, {
            .source = source,
            .block = "GameData",
            .module = "PhysicsSimulationRules",
            .field = field,
            .fallback = fallback});
    if (!parsed) return false;
    value = *parsed;
    return true;
}

void setError(container::String* error, container::String value) {
    if (error) *error = std::move(value);
}

[[nodiscard]] bool applyGameDataBlocks(
    const container::Vector<game::IniBlock>& blocks,
    PhysicsSimulationRules& compiled, bool& modified,
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
            float value = 0.0f;
            if (asciiEqualIgnoreCase(key, "Gravity")) {
                if (!parseFiniteFloat(
                        text, source, key,
                        compiled.gravityUnitsPerSecondSq.to_float(), value)) {
                    continue;
                }
                compiled.gravityUnitsPerSecondSq = math::q32_32{value};
            } else if (asciiEqualIgnoreCase(key, "GroundStiffness")) {
                if (!parseFiniteFloat(text, source, key,
                                      compiled.groundStiffness.to_float(), value)) {
                    continue;
                }
                compiled.groundStiffness = math::q32_32{value};
            } else if (asciiEqualIgnoreCase(key, "StructureStiffness")) {
                if (!parseFiniteFloat(text, source, key,
                                      compiled.structureStiffness.to_float(), value)) {
                    continue;
                }
                compiled.structureStiffness = math::q32_32{value};
            } else if (asciiEqualIgnoreCase(
                           key, "DefaultStructureRubbleHeight")) {
                if (!parseFiniteFloat(
                        text, source, key,
                        compiled.defaultStructureRubbleHeight.to_float(), value)) {
                    continue;
                }
                compiled.defaultStructureRubbleHeight = math::q32_32{value};
            } else {
                continue;
            }
            if ((asciiEqualIgnoreCase(key, "GroundStiffness") ||
                 asciiEqualIgnoreCase(key, "StructureStiffness")) &&
                (value < PhysicsSimulationRules::kMinimumGroundStiffness ||
                 value > PhysicsSimulationRules::kMaximumGroundStiffness)) {
                const float adopted = std::clamp(
                    value, PhysicsSimulationRules::kMinimumGroundStiffness,
                    PhysicsSimulationRules::kMaximumGroundStiffness);
                game::processContentDiagnostics().warn({
                    .source = container::String{source},
                    .block = "GameData",
                    .module = "PhysicsSimulationRules",
                    .field = key,
                    .rawValue = text,
                    .adoptedValue = std::to_string(value) +
                        " (effective bounce=" + std::to_string(adopted) + ")",
                    .reason = "preserved original authored stiffness; bounce consumer applies the original safe 0.01..0.99 bound",
                });
            } else if (asciiEqualIgnoreCase(key, "Gravity") && value >= 0.0f) {
                game::processContentDiagnostics().warn({
                    .source = container::String{source},
                    .block = "GameData",
                    .module = "PhysicsSimulationRules",
                    .field = key,
                    .rawValue = text,
                    .adoptedValue = std::to_string(value),
                    .reason = "accepted original non-negative gravity domain; airborne-height consumer derives zero where required",
                });
            } else if (asciiEqualIgnoreCase(
                           key, "DefaultStructureRubbleHeight") &&
                       value < 0.0f) {
                game::processContentDiagnostics().warn({
                    .source = container::String{source},
                    .block = "GameData",
                    .module = "PhysicsSimulationRules",
                    .field = key,
                    .rawValue = text,
                    .adoptedValue = std::to_string(value),
                    .reason = "accepted original negative rubble-height domain",
                });
            }
            modified = true;
        }
    }
    return true;
}

} // namespace

void PhysicsSimulationRules::canonicalize() noexcept {
    // Finite authored values are preserved. The physics consumer derives the
    // original bounce-time stiffness bounds and finite fallback explicitly.
}

bool PhysicsSimulationRules::applyLegacyGameDataOverrides(
    container::StringView content, container::StringView sourceName,
    container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parse(content, sourceName)) {
        setError(error, "could not parse GameData modifier '" +
                            container::String{sourceName} + "'");
        return false;
    }
    PhysicsSimulationRules candidate = *this;
    bool modified = false;
    if (!applyGameDataBlocks(parser.blocks(), candidate, modified,
                             sourceName, error))
        return false;
    if (!modified) return true;
    candidate.canonicalize();
    *this = candidate;
    return true;
}

bool PhysicsSimulationRules::loadFromLegacyGameData(container::StringView path,
                                                     PhysicsSimulationRules& rules,
                                                     container::String* error) {
    if (error) error->clear();

    game::GeneralsIniParser parser;
    if (!parser.parseFile(container::String{path})) {
        setError(error, "could not parse GameData source '" + container::String{path} + "'");
        return false;
    }

    PhysicsSimulationRules compiled;
    bool modified = false;
    if (!applyGameDataBlocks(parser.blocks(), compiled, modified, path, error))
        return false;

    compiled.canonicalize();
    rules = compiled;
    return true;
}

} // namespace engine
