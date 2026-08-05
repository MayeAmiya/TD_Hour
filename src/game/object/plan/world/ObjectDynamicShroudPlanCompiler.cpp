#include "game/object/plan/world/ObjectDynamicShroudPlanTypes.h"

#include "core/container/string_utils.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "debug/debug.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

namespace game {
namespace {

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] container::StringView trim(container::StringView value) noexcept {
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

[[nodiscard]] container::StringView moduleClass(
    const ModuleData& module) noexcept {
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass}
                                       : container::StringView{module.type};
}

[[nodiscard]] const container::String* moduleValueLast(
    const ModuleData& module, container::StringView key) noexcept {
    for (auto found = module.values.rbegin(); found != module.values.rend();
         ++found) {
        if (equalInsensitive(found->first, key)) return &found->second;
    }
    for (const auto& [candidate, value] : module.properties) {
        if (equalInsensitive(candidate, key)) return &value;
    }
    return nullptr;
}

[[nodiscard]] std::optional<uint32_t> parseUnsignedMilliseconds(
    container::StringView value) noexcept {
    value = trim(value);
    if (value.empty()) return std::nullopt;
    uint64_t parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size() ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(parsed);
}

[[nodiscard]] std::optional<double> parseFiniteReal(
    container::StringView value) noexcept {
    value = trim(value);
    if (value.empty()) return std::nullopt;
    double parsed = 0.0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed,
        std::chars_format::general);
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size() || !std::isfinite(parsed)) {
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] std::optional<math::q32_32> fixedFromDouble(
    double value) noexcept {
    if (!std::isfinite(value)) return std::nullopt;
    constexpr double kScale = 4294967296.0;
    constexpr double kMinimum = -2147483648.0;
    const double maximum = std::nextafter(2147483648.0, 0.0);
    if (value < kMinimum || value > maximum) return std::nullopt;
    return math::q32_32::from_raw(static_cast<int64_t>(value * kScale));
}

[[nodiscard]] std::optional<math::q32_32> parseFixed(
    container::StringView value) noexcept {
    const std::optional<double> parsed = parseFiniteReal(value);
    if (!parsed) return std::nullopt;
    return fixedFromDouble(*parsed);
}

[[nodiscard]] std::optional<math::q32_32> parsePercent(
    container::StringView value) noexcept {
    value = trim(value);
    if (!value.empty() && value.back() == '%') {
        value.remove_suffix(1);
    }
    const std::optional<double> parsed = parseFiniteReal(value);
    if (!parsed) return std::nullopt;
    return fixedFromDouble(*parsed / 100.0);
}

[[nodiscard]] std::optional<bool> parseBoolean(
    container::StringView value) noexcept {
    value = trim(value);
    if (equalInsensitive(value, "YES") ||
        equalInsensitive(value, "TRUE") || value == "1") return true;
    if (equalInsensitive(value, "NO") ||
        equalInsensitive(value, "FALSE") || value == "0") return false;
    return std::nullopt;
}

[[nodiscard]] container::Vector<container::StringView> tokens(
    container::StringView value) {
    container::Vector<container::StringView> result;
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t,|+", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t,|+", cursor);
        result.push_back(value.substr(cursor, end - cursor));
        cursor = end;
    }
    return result;
}

[[nodiscard]] std::optional<uint32_t> parseShadowTypeMask(
    container::StringView value) {
    container::Vector<container::StringView> bitTokens;
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t,|", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t,|", cursor);
        bitTokens.push_back(value.substr(cursor, end - cursor));
        cursor = end;
    }

    uint32_t result = 0x20u; // RadiusDecalTemplate default
    bool foundNormal = false;
    bool foundOperation = false;
    for (container::StringView token : bitTokens) {
        const bool add = !token.empty() && token.front() == '+';
        const bool remove = !token.empty() && token.front() == '-';
        if (add || remove) token.remove_prefix(1);
        uint32_t bit = 0;
        if (equalInsensitive(token, "SHADOW_NONE") ||
            equalInsensitive(token, "NONE")) {
            if (foundNormal || foundOperation) return std::nullopt;
            result = 0;
            return result;
        } else if (equalInsensitive(token, "SHADOW_DECAL")) {
            bit = 0x01u;
        } else if (equalInsensitive(token, "SHADOW_VOLUME")) {
            bit = 0x02u;
        } else if (equalInsensitive(token, "SHADOW_PROJECTION")) {
            bit = 0x04u;
        } else if (equalInsensitive(token, "SHADOW_DYNAMIC_PROJECTION")) {
            bit = 0x08u;
        } else if (equalInsensitive(token, "SHADOW_DIRECTIONAL_PROJECTION")) {
            bit = 0x10u;
        } else if (equalInsensitive(token, "SHADOW_ALPHA_DECAL")) {
            bit = 0x20u;
        } else if (equalInsensitive(token, "SHADOW_ADDITIVE_DECAL")) {
            bit = 0x40u;
        } else {
            return std::nullopt;
        }
        if (add || remove) {
            if (foundNormal) return std::nullopt;
            foundOperation = true;
            if (remove) result &= ~bit;
            else result |= bit;
        } else {
            if (foundOperation) return std::nullopt;
            if (!foundNormal) result = 0;
            foundNormal = true;
            result |= bit;
        }
    }
    return foundNormal || foundOperation
        ? std::optional<uint32_t>{result} : std::nullopt;
}

[[nodiscard]] std::optional<std::array<uint8_t, 4>> parseColor(
    container::StringView value) {
    value = trim(value);
    if (const size_t comment = value.find("//");
        comment != container::StringView::npos) {
        value = trim(value.substr(0, comment));
    }
    const container::Vector<container::StringView> parts = tokens(value);
    if (parts.size() < 3 || parts.size() > 4) return std::nullopt;
    constexpr std::array<char, 4> kNames{'R', 'G', 'B', 'A'};
    std::array<uint8_t, 4> result{0, 0, 0, 255};
    for (size_t index = 0; index < parts.size(); ++index) {
        const container::StringView part = parts[index];
        if (part.size() < 3 || part[1] != ':' ||
            std::toupper(static_cast<unsigned char>(part.front())) !=
                kNames[index]) {
            return std::nullopt;
        }
        uint32_t channel = 0;
        const auto parsed = std::from_chars(
            part.data() + 2, part.data() + part.size(), channel);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != part.data() + part.size() || channel > 255) {
            return std::nullopt;
        }
        result[index] = static_cast<uint8_t>(channel);
    }
    return result;
}

void appendDiagnostic(ObjectDynamicShroudPlan& plan, const ModuleData& module,
                      container::String message) {
    const container::String tag = !module.moduleTag.empty() ? module.moduleTag
                           : !module.tag.empty() ? module.tag
                                                 : container::String{moduleClass(module)};
    plan.diagnostics.push_back(tag + ": " + std::move(message));
}

[[nodiscard]] bool isModuleField(container::StringView key) noexcept {
    constexpr std::array<container::StringView, 7> kFields{
        "ChangeInterval", "GrowInterval", "ShrinkDelay", "ShrinkTime",
        "GrowDelay", "GrowTime", "FinalVision"};
    return std::any_of(kFields.begin(), kFields.end(), [key](auto candidate) {
        return equalInsensitive(key, candidate);
    });
}

[[nodiscard]] bool isDecalField(container::StringView key) noexcept {
    constexpr std::array<container::StringView, 7> kFields{
        "Texture", "Style", "OpacityMin", "OpacityMax",
        "OpacityThrobTime", "Color", "OnlyVisibleToOwningPlayer"};
    return std::any_of(kFields.begin(), kFields.end(), [key](auto candidate) {
        return equalInsensitive(key, candidate);
    });
}

void parseGridDecal(const ModuleData& module, const ModuleData& decal,
                    ObjectDynamicShroudPlan& plan,
                    ObjectDynamicShroudDecalRecipe& result,
                    bool& valid) {
    const auto invalid = [&](container::String message) {
        appendDiagnostic(plan, module, "GridDecalTemplate " + std::move(message));
        valid = false;
    };
    if (!decal.values.empty()) {
        for (const auto& [key, value] : decal.values) {
            static_cast<void>(value);
            if (!isDecalField(key)) invalid("contains unknown field '" + key + "'");
        }
    } else {
        for (const auto& [key, value] : decal.properties) {
            static_cast<void>(value);
            if (!isDecalField(key)) invalid("contains unknown field '" + key + "'");
        }
    }
    for (const ModuleData& child : decal.children) {
        invalid("contains unsupported nested child block '" +
                container::String{moduleClass(child)} + "'");
    }

    if (const container::String* value = moduleValueLast(decal, "Texture")) {
        result.texture = container::String{trim(*value)};
    }
    if (const container::String* value = moduleValueLast(decal, "Style")) {
        const std::optional<uint32_t> parsed = parseShadowTypeMask(*value);
        if (parsed) result.shadowTypeMask = *parsed;
        else invalid("Style has an unknown or empty shadow flag list '" + *value + "'");
    }
    const auto readPercent = [&](container::StringView field,
                                 math::q32_32& destination) {
        const container::String* value = moduleValueLast(decal, field);
        if (!value) return;
        const std::optional<math::q32_32> parsed = parsePercent(*value);
        if (parsed) destination = *parsed;
        else invalid(container::String{field} + " must be a finite percentage");
    };
    readPercent("OpacityMin", result.minimumOpacity);
    readPercent("OpacityMax", result.maximumOpacity);
    if (const container::String* value =
            moduleValueLast(decal, "OpacityThrobTime")) {
        const std::optional<uint32_t> parsed = parseUnsignedMilliseconds(*value);
        if (parsed) result.opacityThrobMilliseconds = *parsed;
        else invalid("OpacityThrobTime must be unsigned milliseconds");
    }
    if (const container::String* value = moduleValueLast(decal, "Color")) {
        const auto parsed = parseColor(*value);
        if (parsed) {
            result.color = *parsed;
            result.usesPlayerColor = std::all_of(
                result.color.begin(), result.color.end(),
                [](uint8_t channel) { return channel == 0; });
        } else {
            invalid("Color must be ordered R:<0..255> G:<0..255> B:<0..255> [A:<0..255>]");
        }
    }
    if (const container::String* value =
            moduleValueLast(decal, "OnlyVisibleToOwningPlayer")) {
        const std::optional<bool> parsed = parseBoolean(*value);
        if (parsed) result.onlyVisibleToOwningPlayer = *parsed;
        else invalid("OnlyVisibleToOwningPlayer must be a boolean");
    }
}

} // namespace

container::SharedPtr<const ObjectDynamicShroudPlan>
compileObjectDynamicShroudPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectDynamicShroudPlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!equalInsensitive(moduleClass(module),
                              "DynamicShroudClearingRangeUpdate")) {
            continue;
        }

        ObjectDynamicShroudRule rule{.authoredOrder = module.authoredOrder};
        bool valid = true;
        const auto invalid = [&](container::String message) {
            appendDiagnostic(*plan, module, std::move(message));
            valid = false;
        };
        if (!module.values.empty()) {
            for (const auto& [key, value] : module.values) {
                static_cast<void>(value);
                if (!isModuleField(key)) {
                    invalid("contains unknown field '" + key + "'");
                }
            }
        } else {
            for (const auto& [key, value] : module.properties) {
                static_cast<void>(value);
                if (!isModuleField(key)) {
                    invalid("contains unknown field '" + key + "'");
                }
            }
        }

        const auto readDuration = [&](container::StringView field,
                                      uint32_t& destination) {
            const container::String* value = moduleValueLast(module, field);
            if (!value) return;
            const std::optional<uint32_t> parsed =
                parseUnsignedMilliseconds(*value);
            if (parsed) destination = *parsed;
            else invalid(container::String{field} +
                         " must be unsigned milliseconds");
        };
        readDuration("ChangeInterval", rule.changeIntervalMilliseconds);
        readDuration("GrowInterval", rule.growIntervalMilliseconds);
        readDuration("ShrinkDelay", rule.shrinkDelayMilliseconds);
        readDuration("ShrinkTime", rule.shrinkTimeMilliseconds);
        readDuration("GrowDelay", rule.growDelayMilliseconds);
        readDuration("GrowTime", rule.growTimeMilliseconds);
        if (const container::String* value =
                moduleValueLast(module, "FinalVision")) {
            const std::optional<math::q32_32> parsed = parseFixed(*value);
            if (parsed) rule.finalVision = *parsed;
            else invalid("FinalVision must be a finite Q32.32-range real");
        }

        size_t decalCount = 0;
        for (const ModuleData& child : module.children) {
            if (!equalInsensitive(moduleClass(child), "GridDecalTemplate")) {
                invalid("contains unknown child block '" +
                        container::String{moduleClass(child)} + "'");
                continue;
            }
            ++decalCount;
            if (decalCount > 1) {
                invalid("contains more than one GridDecalTemplate block");
                continue;
            }
            parseGridDecal(module, child, *plan, rule.gridDecal, valid);
        }

        const uint64_t growEnd =
            static_cast<uint64_t>(rule.growDelayMilliseconds) +
            rule.growTimeMilliseconds;
        if (growEnd > rule.shrinkDelayMilliseconds) {
            invalid("GrowDelay + GrowTime must not exceed ShrinkDelay; "
                    "the legacy module asserts because full clearing range "
                    "would be unreachable");
        }
        if (valid) plan->rules.push_back(std::move(rule));
    }

    std::stable_sort(plan->rules.begin(), plan->rules.end(),
        [](const ObjectDynamicShroudRule& left,
           const ObjectDynamicShroudRule& right) {
            return left.authoredOrder < right.authoredOrder;
        });
    return plan->rules.empty() && plan->diagnostics.empty() ? nullptr : plan;
}

} // namespace game
