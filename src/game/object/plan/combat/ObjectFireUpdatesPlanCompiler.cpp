#include "core/container/string_utils.h"
#include "game/data/base/ContentBoolParsing.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/plan/combat/ObjectFireUpdatesPlanTypes.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
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
            .source = __FILE__, .block = "Object", .module = "FireUpdate",
            .field = "Real"});
    return parsed ? std::optional<math::q32_32>{math::q32_32{*parsed}}
                  : std::nullopt;
}

void appendDiagnostic(container::Vector<container::String>& diagnostics,
                      const ModuleData& module, container::String message) {
    const container::String tag = !module.moduleTag.empty() ? module.moduleTag
                           : !module.tag.empty() ? module.tag
                                                 : container::String{moduleClass(module)};
    diagnostics.push_back(tag + ": " + std::move(message));
}

void parseStringVector(const ModuleData& module, container::StringView key,
                       container::Vector<container::String>& destination) {
    destination.clear();
    const container::String* value = moduleValueLast(module, key);
    if (!value) return;
    for (const container::StringView token : splitTokens(*value)) {
        if (!token.empty()) destination.emplace_back(token);
    }
}

void parseUpgradeMux(const ModuleData& module, ObjectUpgradeMuxRecipe& mux) {
    parseStringVector(module, "TriggeredBy", mux.triggeredBy);
    parseStringVector(module, "ConflictsWith", mux.conflictsWith);
    parseStringVector(module, "RemovesUpgrades", mux.removesUpgrades);
    if (const container::String* value = moduleValueLast(module, "FXListUpgrade")) {
        mux.upgradeFx = container::String{trim(*value)};
    }
    if (const container::String* value =
            moduleValueLast(module, "RequiresAllTriggers")) {
        mux.requiresAllTriggers = parseBoolean(*value);
    }
}

[[nodiscard]] std::optional<WeaponSlot> parseWeaponSlot(
    container::StringView value) noexcept {
    value = trim(value);
    if (equalInsensitive(value, "PRIMARY")) return WeaponSlot::Primary;
    if (equalInsensitive(value, "SECONDARY")) return WeaponSlot::Secondary;
    if (equalInsensitive(value, "TERTIARY")) return WeaponSlot::Tertiary;
    return std::nullopt;
}

template <typename Rule>
void sortRules(container::Vector<Rule>& rules) {
    std::stable_sort(rules.begin(), rules.end(),
        [](const Rule& left, const Rule& right) {
            return left.authoredOrder < right.authoredOrder;
        });
}

} // namespace

container::SharedPtr<const ObjectFlammablePlan>
compileObjectFlammablePlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectFlammablePlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!equalInsensitive(moduleClass(module), "FlammableUpdate")) continue;
        ObjectFlammableParameters rule;
        rule.authoredOrder = module.authoredOrder;
        const auto duration = [&](container::StringView key,
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
        duration("BurnedDelay", rule.burnedDelayMilliseconds);
        duration("AflameDuration", rule.aflameDurationMilliseconds);
        duration("AflameDamageDelay", rule.aflameDamageDelayMilliseconds);
        duration("FlameDamageExpiration",
                 rule.flameDamageExpirationMilliseconds);
        if (const container::String* value =
                moduleValueLast(module, "AflameDamageAmount")) {
            if (const auto parsed = parseInteger<int32_t>(*value)) {
                rule.aflameDamageAmount = *parsed;
            } else {
                appendDiagnostic(plan->diagnostics, module,
                                 "AflameDamageAmount must be a signed integer");
            }
        }
        if (const container::String* value =
                moduleValueLast(module, "BurningSoundName")) {
            if (!equalInsensitive(trim(*value), "NONE")) {
                rule.burningSoundName = container::String{trim(*value)};
            }
        }
        if (const container::String* value =
                moduleValueLast(module, "FlameDamageLimit")) {
            if (const auto parsed = parseFixed(*value)) {
                rule.flameDamageLimit = *parsed;
            } else {
                appendDiagnostic(plan->diagnostics, module,
                                 "FlameDamageLimit must be a finite scalar");
            }
        }
        plan->rules.push_back(std::move(rule));
    }
    if (plan->rules.empty()) return nullptr;
    sortRules(plan->rules);
    return plan;
}

container::SharedPtr<const ObjectFireSpreadPlan>
compileObjectFireSpreadPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectFireSpreadPlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!equalInsensitive(moduleClass(module), "FireSpreadUpdate")) continue;
        ObjectFireSpreadParameters rule;
        rule.authoredOrder = module.authoredOrder;
        if (const container::String* value = moduleValueLast(module, "OCLEmbers")) {
            if (!equalInsensitive(trim(*value), "NONE")) {
                rule.embersObjectCreationList = container::String{trim(*value)};
            }
        }
        const auto duration = [&](container::StringView key,
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
        duration("MinSpreadDelay", rule.minimumSpreadDelayMilliseconds);
        duration("MaxSpreadDelay", rule.maximumSpreadDelayMilliseconds);
        if (rule.maximumSpreadDelayMilliseconds <
            rule.minimumSpreadDelayMilliseconds) {
            rule.maximumSpreadDelayMilliseconds =
                rule.minimumSpreadDelayMilliseconds;
        }
        if (const container::String* value =
                moduleValueLast(module, "SpreadTryRange")) {
            if (const auto parsed = parseFixed(*value)) {
                rule.spreadTryRange = std::max(*parsed, math::q32_32{});
            } else {
                appendDiagnostic(plan->diagnostics, module,
                                 "SpreadTryRange must be a finite scalar");
            }
        }
        plan->rules.push_back(std::move(rule));
    }
    if (plan->rules.empty()) return nullptr;
    sortRules(plan->rules);
    return plan;
}

container::SharedPtr<const ObjectFireOclAfterCooldownPlan>
compileObjectFireOclAfterCooldownPlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog) {
    auto plan = std::make_shared<ObjectFireOclAfterCooldownPlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!equalInsensitive(moduleClass(module),
                              "FireOCLAfterWeaponCooldownUpdate")) continue;
        ObjectFireOclAfterCooldownParameters rule;
        rule.authoredOrder = module.authoredOrder;
        if (const container::String* value = moduleValueLast(module, "WeaponSlot")) {
            if (const auto parsed = parseWeaponSlot(*value)) {
                rule.weaponSlot = *parsed;
            } else {
                appendDiagnostic(plan->diagnostics, module,
                                 "WeaponSlot must be PRIMARY, SECONDARY or TERTIARY");
            }
        }
        if (const container::String* value = moduleValueLast(module, "OCL")) {
            if (!equalInsensitive(trim(*value), "NONE")) {
                rule.objectCreationList = container::String{trim(*value)};
            }
        }
        if (rule.objectCreationList.empty()) {
            appendDiagnostic(plan->diagnostics, module, "OCL is required");
        }
        const auto unsignedField = [&](container::StringView key,
                                       uint32_t& destination) {
            const container::String* value = moduleValueLast(module, key);
            if (!value) return false;
            if (const auto parsed = parseInteger<uint32_t>(*value)) {
                destination = *parsed;
                return true;
            }
            appendDiagnostic(plan->diagnostics, module,
                container::String{key} + " must be unsigned");
            return false;
        };
        static_cast<void>(unsignedField("MinShotsToCreateOCL",
                                        rule.minimumShotsRequired));
        static_cast<void>(unsignedField("OCLLifetimePerSecond",
                                        rule.oclLifetimePerSecondMilliseconds));
        rule.hasAuthoredLifetimeMaximum = unsignedField(
            "OCLLifetimeMaxCap", rule.oclLifetimeMaximumMilliseconds);
        parseUpgradeMux(module, rule.upgradeMux);
        compileObjectUpgradeMuxRecipe(rule.upgradeMux, upgradeCatalog);
        plan->rules.push_back(std::move(rule));
    }
    if (plan->rules.empty()) return nullptr;
    sortRules(plan->rules);
    return plan;
}

} // namespace game
