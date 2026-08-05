#include "game/object/plan/economy/ObjectEconomyPlanTypes.h"

#include "core/container/string_utils.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/plan/economy/ObjectBuilderPlanTypes.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/base/GameBalanceConstants.h"
#include "core/math/fixed/q32_32_trig.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>

#include <cmath>
#include <limits>
#include <optional>

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
    const game::ModuleData& module) noexcept {
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass}
                                       : container::StringView{module.type};
}

[[nodiscard]] const container::String* moduleValueLast(
    const game::ModuleData& module, container::StringView key) noexcept {
    for (auto it = module.values.rbegin(); it != module.values.rend(); ++it) {
        if (equalInsensitive(it->first, key)) return &it->second;
    }
    for (const auto& [entryKey, value] : module.properties) {
        if (equalInsensitive(entryKey, key)) return &value;
    }
    return nullptr;
}

[[nodiscard]] std::optional<double> parseFiniteDouble(
    container::StringView value) noexcept {
    value = trim(value);
    if (value.empty()) return std::nullopt;
    double parsed = 0.0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto [cursor, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || cursor != end || !std::isfinite(parsed)) {
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] std::optional<uint32_t> parseUnsigned(
    container::StringView value) noexcept {
    value = trim(value);
    if (value.empty() || value.front() == '-') return std::nullopt;
    uint64_t parsed = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto [cursor, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || cursor != end ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(parsed);
}

[[nodiscard]] std::optional<int32_t> parseSigned(
    container::StringView value) noexcept {
    value = trim(value);
    if (value.empty()) return std::nullopt;
    int64_t parsed = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto [cursor, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || cursor != end ||
        parsed < std::numeric_limits<int32_t>::min() ||
        parsed > std::numeric_limits<int32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<int32_t>(parsed);
}

[[nodiscard]] std::optional<bool> parseBoolean(
    container::StringView value) noexcept {
    value = trim(value);
    if (equalInsensitive(value, "YES") || equalInsensitive(value, "TRUE") ||
        value == "1") {
        return true;
    }
    if (equalInsensitive(value, "NO") || equalInsensitive(value, "FALSE") ||
        value == "0") {
        return false;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<math::q32_32> parseNonNegativeFixed(
    container::StringView value) noexcept {
    const std::optional<double> parsed = parseFiniteDouble(value);
    if (!parsed || *parsed < 0.0 ||
        *parsed >= static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return std::nullopt;
    }
    return math::q32_32{*parsed};
}

[[nodiscard]] std::optional<math::q32_32> parseRatio(
    container::StringView value) noexcept {
    const std::optional<double> parsed = parseFiniteDouble(value);
    if (!parsed || *parsed < 0.0) return std::nullopt;
    return math::q32_32{*parsed};
}


void parseSupplyDockRule(const game::ModuleData& module,
                         game::ObjectSupplyDockRule& rule,
                         container::Vector<container::String>& diagnostics,
                         container::StringView moduleName) {
    if (const container::String* value =
            moduleValueLast(module, "NumberApproachPositions")) {
        if (const std::optional<int32_t> parsed = parseSigned(*value);
            parsed && *parsed >= -1) {
            rule.numberApproachPositions = *parsed;
        } else {
            diagnostics.push_back(container::String{moduleName} +
                " NumberApproachPositions must be -1 or non-negative");
        }
    }
    if (const container::String* value =
            moduleValueLast(module, "AllowsPassthrough")) {
        if (const std::optional<bool> parsed = parseBoolean(*value)) {
            rule.allowsPassthrough = *parsed;
        } else {
            diagnostics.push_back(container::String{moduleName} +
                " AllowsPassthrough must be boolean");
        }
    }
}


} // namespace

namespace game {

container::SharedPtr<const ObjectEconomyPlan>
compileObjectEconomyPlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog) {
    auto plan = std::make_shared<ObjectEconomyPlan>();
    const auto addPresence = [&](ObjectEconomyModuleKind kind,
                                 uint32_t authoredOrder) {
        plan->modules.push_back({.kind = kind, .authoredOrder = authoredOrder});
    };

    for (const ModuleData& module : templateData.modules) {
        const container::StringView klass = moduleClass(module);
        if (equalInsensitive(klass, "AutoFindHealingUpdate")) {
            ObjectAutoFindHealingRule rule;
            rule.authoredOrder = module.authoredOrder;
            if (const container::String* value =
                    moduleValueLast(module, "ScanRate")) {
                if (const std::optional<uint32_t> parsed =
                        parseUnsigned(*value)) {
                    rule.scanRateMilliseconds = *parsed;
                } else {
                    plan->diagnostics.push_back(
                        "AutoFindHealingUpdate ScanRate must be an unsigned integer");
                }
            }
            if (const container::String* value =
                    moduleValueLast(module, "ScanRange")) {
                if (const std::optional<math::q32_32> parsed =
                        parseNonNegativeFixed(*value)) {
                    rule.scanRange = *parsed;
                } else {
                    plan->diagnostics.push_back(
                        "AutoFindHealingUpdate ScanRange must be non-negative");
                }
            }
            if (const container::String* value =
                    moduleValueLast(module, "NeverHeal")) {
                if (const std::optional<math::q32_32> parsed =
                        parseRatio(*value)) {
                    rule.neverHealRatio = *parsed;
                }
            }
            if (const container::String* value =
                    moduleValueLast(module, "AlwaysHeal")) {
                if (const std::optional<math::q32_32> parsed =
                        parseRatio(*value)) {
                    rule.alwaysHealRatio = *parsed;
                }
            }
            plan->autoFindHealing.push_back(rule);
            addPresence(ObjectEconomyModuleKind::AutoFindHealingUpdate,
                        module.authoredOrder);
        } else if (equalInsensitive(klass, "RepairDockUpdate")) {
            ObjectRepairDockRule rule;
            rule.authoredOrder = module.authoredOrder;
            parseSupplyDockRule(module, rule.dock, plan->diagnostics,
                                "RepairDockUpdate");
            if (const container::String* value =
                    moduleValueLast(module, "TimeForFullHeal")) {
                if (const std::optional<uint32_t> parsed =
                        parseUnsigned(*value)) {
                    rule.timeForFullHealMilliseconds = *parsed;
                } else {
                    plan->diagnostics.push_back(
                        "RepairDockUpdate TimeForFullHeal must be an unsigned duration");
                }
            }
            plan->repairDocks.push_back(rule);
            addPresence(ObjectEconomyModuleKind::RepairDockUpdate,
                        module.authoredOrder);
        } else if (equalInsensitive(klass, "InternetHackContain")) {
            addPresence(ObjectEconomyModuleKind::InternetHackContain,
                        module.authoredOrder);
        } else if (equalInsensitive(klass, "SupplyTruckAIUpdate") ||
                   equalInsensitive(klass, "ChinookAIUpdate")) {
            const bool chinook = equalInsensitive(klass, "ChinookAIUpdate");
            ObjectSupplyTruckRule rule;
            rule.authoredOrder = module.authoredOrder;
            if (const container::String* value =
                    moduleValueLast(module, "MaxBoxes")) {
                if (const std::optional<uint32_t> parsed =
                        parseUnsigned(*value)) {
                    rule.maxBoxes = *parsed;
                }
            }
            if (const container::String* value =
                    moduleValueLast(module, "SupplyCenterActionDelay")) {
                if (const std::optional<uint32_t> parsed =
                        parseUnsigned(*value)) {
                    rule.supplyCenterActionDelayMilliseconds = *parsed;
                }
            }
            if (const container::String* value =
                    moduleValueLast(module, "SupplyWarehouseActionDelay")) {
                if (const std::optional<uint32_t> parsed =
                        parseUnsigned(*value)) {
                    rule.supplyWarehouseActionDelayMilliseconds = *parsed;
                }
            }
            if (const container::String* value =
                    moduleValueLast(module, "SupplyWarehouseScanDistance")) {
                if (const std::optional<math::q32_32> parsed =
                        parseNonNegativeFixed(*value)) {
                    rule.supplyWarehouseScanDistance = *parsed;
                }
            }
            if (const container::String* value =
                    moduleValueLast(module, "SuppliesDepletedVoice")) {
                rule.suppliesDepletedVoice = *value;
            }
            if (chinook) {
                rule.airborneTransport = true;
                if (const container::String* value =
                        moduleValueLast(module, "UpgradedSupplyBoost")) {
                    if (const std::optional<uint32_t> parsed =
                            parseUnsigned(*value)) {
                        rule.upgradedSupplyBoost = *parsed;
                    }
                }
                rule.usesUpgradedSupplyBoost =
                    rule.upgradedSupplyBoost != 0;
                if (upgradeCatalog) {
                    if (const engine::UpgradeDefinition* definition =
                            upgradeCatalog->find(
                                engine::well_known_upgrade::
                                    AmericaSupplyLines)) {
                        rule.upgradedSupplyBoostUpgrade = definition->id;
                    }
                }
            }
            plan->supplyTrucks.push_back(std::move(rule));
            addPresence(chinook
                            ? ObjectEconomyModuleKind::ChinookAIUpdate
                            : ObjectEconomyModuleKind::SupplyTruckAIUpdate,
                        module.authoredOrder);
        } else if (equalInsensitive(klass, "HackInternetAIUpdate")) {
            ObjectHackInternetRule rule;
            rule.authoredOrder = module.authoredOrder;
            const auto parseDuration = [&](container::StringView key,
                                           uint32_t& destination) {
                if (const container::String* value = moduleValueLast(module, key)) {
                    if (const std::optional<uint32_t> parsed = parseUnsigned(*value)) {
                        destination = *parsed;
                    } else {
                        plan->diagnostics.push_back(
                            "HackInternetAIUpdate " + container::String{key} +
                            " must be an unsigned duration");
                    }
                }
            };
            parseDuration("UnpackTime", rule.unpackTimeMilliseconds);
            parseDuration("PackTime", rule.packTimeMilliseconds);
            parseDuration("CashUpdateDelay", rule.cashUpdateDelayMilliseconds);
            parseDuration("CashUpdateDelayFast",
                          rule.cashUpdateDelayFastMilliseconds);
            parseDuration("RegularCashAmount", rule.regularCashAmount);
            parseDuration("VeteranCashAmount", rule.veteranCashAmount);
            parseDuration("EliteCashAmount", rule.eliteCashAmount);
            parseDuration("HeroicCashAmount", rule.heroicCashAmount);
            parseDuration("XpPerCashUpdate", rule.xpPerCashUpdate);
            if (const container::String* value =
                    moduleValueLast(module, "PackUnpackVariationFactor")) {
                if (const std::optional<math::q32_32> parsed = parseRatio(*value)) {
                    rule.packUnpackVariationFactor = *parsed;
                } else {
                    plan->diagnostics.push_back(
                        "HackInternetAIUpdate PackUnpackVariationFactor must be non-negative");
                }
            }
            plan->hackInternet.push_back(rule);
            addPresence(ObjectEconomyModuleKind::HackInternetAIUpdate,
                        module.authoredOrder);
        } else if (equalInsensitive(klass, "SupplyCenterDockUpdate")) {
            ObjectSupplyCenterDockRule rule;
            rule.authoredOrder = module.authoredOrder;
            parseSupplyDockRule(module, rule.dock, plan->diagnostics,
                                "SupplyCenterDockUpdate");
            if (const container::String* value =
                    moduleValueLast(module, "GrantTemporaryStealth")) {
                if (const std::optional<uint32_t> parsed =
                        parseUnsigned(*value)) {
                    rule.grantTemporaryStealthMilliseconds = *parsed;
                }
            }
            plan->supplyCenterDocks.push_back(rule);
            addPresence(ObjectEconomyModuleKind::SupplyCenterDockUpdate,
                        module.authoredOrder);
        } else if (equalInsensitive(klass, "SupplyWarehouseDockUpdate")) {
            ObjectSupplyWarehouseDockRule rule;
            rule.authoredOrder = module.authoredOrder;
            parseSupplyDockRule(module, rule.dock, plan->diagnostics,
                                "SupplyWarehouseDockUpdate");
            if (const container::String* value =
                    moduleValueLast(module, "StartingBoxes")) {
                if (const std::optional<uint32_t> parsed =
                        parseUnsigned(*value)) {
                    rule.startingBoxes = *parsed;
                }
            }
            if (const container::String* value =
                    moduleValueLast(module, "DeleteWhenEmpty")) {
                if (const std::optional<bool> parsed = parseBoolean(*value)) {
                    rule.deleteWhenEmpty = *parsed;
                }
            }
            plan->supplyWarehouseDocks.push_back(rule);
            addPresence(ObjectEconomyModuleKind::SupplyWarehouseDockUpdate,
                        module.authoredOrder);
        } else if (equalInsensitive(klass, "WorkerAIUpdate")) {
            // WorkerAI is composition in the modern runtime: the builder
            // controller owns construction/repair while this shared supply
            // rule owns the worker's idle economy brain.
            ObjectSupplyTruckRule rule;
            rule.authoredOrder = module.authoredOrder;
            rule.workerMode = true;
            rule.usesUpgradedSupplyBoost = true;
            if (const container::String* value = moduleValueLast(module, "MaxBoxes"))
                if (const std::optional<uint32_t> parsed = parseUnsigned(*value))
                    rule.maxBoxes = *parsed;
            if (const container::String* value =
                    moduleValueLast(module, "SupplyCenterActionDelay"))
                if (const std::optional<uint32_t> parsed = parseUnsigned(*value))
                    rule.supplyCenterActionDelayMilliseconds = *parsed;
            if (const container::String* value =
                    moduleValueLast(module, "SupplyWarehouseActionDelay"))
                if (const std::optional<uint32_t> parsed = parseUnsigned(*value))
                    rule.supplyWarehouseActionDelayMilliseconds = *parsed;
            if (const container::String* value =
                    moduleValueLast(module, "SupplyWarehouseScanDistance"))
                if (const std::optional<math::q32_32> parsed =
                        parseNonNegativeFixed(*value))
                    rule.supplyWarehouseScanDistance = *parsed;
            if (const container::String* value =
                    moduleValueLast(module, "SuppliesDepletedVoice"))
                rule.suppliesDepletedVoice = *value;
            if (const container::String* value =
                    moduleValueLast(module, "UpgradedSupplyBoost"))
                if (const std::optional<uint32_t> parsed = parseUnsigned(*value))
                    rule.upgradedSupplyBoost = *parsed;
            if (upgradeCatalog) {
                if (const engine::UpgradeDefinition* definition =
                        upgradeCatalog->find(
                            engine::well_known_upgrade::GlaWorkerShoes)) {
                    rule.upgradedSupplyBoostUpgrade = definition->id;
                }
            }
            plan->supplyTrucks.push_back(std::move(rule));
            addPresence(ObjectEconomyModuleKind::WorkerAIUpdate,
                        module.authoredOrder);
        }
    }

    if (plan->modules.empty()) return nullptr;
    std::sort(plan->autoFindHealing.begin(), plan->autoFindHealing.end(),
              [](const ObjectAutoFindHealingRule& left,
                 const ObjectAutoFindHealingRule& right) {
                  return left.authoredOrder < right.authoredOrder;
              });
    std::sort(plan->repairDocks.begin(), plan->repairDocks.end(),
              [](const ObjectRepairDockRule& left,
                 const ObjectRepairDockRule& right) {
                  return left.authoredOrder < right.authoredOrder;
              });
    std::sort(plan->supplyTrucks.begin(), plan->supplyTrucks.end(),
              [](const ObjectSupplyTruckRule& left,
                 const ObjectSupplyTruckRule& right) {
                  return left.authoredOrder < right.authoredOrder;
              });
    std::sort(plan->hackInternet.begin(), plan->hackInternet.end(),
              [](const ObjectHackInternetRule& left,
                 const ObjectHackInternetRule& right) {
                  return left.authoredOrder < right.authoredOrder;
              });
    std::sort(plan->supplyCenterDocks.begin(), plan->supplyCenterDocks.end(),
              [](const ObjectSupplyCenterDockRule& left,
                 const ObjectSupplyCenterDockRule& right) {
                  return left.authoredOrder < right.authoredOrder;
              });
    std::sort(plan->supplyWarehouseDocks.begin(),
              plan->supplyWarehouseDocks.end(),
              [](const ObjectSupplyWarehouseDockRule& left,
                 const ObjectSupplyWarehouseDockRule& right) {
                  return left.authoredOrder < right.authoredOrder;
              });
    std::sort(plan->modules.begin(), plan->modules.end(),
              [](const ObjectEconomyModulePresence& left,
                 const ObjectEconomyModulePresence& right) {
                  return left.authoredOrder < right.authoredOrder;
              });
    return plan;
}

} // namespace game
