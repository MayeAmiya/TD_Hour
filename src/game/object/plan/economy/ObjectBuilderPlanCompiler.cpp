#include "game/object/plan/economy/ObjectBuilderPlanTypes.h"

#include "core/container/string_utils.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>

namespace {

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] container::StringView moduleClass(
    const game::ModuleData& module) noexcept {
    return module.moduleClass.empty() ? container::StringView{module.type}
                                      : container::StringView{module.moduleClass};
}

[[nodiscard]] const container::String* moduleValueLast(
    const game::ModuleData& module, container::StringView key) noexcept {
    for (auto found = module.values.rbegin(); found != module.values.rend();
         ++found) {
        if (equalInsensitive(found->first, key)) return &found->second;
    }
    for (const auto& [entryKey, value] : module.properties) {
        if (equalInsensitive(entryKey, key)) return &value;
    }
    return nullptr;
}

[[nodiscard]] container::StringView trim(container::StringView value) noexcept {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value;
}

[[nodiscard]] std::optional<uint32_t> parseUnsigned(
    container::StringView value) noexcept {
    value = trim(value);
    uint64_t parsed = 0;
    if (value.empty() || value.front() == '-') return std::nullopt;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() ||
        parsed > std::numeric_limits<uint32_t>::max()) return std::nullopt;
    return static_cast<uint32_t>(parsed);
}

[[nodiscard]] std::optional<math::q32_32> parseFixed(
    container::StringView value, bool percent) noexcept {
    value = trim(value);
    if (percent && !value.empty() && value.back() == '%') {
        value.remove_suffix(1);
    }
    double parsed = 0.0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() ||
        !std::isfinite(parsed) || parsed < 0.0) return std::nullopt;
    if (percent) parsed /= 100.0;
    if (parsed >= static_cast<double>(std::numeric_limits<int32_t>::max()))
        return std::nullopt;
    return math::q32_32{parsed};
}

} // namespace

namespace game {

container::SharedPtr<const ObjectBuilderPlan>
compileObjectBuilderPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectBuilderPlan>();
    for (const ModuleData& module : templateData.modules) {
        const container::StringView klass = moduleClass(module);
        const bool worker = equalInsensitive(klass, "WorkerAIUpdate");
        if (!worker && !equalInsensitive(klass, "DozerAIUpdate")) continue;
        ObjectBuilderRule rule;
        rule.authoredOrder = module.authoredOrder;
        rule.kind = worker ? ObjectBuilderKind::Worker : ObjectBuilderKind::Dozer;
        const auto readUnsigned = [&](container::StringView key,
                                      uint32_t& destination) {
            if (const container::String* value = moduleValueLast(module, key)) {
                if (const std::optional<uint32_t> parsed = parseUnsigned(*value))
                    destination = *parsed;
                else
                    plan->diagnostics.push_back(container::String{klass} + " " +
                        container::String{key} + " must be unsigned");
            }
        };
        const auto readFixed = [&](container::StringView key,
                                   math::q32_32& destination, bool percent) {
            if (const container::String* value = moduleValueLast(module, key)) {
                if (const std::optional<math::q32_32> parsed =
                        parseFixed(*value, percent)) destination = *parsed;
                else
                    plan->diagnostics.push_back(container::String{klass} + " " +
                        container::String{key} + " must be non-negative");
            }
        };
        readFixed("RepairHealthPercentPerSecond",
                  rule.repairHealthRatioPerSecond, true);
        readUnsigned("BoredTime", rule.boredTimeMilliseconds);
        readFixed("BoredRange", rule.boredRange, false);
        if (worker) {
            readUnsigned("MaxBoxes", rule.maxBoxes);
            readUnsigned("SupplyCenterActionDelay",
                         rule.supplyCenterActionDelayMilliseconds);
            readUnsigned("SupplyWarehouseActionDelay",
                         rule.supplyWarehouseActionDelayMilliseconds);
            readFixed("SupplyWarehouseScanDistance",
                      rule.supplyWarehouseScanDistance, false);
            readUnsigned("UpgradedSupplyBoost", rule.upgradedSupplyBoost);
            if (const container::String* voice =
                    moduleValueLast(module, "SuppliesDepletedVoice"))
                rule.suppliesDepletedVoice = *voice;
        }
        plan->rules.push_back(std::move(rule));
    }
    if (plan->rules.empty()) return nullptr;
    std::sort(plan->rules.begin(), plan->rules.end(),
              [](const ObjectBuilderRule& left,
                 const ObjectBuilderRule& right) {
                  return left.authoredOrder < right.authoredOrder;
              });
    return plan;
}

} // namespace game
