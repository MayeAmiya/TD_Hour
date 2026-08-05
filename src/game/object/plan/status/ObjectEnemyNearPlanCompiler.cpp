#include "game/object/plan/status/ObjectEnemyNearPlanTypes.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/MapVisibilityAuthority.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace game {
namespace {

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] container::StringView moduleClass(
    const ModuleData& module) noexcept {
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass}
                                       : container::StringView{module.type};
}

[[nodiscard]] const container::String* moduleValueLast(
    const ModuleData& module, container::StringView key) noexcept {
    for (auto it = module.values.rbegin(); it != module.values.rend(); ++it) {
        if (equalInsensitive(it->first, key)) return &it->second;
    }
    const auto found = module.properties.find(container::String{key});
    return found == module.properties.end() ? nullptr : &found->second;
}

[[nodiscard]] uint32_t parseMilliseconds(container::StringView value,
                                         uint32_t fallback) noexcept {
    const std::optional<float> parsed =
        parseContentFloat(value, {
            .source = __FILE__, .block = "Object", .module = "EnemyNear",
            .field = "Duration", .fallback = static_cast<float>(fallback)});
    if (!parsed || *parsed < 0.0f) {
        return fallback;
    }
    if (*parsed >= static_cast<float>(std::numeric_limits<uint32_t>::max())) {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(std::ceil(*parsed));
}

} // namespace

container::SharedPtr<const ObjectEnemyNearPlan>
compileObjectEnemyNearPlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectEnemyNearPlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!equalInsensitive(moduleClass(module), "EnemyNearUpdate")) {
            continue;
        }

        ObjectEnemyNearRule rule;
        rule.authoredOrder = module.authoredOrder;
        rule.scanDelayMilliseconds = 1000u;
        if (const container::String* value =
                moduleValueLast(module, "ScanDelayTime")) {
            rule.scanDelayMilliseconds = parseMilliseconds(*value, 1000u);
        }
        plan->rules.push_back(rule);
    }

    if (plan->rules.empty()) return nullptr;
    std::sort(plan->rules.begin(), plan->rules.end(),
              [](const ObjectEnemyNearRule& left,
                 const ObjectEnemyNearRule& right) {
                  return left.authoredOrder < right.authoredOrder;
              });
    return plan;
}

} // namespace game
