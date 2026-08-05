#include "game/object/plan/combat/ObjectSmartBombPlanTypes.h"

#include "core/container/string_utils.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <memory>
#include <optional>

namespace game {
namespace {

constexpr auto equal = container::asciiEqualIgnoreCase;

[[nodiscard]] const container::String* valueLast(
    const ModuleData& module, container::StringView key) noexcept {
    for (auto it = module.values.rbegin(); it != module.values.rend(); ++it) {
        if (equal(it->first, key)) return &it->second;
    }
    return nullptr;
}

[[nodiscard]] std::optional<math::q32_32> parseFixed(
    container::StringView value) noexcept {
    double parsed = 0.0;
    const auto [cursor, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || cursor != value.data() + value.size() ||
        !std::isfinite(parsed)) return std::nullopt;
    return math::q32_32{parsed};
}

} // namespace

container::SharedPtr<const ObjectSmartBombPlan>
compileObjectSmartBombPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectSmartBombPlan>();
    for (const ModuleData& module : templateData.modules) {
        const container::StringView klass = !module.moduleClass.empty()
            ? container::StringView{module.moduleClass}
            : container::StringView{module.type};
        if (!equal(klass, "SmartBombTargetHomingUpdate")) continue;
        ObjectSmartBombRule rule{.authoredOrder = module.authoredOrder};
        if (const container::String* value =
                valueLast(module, "CourseCorrectionScalar")) {
            if (const auto parsed = parseFixed(*value)) {
                rule.courseCorrectionScalar = math::q32_32::clamp(
                    *parsed, {}, math::q32_32{int32_t{1}});
            } else {
                plan->diagnostics.push_back(
                    "CourseCorrectionScalar must be finite");
            }
        }
        plan->rules.push_back(rule);
    }
    if (plan->rules.empty()) return {};
    std::stable_sort(plan->rules.begin(), plan->rules.end(),
        [](const ObjectSmartBombRule& left,
           const ObjectSmartBombRule& right) {
            return left.authoredOrder < right.authoredOrder;
        });
    return plan;
}

} // namespace game
