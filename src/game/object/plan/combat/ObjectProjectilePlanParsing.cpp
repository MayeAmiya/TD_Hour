#include "game/object/plan/combat/ObjectProjectilePlanParsing.h"

#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace game::object_projectile_plan_detail {

using container::asciiEqualIgnoreCase;

const ModuleData* findModule(
    const ThingTemplate& templateData,
    container::StringView moduleClass) noexcept {
    const auto found = std::find_if(
        templateData.modules.begin(), templateData.modules.end(),
        [moduleClass](const ModuleData& module) {
            return asciiEqualIgnoreCase(module.moduleClass, moduleClass);
        });
    return found == templateData.modules.end() ? nullptr : &*found;
}

bool hasModule(const ThingTemplate& templateData,
               container::StringView moduleClass) noexcept {
    return findModule(templateData, moduleClass) != nullptr;
}

const container::String* moduleValue(
    const ModuleData& module, container::StringView key) noexcept {
    const auto found = std::find_if(
        module.values.begin(), module.values.end(),
        [key](const auto& pair) {
            return asciiEqualIgnoreCase(pair.first, key);
        });
    return found == module.values.end() ? nullptr : &found->second;
}

const ModuleData* moduleChild(
    const ModuleData& module,
    container::StringView childClass) noexcept {
    const auto found = std::find_if(
        module.children.begin(), module.children.end(),
        [childClass](const ModuleData& child) {
            const container::StringView name = !child.moduleClass.empty()
                ? container::StringView{child.moduleClass}
                : container::StringView{child.type};
            return asciiEqualIgnoreCase(name, childClass);
        });
    return found == module.children.end() ? nullptr : &*found;
}

uint32_t parseDecalStyle(
    container::StringView text, uint32_t fallback) noexcept {
    uint32_t result = 0;
    bool found = false;
    size_t cursor = 0;
    while (cursor < text.size()) {
        cursor = text.find_first_not_of(" \t,|+", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = text.find_first_of(" \t,|+", cursor);
        const container::StringView token = text.substr(cursor, end - cursor);
        uint32_t bit = 0;
        if (asciiEqualIgnoreCase(token, "SHADOW_NONE") ||
            asciiEqualIgnoreCase(token, "NONE")) return 0;
        if (asciiEqualIgnoreCase(token, "SHADOW_DECAL")) bit = 0x01u;
        else if (asciiEqualIgnoreCase(token, "SHADOW_VOLUME")) bit = 0x02u;
        else if (asciiEqualIgnoreCase(token, "SHADOW_PROJECTION")) bit = 0x04u;
        else if (asciiEqualIgnoreCase(token, "SHADOW_DYNAMIC_PROJECTION")) bit = 0x08u;
        else if (asciiEqualIgnoreCase(token, "SHADOW_DIRECTIONAL_PROJECTION")) bit = 0x10u;
        else if (asciiEqualIgnoreCase(token, "SHADOW_ALPHA_DECAL")) bit = 0x20u;
        else if (asciiEqualIgnoreCase(token, "SHADOW_ADDITIVE_DECAL")) bit = 0x40u;
        if (bit != 0) {
            result |= bit;
            found = true;
        }
        cursor = end;
    }
    return found ? result : fallback;
}

container::Array<uint8_t, 4> parseDecalColor(
    container::StringView text,
    container::Array<uint8_t, 4> fallback) noexcept {
    container::Array<uint8_t, 4> output{0, 0, 0, 255};
    constexpr container::Array<char, 4> names{'R', 'G', 'B', 'A'};
    size_t cursor = 0;
    size_t count = 0;
    while (cursor < text.size() && count < output.size()) {
        cursor = text.find_first_not_of(" \t,", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = text.find_first_of(" \t,", cursor);
        const container::StringView token = text.substr(cursor, end - cursor);
        if (token.size() < 3 || token[1] != ':' ||
            std::toupper(static_cast<unsigned char>(token.front())) !=
                names[count]) return fallback;
        const container::String owned{token.substr(2)};
        char* parsedEnd = nullptr;
        const unsigned long value = std::strtoul(
            owned.c_str(), &parsedEnd, 10);
        if (parsedEnd == owned.c_str() || *parsedEnd != '\0' || value > 255u) {
            return fallback;
        }
        output[count++] = static_cast<uint8_t>(value);
        cursor = end;
    }
    return count >= 3 ? output : fallback;
}

float parseFiniteFloat(
    container::StringView text, float fallback) noexcept {
    return parseContentFloatOr(text, {
        .source = __FILE__, .block = "Object", .module = "Projectile",
        .field = "Real", .fallback = fallback});
}

float parsePercentToUnit(
    container::StringView text, float fallback) noexcept {
    while (!text.empty() &&
           (text.back() == ' ' || text.back() == '\t' || text.back() == '%')) {
        text.remove_suffix(1);
    }
    const float percent = parseFiniteFloat(text, fallback * 100.0f);
    return percent * 0.01f;
}

uint32_t parseMilliseconds(
    container::StringView text, uint32_t fallback) noexcept {
    const container::String owned(text);
    char* end = nullptr;
    const unsigned long value = std::strtoul(owned.c_str(), &end, 10);
    if (end == owned.c_str()) return fallback;
    return value > std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(value);
}

uint32_t parseUnsigned(
    container::StringView text, uint32_t fallback) noexcept {
    return parseMilliseconds(text, fallback);
}

bool parseBool(container::StringView text, bool fallback) noexcept {
    if (asciiEqualIgnoreCase(text, "yes") ||
        asciiEqualIgnoreCase(text, "true") || text == "1") return true;
    if (asciiEqualIgnoreCase(text, "no") ||
        asciiEqualIgnoreCase(text, "false") || text == "0") return false;
    return fallback;
}

Fixed fixedFinite(float value, float fallback) noexcept {
    return Fixed{std::isfinite(value) ? value : fallback};
}

} // namespace game::object_projectile_plan_detail
