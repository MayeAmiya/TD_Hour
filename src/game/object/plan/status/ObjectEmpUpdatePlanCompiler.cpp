#include "core/container/string_utils.h"
#include "game/data/base/ContentBoolParsing.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/plan/status/ObjectEmpUpdatePlanTypes.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <optional>
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

[[nodiscard]] container::StringView moduleClass(const ModuleData& module) noexcept {
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

[[nodiscard]] container::Vector<container::StringView> splitTokens(
    container::StringView value) {
    container::Vector<container::StringView> result;
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t,", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t,", cursor);
        result.push_back(value.substr(cursor, end - cursor));
        cursor = end;
    }
    return result;
}

[[nodiscard]] bool parseBoolean(container::StringView value,
                                bool fallback = false) noexcept {
    return parseContentBool(value, fallback);
}

template <typename Integer>
[[nodiscard]] std::optional<Integer> parseInteger(
    container::StringView value) noexcept {
    value = trim(value);
    if (value.empty()) return std::nullopt;
    Integer parsed{};
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size()) return std::nullopt;
    return parsed;
}

[[nodiscard]] std::optional<math::q32_32> parseFixed(
    container::StringView value) noexcept {
    const std::optional<float> parsed =
        parseContentFloat(value, {
            .source = __FILE__, .block = "Object", .module = "EMPUpdate",
            .field = "Real"});
    return parsed ? std::optional<math::q32_32>{math::q32_32{*parsed}}
                  : std::nullopt;
}

[[nodiscard]] std::optional<container::Array<float, 3>> parseColor(
    container::StringView value) {
    container::Array<float, 3> result{};
    container::Array<bool, 3> found{};
    for (const container::StringView token : splitTokens(value)) {
        if (token.size() < 3 || token[1] != ':') continue;
        size_t channel = 3;
        if (token.front() == 'R' || token.front() == 'r') channel = 0;
        if (token.front() == 'G' || token.front() == 'g') channel = 1;
        if (token.front() == 'B' || token.front() == 'b') channel = 2;
        if (channel >= result.size()) continue;
        const auto parsed = parseInteger<int32_t>(token.substr(2));
        if (!parsed) return std::nullopt;
        result[channel] = static_cast<float>(std::clamp(*parsed, 0, 255)) /
                          255.0f;
        found[channel] = true;
    }
    return std::all_of(found.begin(), found.end(),
                       [](bool present) { return present; })
        ? std::optional<container::Array<float, 3>>{result} : std::nullopt;
}

void appendDiagnostic(container::Vector<container::String>& diagnostics,
                      const ModuleData& module, container::String message) {
    const container::String tag = !module.moduleTag.empty() ? module.moduleTag
                           : !module.tag.empty() ? module.tag
                                                 : container::String{moduleClass(module)};
    diagnostics.push_back(tag + ": " + std::move(message));
}

void parseKindVector(const ModuleData& module, container::StringView key,
                     container::Vector<container::String>& destination) {
    const container::String* value = moduleValueLast(module, key);
    if (!value) return;
    for (const container::StringView token : splitTokens(*value)) {
        if (!token.empty()) destination.emplace_back(token);
    }
}

[[nodiscard]] WeaponAffectsMask parseDoesNotAffect(
    const ModuleData& module, container::Vector<container::String>& diagnostics) {
    const container::String* value = moduleValueLast(module, "DoesNotAffect");
    if (!value) return 0;
    WeaponAffectsMask result = 0;
    for (const container::StringView token : splitTokens(*value)) {
        if (equalInsensitive(token, "SELF")) {
            result |= weaponAffectsBit(WeaponAffectsTarget::Self);
        } else if (equalInsensitive(token, "ALLIES")) {
            result |= weaponAffectsBit(WeaponAffectsTarget::Allies);
        } else if (equalInsensitive(token, "ENEMIES")) {
            result |= weaponAffectsBit(WeaponAffectsTarget::Enemies);
        } else if (equalInsensitive(token, "NEUTRALS")) {
            result |= weaponAffectsBit(WeaponAffectsTarget::Neutrals);
        } else {
            appendDiagnostic(diagnostics, module,
                "unknown DoesNotAffect token '" + container::String{token} + "'");
        }
    }
    return result;
}

} // namespace

container::SharedPtr<const ObjectEmpPlan>
compileObjectEmpPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectEmpPlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!equalInsensitive(moduleClass(module), "EMPUpdate")) continue;
        ObjectEmpParameters rule;
        rule.authoredOrder = module.authoredOrder;
        const auto readDuration = [&](container::StringView key,
                                      uint32_t& destination) {
            const container::String* value = moduleValueLast(module, key);
            if (!value) return;
            if (const auto parsed = parseInteger<uint32_t>(*value)) {
                destination = *parsed;
            } else {
                appendDiagnostic(plan->diagnostics, module,
                    container::String{key} + " must be unsigned milliseconds");
            }
        };
        const auto readFixed = [&](container::StringView key,
                                   math::q32_32& destination) {
            const container::String* value = moduleValueLast(module, key);
            if (!value) return;
            if (const auto parsed = parseFixed(*value)) {
                destination = *parsed;
            } else {
                appendDiagnostic(plan->diagnostics, module,
                    container::String{key} + " must be a finite scalar");
            }
        };
        readDuration("Lifetime", rule.lifetimeMilliseconds);
        readDuration("StartFadeTime", rule.startFadeMilliseconds);
        readDuration("DisabledDuration", rule.disabledDurationMilliseconds);
        readFixed("StartScale", rule.startScale);
        readFixed("TargetScaleMin", rule.targetScaleMinimum);
        readFixed("TargetScaleMax", rule.targetScaleMaximum);
        readFixed("SparksPerCubicFoot", rule.sparksPerCubicFoot);
        readFixed("EffectRadius", rule.effectRadius);
        if (rule.targetScaleMaximum < rule.targetScaleMinimum) {
            std::swap(rule.targetScaleMinimum, rule.targetScaleMaximum);
        }
        rule.sparksPerCubicFoot = std::max(
            rule.sparksPerCubicFoot, math::q32_32{});
        rule.effectRadius = std::max(rule.effectRadius, math::q32_32{});
        if (const container::String* value = moduleValueLast(module, "StartColor")) {
            if (const auto parsed = parseColor(*value)) rule.startColor = *parsed;
            else appendDiagnostic(plan->diagnostics, module,
                                  "StartColor must contain R:G:B channels");
        }
        if (const container::String* value = moduleValueLast(module, "EndColor")) {
            if (const auto parsed = parseColor(*value)) rule.endColor = *parsed;
            else appendDiagnostic(plan->diagnostics, module,
                                  "EndColor must contain R:G:B channels");
        }
        if (const container::String* value =
                moduleValueLast(module, "DisableFXParticleSystem")) {
            if (!equalInsensitive(trim(*value), "NONE")) {
                rule.disableParticleSystem = container::String{trim(*value)};
            }
        }
        if (const container::String* value = moduleValueLast(
                module, "DoesNotAffectMyOwnBuildings")) {
            rule.doesNotAffectOwnBuildings = parseBoolean(*value);
        }
        rule.doesNotAffect = parseDoesNotAffect(module, plan->diagnostics);
        parseKindVector(module, "VictimRequiredKindOf",
                        rule.victimRequiredKindOf);
        parseKindVector(module, "VictimForbiddenKindOf",
                        rule.victimForbiddenKindOf);
        // RefCode parses both fields but its victim filter is commented out.
        // Preserve the authored data for compatibility/tooling without
        // treating stock content as an invalid recipe.
        if (rule.lifetimeMilliseconds != 0 &&
            rule.startFadeMilliseconds >= rule.lifetimeMilliseconds) {
            appendDiagnostic(plan->diagnostics, module,
                "StartFadeTime must be earlier than Lifetime; runtime clamps it to the last live frame");
        }
        plan->rules.push_back(std::move(rule));
    }
    if (plan->rules.empty()) return nullptr;
    std::stable_sort(plan->rules.begin(), plan->rules.end(),
        [](const ObjectEmpParameters& left,
           const ObjectEmpParameters& right) {
            return left.authoredOrder < right.authoredOrder;
        });
    return plan;
}

} // namespace game
