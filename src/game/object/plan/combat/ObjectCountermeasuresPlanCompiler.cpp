#include "game/object/plan/combat/ObjectCountermeasuresPlanTypes.h"

#include "core/container/string_utils.h"
#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>

namespace game {
namespace {

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] container::StringView moduleClass(
    const ModuleData& module) noexcept {
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass}
                                       : container::StringView{module.type};
}

[[nodiscard]] const container::String* valueLast(
    const ModuleData& module, container::StringView key) noexcept {
    for (auto iterator = module.values.rbegin();
         iterator != module.values.rend(); ++iterator) {
        if (equalInsensitive(iterator->first, key)) return &iterator->second;
    }
    const auto found = module.properties.find(container::String{key});
    return found == module.properties.end() ? nullptr : &found->second;
}

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

[[nodiscard]] std::optional<double> parseDouble(
    container::StringView value) noexcept {
    value = trim(value);
    if (value.empty()) return std::nullopt;
    double parsed = 0.0;
    const auto [cursor, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || cursor != value.data() + value.size() ||
        !std::isfinite(parsed)) {
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] std::optional<uint32_t> parseUnsigned(
    container::StringView value) noexcept {
    const std::optional<double> parsed = parseDouble(value);
    if (!parsed || *parsed < 0.0) return std::nullopt;
    if (*parsed >= static_cast<double>(std::numeric_limits<uint32_t>::max())) {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(std::ceil(*parsed));
}

[[nodiscard]] std::optional<math::q32_32> parseFixed(
    container::StringView value, bool percent = false) noexcept {
    value = trim(value);
    bool hasPercent = false;
    if (!value.empty() && value.back() == '%') {
        value.remove_suffix(1);
        hasPercent = true;
    }
    const std::optional<double> parsed = parseDouble(value);
    if (!parsed) return std::nullopt;
    double result = *parsed;
    // INI::parsePercentToReal accepts the ordinary fractional spelling as
    // already normalized and divides only an explicit `%` token.
    if (hasPercent) result /= 100.0;
    static_cast<void>(percent);
    constexpr double maximumExclusive =
        static_cast<double>(std::numeric_limits<int32_t>::max()) + 1.0;
    if (result <= -maximumExclusive || result >= maximumExclusive) {
        return std::nullopt;
    }
    return math::q32_32{result};
}

[[nodiscard]] bool parseBool(container::StringView value,
                             bool fallback) noexcept {
    value = trim(value);
    if (equalInsensitive(value, "YES") || equalInsensitive(value, "TRUE") ||
        equalInsensitive(value, "1")) return true;
    if (equalInsensitive(value, "NO") || equalInsensitive(value, "FALSE") ||
        equalInsensitive(value, "0")) return false;
    return fallback;
}

void appendTokens(const ModuleData& module, container::StringView key,
                  container::Vector<container::String>& output) {
    for (const auto& [candidate, raw] : module.values) {
        if (!equalInsensitive(candidate, key)) continue;
        container::StringView remaining = raw;
        while (!remaining.empty()) {
            while (!remaining.empty() &&
                   (std::isspace(static_cast<unsigned char>(remaining.front())) ||
                    remaining.front() == ',')) remaining.remove_prefix(1);
            size_t length = 0;
            while (length < remaining.size() &&
                   !std::isspace(static_cast<unsigned char>(remaining[length])) &&
                   remaining[length] != ',') ++length;
            if (length != 0) output.emplace_back(remaining.substr(0, length));
            remaining.remove_prefix(length);
        }
    }
}

void parseMux(const ModuleData& module, ObjectUpgradeMuxRecipe& mux) {
    appendTokens(module, "TriggeredBy", mux.triggeredBy);
    appendTokens(module, "ConflictsWith", mux.conflictsWith);
    appendTokens(module, "RemovesUpgrades", mux.removesUpgrades);
    if (const container::String* value = valueLast(module, "FXListUpgrade")) {
        mux.upgradeFx = container::String{trim(*value)};
    }
    if (const container::String* value = valueLast(module, "RequiresAllTriggers")) {
        mux.requiresAllTriggers = parseBool(*value, false);
    }
}

} // namespace

container::SharedPtr<const ObjectCountermeasuresPlan>
compileObjectCountermeasuresPlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog) {
    auto plan = std::make_shared<ObjectCountermeasuresPlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!equalInsensitive(moduleClass(module), "CountermeasuresBehavior")) {
            continue;
        }
        ObjectCountermeasuresRule rule;
        rule.authoredOrder = module.authoredOrder;
        const auto stringField = [&](container::StringView key,
                                     container::String& destination) {
            if (const container::String* value = valueLast(module, key)) {
                const container::StringView text = trim(*value);
                if (!equalInsensitive(text, "NONE")) {
                    destination = container::String{text};
                }
            }
        };
        const auto unsignedField = [&](container::StringView key,
                                       uint32_t& destination) {
            if (const container::String* value = valueLast(module, key)) {
                if (const std::optional<uint32_t> parsed = parseUnsigned(*value)) {
                    destination = *parsed;
                } else {
                    plan->diagnostics.push_back(container::String{key} +
                        " must be a non-negative finite value");
                }
            }
        };
        stringField("FlareTemplateName", rule.flareTemplate);
        stringField("FlareBoneBaseName", rule.flareBoneBaseName);
        unsignedField("VolleySize", rule.volleySize);
        unsignedField("NumberOfVolleys", rule.numberOfVolleys);
        unsignedField("DelayBetweenVolleys",
                      rule.delayBetweenVolleysMilliseconds);
        unsignedField("ReloadTime", rule.reloadMilliseconds);
        unsignedField("MissileDecoyDelay", rule.missileDecoyMilliseconds);
        unsignedField("ReactionLaunchLatency",
                      rule.reactionLaunchLatencyMilliseconds);
        if (const container::String* value = valueLast(module, "VolleyArcAngle")) {
            if (const std::optional<math::q32_32> degrees = parseFixed(*value)) {
                rule.volleyArcRadians = *degrees *
                    math::q32_32{std::numbers::pi / 180.0};
            } else {
                plan->diagnostics.push_back("VolleyArcAngle must be finite");
            }
        }
        if (const container::String* value =
                valueLast(module, "VolleyVelocityFactor")) {
            if (const std::optional<math::q32_32> parsed = parseFixed(*value)) {
                rule.volleyVelocityFactor = *parsed;
            } else {
                plan->diagnostics.push_back(
                    "VolleyVelocityFactor must be finite");
            }
        }
        if (const container::String* value = valueLast(module, "EvasionRate")) {
            if (const std::optional<math::q32_32> parsed =
                    parseFixed(*value, true)) {
                rule.evasionRate = math::q32_32::clamp(
                    *parsed, {}, math::q32_32{int32_t{1}});
            } else {
                plan->diagnostics.push_back("EvasionRate must be a percentage");
            }
        }
        if (const container::String* value =
                valueLast(module, "MustReloadAtAirfield")) {
            rule.mustReloadAtAirfield = parseBool(*value, false);
        }
        parseMux(module, rule.upgradeMux);
        compileObjectUpgradeMuxRecipe(rule.upgradeMux, upgradeCatalog);
        if (rule.reactionLaunchLatencyMilliseconds >=
                rule.missileDecoyMilliseconds &&
            rule.missileDecoyMilliseconds != 0) {
            plan->diagnostics.push_back(
                "ReactionLaunchLatency must be less than MissileDecoyDelay");
        }
        plan->rules.push_back(std::move(rule));
    }
    if (plan->rules.empty()) return {};
    std::stable_sort(plan->rules.begin(), plan->rules.end(),
        [](const ObjectCountermeasuresRule& left,
           const ObjectCountermeasuresRule& right) {
            return left.authoredOrder < right.authoredOrder;
        });
    return plan;
}

} // namespace game
