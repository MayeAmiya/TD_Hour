#include "CrateTemplateCatalog.h"

#include "LegacyIniDirectory.h"
#include "VFS.h"
#include "debug/debug.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "game/data/base/ContentFloatParsing.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>

namespace game {
namespace {

[[nodiscard]] container::StringView trim(
    container::StringView value) noexcept {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] container::String canonical(container::StringView value) {
    value = trim(value);
    container::String result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        const char folded = character >= 'A' && character <= 'Z'
            ? static_cast<char>(character + ('a' - 'A'))
            : static_cast<char>(character);
        result.push_back(folded == '\\' ? '/' : folded);
    }
    while (!result.empty() && result.back() == '/') result.pop_back();
    return result;
}

[[nodiscard]] bool equalInsensitive(container::StringView left,
                                    container::StringView right) {
    return canonical(left) == canonical(right);
}

// A field that cannot be parsed, or whose finite prefix does not fit the
// Q32.32 field range, is a content error: callers reject the whole CrateData
// block so the prior definition survives untouched. `priorValue` only labels
// the diagnostic with what stays in effect; it is never adopted as a result.
[[nodiscard]] std::optional<math::q32_32> parseFixed(
    container::StringView value, math::q32_32 priorValue = {}) noexcept {
    const game::ContentFloatContext context{
        .source = __FILE__, .block = "CrateData", .field = "FixedReal",
        .fallback = priorValue.to_float()};
    const std::optional<float> parsed =
        game::parseContentFloat(value, context);
    if (!parsed) return std::nullopt;
    constexpr float kMinimum =
        static_cast<float>(std::numeric_limits<int32_t>::min());
    constexpr float kMaximumExclusive =
        static_cast<float>(std::numeric_limits<int32_t>::max());
    if (*parsed < kMinimum || *parsed >= kMaximumExclusive) {
        game::warnContentFloatFallback(
            value, context,
            "finite numeric prefix is outside the Q32.32 field range; rejected the CrateData block");
        return std::nullopt;
    }
    return math::q32_32{*parsed};
}

[[nodiscard]] std::optional<bool> parseBool(container::StringView value) {
    const container::String text = canonical(value);
    if (text == "yes" || text == "true" || text == "on" || text == "1") {
        return true;
    }
    if (text == "no" || text == "false" || text == "off" || text == "0") {
        return false;
    }
    return std::nullopt;
}

[[nodiscard]] container::Vector<container::String> words(
    container::StringView value) {
    container::Vector<container::String> output;
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t,", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t,", cursor);
        output.emplace_back(value.substr(cursor, end - cursor));
        cursor = end;
    }
    return output;
}

[[nodiscard]] bool applyKindList(
    container::StringView value,
    container::Vector<container::String>& output) {
    const container::Vector<container::String> tokens = words(value);
    if (tokens.empty()) {
        output.clear();
        return true;
    }
    if (tokens.size() == 1 && equalInsensitive(tokens.front(), "NONE")) {
        output.clear();
        return true;
    }
    const bool deltaSyntax = std::all_of(
        tokens.begin(), tokens.end(), [](const container::String& token) {
            return !token.empty() && (token.front() == '+' || token.front() == '-');
        });
    const bool containsDelta = std::any_of(
        tokens.begin(), tokens.end(), [](const container::String& token) {
            return !token.empty() && (token.front() == '+' || token.front() == '-');
        });
    if (containsDelta && !deltaSyntax) return false;
    if (!deltaSyntax) output.clear();
    for (const container::String& authored : tokens) {
        const bool remove = !authored.empty() && authored.front() == '-';
        const size_t prefix = !authored.empty() &&
                (authored.front() == '+' || authored.front() == '-')
            ? 1u : 0u;
        const container::StringView name =
            trim(container::StringView{authored}.substr(prefix));
        if (name.empty()) continue;
        const auto found = std::find_if(
            output.begin(), output.end(), [&](const container::String& current) {
                return equalInsensitive(current, name);
            });
        if (remove) {
            if (found != output.end()) output.erase(found);
        } else if (found == output.end()) {
            output.emplace_back(name);
        }
    }
    return true;
}

[[nodiscard]] std::optional<ObjectVeterancyLevel> parseVeterancy(
    container::StringView value) noexcept {
    value = trim(value);
    if (equalInsensitive(value, "REGULAR")) return ObjectVeterancyLevel::Regular;
    if (equalInsensitive(value, "VETERAN")) return ObjectVeterancyLevel::Veteran;
    if (equalInsensitive(value, "ELITE")) return ObjectVeterancyLevel::Elite;
    if (equalInsensitive(value, "HEROIC")) return ObjectVeterancyLevel::Heroic;
    return std::nullopt;
}

[[nodiscard]] bool applyBlock(const IniBlock& block,
                              CrateTemplateDefinition& output,
                              container::String* error) {
    for (size_t valueIndex = 0;
         valueIndex < block.values.size(); ++valueIndex) {
        const auto& [key, value] = block.values[valueIndex];
        const auto fieldDiagnosticScope =
            contentDiagnosticProvenanceScope(
                block.valueSource(valueIndex));
        if (equalInsensitive(key, "CreationChance")) {
            const std::optional<math::q32_32> parsed =
                parseFixed(value, output.creationChance);
            if (!parsed) {
                if (error) *error = "CrateData '" + output.name +
                    "' has invalid CreationChance '" + value + "'";
                return false;
            }
            output.creationChance = *parsed;
        } else if (equalInsensitive(key, "VeterancyLevel")) {
            const container::String normalized = canonical(value);
            if (normalized == "none" || normalized == "invalid" ||
                normalized == "level_invalid") {
                output.veterancyLevel.reset();
            } else {
                const std::optional<ObjectVeterancyLevel> parsed =
                    parseVeterancy(value);
                if (!parsed) {
                    if (error) *error = "CrateData '" + output.name +
                        "' has invalid VeterancyLevel '" + value + "'";
                    return false;
                }
                output.veterancyLevel = *parsed;
            }
        } else if (equalInsensitive(key, "KilledByType")) {
            if (!applyKindList(value, output.killedByKinds)) {
                if (error) *error = "CrateData '" + output.name +
                    "' mixes replacement and +/- KilledByType syntax";
                return false;
            }
            output.killedByKindMask.clear();
            for (const container::String& kind : output.killedByKinds) {
                if (const std::optional<ObjectKindOf> parsed =
                        parseObjectKindOf(kind)) {
                    setObjectKind(output.killedByKindMask, *parsed);
                }
            }
        } else if (equalInsensitive(key, "KillerScience")) {
            const container::StringView science = trim(value);
            output.killerScience =
                equalInsensitive(science, "NONE") ||
                equalInsensitive(science, "SCIENCE_INVALID")
                ? container::String{} : container::String{science};
        } else if (equalInsensitive(key, "OwnedByMaker")) {
            const std::optional<bool> parsed = parseBool(value);
            if (!parsed) {
                if (error) *error = "CrateData '" + output.name +
                    "' has invalid OwnedByMaker value '" + value + "'";
                return false;
            }
            output.ownedByMaker = *parsed;
        } else if (equalInsensitive(key, "CrateObject")) {
            const container::Vector<container::String> tokens = words(value);
            const std::optional<math::q32_32> chance = tokens.size() >= 2
                ? parseFixed(tokens[1]) : std::nullopt;
            if (tokens.size() < 2 || tokens[0].empty() || !chance) {
                if (error) *error = "CrateData '" + output.name +
                    "' has invalid CrateObject entry '" + value + "'";
                return false;
            }
            output.possibleCrates.push_back({
                .objectTemplate = tokens[0],
                .chance = *chance,
            });
        }
    }
    return true;
}

void warnCrateDefinition(container::StringView source,
                         container::StringView definition,
                         container::String reason,
                         container::String adoptedValue) {
    processContentDiagnostics().warn({
        .source = container::String{source},
        .block = "CrateData",
        .definition = container::String{definition},
        .module = "CrateTemplateCatalog",
        .adoptedValue = std::move(adoptedValue),
        .reason = std::move(reason),
    });
}

} // namespace

container::Vector<container::String>
CrateTemplateCatalog::enumerateVfsLoadFiles(
    container::Span<const container::StringView> loadRoots) {
    return ini::enumerateLegacyIniDirectories(loadRoots);
}

bool CrateTemplateCatalog::loadFromVfsFiles(
    const container::Vector<container::String>& logicalFiles,
    container::String* error) {
    if (error) error->clear();
    clear();
    container::HashMap<container::String, CrateTemplateDefinition> compiled;
    auto& vfs = io::VFS::instance();
    for (const container::String& path : logicalFiles) {
        if (!vfs.exists(path)) {
            warnCrateDefinition(
                path, {}, "CrateData source disappeared from VFS during load",
                "source skipped; definitions from other sources retained");
            continue;
        }
        const container::String layer = vfs.readAll(path);
        GeneralsIniParser parser;
        if (!parser.parse(layer, path)) {
            warnCrateDefinition(
                path, {}, "could not parse CrateData source",
                "source skipped; definitions from other sources retained");
            continue;
        }
        for (const IniBlock& block : parser.blocks()) {
            if (!equalInsensitive(block.type, "CrateData")) continue;
            const auto diagnosticScope =
                contentDiagnosticProvenanceScope(block.source);
            const container::String key = canonical(block.name);
            if (key.empty()) {
                warnCrateDefinition(
                    path, {}, "CrateData block has an empty name",
                    "block skipped");
                continue;
            }
            CrateTemplateDefinition definition;
            if (const auto existing = compiled.find(key);
                existing != compiled.end()) {
                definition = existing->second;
            } else if (key != "defaultcrate") {
                if (const auto defaults = compiled.find("defaultcrate");
                    defaults != compiled.end()) {
                    definition = defaults->second;
                }
            }
            definition.name = container::String{trim(block.name)};
            container::String blockError;
            if (!applyBlock(block, definition, &blockError)) {
                warnCrateDefinition(
                    path, block.name,
                    blockError.empty() ? "invalid CrateData definition"
                                       : std::move(blockError),
                    compiled.contains(key)
                        ? "prior definition retained; bad override block skipped"
                        : "bad definition skipped");
                continue;
            }
            compiled.insert_or_assign(key, std::move(definition));
        }
    }

    container::Vector<std::pair<container::String, CrateTemplateDefinition>>
        ordered;
    ordered.reserve(compiled.size());
    for (auto& entry : compiled) ordered.push_back(std::move(entry));
    std::sort(ordered.begin(), ordered.end(),
        [](const auto& left, const auto& right) {
            return left.first < right.first;
        });
    m_definitions.reserve(ordered.size());
    m_indices.reserve(ordered.size());
    for (auto& [key, definition] : ordered) {
        m_indices.emplace(std::move(key), m_definitions.size());
        m_definitions.push_back(std::move(definition));
    }
    m_loaded = true;
    TD_LOG_INFO("[CrateTemplateCatalog] Loaded {} CrateData recipes from {} source files",
                m_definitions.size(), logicalFiles.size());
    return true;
}

bool CrateTemplateCatalog::applyOverridesFromVfs(
    container::StringView rawPath, container::String* error) {
    if (error) error->clear();
    if (!m_loaded) {
        if (error) *error = "CrateData override requires a loaded base catalog";
        return false;
    }

    const container::String path = canonical(rawPath);
    if (path.empty()) {
        warnCrateDefinition({}, {}, "CrateData override path is empty",
                            "existing catalog retained");
        return true;
    }
    auto& vfs = io::VFS::instance();
    if (!vfs.exists(path)) {
        warnCrateDefinition(path, {},
                            "CrateData override source is absent from VFS",
                            "existing catalog retained");
        return true;
    }

    GeneralsIniParser parser;
    const container::String content = vfs.readAll(path);
    if (!parser.parse(content)) {
        warnCrateDefinition(path, {},
                            "could not parse CrateData override source",
                            "existing catalog retained");
        return true;
    }

    container::HashMap<container::String, CrateTemplateDefinition> compiled;
    compiled.reserve(m_definitions.size());
    for (const CrateTemplateDefinition& current : m_definitions) {
        compiled.insert_or_assign(canonical(current.name), current);
    }

    for (const IniBlock& block : parser.blocks()) {
        if (!equalInsensitive(block.type, "CrateData")) continue;
        const container::String key = canonical(block.name);
        if (key.empty()) {
            warnCrateDefinition(path, {},
                                "CrateData override block has an empty name",
                                "block skipped");
            continue;
        }

        // CreateOverrides copies the current end of the override chain. A
        // first-time definition instead copies DefaultCrate as it exists at
        // this exact parser position. CrateObject fields append to that copy.
        CrateTemplateDefinition definition;
        if (const auto existing = compiled.find(key);
            existing != compiled.end()) {
            definition = existing->second;
        } else if (key != "defaultcrate") {
            if (const auto defaults = compiled.find("defaultcrate");
                defaults != compiled.end()) {
                definition = defaults->second;
            }
        }
        definition.name = container::String{trim(block.name)};
        container::String blockError;
        if (!applyBlock(block, definition, &blockError)) {
            warnCrateDefinition(
                path, block.name,
                blockError.empty() ? "invalid CrateData override definition"
                                   : std::move(blockError),
                compiled.contains(key)
                    ? "prior definition retained; bad override block skipped"
                    : "bad definition skipped");
            continue;
        }
        compiled.insert_or_assign(key, std::move(definition));
    }

    container::Vector<std::pair<container::String, CrateTemplateDefinition>>
        ordered;
    ordered.reserve(compiled.size());
    for (auto& entry : compiled) ordered.push_back(std::move(entry));
    std::sort(ordered.begin(), ordered.end(),
        [](const auto& left, const auto& right) {
            return left.first < right.first;
        });

    container::Vector<CrateTemplateDefinition> sealedDefinitions;
    container::HashMap<container::String, size_t> sealedIndices;
    sealedDefinitions.reserve(ordered.size());
    sealedIndices.reserve(ordered.size());
    for (auto& [key, definition] : ordered) {
        sealedIndices.emplace(std::move(key), sealedDefinitions.size());
        sealedDefinitions.push_back(std::move(definition));
    }

    m_definitions = std::move(sealedDefinitions);
    m_indices = std::move(sealedIndices);
    m_loaded = true;
    return true;
}

bool CrateTemplateCatalog::loadFromVfsLoadDirectories(
    container::Span<const container::StringView> loadRoots,
    container::String* error) {
    return loadFromVfsFiles(enumerateVfsLoadFiles(loadRoots), error);
}

void CrateTemplateCatalog::clear() noexcept {
    m_definitions.clear();
    m_indices.clear();
    m_loaded = false;
}

const CrateTemplateDefinition* CrateTemplateCatalog::find(
    container::StringView name) const noexcept {
    const auto found = m_indices.find(canonical(name));
    return found != m_indices.end() && found->second < m_definitions.size()
        ? &m_definitions[found->second] : nullptr;
}

} // namespace game
