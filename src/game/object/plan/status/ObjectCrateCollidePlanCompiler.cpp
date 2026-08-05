#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/plan/status/ObjectCrateCollidePlanTypes.h"

#include "game/data/base/ScienceCatalog.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/CombatProfile.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/contracts/ObjectDeathReaction.h"
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
#include <utility>

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
    for (auto found = module.values.rbegin(); found != module.values.rend(); ++found) {
        if (asciiEqualIgnoreCase(found->first, key)) return &found->second;
    }
    for (const auto& [candidate, value] : module.properties) {
        if (asciiEqualIgnoreCase(candidate, key)) return &value;
    }
    return nullptr;
}

[[nodiscard]] std::optional<uint32_t> parseUnsigned(container::StringView value) noexcept {
    value = trimAsciiView(value);
    uint32_t parsed = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
    return parsed;
}

[[nodiscard]] std::optional<int32_t> parseSigned(container::StringView value) noexcept {
    value = trimAsciiView(value);
    int32_t parsed = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
    return parsed;
}


[[nodiscard]] std::optional<math::q32_32> parsePercentFixed(
    container::StringView value) noexcept {
    value = trimAsciiView(value);
    bool percent = false;
    if (!value.empty() && value.back() == '%') {
        value.remove_suffix(1);
        percent = true;
    }
    const std::optional<float> parsed = parseContentFloat(value, {
        .source = __FILE__, .block = "Object", .module = "CrateCollide",
        .field = "Percent"});
    if (!parsed) return std::nullopt;
    const double normalized = percent
        ? static_cast<double>(*parsed) / 100.0
        : static_cast<double>(*parsed);
    constexpr double kMinimum =
        static_cast<double>(std::numeric_limits<int32_t>::min());
    constexpr double kMaximumExclusive =
        static_cast<double>(std::numeric_limits<int32_t>::max()) + 1.0;
    if (!std::isfinite(normalized) || normalized < kMinimum ||
        normalized >= kMaximumExclusive) {
        return std::nullopt;
    }
    return math::q32_32{normalized};
}

[[nodiscard]] std::optional<bool> parseBoolean(container::StringView value) noexcept {
    value = trimAsciiView(value);
    if (asciiEqualIgnoreCase(value, "yes") || asciiEqualIgnoreCase(value, "true") ||
        value == "1") return true;
    if (asciiEqualIgnoreCase(value, "no") || asciiEqualIgnoreCase(value, "false") ||
        value == "0") return false;
    return std::nullopt;
}

[[nodiscard]] container::Vector<container::StringView> splitPairTokens(container::StringView value) {
    container::Vector<container::StringView> result;
    size_t cursor = 0;
    while (cursor < value.size()) {
        while (cursor < value.size() &&
               (std::isspace(static_cast<unsigned char>(value[cursor])) ||
                value[cursor] == ':' || value[cursor] == ',')) ++cursor;
        const size_t begin = cursor;
        while (cursor < value.size() &&
               !std::isspace(static_cast<unsigned char>(value[cursor])) &&
               value[cursor] != ':' && value[cursor] != ',') ++cursor;
        if (begin != cursor) result.push_back(value.substr(begin, cursor - begin));
    }
    return result;
}

[[nodiscard]] std::optional<ObjectCrateUpgradeBoost>
parseUpgradeBoost(container::StringView value) {
    const container::Vector<container::StringView> tokens = splitPairTokens(value);
    if (tokens.size() != 4 ||
        !asciiEqualIgnoreCase(tokens[0], "UpgradeType") ||
        !asciiEqualIgnoreCase(tokens[2], "Boost")) return std::nullopt;
    const std::optional<int32_t> amount = parseSigned(tokens[3]);
    if (!amount || tokens[1].empty()) return std::nullopt;
    return ObjectCrateUpgradeBoost{.upgrade = container::String(tokens[1]), .amount = *amount};
}

void appendDiagnostic(ObjectCrateCollidePlan& plan, const ModuleData& module,
                      container::String message) {
    const container::String tag = !module.moduleTag.empty() ? module.moduleTag
                           : !module.tag.empty() ? module.tag
                                                 : container::String{moduleClass(module)};
    plan.diagnostics.push_back(tag + ": " + std::move(message));
}

[[nodiscard]] std::optional<ObjectCrateCollideKind>
crateKind(container::StringView name) noexcept {
    if (asciiEqualIgnoreCase(name, "HealCrateCollide")) return ObjectCrateCollideKind::Heal;
    if (asciiEqualIgnoreCase(name, "MoneyCrateCollide")) return ObjectCrateCollideKind::Money;
    if (asciiEqualIgnoreCase(name, "ShroudCrateCollide")) return ObjectCrateCollideKind::Shroud;
    if (asciiEqualIgnoreCase(name, "UnitCrateCollide")) return ObjectCrateCollideKind::Unit;
    if (asciiEqualIgnoreCase(name, "VeterancyCrateCollide")) return ObjectCrateCollideKind::Veterancy;
    if (asciiEqualIgnoreCase(name, "SalvageCrateCollide")) return ObjectCrateCollideKind::Salvage;
    if (asciiEqualIgnoreCase(name, "ConvertToCarBombCrateCollide"))
        return ObjectCrateCollideKind::ConvertToCarBomb;
    if (asciiEqualIgnoreCase(name, "ConvertToHijackedVehicleCrateCollide"))
        return ObjectCrateCollideKind::ConvertToHijackedVehicle;
    if (asciiEqualIgnoreCase(name, "SabotageCommandCenterCrateCollide"))
        return ObjectCrateCollideKind::SabotageCommandCenter;
    if (asciiEqualIgnoreCase(name, "SabotageFakeBuildingCrateCollide"))
        return ObjectCrateCollideKind::SabotageFakeBuilding;
    if (asciiEqualIgnoreCase(name, "SabotageInternetCenterCrateCollide"))
        return ObjectCrateCollideKind::SabotageInternetCenter;
    if (asciiEqualIgnoreCase(name, "SabotageMilitaryFactoryCrateCollide"))
        return ObjectCrateCollideKind::SabotageMilitaryFactory;
    if (asciiEqualIgnoreCase(name, "SabotagePowerPlantCrateCollide"))
        return ObjectCrateCollideKind::SabotagePowerPlant;
    if (asciiEqualIgnoreCase(name, "SabotageSuperweaponCrateCollide"))
        return ObjectCrateCollideKind::SabotageSuperweapon;
    if (asciiEqualIgnoreCase(name, "SabotageSupplyCenterCrateCollide"))
        return ObjectCrateCollideKind::SabotageSupplyCenter;
    if (asciiEqualIgnoreCase(name, "SabotageSupplyDropzoneCrateCollide"))
        return ObjectCrateCollideKind::SabotageSupplyDropzone;
    return std::nullopt;
}

} // namespace

container::SharedPtr<const ObjectCrateCollidePlan>
compileObjectCrateCollidePlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectCrateCollidePlan>();
    for (const ModuleData& module : templateData.modules) {
        const std::optional<ObjectCrateCollideKind> kind = crateKind(moduleClass(module));
        if (!kind) continue;

        ObjectCrateCollideRule rule;
        rule.kind = *kind;
        rule.authoredOrder = module.authoredOrder;
        if (const container::String* value = moduleValueLast(module, "RequiredKindOf"))
            static_cast<void>(compileObjectKindOfMask(
                *value, rule.requiredKinds));
        if (const container::String* value = moduleValueLast(module, "ForbiddenKindOf"))
            static_cast<void>(compileObjectKindOfMask(
                *value, rule.forbiddenKinds));
        if (const container::String* value = moduleValueLast(module, "PickupScience")) {
            const container::StringView parsed = trimAsciiView(*value);
            if (!asciiEqualIgnoreCase(parsed, "NONE") &&
                !asciiEqualIgnoreCase(parsed, "SCIENCE_INVALID"))
                rule.pickupScience = container::String(parsed);
        }
        if (const container::String* value = moduleValueLast(module, "ExecuteFX")) {
            const container::StringView parsed = trimAsciiView(*value);
            if (!asciiEqualIgnoreCase(parsed, "NONE")) rule.executeFx = container::String(parsed);
        }
        if (const container::String* value = moduleValueLast(module, "ExecuteAnimation")) {
            const container::StringView parsed = trimAsciiView(*value);
            if (!asciiEqualIgnoreCase(parsed, "NONE"))
                rule.executeAnimation = container::String(parsed);
        }
        const auto parseBoolField = [&](container::StringView key, bool& destination) {
            if (const container::String* value = moduleValueLast(module, key)) {
                if (const std::optional<bool> parsed = parseBoolean(*value)) destination = *parsed;
                else appendDiagnostic(*plan, module, container::String(key) + " must be Yes or No");
            }
        };
        parseBoolField("ForbidOwnerPlayer", rule.forbidOwnerPlayer);
        parseBoolField("BuildingPickup", rule.buildingPickup);
        parseBoolField("HumanOnly", rule.humanOnly);
        parseBoolField("AllowMultiPickup", rule.allowMultiPickup);
        parseBoolField("ExecuteAnimationFades", rule.executeAnimationFades);
        const auto parseFixedField = [&](container::StringView key,
                                         math::q32_32& destination) {
            if (const container::String* value = moduleValueLast(module, key)) {
                if (const std::optional<float> parsed =
                        parseContentFloat(*value, {
                            .source = __FILE__, .block = "Object",
                            .module = "CrateCollide", .field = "Real",
                            .fallback = destination.to_float()})) {
                    destination = math::q32_32{*parsed};
                }
                else appendDiagnostic(*plan, module, container::String(key) + " must be a finite scalar");
            }
        };
        parseFixedField("ExecuteAnimationTime", rule.executeAnimationTimeSeconds);
        parseFixedField("ExecuteAnimationZRise", rule.executeAnimationZRisePerSecond);

        if (*kind == ObjectCrateCollideKind::Money) {
            if (const container::String* value = moduleValueLast(module, "MoneyProvided")) {
                if (const std::optional<uint32_t> parsed = parseUnsigned(*value))
                    rule.moneyProvided = *parsed;
                else appendDiagnostic(*plan, module, "MoneyProvided must be an unsigned integer");
            }
            bool sawOrderedBoost = false;
            for (const auto& [key, value] : module.values) {
                if (!asciiEqualIgnoreCase(key, "UpgradedBoost")) continue;
                sawOrderedBoost = true;
                if (std::optional<ObjectCrateUpgradeBoost> boost = parseUpgradeBoost(value))
                    rule.upgradedBoosts.push_back(std::move(*boost));
                else appendDiagnostic(*plan, module,
                    "UpgradedBoost must be UpgradeType:<name> Boost:<integer>");
            }
            if (!sawOrderedBoost) {
                for (const auto& [key, value] : module.properties) {
                    if (!asciiEqualIgnoreCase(key, "UpgradedBoost")) continue;
                    if (std::optional<ObjectCrateUpgradeBoost> boost = parseUpgradeBoost(value))
                        rule.upgradedBoosts.push_back(std::move(*boost));
                    else appendDiagnostic(*plan, module,
                        "UpgradedBoost must be UpgradeType:<name> Boost:<integer>");
                }
            }
        } else if (*kind == ObjectCrateCollideKind::Unit) {
            if (const container::String* value = moduleValueLast(module, "UnitCount")) {
                if (const std::optional<uint32_t> parsed = parseUnsigned(*value))
                    rule.unitCount = *parsed;
                else appendDiagnostic(*plan, module, "UnitCount must be an unsigned integer");
            }
            if (const container::String* value = moduleValueLast(module, "UnitName"))
                rule.unitName = container::String(trimAsciiView(*value));
            if (rule.unitName.empty())
                appendDiagnostic(*plan, module, "UnitName must name an Object template");
        } else if (*kind == ObjectCrateCollideKind::Veterancy) {
            if (const container::String* value = moduleValueLast(module, "EffectRange")) {
                if (const std::optional<uint32_t> parsed = parseUnsigned(*value))
                    rule.veterancyEffectRange = *parsed;
                else appendDiagnostic(*plan, module, "EffectRange must be an unsigned integer");
            }
            const auto parseVeterancyBool = [&](container::StringView key,
                                                bool& destination) {
                if (const container::String* value = moduleValueLast(module, key)) {
                    if (const std::optional<bool> parsed = parseBoolean(*value)) {
                        destination = *parsed;
                    } else {
                        appendDiagnostic(*plan, module,
                            container::String(key) + " must be Yes or No");
                    }
                }
            };
            parseVeterancyBool("AddsOwnerVeterancy",
                               rule.veterancyAddsOwnerVeterancy);
            parseVeterancyBool("IsPilot", rule.veterancyIsPilot);
        } else if (*kind == ObjectCrateCollideKind::Salvage) {
            const auto parseChance = [&](container::StringView key,
                                         math::q32_32& destination) {
                if (const container::String* value =
                        moduleValueLast(module, key)) {
                    if (const std::optional<math::q32_32> parsed =
                            parsePercentFixed(*value)) {
                        destination = *parsed;
                    } else {
                        appendDiagnostic(*plan, module,
                            container::String{key} +
                            " must be a finite percent");
                    }
                }
            };
            const auto parseMoney = [&](container::StringView key,
                                        int32_t& destination) {
                if (const container::String* value =
                        moduleValueLast(module, key)) {
                    if (const std::optional<int32_t> parsed =
                            parseSigned(*value)) {
                        destination = *parsed;
                    } else {
                        appendDiagnostic(*plan, module,
                            container::String{key} +
                            " must be a signed integer");
                    }
                }
            };
            parseChance("WeaponChance", rule.salvageWeaponChance);
            parseChance("LevelChance", rule.salvageLevelChance);
            parseChance("MoneyChance", rule.salvageMoneyChance);
            parseMoney("MinMoney", rule.salvageMinimumMoney);
            parseMoney("MaxMoney", rule.salvageMaximumMoney);
        } else if (*kind == ObjectCrateCollideKind::ConvertToCarBomb) {
            if (const container::String* value = moduleValueLast(module, "FXList")) {
                const container::StringView parsed = trimAsciiView(*value);
                if (!asciiEqualIgnoreCase(parsed, "NONE"))
                    rule.convertFxList = container::String(parsed);
            }
        } else if (*kind == ObjectCrateCollideKind::SabotageInternetCenter ||
                   *kind == ObjectCrateCollideKind::SabotageMilitaryFactory ||
                   *kind == ObjectCrateCollideKind::SabotagePowerPlant) {
            const container::StringView durationKey =
                *kind == ObjectCrateCollideKind::SabotagePowerPlant
                    ? container::StringView{"SabotagePowerDuration"}
                    : container::StringView{"SabotageDuration"};
            if (const container::String* value =
                    moduleValueLast(module, durationKey)) {
                if (const std::optional<uint32_t> parsed =
                        parseUnsigned(*value)) {
                    rule.sabotageDurationMilliseconds = *parsed;
                } else {
                    appendDiagnostic(
                        *plan, module,
                        container::String{durationKey} +
                            " must be unsigned milliseconds");
                }
            }
        } else if (*kind == ObjectCrateCollideKind::SabotageSupplyCenter ||
                   *kind == ObjectCrateCollideKind::SabotageSupplyDropzone) {
            if (const container::String* value =
                    moduleValueLast(module, "StealCashAmount")) {
                if (const std::optional<uint32_t> parsed =
                        parseUnsigned(*value)) {
                    rule.stealCashAmount = *parsed;
                } else {
                    appendDiagnostic(
                        *plan, module,
                        "StealCashAmount must be an unsigned integer");
                }
            }
        }
        plan->rules.push_back(std::move(rule));
    }

    if (plan->rules.empty()) return nullptr;
    std::sort(plan->rules.begin(), plan->rules.end(),
              [](const ObjectCrateCollideRule& left,
                 const ObjectCrateCollideRule& right) {
                  return left.authoredOrder < right.authoredOrder;
              });
    return plan;
}

} // namespace game
