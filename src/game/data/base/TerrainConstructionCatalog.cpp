#include "TerrainConstructionCatalog.h"

#include "core/container/string_utils.h"
#include "core/data/ini/GeneralsIniParser.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <utility>

namespace engine {
namespace {

using container::asciiEqualIgnoreCase;

[[nodiscard]] container::StringView trimAscii(
    container::StringView value) noexcept {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == container::StringView::npos) return {};
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1u);
}

[[nodiscard]] std::optional<bool> parseLegacyBool(
    container::StringView value) noexcept {
    value = trimAscii(value);
    if (asciiEqualIgnoreCase(value, "yes") ||
        asciiEqualIgnoreCase(value, "true") || value == "1") {
        return true;
    }
    if (asciiEqualIgnoreCase(value, "no") ||
        asciiEqualIgnoreCase(value, "false") || value == "0") {
        return false;
    }
    return std::nullopt;
}

void setError(container::String* error, container::String message) {
    if (error) *error = std::move(message);
}

} // namespace

bool TerrainConstructionCatalog::applyLegacyIni(
    container::StringView content, container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parse(content)) {
        setError(error, "could not parse Terrain.ini content");
        return false;
    }
    TerrainConstructionCatalog compiled = *this;

    for (const game::IniBlock& block : parser.blocks()) {
        if (!asciiEqualIgnoreCase(block.type, "Terrain") ||
            block.name.empty()) {
            continue;
        }
        auto found = compiled.m_definitions.find(block.name);
        if (found == compiled.m_definitions.end()) {
            TerrainConstructionDefinition definition;
            if (const auto defaultTerrain =
                    compiled.m_definitions.find("DefaultTerrain");
                defaultTerrain != compiled.m_definitions.end()) {
                definition = defaultTerrain->second;
            }
            definition.name = block.name;
            found = compiled.m_definitions.emplace(
                block.name, std::move(definition)).first;
        }
        for (const auto& [key, value] : block.values) {
            if (!asciiEqualIgnoreCase(key, "RestrictConstruction")) continue;
            const std::optional<bool> parsed = parseLegacyBool(value);
            if (!parsed) {
                setError(error, "invalid Terrain " + block.name +
                    ".RestrictConstruction value '" + value + "'");
                return false;
            }
            found->second.restrictConstruction = *parsed;
        }
    }
    *this = std::move(compiled);
    return true;
}

bool TerrainConstructionCatalog::applyLegacyIniFile(
    container::StringView path, container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parseFile(container::String{path})) {
        setError(error, "could not parse Terrain.ini source '" +
            container::String{path} + "'");
        return false;
    }
    TerrainConstructionCatalog compiled = *this;
    // parseFile has already selected the VFS winner. Reapply the typed blocks
    // directly so an existing catalog can first
    // consume Default/Terrain.ini and then Terrain.ini.
    for (const game::IniBlock& block : parser.blocks()) {
        if (!asciiEqualIgnoreCase(block.type, "Terrain") ||
            block.name.empty()) {
            continue;
        }
        auto found = compiled.m_definitions.find(block.name);
        if (found == compiled.m_definitions.end()) {
            TerrainConstructionDefinition definition;
            if (const auto defaultTerrain =
                    compiled.m_definitions.find("DefaultTerrain");
                defaultTerrain != compiled.m_definitions.end()) {
                definition = defaultTerrain->second;
            }
            definition.name = block.name;
            found = compiled.m_definitions.emplace(
                block.name, std::move(definition)).first;
        }
        for (const auto& [key, value] : block.values) {
            if (!asciiEqualIgnoreCase(key, "RestrictConstruction")) continue;
            const std::optional<bool> parsed = parseLegacyBool(value);
            if (!parsed) {
                setError(error, "invalid Terrain " + block.name +
                    ".RestrictConstruction value '" + value + "'");
                return false;
            }
            found->second.restrictConstruction = *parsed;
        }
    }
    *this = std::move(compiled);
    return true;
}

const TerrainConstructionDefinition* TerrainConstructionCatalog::find(
    container::StringView name) const noexcept {
    const auto found = m_definitions.find(container::String{name});
    return found == m_definitions.end() ? nullptr : &found->second;
}

bool TerrainConstructionCatalog::restrictsConstruction(
    container::StringView name) const noexcept {
    const TerrainConstructionDefinition* definition = find(name);
    return definition && definition->restrictConstruction;
}

size_t TerrainConstructionCatalog::restrictedCount() const noexcept {
    return static_cast<size_t>(std::count_if(
        m_definitions.begin(), m_definitions.end(),
        [](const auto& entry) {
            return entry.second.restrictConstruction;
        }));
}

} // namespace engine
