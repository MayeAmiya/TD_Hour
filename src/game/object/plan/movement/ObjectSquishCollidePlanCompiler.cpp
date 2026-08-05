#include "game/object/plan/movement/ObjectSquishCollidePlanTypes.h"
#include "core/container/string_utils.h"

#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <utility>

namespace {

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] container::StringView moduleClass(
    const game::ModuleData& module) noexcept {
    return !module.moduleClass.empty() ? container::StringView{module.moduleClass}
                                       : container::StringView{module.type};
}

} // namespace

namespace game {

container::SharedPtr<const ObjectSquishCollidePlan>
compileObjectSquishCollidePlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectSquishCollidePlan>();
    for (const ModuleData& module : templateData.modules) {
        const container::StringView klass = moduleClass(module);
        if (equalInsensitive(klass, "HijackerUpdate")) {
            plan->hasHijackerUpdate = true;
        }
        if (equalInsensitive(klass, "SquishCollide")) {
            plan->rules.push_back({.authoredOrder = module.authoredOrder});
        }
    }
    if (plan->rules.empty()) return nullptr;
    std::sort(plan->rules.begin(), plan->rules.end(),
              [](const ObjectSquishCollideRule& left,
                 const ObjectSquishCollideRule& right) {
                  return left.authoredOrder < right.authoredOrder;
              });
    return plan;
}

} // namespace game
