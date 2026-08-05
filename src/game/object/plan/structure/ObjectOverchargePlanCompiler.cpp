#include "game/object/plan/structure/ObjectOverchargePlanTypes.h"

#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/base/DamageTypes.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

namespace game {
namespace {

using container::asciiEqualIgnoreCase;
constexpr auto trim = container::trimAsciiView;

[[nodiscard]] container::StringView moduleClass(
    const ModuleData& module) noexcept {
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

[[nodiscard]] std::optional<math::q32_32> parsePercentToReal(
    container::StringView value, math::q32_32 fallback = {}) noexcept {
    value = trim(value);
    const bool hasPercentSuffix = !value.empty() && value.back() == '%';
    if (hasPercentSuffix) value.remove_suffix(1);
    value = trim(value);
    const game::ContentFloatContext context{
        .source = __FILE__, .block = "Object",
        .module = "OverchargeBehavior", .field = "Percent",
        .fallback = fallback.to_float()};
    const std::optional<float> parsed =
        game::parseContentFloat(value, context);
    if (!parsed) return fallback;
    const double ratio = hasPercentSuffix || *parsed > 1.0f
        ? static_cast<double>(*parsed) / 100.0
        : static_cast<double>(*parsed);
    constexpr double kQ32Minimum = -2'147'483'648.0;
    constexpr double kQ32MaximumExclusive = 2'147'483'648.0;
    if (ratio < kQ32Minimum || ratio >= kQ32MaximumExclusive) {
        game::warnContentFloatFallback(
            value, context,
            "finite percent prefix is outside the Q32.32 field range; retained the prior/default value");
        return fallback;
    }
    if (ratio < 0.0) {
        game::processContentDiagnostics().warn({
            .source = container::String{context.source},
            .block = container::String{context.block},
            .module = container::String{context.module},
            .field = container::String{context.field},
            .rawValue = container::String{value},
            .adoptedValue = std::to_string(ratio),
            .reason = "negative authored percent is unusual but Q32.32-representable; retained for legacy compatibility",
        });
    }
    return math::q32_32{static_cast<float>(ratio)};
}

void appendDiagnostic(ObjectOverchargePlan& plan, const ModuleData& module,
                      container::StringView message) {
    const container::String tag = !module.moduleTag.empty()
        ? module.moduleTag
        : !module.tag.empty() ? module.tag : container::String{moduleClass(module)};
    plan.diagnostics.push_back(tag + ": " + container::String(message));
}

} // namespace

container::SharedPtr<const ObjectOverchargePlan>
compileObjectOverchargePlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectOverchargePlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!asciiEqualIgnoreCase(moduleClass(module), "OverchargeBehavior")) {
            continue;
        }

        ObjectOverchargeParameters parameters;
        parameters.authoredOrder = module.authoredOrder;
        if (const container::String* value =
                moduleValueLast(module, "HealthPercentToDrainPerSecond")) {
            if (const std::optional<math::q32_32> parsed =
                    parsePercentToReal(
                        *value, parameters.healthPercentToDrainPerSecond)) {
                parameters.healthPercentToDrainPerSecond = *parsed;
            } else {
                appendDiagnostic(*plan, module,
                                 "HealthPercentToDrainPerSecond must be a percent");
            }
        }
        if (const container::String* value =
                moduleValueLast(module, "NotAllowedWhenHealthBelowPercent")) {
            if (const std::optional<math::q32_32> parsed =
                    parsePercentToReal(
                        *value, parameters.notAllowedWhenHealthBelowPercent)) {
                parameters.notAllowedWhenHealthBelowPercent = *parsed;
            } else {
                appendDiagnostic(
                    *plan, module,
                    "NotAllowedWhenHealthBelowPercent must be a percent");
            }
        }
        plan->rules.push_back(parameters);
    }
    if (plan->rules.empty()) return nullptr;
    std::sort(plan->rules.begin(), plan->rules.end(),
              [](const ObjectOverchargeParameters& left,
                 const ObjectOverchargeParameters& right) {
                  return left.authoredOrder < right.authoredOrder;
              });
    return plan;
}

} // namespace game
