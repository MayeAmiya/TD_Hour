#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/plan/combat/ObjectWeaponBonusUpdatePlanTypes.h"

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
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
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
    for (auto found = module.values.rbegin(); found != module.values.rend(); ++found) {
        if (asciiEqualIgnoreCase(found->first, key)) return &found->second;
    }
    for (const auto& [candidate, value] : module.properties) {
        if (asciiEqualIgnoreCase(candidate, key)) return &value;
    }
    return nullptr;
}

[[nodiscard]] std::optional<uint32_t> parseDurationMilliseconds(
    container::StringView value) noexcept {
    value = trim(value);
    uint32_t parsed = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] std::optional<float> parseFiniteFloat(
    container::StringView value) noexcept {
    return parseContentFloat(value, {
        .source = __FILE__, .block = "Object",
        .module = "WeaponBonusUpdate", .field = "Real"});
}

void appendDiagnostic(ObjectWeaponBonusUpdatePlan& plan,
                      const ModuleData& module, container::String message) {
    const container::String tag = !module.moduleTag.empty() ? module.moduleTag
                           : !module.tag.empty() ? module.tag
                                                 : container::String{moduleClass(module)};
    plan.diagnostics.push_back(tag + ": " + std::move(message));
}

} // namespace

container::SharedPtr<const ObjectWeaponBonusUpdatePlan>
compileObjectWeaponBonusUpdatePlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectWeaponBonusUpdatePlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!asciiEqualIgnoreCase(moduleClass(module), "WeaponBonusUpdate")) continue;

        ObjectWeaponBonusUpdateParameters parameters;
        parameters.authoredOrder = module.authoredOrder;
        if (const container::String* value =
                moduleValueLast(module, "RequiredAffectKindOf")) {
            if (!compileObjectKindOfMask(
                    *value, parameters.requiredAffectKinds)) {
                appendDiagnostic(*plan, module,
                                 "RequiredAffectKindOf contains an unknown KindOf");
            }
        }
        if (const container::String* value =
                moduleValueLast(module, "ForbiddenAffectKindOf")) {
            if (!compileObjectKindOfMask(
                    *value, parameters.forbiddenAffectKinds)) {
                appendDiagnostic(*plan, module,
                                 "ForbiddenAffectKindOf contains an unknown KindOf");
            }
        }
        const auto parseDuration = [&](container::StringView key,
                                       uint32_t& destination) {
            if (const container::String* value = moduleValueLast(module, key)) {
                if (const std::optional<uint32_t> parsed =
                        parseDurationMilliseconds(*value)) {
                    destination = *parsed;
                } else {
                    appendDiagnostic(*plan, module,
                                     container::String(key) +
                                         " must be a non-negative duration in milliseconds");
                }
            }
        };
        parseDuration("BonusDuration", parameters.bonusDurationMilliseconds);
        parseDuration("BonusDelay", parameters.bonusDelayMilliseconds);
        if (const container::String* value = moduleValueLast(module, "BonusRange")) {
            if (const std::optional<float> parsed = parseFiniteFloat(*value)) {
                if (*parsed < 0.0f) {
                    parameters.bonusRange = math::q32_32{int32_t{-1}};
                } else {
                    parameters.bonusRange = math::q32_32{*parsed};
                }
            } else {
                appendDiagnostic(*plan, module,
                                 "BonusRange must be a finite non-negative scalar");
            }
        }
        if (const container::String* value =
                moduleValueLast(module, "BonusConditionType")) {
            if (const std::optional<WeaponBonusCondition> parsed =
                    parseWeaponBonusCondition(*value)) {
                parameters.bonusCondition = *parsed;
            } else {
                appendDiagnostic(*plan, module,
                                 "BonusConditionType is not a known WeaponBonus condition");
            }
        } else {
            appendDiagnostic(*plan, module, "BonusConditionType is required");
        }
        plan->rules.push_back(std::move(parameters));
    }
    if (plan->rules.empty()) return nullptr;
    std::sort(plan->rules.begin(), plan->rules.end(),
              [](const ObjectWeaponBonusUpdateParameters& left,
                 const ObjectWeaponBonusUpdateParameters& right) {
                  return left.authoredOrder < right.authoredOrder;
              });
    return plan;
}

} // namespace game
