#include "game/object/plan/status/ObjectBaseRegeneratePlanTypes.h"

#include "core/container/string_utils.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"

#include <memory>

namespace game {
namespace {

[[nodiscard]] container::StringView moduleClass(
    const ModuleData& module) noexcept {
    return !module.moduleClass.empty()
        ? container::StringView{module.moduleClass}
        : container::StringView{module.type};
}

} // namespace

container::SharedPtr<const ObjectBaseRegeneratePlan>
compileObjectBaseRegeneratePlan(const ThingTemplate& templateData) {
    auto plan = std::make_shared<ObjectBaseRegeneratePlan>();
    for (const ModuleData& module : templateData.modules) {
        if (!container::asciiEqualIgnoreCase(
                moduleClass(module), "BaseRegenerateUpdate")) {
            continue;
        }
        plan->rules.push_back({.authoredOrder = module.authoredOrder});
    }
    return plan->rules.empty() ? nullptr : plan;
}

} // namespace game
