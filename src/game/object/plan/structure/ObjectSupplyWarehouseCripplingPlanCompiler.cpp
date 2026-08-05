#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/plan/structure/ObjectSupplyWarehouseCripplingPlanTypes.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cerrno>
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
    value = trim(value);
    if (value.empty()) return std::nullopt;
    uint64_t parsed = 0;
    for (const char character : value) {
        if (character < '0' || character > '9') return std::nullopt;
        const uint64_t digit = static_cast<uint64_t>(character - '0');
        if (parsed > (std::numeric_limits<uint32_t>::max() - digit) / 10u) {
            return std::nullopt;
        }
        parsed = parsed * 10u + digit;
    }
    return static_cast<uint32_t>(parsed);
}

[[nodiscard]] std::optional<math::q32_32> parseFixed(
    container::StringView value, math::q32_32 fallback = {}) noexcept {
    value = trim(value);
    const game::ContentFloatContext context{
        .source = __FILE__, .block = "Object",
        .module = "SupplyWarehouseCripplingBehavior",
        .field = "FixedReal", .fallback = fallback.to_float()};
    const std::optional<float> parsed =
        game::parseContentFloat(value, context);
    if (!parsed) return fallback;

    constexpr long double scale = 4294967296.0L;
    const long double raw = static_cast<long double>(*parsed) * scale;
    if (raw < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
        raw > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        game::warnContentFloatFallback(
            value, context,
            "finite numeric prefix is outside the Q32.32 field range; retained the prior/default value");
        return fallback;
    }
    return math::q32_32::from_raw(static_cast<int64_t>(raw));
}

void appendDiagnostic(ObjectSupplyWarehouseCripplingPlan& plan,
                      const ModuleData& module, container::String message) {
    const container::String tag = !module.moduleTag.empty() ? module.moduleTag
                           : !module.tag.empty() ? module.tag
                                                 : container::String{moduleClass(module)};
    plan.diagnostics.push_back(tag + ": " + std::move(message));
}

} // namespace

container::SharedPtr<const ObjectSupplyWarehouseCripplingPlan>
compileObjectSupplyWarehouseCripplingPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectSupplyWarehouseCripplingPlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!asciiEqualIgnoreCase(moduleClass(module),
                                  "SupplyWarehouseCripplingBehavior")) {
            continue;
        }

        ObjectSupplyWarehouseCripplingParameters parameters;
        parameters.authoredOrder = module.authoredOrder;
        const auto parseDuration = [&](container::StringView key,
                                       uint32_t& destination) {
            if (const container::String* value = moduleValueLast(module, key)) {
                if (const std::optional<uint32_t> parsed = parseUnsigned(*value)) {
                    destination = *parsed;
                } else {
                    appendDiagnostic(*plan, module, container::String(key) +
                        " must be an unsigned duration in milliseconds");
                }
            }
        };
        // Preserve the misspelling in the shipped INI/schema.
        parseDuration("SelfHealSupression",
                      parameters.selfHealSuppressionMilliseconds);
        parseDuration("SelfHealDelay", parameters.selfHealDelayMilliseconds);
        if (const container::String* value =
                moduleValueLast(module, "SelfHealAmount")) {
            if (const std::optional<math::q32_32> parsed =
                    parseFixed(*value, parameters.selfHealAmount)) {
                parameters.selfHealAmount = *parsed;
            } else {
                appendDiagnostic(*plan, module,
                    "SelfHealAmount must be a finite Q32.32-representable value");
            }
        }
        plan->rules.push_back(parameters);
    }

    if (plan->rules.empty()) return nullptr;
    std::sort(plan->rules.begin(), plan->rules.end(),
              [](const ObjectSupplyWarehouseCripplingParameters& left,
                 const ObjectSupplyWarehouseCripplingParameters& right) {
                  return left.authoredOrder < right.authoredOrder;
              });
    return plan;
}

} // namespace game
