#include "game/object/plan/movement/ObjectFloatPlanTypes.h"

#include "core/container/string_utils.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace game {
namespace {

using container::asciiEqualIgnoreCase;
constexpr auto trim = container::trimAsciiView;

[[nodiscard]] container::StringView moduleClass(
    const ModuleData& module) noexcept {
    return !module.moduleClass.empty()
        ? container::StringView{module.moduleClass}
        : container::StringView{module.type};
}

[[nodiscard]] const container::String* moduleValueLast(
    const ModuleData& module, container::StringView key) noexcept {
    const container::String* result = nullptr;
    for (const auto& [entryKey, value] : module.values) {
        if (asciiEqualIgnoreCase(entryKey, key)) result = &value;
    }
    if (result) return result;
    for (const auto& [entryKey, value] : module.properties) {
        if (asciiEqualIgnoreCase(entryKey, key)) return &value;
    }
    return nullptr;
}

[[nodiscard]] std::optional<bool> parseBoolean(
    container::StringView value) noexcept {
    value = trim(value);
    if (asciiEqualIgnoreCase(value, "YES") ||
        asciiEqualIgnoreCase(value, "TRUE") || value == "1") {
        return true;
    }
    if (asciiEqualIgnoreCase(value, "NO") ||
        asciiEqualIgnoreCase(value, "FALSE") || value == "0") {
        return false;
    }
    return std::nullopt;
}

void appendDiagnostic(ObjectFloatPlan& plan, const ModuleData& module,
                      container::StringView value) {
    const container::StringView tag = module.moduleTag.empty()
        ? container::StringView{"<untagged>"}
        : container::StringView{module.moduleTag};
    plan.diagnostics.push_back(
        "FloatUpdate (tag '" + container::String(tag) +
        "') has invalid Enabled value '" + container::String(value) + "'");
}

} // namespace

container::SharedPtr<const ObjectFloatPlan>
compileObjectFloatPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectFloatPlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!asciiEqualIgnoreCase(moduleClass(module), "FloatUpdate")) continue;

        ObjectFloatRule rule{.authoredOrder = module.authoredOrder};
        bool valid = true;
        if (const container::String* value = moduleValueLast(module, "Enabled")) {
            const std::optional<bool> parsed = parseBoolean(*value);
            if (!parsed) {
                appendDiagnostic(*plan, module, *value);
                valid = false;
            } else {
                rule.startsEnabled = *parsed;
            }
        }
        if (valid) plan->rules.push_back(std::move(rule));
    }

    std::stable_sort(plan->rules.begin(), plan->rules.end(),
        [](const ObjectFloatRule& left, const ObjectFloatRule& right) {
            return left.authoredOrder < right.authoredOrder;
        });
    return plan->rules.empty() && plan->diagnostics.empty() ? nullptr : plan;
}

} // namespace game
