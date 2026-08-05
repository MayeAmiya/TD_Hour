#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/plan/economy/ObjectAutoDepositPlanTypes.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <optional>
#include <utility>

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"

namespace game {
namespace {

using container::asciiEqualIgnoreCase;

using container::trimAsciiView;

[[nodiscard]] container::StringView moduleClass(const ModuleData& module) noexcept {
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass}
                                       : container::StringView{module.type};
}

[[nodiscard]] const container::String* moduleValueLast(
    const ModuleData& module, container::StringView key) noexcept {
    for (auto found = module.values.rbegin(); found != module.values.rend();
         ++found) {
        if (asciiEqualIgnoreCase(found->first, key)) return &found->second;
    }
    for (const auto& [candidate, value] : module.properties) {
        if (asciiEqualIgnoreCase(candidate, key)) return &value;
    }
    return nullptr;
}

[[nodiscard]] std::optional<uint32_t> parseUnsigned(
    container::StringView value) noexcept {
    value = trimAsciiView(value);
    uint32_t parsed = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] std::optional<int32_t> parseSigned(
    container::StringView value) noexcept {
    value = trimAsciiView(value);
    int32_t parsed = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] std::optional<bool> parseBoolean(
    container::StringView value) noexcept {
    value = trimAsciiView(value);
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

[[nodiscard]] container::Vector<container::StringView> splitPairTokens(
    container::StringView value) {
    container::Vector<container::StringView> result;
    size_t cursor = 0;
    while (cursor < value.size()) {
        while (cursor < value.size() &&
               (std::isspace(static_cast<unsigned char>(value[cursor])) ||
                value[cursor] == ':' || value[cursor] == ',')) {
            ++cursor;
        }
        const size_t begin = cursor;
        while (cursor < value.size() &&
               !std::isspace(static_cast<unsigned char>(value[cursor])) &&
               value[cursor] != ':' && value[cursor] != ',') {
            ++cursor;
        }
        if (begin != cursor) result.push_back(value.substr(begin, cursor - begin));
    }
    return result;
}

[[nodiscard]] std::optional<ObjectAutoDepositBoost> parseBoost(
    container::StringView value) {
    const container::Vector<container::StringView> tokens = splitPairTokens(value);
    if (tokens.size() != 4 ||
        !asciiEqualIgnoreCase(tokens[0], "UpgradeType") ||
        !asciiEqualIgnoreCase(tokens[2], "Boost")) {
        return std::nullopt;
    }
    const std::optional<int32_t> amount = parseSigned(tokens[3]);
    if (tokens[1].empty() || !amount) return std::nullopt;
    return ObjectAutoDepositBoost{
        .upgrade = container::String(tokens[1]),
        .amount = *amount,
    };
}

void appendDiagnostic(ObjectAutoDepositPlan& plan, const ModuleData& module,
                      container::String message) {
    const container::String tag = !module.moduleTag.empty() ? module.moduleTag
                           : !module.tag.empty() ? module.tag
                                                 : container::String{moduleClass(module)};
    plan.diagnostics.push_back(tag + ": " + std::move(message));
}

} // namespace

container::SharedPtr<const ObjectAutoDepositPlan>
compileObjectAutoDepositPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectAutoDepositPlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!asciiEqualIgnoreCase(moduleClass(module), "AutoDepositUpdate")) {
            continue;
        }

        ObjectAutoDepositParameters parameters;
        parameters.authoredOrder = module.authoredOrder;
        if (const container::String* value = moduleValueLast(module, "DepositTiming")) {
            if (const std::optional<uint32_t> parsed = parseUnsigned(*value)) {
                parameters.depositTimingMilliseconds = *parsed;
            } else {
                appendDiagnostic(*plan, module,
                                 "DepositTiming must be unsigned milliseconds");
            }
        }
        if (const container::String* value = moduleValueLast(module, "DepositAmount")) {
            if (const std::optional<int32_t> parsed = parseSigned(*value)) {
                parameters.depositAmount = *parsed;
            } else {
                appendDiagnostic(*plan, module,
                                 "DepositAmount must be a signed 32-bit integer");
            }
        }
        if (const container::String* value = moduleValueLast(module,
                                                       "InitialCaptureBonus")) {
            if (const std::optional<int32_t> parsed = parseSigned(*value)) {
                parameters.initialCaptureBonus = *parsed;
            } else {
                appendDiagnostic(*plan, module,
                                 "InitialCaptureBonus must be a signed 32-bit integer");
            }
        }
        if (const container::String* value = moduleValueLast(module, "ActualMoney")) {
            if (const std::optional<bool> parsed = parseBoolean(*value)) {
                parameters.actualMoney = *parsed;
            } else {
                appendDiagnostic(*plan, module,
                                 "ActualMoney must be Yes or No");
            }
        }

        bool sawOrderedBoost = false;
        for (const auto& [key, value] : module.values) {
            if (!asciiEqualIgnoreCase(key, "UpgradedBoost")) continue;
            sawOrderedBoost = true;
            if (std::optional<ObjectAutoDepositBoost> boost = parseBoost(value)) {
                parameters.upgradedBoosts.push_back(std::move(*boost));
            } else {
                appendDiagnostic(
                    *plan, module,
                    "UpgradedBoost must be UpgradeType:<name> Boost:<integer>");
            }
        }
        if (!sawOrderedBoost) {
            for (const auto& [key, value] : module.properties) {
                if (!asciiEqualIgnoreCase(key, "UpgradedBoost")) continue;
                if (std::optional<ObjectAutoDepositBoost> boost = parseBoost(value)) {
                    parameters.upgradedBoosts.push_back(std::move(*boost));
                } else {
                    appendDiagnostic(
                        *plan, module,
                        "UpgradedBoost must be UpgradeType:<name> Boost:<integer>");
                }
            }
        }
        plan->rules.push_back(std::move(parameters));
    }

    if (plan->rules.empty()) return nullptr;
    std::sort(plan->rules.begin(), plan->rules.end(),
              [](const ObjectAutoDepositParameters& left,
                 const ObjectAutoDepositParameters& right) {
                  return left.authoredOrder < right.authoredOrder;
              });
    return plan;
}

} // namespace game
