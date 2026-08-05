#include "AISimulationRules.h"

#include "ContentBoolParsing.h"
#include "ContentFloatParsing.h"
#include "VFS.h"
#include "core/container/string_utils.h"
#include "core/data/ini/GeneralsIniParser.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <iterator>
#include <optional>
#include <string>
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
        .block = "AIData",
        .module = "AISimulationRules",
        .field = container::String{field},
        .rawValue = container::String{raw},
        .adoptedValue = std::move(adopted),
        .reason = std::move(reason),
    });
}

[[nodiscard]] std::optional<AISimulationRules::Scalar> parseReal(
    container::StringView text, container::StringView source,
    container::StringView field, AISimulationRules::Scalar fallback) {
    const auto parsed = game::parseContentFloat(text, {
        .source = source,
        .block = "AIData",
        .module = "AISimulationRules",
        .field = field,
        .fallback = fallback.to_float(),
    });
    if (!parsed) return std::nullopt;
    constexpr float maximum =
        static_cast<float>(std::numeric_limits<int32_t>::max());
    constexpr float minimum =
        static_cast<float>(std::numeric_limits<int32_t>::min());
    if (*parsed >= maximum || *parsed <= minimum) {
        warnField(source, field, text, std::to_string(fallback.to_float()),
                  "real is not representable in Q32.32; retained the prior/default value");
        return std::nullopt;
    }
    return AISimulationRules::Scalar{*parsed};
}

[[nodiscard]] std::optional<uint32_t> parseMilliseconds(
    container::StringView text, container::StringView source,
    container::StringView field, uint32_t fallback) {
    text = trim(text);
    const container::String owned{text};
    char* end = nullptr;
    errno = 0;
    const long long value = std::strtoll(owned.c_str(), &end, 10);
    if (end == owned.c_str() || errno == ERANGE) {
        warnField(source, field, text, std::to_string(fallback),
                  "duration has no representable integer prefix; retained the prior/default value");
        return std::nullopt;
    }
    const uint32_t adopted = static_cast<uint32_t>(value);
    if (*end != '\0') {
        warnField(source, field, text, std::to_string(adopted),
                  "accepted original integer prefix and ignored noncanonical trailing text");
    }
    if (value < 0 || static_cast<unsigned long long>(value) >
                         std::numeric_limits<uint32_t>::max()) {
        warnField(source, field, text, std::to_string(adopted),
                  "accepted original signed-to-unsigned duration conversion");
    }
    return adopted;
}

[[nodiscard]] std::optional<int32_t> parseInteger(
    container::StringView text) noexcept {
    text = trim(text);
    const container::String owned{text};
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(owned.c_str(), &end, 10);
    if (end == owned.c_str() || errno == ERANGE ||
        value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) return std::nullopt;
    return static_cast<int32_t>(value);
}

[[nodiscard]] const container::String* valueLast(
    const game::IniBlock& block, container::StringView key) noexcept {
    for (auto iterator = block.values.rbegin();
         iterator != block.values.rend(); ++iterator) {
        if (asciiEqualIgnoreCase(iterator->first, key))
            return &iterator->second;
    }
    return nullptr;
}

[[nodiscard]] std::optional<AISimulationRules::Scalar> coordinateAxis(
    container::StringView value, char axis, container::StringView source,
    container::StringView field) {
    for (size_t cursor = 0; cursor + 1 < value.size(); ++cursor) {
        const char current = static_cast<char>(std::toupper(
            static_cast<unsigned char>(value[cursor])));
        if (current != axis) continue;
        size_t colon = cursor + 1;
        while (colon < value.size() &&
               (value[colon] == ' ' || value[colon] == '\t')) ++colon;
        if (colon >= value.size() || value[colon] != ':') continue;
        size_t begin = colon + 1;
        while (begin < value.size() &&
               (value[begin] == ' ' || value[begin] == '\t')) ++begin;
        size_t end = begin;
        while (end < value.size() && value[end] != ' ' &&
               value[end] != '\t' && value[end] != ',') ++end;
        return parseReal(value.substr(begin, end - begin), source, field, {});
    }
    return std::nullopt;
}

void applySideInfo(const game::IniBlock& block,
                   AISimulationRules& compiled) {
    auto found = std::find_if(
        compiled.sides.begin(), compiled.sides.end(),
        [&block](const AISideInfoRule& side) {
            return asciiEqualIgnoreCase(side.side, block.name);
        });
    if (found == compiled.sides.end()) {
        compiled.sides.push_back({.side = block.name});
        found = std::prev(compiled.sides.end());
    }
    const auto gatherers = [&](container::StringView key, size_t index) {
        if (const container::String* value = valueLast(block, key)) {
            if (const std::optional<int32_t> parsed = parseInteger(*value);
                parsed && *parsed >= 0) {
                found->resourceGatherers[index] =
                    static_cast<uint32_t>(*parsed);
            }
        }
    };
    gatherers("ResourceGatherersEasy", 0);
    gatherers("ResourceGatherersNormal", 1);
    gatherers("ResourceGatherersHard", 2);
    if (const container::String* value =
            valueLast(block, "BaseDefenseStructure1")) {
        found->baseDefenseStructure = *value;
    }
    for (const game::IniBlock& child : block.children) {
        size_t skillIndex = found->skillSets.size();
        for (size_t index = 0; index < found->skillSets.size(); ++index) {
            const container::String ordinal = std::to_string(index + 1u);
            if (asciiEqualIgnoreCase(
                    child.type,
                    container::String{"SkillSet"} + ordinal) ||
                (asciiEqualIgnoreCase(child.type, "SkillSet") &&
                 asciiEqualIgnoreCase(child.name, ordinal))) {
                skillIndex = index;
                break;
            }
        }
        if (skillIndex >= found->skillSets.size()) continue;
        container::Vector<container::String>& skills =
            found->skillSets[skillIndex];
        skills.clear();
        for (const auto& [key, value] : child.values) {
            if (asciiEqualIgnoreCase(key, "Science") && !value.empty())
                skills.push_back(value);
        }
    }
}

void applySkirmishBuildList(const game::IniBlock& block,
                            AISimulationRules& compiled,
                            container::StringView source) {
    AISkirmishBuildListRule list{.side = block.name};
    list.structures.reserve(block.children.size());
    for (const game::IniBlock& child : block.children) {
        if (!asciiEqualIgnoreCase(child.type, "Structure") ||
            child.name.empty()) continue;
        AISkirmishBuildStructureRule structure{.objectType = child.name};
        const container::String* location = valueLast(child, "Location");
        const std::optional<AISimulationRules::Scalar> x = location
            ? coordinateAxis(*location, 'X', source, "Location.X")
            : std::nullopt;
        const std::optional<AISimulationRules::Scalar> y = location
            ? coordinateAxis(*location, 'Y', source, "Location.Y")
            : std::nullopt;
        if (!x || !y) continue;
        structure.x = *x;
        structure.y = *y;
        if (const container::String* value = valueLast(child, "Angle")) {
            if (const std::optional<AISimulationRules::Scalar> degrees =
                    parseReal(*value, source, "Angle", {})) {
                structure.yawRadians = *degrees *
                    AISimulationRules::Scalar{3.14159265358979323846} /
                    AISimulationRules::Scalar{180};
            }
        }
        if (const container::String* value = valueLast(child, "Rebuilds")) {
            if (const std::optional<int32_t> parsed = parseInteger(*value))
                structure.rebuilds = *parsed;
        }
        if (const container::String* value =
                valueLast(child, "InitiallyBuilt")) {
            structure.initiallyBuilt =
                game::tryParseContentBool(*value).value_or(false);
        }
        if (const container::String* value =
                valueLast(child, "AutomaticallyBuild")) {
            structure.automaticallyBuild =
                game::tryParseContentBool(*value).value_or(false);
        }
        list.structures.push_back(std::move(structure));
    }
    auto found = std::find_if(
        compiled.skirmishBuildLists.begin(),
        compiled.skirmishBuildLists.end(),
        [&block](const AISkirmishBuildListRule& value) {
            return asciiEqualIgnoreCase(value.side, block.name);
        });
    if (found == compiled.skirmishBuildLists.end())
        compiled.skirmishBuildLists.push_back(std::move(list));
    else
        *found = std::move(list);
}

void setError(container::String* error, container::String message) {
    if (error) *error = std::move(message);
}

[[nodiscard]] bool applyBlocks(
    const container::Vector<game::IniBlock>& blocks,
    AISimulationRules& compiled, bool& modified,
    container::StringView source, container::String* /*error*/) {
    for (const game::IniBlock& block : blocks) {
        if (!asciiEqualIgnoreCase(block.type, "AIData")) continue;
        const auto blockDiagnosticScope =
            game::contentDiagnosticProvenanceScope(block.source);
        for (size_t valueIndex = 0;
             valueIndex < block.values.size(); ++valueIndex) {
            const auto& [key, text] = block.values[valueIndex];
            const auto fieldDiagnosticScope =
                game::contentDiagnosticProvenanceScope(
                    block.valueSource(valueIndex));
            AISimulationRules::Scalar* realDestination = nullptr;
            uint32_t* durationDestination = nullptr;
            uint32_t* unsignedDestination = nullptr;
            bool* boolDestination = nullptr;
            if (asciiEqualIgnoreCase(key, "AttackUsesLineOfSight")) {
                const std::optional<bool> parsed =
                    game::tryParseContentBool(text);
                if (!parsed) {
                    warnField(source, key, text,
                              compiled.attackUsesLineOfSight ? "Yes" : "No",
                              "boolean has no recognizable ZH token; retained the prior/default value");
                    continue;
                }
                compiled.attackUsesLineOfSight = *parsed;
                modified = true;
                continue;
            }
            if (asciiEqualIgnoreCase(key, "GuardInnerModifierAI"))
                realDestination = &compiled.guardInnerModifierAI;
            else if (asciiEqualIgnoreCase(key, "GuardOuterModifierAI"))
                realDestination = &compiled.guardOuterModifierAI;
            else if (asciiEqualIgnoreCase(key, "GuardInnerModifierHuman"))
                realDestination = &compiled.guardInnerModifierHuman;
            else if (asciiEqualIgnoreCase(key, "GuardOuterModifierHuman"))
                realDestination = &compiled.guardOuterModifierHuman;
            else if (asciiEqualIgnoreCase(key, "MaxRetaliationDistance"))
                realDestination = &compiled.maximumRetaliationDistance;
            else if (asciiEqualIgnoreCase(key, "RetaliationFriendsRadius"))
                realDestination = &compiled.retaliationFriendsRadius;
            else if (asciiEqualIgnoreCase(key, "AlertRangeModifier"))
                realDestination = &compiled.alertRangeModifier;
            else if (asciiEqualIgnoreCase(key, "AggressiveRangeModifier"))
                realDestination = &compiled.aggressiveRangeModifier;
            else if (asciiEqualIgnoreCase(
                         key, "AttackPriorityDistanceModifier"))
                realDestination = &compiled.attackPriorityDistanceModifier;
            else if (asciiEqualIgnoreCase(key, "MaxRecruitRadius"))
                realDestination = &compiled.maximumRecruitDistance;
            else if (asciiEqualIgnoreCase(key, "RepulsedDistance"))
                realDestination = &compiled.repulsedDistance;
            else if (asciiEqualIgnoreCase(key, "StructureSeconds"))
                realDestination = &compiled.structureSeconds;
            else if (asciiEqualIgnoreCase(key, "TeamSeconds"))
                realDestination = &compiled.teamSeconds;
            else if (asciiEqualIgnoreCase(key, "Wealthy"))
                realDestination = &compiled.wealthy;
            else if (asciiEqualIgnoreCase(key, "Poor"))
                realDestination = &compiled.poor;
            else if (asciiEqualIgnoreCase(key, "StructuresWealthyRate"))
                realDestination = &compiled.structuresWealthyRate;
            else if (asciiEqualIgnoreCase(key, "StructuresPoorRate"))
                realDestination = &compiled.structuresPoorRate;
            else if (asciiEqualIgnoreCase(key, "TeamsWealthyRate"))
                realDestination = &compiled.teamsWealthyRate;
            else if (asciiEqualIgnoreCase(key, "TeamsPoorRate"))
                realDestination = &compiled.teamsPoorRate;
            else if (asciiEqualIgnoreCase(key, "TeamResourcesToStart"))
                realDestination = &compiled.teamResourcesToStart;
            else if (asciiEqualIgnoreCase(key, "SkirmishBaseDefenseExtraDistance"))
                realDestination = &compiled.skirmishBaseDefenseExtraDistance;
            else if (asciiEqualIgnoreCase(key, "WallHeight"))
                realDestination = &compiled.wallHeight;
            else if (asciiEqualIgnoreCase(key, "SkirmishGroupFudgeDistance"))
                realDestination = &compiled.skirmishGroupFudgeDistance;
            else if (asciiEqualIgnoreCase(key, "MinDistanceForGroup"))
                realDestination = &compiled.minimumDistanceForGroup;
            else if (asciiEqualIgnoreCase(key, "DistanceRequiresGroup"))
                realDestination = &compiled.distanceRequiresGroup;
            else if (asciiEqualIgnoreCase(key, "SupplyCenterSafeRadius"))
                realDestination = &compiled.supplyCenterSafeRadius;
            else if (asciiEqualIgnoreCase(key, "RebuildDelayTimeSeconds"))
                realDestination = &compiled.rebuildDelayTimeSeconds;
            else if (asciiEqualIgnoreCase(key, "AIDozerBoredRadiusModifier"))
                realDestination = &compiled.aiDozerBoredRadiusModifier;
            else if (asciiEqualIgnoreCase(key, "ForceIdleMSEC"))
                durationDestination = &compiled.forceIdleMilliseconds;
            else if (asciiEqualIgnoreCase(key, "GuardChaseUnitsDuration"))
                durationDestination = &compiled.guardChaseDurationMilliseconds;
            else if (asciiEqualIgnoreCase(key, "GuardEnemyScanRate"))
                durationDestination = &compiled.guardEnemyScanMilliseconds;
            else if (asciiEqualIgnoreCase(key, "GuardEnemyReturnScanRate"))
                durationDestination = &compiled.guardEnemyReturnScanMilliseconds;
            else if (asciiEqualIgnoreCase(key, "MinInfantryForGroup"))
                unsignedDestination = &compiled.minimumInfantryForGroup;
            else if (asciiEqualIgnoreCase(key, "MinVehiclesForGroup"))
                unsignedDestination = &compiled.minimumVehiclesForGroup;
            else if (asciiEqualIgnoreCase(key, "InfantryPathfindDiameter"))
                unsignedDestination = &compiled.infantryPathfindDiameter;
            else if (asciiEqualIgnoreCase(key, "VehiclePathfindDiameter"))
                unsignedDestination = &compiled.vehiclePathfindDiameter;
            else if (asciiEqualIgnoreCase(key, "ForceSkirmishAI"))
                boolDestination = &compiled.forceSkirmishAI;
            else if (asciiEqualIgnoreCase(key, "RotateSkirmishBases"))
                boolDestination = &compiled.rotateSkirmishBases;
            else if (asciiEqualIgnoreCase(key, "EnableRepulsors"))
                boolDestination = &compiled.enableRepulsors;
            else if (asciiEqualIgnoreCase(key, "AttackIgnoreInsignificantBuildings"))
                boolDestination = &compiled.attackIgnoreInsignificantBuildings;
            else if (asciiEqualIgnoreCase(key, "AICrushesInfantry"))
                boolDestination = &compiled.aiCrushesInfantry;
            else
                continue;

            if (realDestination) {
                const auto parsed = parseReal(
                    text, source, key, *realDestination);
                if (!parsed) continue;
                *realDestination = *parsed;
                const bool unusual =
                    *parsed < AISimulationRules::Scalar{};
                if (unusual) {
                    warnField(source, key, text,
                              std::to_string(parsed->to_float()),
                              "accepted original out-of-recommended-domain value; consumers derive a safe zero/disabled behavior where required");
                }
            } else if (durationDestination) {
                const auto parsed = parseMilliseconds(
                    text, source, key, *durationDestination);
                if (!parsed) continue;
                *durationDestination = *parsed;
            } else if (unsignedDestination) {
                const std::optional<int32_t> parsed = parseInteger(text);
                if (!parsed || *parsed < 0) continue;
                *unsignedDestination = static_cast<uint32_t>(*parsed);
            } else if (boolDestination) {
                const std::optional<bool> parsed =
                    game::tryParseContentBool(text);
                if (!parsed) continue;
                *boolDestination = *parsed;
            }
            modified = true;
        }
        for (const game::IniBlock& child : block.children) {
            if (asciiEqualIgnoreCase(child.type, "SideInfo")) {
                applySideInfo(child, compiled);
                modified = true;
            } else if (asciiEqualIgnoreCase(
                           child.type, "SkirmishBuildList")) {
                applySkirmishBuildList(child, compiled, source);
                modified = true;
            }
        }
    }
    return true;
}

// GeneralsIniParser::parseFile already selects the single VFS winner for one
// logical path. Keep the local-file fallback for tool/test fixtures which pass
// a physical path that was never mounted.
[[nodiscard]] bool applyAIDataFile(
    container::StringView path, AISimulationRules& candidate, bool& modified,
    container::String* error) {
    game::GeneralsIniParser parser;
    auto& vfs = io::VFS::instance();
    const bool parsed = vfs.exists(path)
        ? parser.parse(vfs.readAll(path), path)
        : parser.parseFile(container::String{path});
    if (!parsed ||
        !applyBlocks(parser.blocks(), candidate, modified, path, error)) {
        if (error && error->empty())
            *error = "could not parse AIData source '" +
                container::String{path} + "'";
        return false;
    }
    return true;
}

} // namespace

void AISimulationRules::canonicalize() noexcept {
    // Preserve authored AIData domain values. Individual consumers already
    // derive safe disabled/no-penalty behavior for non-positive parameters.
}

const AISideInfoRule* AISimulationRules::sideInfo(
    container::StringView side) const noexcept {
    const auto found = std::find_if(
        sides.begin(), sides.end(), [side](const AISideInfoRule& value) {
            return asciiEqualIgnoreCase(value.side, side);
        });
    return found == sides.end() ? nullptr : &*found;
}

const AISkirmishBuildListRule* AISimulationRules::skirmishBuildList(
    container::StringView side) const noexcept {
    const auto found = std::find_if(
        skirmishBuildLists.begin(), skirmishBuildLists.end(),
        [side](const AISkirmishBuildListRule& value) {
            return asciiEqualIgnoreCase(value.side, side);
        });
    return found == skirmishBuildLists.end() ? nullptr : &*found;
}

bool AISimulationRules::applyLegacyAIDataOverrides(
    container::StringView content, container::StringView sourceName,
    container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parse(content, sourceName)) {
        setError(error, "could not parse AIData modifier '" +
                            container::String{sourceName} + "'");
        return false;
    }
    AISimulationRules candidate = *this;
    bool modified = false;
    if (!applyBlocks(parser.blocks(), candidate, modified, sourceName, error))
        return false;
    if (!modified) return true;
    candidate.canonicalize();
    *this = candidate;
    return true;
}

bool AISimulationRules::loadFromLegacyAIData(
    container::StringView path, AISimulationRules& rules,
    container::String* error) {
    if (error) error->clear();
    AISimulationRules compiled;
    bool modified = false;
    if (!applyAIDataFile(path, compiled, modified, error)) return false;
    compiled.canonicalize();
    rules = compiled;
    return true;
}

bool AISimulationRules::applyLegacyAIDataFile(
    container::StringView path, AISimulationRules& rules,
    container::String* error) {
    if (error) error->clear();
    AISimulationRules candidate = rules;
    bool modified = false;
    if (!applyAIDataFile(path, candidate, modified, error)) return false;
    if (!modified) return true;
    candidate.canonicalize();
    rules = candidate;
    return true;
}

} // namespace engine
