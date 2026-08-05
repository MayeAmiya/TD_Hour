#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/presentation/ScriptWaterPresentationSettings.h"

#include "core/data/ini/GeneralsIniParser.h"
#include "game/data/base/ContentFloatParsing.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <optional>

namespace engine::script {
namespace {

constexpr auto equalAsciiInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] std::optional<float> parseFiniteFloat(container::StringView value) noexcept {
    return game::parseContentFloat(value, {
        .source = __FILE__, .block = "Water", .field = "Real"});
}

[[nodiscard]] std::optional<int> parseInteger(container::StringView value) noexcept {
    int parsed = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto [cursor, error] = std::from_chars(begin, end, parsed);
    return error == std::errc{} && cursor != begin
        ? std::optional<int>{parsed} : std::nullopt;
}

[[nodiscard]] bool parseBoolean(container::StringView value, bool fallback) noexcept {
    if (equalAsciiInsensitive(value, "yes") || equalAsciiInsensitive(value, "true") ||
        value == "1") return true;
    if (equalAsciiInsensitive(value, "no") || equalAsciiInsensitive(value, "false") ||
        value == "0") return false;
    return fallback;
}

[[nodiscard]] std::optional<ScriptWaterColor> parseColor(
    container::StringView value,
    ScriptWaterColor fallback) noexcept {
    container::Array<float, 4> components{
        fallback.red, fallback.green, fallback.blue, fallback.alpha};
    container::Array<bool, 4> present{};
    size_t cursor = 0;
    while (cursor < value.size()) {
        while (cursor < value.size() &&
               (value[cursor] == ' ' || value[cursor] == '\t' || value[cursor] == ',')) {
            ++cursor;
        }
        if (cursor >= value.size()) break;
        const char channel = value[cursor++];
        if (cursor >= value.size() || value[cursor] != ':') {
            while (cursor < value.size() && value[cursor] != ' ' && value[cursor] != '\t') ++cursor;
            continue;
        }
        ++cursor;
        const size_t begin = cursor;
        while (cursor < value.size() && value[cursor] != ' ' && value[cursor] != '\t' &&
               value[cursor] != ',') ++cursor;
        const std::optional<float> parsed = parseFiniteFloat(value.substr(begin, cursor - begin));
        if (!parsed) return std::nullopt;
        size_t component = 4;
        if (channel == 'R' || channel == 'r') component = 0;
        else if (channel == 'G' || channel == 'g') component = 1;
        else if (channel == 'B' || channel == 'b') component = 2;
        else if (channel == 'A' || channel == 'a') component = 3;
        if (component < components.size()) {
            components[component] = *parsed;
            present[component] = true;
        }
    }
    if (!present[0] || !present[1] || !present[2]) return std::nullopt;
    float largest = 0.0f;
    for (size_t index = 0; index < components.size(); ++index) {
        if (present[index]) largest = std::max(largest, components[index]);
    }
    const float divisor = largest > 1.0f ? 255.0f : 1.0f;
    return ScriptWaterColor{
        std::clamp(components[0] / divisor, 0.0f, 1.0f),
        std::clamp(components[1] / divisor, 0.0f, 1.0f),
        std::clamp(components[2] / divisor, 0.0f, 1.0f),
        present[3]
            ? std::clamp(components[3] / divisor, 0.0f, 1.0f)
            : fallback.alpha,
    };
}

[[nodiscard]] std::optional<size_t> timeOfDayIndex(container::StringView name) noexcept {
    if (equalAsciiInsensitive(name, "MORNING")) return 0;
    if (equalAsciiInsensitive(name, "AFTERNOON")) return 1;
    if (equalAsciiInsensitive(name, "EVENING")) return 2;
    if (equalAsciiInsensitive(name, "NIGHT")) return 3;
    return std::nullopt;
}

void applyWaterSet(ScriptWaterTimeOfDaySettings& output,
                   const game::IniBlock& block) {
    for (const auto& [key, value] : block.values) {
        if (equalAsciiInsensitive(key, "SkyTexture")) output.skyTexture = value;
        else if (equalAsciiInsensitive(key, "WaterTexture")) output.waterTexture = value;
        else if (equalAsciiInsensitive(key, "Vertex00Color")) {
            if (const auto parsed = parseColor(value, output.vertexColors[0])) output.vertexColors[0] = *parsed;
        } else if (equalAsciiInsensitive(key, "Vertex10Color")) {
            if (const auto parsed = parseColor(value, output.vertexColors[1])) output.vertexColors[1] = *parsed;
        } else if (equalAsciiInsensitive(key, "Vertex01Color")) {
            if (const auto parsed = parseColor(value, output.vertexColors[2])) output.vertexColors[2] = *parsed;
        } else if (equalAsciiInsensitive(key, "Vertex11Color")) {
            if (const auto parsed = parseColor(value, output.vertexColors[3])) output.vertexColors[3] = *parsed;
        } else if (equalAsciiInsensitive(key, "DiffuseColor")) {
            if (const auto parsed = parseColor(value, output.diffuseColor)) output.diffuseColor = *parsed;
        } else if (equalAsciiInsensitive(key, "TransparentDiffuseColor")) {
            if (const auto parsed = parseColor(value, output.transparentDiffuseColor)) output.transparentDiffuseColor = *parsed;
        } else if (equalAsciiInsensitive(key, "UScrollPerMS")) {
            if (const auto parsed = parseFiniteFloat(value)) output.uScrollPerMillisecond = *parsed;
        } else if (equalAsciiInsensitive(key, "VScrollPerMS")) {
            if (const auto parsed = parseFiniteFloat(value)) output.vScrollPerMillisecond = *parsed;
        } else if (equalAsciiInsensitive(key, "SkyTexelsPerUnit")) {
            if (const auto parsed = parseFiniteFloat(value)) output.skyTexelsPerUnit = *parsed;
        } else if (equalAsciiInsensitive(key, "WaterRepeatCount")) {
            if (const auto parsed = parseInteger(value)) output.waterRepeatCount = std::max(1, *parsed);
        }
    }
}

void applyWaterTransparency(ScriptWaterPresentationSettings& output,
                            const game::IniBlock& block) {
    for (const auto& [key, value] : block.values) {
        if (equalAsciiInsensitive(key, "TransparentWaterDepth")) {
            if (const auto parsed = parseFiniteFloat(value)) output.transparentWaterDepth = std::max(0.0f, *parsed);
        } else if (equalAsciiInsensitive(key, "TransparentWaterMinOpacity")) {
            if (const auto parsed = parseFiniteFloat(value)) output.minimumWaterOpacity = std::clamp(*parsed, 0.0f, 1.0f);
        } else if (equalAsciiInsensitive(key, "StandingWaterColor")) {
            if (const auto parsed = parseColor(value, output.standingWaterColor)) output.standingWaterColor = *parsed;
        } else if (equalAsciiInsensitive(key, "RadarWaterColor")) {
            if (const auto parsed = parseColor(value, output.radarWaterColor)) output.radarWaterColor = *parsed;
        } else if (equalAsciiInsensitive(key, "StandingWaterTexture")) {
            output.standingWaterTexture = value;
        } else if (equalAsciiInsensitive(key, "AdditiveBlending")) {
            output.additiveBlending = parseBoolean(value, output.additiveBlending);
        }
    }
}

} // namespace

ScriptWaterPresentationSettings::ScriptWaterPresentationSettings() {
    constexpr container::Array<ScriptWaterColor, 4> vertices{
        ScriptWaterColor{200.0f / 255.0f, 200.0f / 255.0f, 200.0f / 255.0f, 1.0f},
        ScriptWaterColor{225.0f / 255.0f, 225.0f / 255.0f, 225.0f / 255.0f, 1.0f},
        ScriptWaterColor{150.0f / 255.0f, 150.0f / 255.0f, 150.0f / 255.0f, 1.0f},
        ScriptWaterColor{1.0f, 1.0f, 1.0f, 1.0f},
    };
    constexpr container::Array<ScriptWaterColor, 4> diffuse{
        ScriptWaterColor{175.0f / 255.0f, 175.0f / 255.0f, 175.0f / 255.0f, 1.0f},
        ScriptWaterColor{185.0f / 255.0f, 185.0f / 255.0f, 185.0f / 255.0f, 1.0f},
        ScriptWaterColor{225.0f / 255.0f, 225.0f / 255.0f, 225.0f / 255.0f, 1.0f},
        ScriptWaterColor{100.0f / 255.0f, 100.0f / 255.0f, 100.0f / 255.0f, 1.0f},
    };
    constexpr container::Array<ScriptWaterColor, 4> transparent{
        ScriptWaterColor{150.0f / 255.0f, 150.0f / 255.0f, 150.0f / 255.0f, 128.0f / 255.0f},
        ScriptWaterColor{1.0f, 1.0f, 1.0f, 128.0f / 255.0f},
        ScriptWaterColor{150.0f / 255.0f, 150.0f / 255.0f, 150.0f / 255.0f, 96.0f / 255.0f},
        ScriptWaterColor{1.0f, 1.0f, 1.0f, 128.0f / 255.0f},
    };
    constexpr container::Array<container::StringView, 4> skies{
        "TSCloudWis.tga", "TSCloudWis.tga", "TSCloudSun.tga", "TSStarFeld.tga"};
    for (size_t index = 0; index < timeOfDay.size(); ++index) {
        timeOfDay[index].skyTexture = skies[index];
        timeOfDay[index].vertexColors.fill(vertices[index]);
        timeOfDay[index].diffuseColor = diffuse[index];
        timeOfDay[index].transparentDiffuseColor = transparent[index];
        timeOfDay[index].uScrollPerMillisecond = index == 3 ? 0.0f : 0.002f;
        timeOfDay[index].vScrollPerMillisecond = index == 3 ? 0.0f : 0.002f;
        timeOfDay[index].skyTexelsPerUnit = index == 3 ? 1.6f : 0.8f;
    }
}

bool applyScriptWaterPresentationIni(container::StringView content,
                                     ScriptWaterPresentationSettings& settings,
                                     container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parse(content)) {
        if (error) *error = "Could not parse Water INI content";
        return false;
    }
    for (const game::IniBlock& block : parser.blocks()) {
        if (equalAsciiInsensitive(block.type, "WaterSet")) {
            const std::optional<size_t> index = timeOfDayIndex(block.name);
            if (!index) {
                if (error) *error = "Unknown WaterSet time of day: " + block.name;
                return false;
            }
            applyWaterSet(settings.timeOfDay[*index], block);
        } else if (equalAsciiInsensitive(block.type, "WaterTransparency")) {
            applyWaterTransparency(settings, block);
        }
    }
    return true;
}

} // namespace engine::script
