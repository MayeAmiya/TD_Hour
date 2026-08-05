#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/plan/status/ObjectPoisonedPlanTypes.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <charconv>
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

void appendDiagnostic(ObjectPoisonedPlan& plan, const ModuleData& module,
                      container::String message) {
    const container::String tag = !module.moduleTag.empty() ? module.moduleTag
                           : !module.tag.empty() ? module.tag
                                                 : container::String{moduleClass(module)};
    plan.diagnostics.push_back(tag + ": " + std::move(message));
}

[[nodiscard]] uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept {
    return right > std::numeric_limits<uint64_t>::max() - left
        ? std::numeric_limits<uint64_t>::max() : left + right;
}

} // namespace

container::SharedPtr<const ObjectPoisonedPlan>
compileObjectPoisonedPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectPoisonedPlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!asciiEqualIgnoreCase(moduleClass(module), "PoisonedBehavior")) continue;

        ObjectPoisonedParameters parameters;
        parameters.authoredOrder = module.authoredOrder;
        const auto parseField = [&](container::StringView key, uint32_t& destination) {
            if (const container::String* value = moduleValueLast(module, key)) {
                if (const std::optional<uint32_t> parsed =
                        parseDurationMilliseconds(*value)) {
                    destination = *parsed;
                } else {
                    appendDiagnostic(*plan, module,
                                     container::String(key) +
                                         " must be an unsigned duration in milliseconds");
                }
            }
        };
        parseField("PoisonDamageInterval",
                   parameters.poisonDamageIntervalMilliseconds);
        parseField("PoisonDuration", parameters.poisonDurationMilliseconds);
        plan->rules.push_back(parameters);
    }
    if (plan->rules.empty()) return nullptr;
    std::sort(plan->rules.begin(), plan->rules.end(),
              [](const ObjectPoisonedParameters& left,
                 const ObjectPoisonedParameters& right) {
                  return left.authoredOrder < right.authoredOrder;
              });
    return plan;
}

} // namespace game
