#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/presentation/ScriptTerrainRoadPresentationSettings.h"

#include "core/data/ini/GeneralsIniParser.h"
#include "game/data/base/ContentFloatParsing.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>

namespace engine::script {
namespace {

constexpr auto equalAsciiInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] std::optional<float> parseFiniteFloat(container::StringView value) noexcept {
    return game::parseContentFloat(value, {
        .source = __FILE__, .block = "TerrainRoad", .field = "Real"});
}

[[nodiscard]] std::optional<int32_t> parseInteger(
    container::StringView value) noexcept {
    container::String owned{value};
    char* end = nullptr;
    const long parsed = std::strtol(owned.c_str(), &end, 10);
    if (end == owned.c_str() || parsed < std::numeric_limits<int32_t>::min() ||
        parsed > std::numeric_limits<int32_t>::max()) return std::nullopt;
    return static_cast<int32_t>(parsed);
}

[[nodiscard]] std::optional<container::Array<float, 3>> parseRgb(
    container::StringView value) noexcept {
    container::Array<float, 3> output{};
    container::Array<bool, 3> present{};
    size_t cursor = 0;
    while (cursor < value.size()) {
        while (cursor < value.size() &&
               (value[cursor] == ' ' || value[cursor] == '\t' ||
                value[cursor] == ',')) ++cursor;
        if (cursor >= value.size()) break;
        const char channel = value[cursor++];
        if (cursor >= value.size() || value[cursor] != ':') {
            while (cursor < value.size() && value[cursor] != ' ' &&
                   value[cursor] != '\t' && value[cursor] != ',') ++cursor;
            continue;
        }
        ++cursor;
        const size_t begin = cursor;
        while (cursor < value.size() && value[cursor] != ' ' &&
               value[cursor] != '\t' && value[cursor] != ',') ++cursor;
        const std::optional<float> parsed =
            parseFiniteFloat(value.substr(begin, cursor - begin));
        if (!parsed) return std::nullopt;
        size_t index = 3u;
        if (channel == 'R' || channel == 'r') index = 0u;
        else if (channel == 'G' || channel == 'g') index = 1u;
        else if (channel == 'B' || channel == 'b') index = 2u;
        if (index < output.size()) {
            output[index] = std::clamp(
                *parsed > 1.0f ? *parsed / 255.0f : *parsed,
                0.0f, 1.0f);
            present[index] = true;
        }
    }
    return std::all_of(present.begin(), present.end(),
                       [](bool value) { return value; })
        ? std::optional<container::Array<float, 3>>{output}
        : std::nullopt;
}

} // namespace

const ScriptTerrainRoadStyle* ScriptTerrainRoadPresentationSettings::find(
    container::StringView name) const noexcept {
    for (auto iterator = roads.rbegin(); iterator != roads.rend(); ++iterator) {
        if (equalAsciiInsensitive(iterator->name, name)) return &*iterator;
    }
    return nullptr;
}

const ScriptTerrainBridgeStyle*
ScriptTerrainRoadPresentationSettings::findBridge(
    container::StringView name) const noexcept {
    for (auto iterator = bridges.rbegin(); iterator != bridges.rend();
         ++iterator) {
        if (equalAsciiInsensitive(iterator->name, name)) return &*iterator;
    }
    return nullptr;
}

bool applyScriptTerrainRoadPresentationIni(
    container::StringView content,
    ScriptTerrainRoadPresentationSettings& settings,
    container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parse(content)) {
        if (error) *error = "Could not parse Roads INI content";
        return false;
    }
    for (const game::IniBlock& block : parser.blocks()) {
        if (block.name.empty()) continue;
        if (equalAsciiInsensitive(block.type, "Road")) {
            ScriptTerrainRoadStyle style;
            ScriptTerrainRoadStyle* existing = nullptr;
            for (auto iterator = settings.roads.rbegin();
                 iterator != settings.roads.rend(); ++iterator) {
                if (!equalAsciiInsensitive(iterator->name, block.name)) continue;
                existing = &*iterator;
                style = *existing;
                break;
            }
            if (!existing) {
                // RefCode TerrainRoadCollection::newRoad copies DefaultRoad
                // once, when the new style is created.  Later DefaultRoad
                // changes must not retroactively rewrite existing styles.
                if (!equalAsciiInsensitive(block.name, "DefaultRoad")) {
                    if (const ScriptTerrainRoadStyle* defaultRoad =
                            settings.find("DefaultRoad")) {
                        style.texture = defaultRoad->texture;
                        style.width = defaultRoad->width;
                        style.widthInTexture = defaultRoad->widthInTexture;
                    }
                }
                style.identity = settings.nextRoadIdentity++;
            }
            style.name = block.name;
            for (const auto& [key, value] : block.values) {
                if (equalAsciiInsensitive(key, "Texture")) {
                    style.texture = value;
                } else if (equalAsciiInsensitive(key, "RoadWidth")) {
                    if (const auto parsed = parseFiniteFloat(value)) {
                        style.width = std::clamp(*parsed, 0.25f, 512.0f);
                    }
                } else if (equalAsciiInsensitive(
                               key, "RoadWidthInTexture")) {
                    if (const auto parsed = parseFiniteFloat(value)) {
                        style.widthInTexture =
                            std::clamp(*parsed, 0.01f, 32.0f);
                    }
                }
            }
            if (existing) {
                *existing = std::move(style);
            } else {
                // Empty texture is a legal intermediate/default definition in
                // the original catalog.  Consumers decide whether it can be
                // rendered after all INI layers have been applied.
                settings.roads.push_back(std::move(style));
            }
            continue;
        }
        if (!equalAsciiInsensitive(block.type, "Bridge")) continue;
        ScriptTerrainBridgeStyle style;
        if (const ScriptTerrainBridgeStyle* inherited =
                settings.findBridge(block.name)) {
            style = *inherited;
        }
        style.name = block.name;
        for (const auto& [key, value] : block.values) {
            if (equalAsciiInsensitive(key, "BridgeScale")) {
                if (const auto parsed = parseFiniteFloat(value)) {
                    style.scale = std::clamp(*parsed, 0.01f, 32.0f);
                }
            } else if (equalAsciiInsensitive(key, "RadarColor")) {
                if (const auto parsed = parseRgb(value)) {
                    style.radarColor = *parsed;
                }
            } else if (equalAsciiInsensitive(key, "BridgeModelName")) {
                style.modelNames[0] = value;
            } else if (equalAsciiInsensitive(
                           key, "BridgeModelNameDamaged")) {
                style.modelNames[1] = value;
            } else if (equalAsciiInsensitive(
                           key, "BridgeModelNameReallyDamaged")) {
                style.modelNames[2] = value;
            } else if (equalAsciiInsensitive(
                           key, "BridgeModelNameBroken")) {
                style.modelNames[3] = value;
            } else if (equalAsciiInsensitive(key, "Texture")) {
                style.textureNames[0] = value;
            } else if (equalAsciiInsensitive(key, "TextureDamaged")) {
                style.textureNames[1] = value;
            } else if (equalAsciiInsensitive(
                           key, "TextureReallyDamaged")) {
                style.textureNames[2] = value;
            } else if (equalAsciiInsensitive(key, "TextureBroken")) {
                style.textureNames[3] = value;
            } else if (equalAsciiInsensitive(
                           key, "TowerObjectNameFromLeft")) {
                style.towerObjectNames[0] = value;
            } else if (equalAsciiInsensitive(
                           key, "TowerObjectNameFromRight")) {
                style.towerObjectNames[1] = value;
            } else if (equalAsciiInsensitive(
                           key, "TowerObjectNameToLeft")) {
                style.towerObjectNames[2] = value;
            } else if (equalAsciiInsensitive(
                           key, "TowerObjectNameToRight")) {
                style.towerObjectNames[3] = value;
            } else if (equalAsciiInsensitive(key, "ScaffoldObjectName")) {
                style.scaffoldObjectName = value;
            } else if (equalAsciiInsensitive(
                           key, "ScaffoldSupportObjectName")) {
                style.scaffoldSupportObjectName = value;
            } else if (equalAsciiInsensitive(
                           key, "TransitionEffectsHeight")) {
                if (const auto parsed = parseFiniteFloat(value)) {
                    style.transitionEffectsHeight = *parsed;
                }
            } else if (equalAsciiInsensitive(key, "NumFXPerType")) {
                if (const auto parsed = parseInteger(value)) {
                    style.effectsPerType = std::max(0, *parsed);
                }
            }
        }
        style.scaleFixed = math::q32_32{style.scale};
        style.transitionEffectsHeightFixed =
            math::q32_32{std::max(0.0f, style.transitionEffectsHeight)};
        if (!style.modelNames[0].empty() || !style.textureNames[0].empty()) {
            settings.bridges.push_back(std::move(style));
        }
    }
    return true;
}

} // namespace engine::script
